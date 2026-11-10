// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QByteArray>
#include <QChar>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QStringList>
#include <Qt>
#include <functional>
#include <qcoreapplication_platform.h>
#include "decoder_host_android.h"
#include "diagnostics_log.h"
#include "display_scale.h"
#include "location_record.h"
#include "presentation_scale.h"
#include "run_status.h"

#include <QCoreApplication>
#include <QFuture>
#include <QGuiApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>
#include <QSaveFile>
#include <QScreen>
#include <QVariant>
#include <QVariantList>
#include <atomic>
#include <cmath>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <mutex>
#include <utility>

#include <jni.h>

#include "dsdneo_jni.h"

namespace dsd_android {

namespace {

constexpr const char* kServiceClass = "io/github/arancormonk/dsdneo/DecoderService";
constexpr const char* kSupportClass = "io/github/arancormonk/dsdneo/AppSupport";
constexpr const char* kLocationClass = "io/github/arancormonk/dsdneo/LocationSupport";
constexpr const char* kUsbClass = "io/github/arancormonk/dsdneo/UsbSourceManager";
constexpr const char* kScreenClass = "io/github/arancormonk/dsdneo/ScreenSupport";
constexpr const char* kDocumentsClass = "io/github/arancormonk/dsdneo/DocumentCopies";
constexpr const char* kPlaybackClass = "io/github/arancormonk/dsdneo/RecordingPlayback";
constexpr const char* kExportClass = "io/github/arancormonk/dsdneo/RecordingExport";
constexpr const char* kRecordingShareClass = "io/github/arancormonk/dsdneo/RecordingShare";
constexpr const char* kPickerClass = "io/github/arancormonk/dsdneo/DocumentPicker";
constexpr const char* kDownloadsClass = "io/github/arancormonk/dsdneo/DownloadsSave";
constexpr const char* kFileBrowserClass = "io/github/arancormonk/dsdneo/FileBrowser";
constexpr const char* kSignInClass = "io/github/arancormonk/dsdneo/SignInAutofill";

/* The Qt-free phase enum and the Q_ENUM QML binds to have to stay in lockstep; the
 * mapping below hands one straight to the other. */
static_assert(static_cast<int>(kSessionIdle) == static_cast<int>(dsd_qt::DecoderHost::Idle), "phase enum drift");
static_assert(static_cast<int>(kSessionStarting) == static_cast<int>(dsd_qt::DecoderHost::Starting),
              "phase enum drift");
static_assert(static_cast<int>(kSessionRunning) == static_cast<int>(dsd_qt::DecoderHost::Running), "phase enum drift");
static_assert(static_cast<int>(kSessionStopping) == static_cast<int>(dsd_qt::DecoderHost::Stopping),
              "phase enum drift");
static_assert(static_cast<int>(kSessionFailed) == static_cast<int>(dsd_qt::DecoderHost::Failed), "phase enum drift");

QJniObject
android_context(void) {
    return QNativeInterface::QAndroidApplication::context();
}

/**
 * @brief Builds a Java String[] from @p values. Local ref, valid for one call.
 *
 * Returns nullptr on failure, always with no exception left pending: an allocation
 * failure here raises one, the caller goes straight on to more JNI calls, and JNI
 * calls made with a pending exception are undefined — in practice an abort rather
 * than the failed start the caller is written to report.
 */
jobjectArray
to_java_string_array(QJniEnvironment& env, const QStringList& values) {
    jclass string_class = env->FindClass("java/lang/String");
    if (env.checkAndClearExceptions() || string_class == nullptr) {
        return nullptr;
    }
    jobjectArray array = env->NewObjectArray(static_cast<jsize>(values.size()), string_class, nullptr);
    /* The Qt main thread stays attached to the VM with no enclosing Java frame, so
     * local refs are never reclaimed for us: every one has to be released by hand
     * or the (512-entry) table fills up over the process's life. */
    env->DeleteLocalRef(string_class);
    if (env.checkAndClearExceptions() || array == nullptr) {
        return nullptr;
    }
    for (qsizetype i = 0; i < values.size(); i++) {
        QJniObject item = QJniObject::fromString(values.at(i));
        env->SetObjectArrayElement(array, static_cast<jsize>(i), item.object());
        if (env.checkAndClearExceptions()) {
            env->DeleteLocalRef(array);
            return nullptr;
        }
    }
    return array;
}

/**
 * @brief A Java String with @p text's UTF-16 units, as a local ref the caller deletes; nullptr, with no exception left
 * pending, when it could not be made.
 *
 * A local ref, not a QJniObject's global one, so a credential's Java copy is let go of as soon as the call returns.
 */
jstring
new_java_string(QJniEnvironment& env, const QString& text) {
    jstring value = env->NewString(reinterpret_cast<const jchar*>(text.utf16()), static_cast<jsize>(text.size()));
    if (env.checkAndClearExceptions()) {
        return nullptr;
    }
    return value;
}

/**
 * @brief The application's ContentResolver, taken on the Qt main thread and held for the process.
 *
 * The document copies run on a worker thread. There the Activity may be finishing or being replaced, and asking Qt
 * for the current context off its main thread is not documented as safe. The application context's resolver
 * outlives both and may be used from any thread, so a worker reads it here without asking anyone. The global
 * reference is never released: it lives as long as the process does.
 */
std::atomic<jobject> g_content_resolver{nullptr};

void
hold_content_resolver() {
    if (g_content_resolver.load() != nullptr) {
        return;
    }
    const QJniObject context = android_context();
    if (!context.isValid()) {
        return;
    }
    const QJniObject application = context.callObjectMethod("getApplicationContext", "()Landroid/content/Context;");
    if (!application.isValid()) {
        return;
    }
    const QJniObject resolver =
        application.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
    if (!resolver.isValid()) {
        return;
    }
    QJniEnvironment env;
    jobject held = env->NewGlobalRef(resolver.object());
    jobject expected = nullptr;
    if (held != nullptr && !g_content_resolver.compare_exchange_strong(expected, held)) {
        env->DeleteGlobalRef(held);
    }
}

/** @brief Prose for the toolbar and the platform notification. */

constexpr const char* kFactsClass = "io/github/arancormonk/dsdneo/ProcessFacts";

/** @brief The strings of a Java String[] @p array, invalid or null entries left out. */
QStringList
java_strings(const QJniObject& array) {
    QStringList strings;
    if (!array.isValid()) {
        return strings;
    }
    QJniEnvironment env;
    const auto items = static_cast<jobjectArray>(array.object());
    const jsize count = env->GetArrayLength(items);
    for (jsize i = 0; i < count; ++i) {
        const QJniObject item = QJniObject::fromLocalRef(env->GetObjectArrayElement(items, i));
        if (item.isValid()) {
            strings.append(item.toString());
        }
    }
    return strings;
}

/* The exit records' logged marker (ProcessFacts.exitsMarker()): where it lives and what it says once the exit lines
 * read with it are durable. Read with the lines, written by android_exit_lines_logged() on the log's build thread. */
std::mutex g_exits_marker_lock;
QString g_exits_marker_path;
QByteArray g_exits_marker_text;

/** @brief Boot count, device and recent app exits for the diagnostics process line (issue #745). */
std::pair<QString, QStringList>
android_process_facts() {
    const QJniObject facts = QJniObject::callStaticObjectMethod(kFactsClass, "processFacts", "()Ljava/lang/String;");
    const QStringList exits =
        java_strings(QJniObject::callStaticObjectMethod(kFactsClass, "exitLines", "()[Ljava/lang/String;"));
    const QStringList marker =
        java_strings(QJniObject::callStaticObjectMethod(kFactsClass, "exitsMarker", "()[Ljava/lang/String;"));
    if (marker.size() == 2) {
        const std::lock_guard<std::mutex> lock(g_exits_marker_lock);
        g_exits_marker_path = marker.at(0);
        g_exits_marker_text = marker.at(1).toUtf8();
    }
    return {facts.isValid() ? facts.toString() : QString(), exits};
}

/**
 * @brief The exit lines are durable: record their keys, so the next start does not log them again (#782).
 *
 * Only now, not when they were read: a process that dies before its log syncs them logs them again at the next start
 * instead of losing them. Plain file I/O on the log's build thread, which has no Java frame to call ProcessFacts from.
 * Unwritten, the next start logs them again, which is all it costs.
 */
void
android_exit_lines_logged() {
    QString path;
    QByteArray text;
    {
        const std::lock_guard<std::mutex> lock(g_exits_marker_lock);
        path.swap(g_exits_marker_path);
        text.swap(g_exits_marker_text);
    }
    if (path.isEmpty()) {
        return;
    }
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(text) == text.size()) {
        (void)file.commit();
    }
}

/* Registered at load, before either installTap() call site (the UI or the service's nativeInit) can run. */
[[maybe_unused]] const bool g_process_facts_registered =
    (dsd_qt::DiagnosticsLog::setPlatformFactsProvider(&android_process_facts, &android_exit_lines_logged), true);

} // namespace

DecoderHostAndroid::DecoderHostAndroid(QObject* parent) : dsd_qt::DecoderHost(parent) {
    dsd_qt::DiagnosticsLog::installTap();
    hold_content_resolver();
    platformStartDownloads();
    platformStartFileBrowser();
    readFileAccess();
    startRecordingShareClean();
    refreshPresentation();
}

namespace {

// Qt's own registered native, the one its display listener calls (Qt 6.11.3 QtDisplayManager.java). Kept by
// qt-rules.pro's native-method rule; tools/check_android_r8.py checks it in the release dex.
constexpr const char* kQtDisplayManagerClass = "org/qtproject/qt/android/QtDisplayManager";

qreal
activity_density(const QJniObject& context) {
    if (!context.isValid()) {
        return 0;
    }
    return QJniObject::callStaticMethod<jfloat>(kSupportClass, "activityDensity", "(Landroid/content/Context;)F",
                                                context.object());
}

bool
static_method_available(const char* owner, const char* name, const char* signature) {
    QJniEnvironment env;
    jclass found = env.findClass(owner);
    return found != nullptr && env.findStaticMethod(found, name, signature) != nullptr;
}

/** @brief A Java float[] returned by a static call into @p values; false when the call or the copy failed. */
bool
read_floats(const QJniObject& array, QVector<float>* values) {
    if (!array.isValid()) {
        return false;
    }
    QJniEnvironment env;
    auto* const floats = static_cast<jfloatArray>(array.object());
    const jsize size = env->GetArrayLength(floats);
    values->resize(size);
    if (size > 0) {
        env->GetFloatArrayRegion(floats, 0, size, values->data());
    }
    return !env.checkAndClearExceptions();
}

/*
 * On the Android main thread, where Qt's display listener writes the density too. Reads the Activity's density now,
 * not the value the UI tick saw, so a push that runs after a newer change cannot install the older one; the density
 * written is reported back with the outcome. Qt's native only stores the value, so the call itself cannot fail once
 * the lookup succeeded.
 */
QVariant
push_display_density() {
    const qreal density = activity_density(android_context());
    if (!std::isfinite(density) || density <= 0) {
        return QVariantList{static_cast<int>(DensityPush::NoActivity), 0.0};
    }
    if (!static_method_available(kQtDisplayManagerClass, "handleScreenDensityChanged", "(D)V")) {
        return QVariantList{static_cast<int>(DensityPush::Unavailable), 0.0};
    }
    QJniObject::callStaticMethod<void>(kQtDisplayManagerClass, "handleScreenDensityChanged", "(D)V",
                                       static_cast<jdouble>(density));
    return QVariantList{static_cast<int>(DensityPush::Pushed), density};
}

/** @brief PresentationScale's view of the app: JNI for the Activity, QPA for Qt's screens. */
class AndroidPresentationPlatform final : public PresentationScale::Platform {
  public:
    AndroidPresentationPlatform(std::function<void()> push, QJniObject context)
        : m_push(std::move(push)), m_context(std::move(context)) {}

    qreal
    activityDensity() override {
        return activity_density(m_context);
    }

    qreal
    qtDensity() override {
        return platform_density(QGuiApplication::primaryScreen());
    }

    qreal
    effectiveRatio() override {
        const QScreen* screen = QGuiApplication::primaryScreen();
        return screen != nullptr ? screen->devicePixelRatio() : 0;
    }

    void
    resync() override {
        resync_display_scale();
    }

    void
    startDensityPush() override {
        if (m_push) {
            m_push();
        }
    }

    FontKey
    fontKey() override {
        QVector<float> values;
        if (!m_context.isValid()
            || !read_floats(QJniObject::callStaticObjectMethod(kSupportClass, "fontKey",
                                                               "(Landroid/content/Context;)[F", m_context.object()),
                            &values)
            || values.size() != 2) {
            return {};
        }
        return FontKey{values[0], values[1]};
    }

    bool
    fontTable(FontKey* key, QVector<qreal>* pixels) override {
        QVector<float> values;
        if (!m_context.isValid()
            || !read_floats(QJniObject::callStaticObjectMethod(kSupportClass, "fontTable",
                                                               "(Landroid/content/Context;)[F", m_context.object()),
                            &values)
            || values.size() != 2 + PresentationScale::kFontTableMaxSp + 1) {
            return false; // empty while Android is updating its resources
        }
        *key = FontKey{values[0], values[1]};
        pixels->clear();
        for (int i = 2; i < values.size(); ++i) {
            pixels->append(values[i]);
        }
        return true;
    }

    bool
    keyboardTopPixels(int* pixels) override {
        // A failed primitive call reads 0, a keyboard covering the whole window; never take that for a reading.
        static const bool available = static_method_available(kSupportClass, "keyboardTopPixels", "()I");
        if (!available) {
            return false;
        }
        *pixels = QJniObject::callStaticMethod<jint>(kSupportClass, "keyboardTopPixels", "()I");
        return true;
    }

    void
    diagnostic(const QString& line) override {
        dsd_qt::DiagnosticsLog::submitHostDiagnostic(line);
    }

  private:
    std::function<void()> m_push;
    QJniObject m_context;
};

} // namespace

void
DecoderHostAndroid::refreshPresentation() {
    refreshNotificationPermission();
    const auto context = android_context();
    if (!context.isValid()) {
        return;
    }
    AndroidPresentationPlatform platform([this] { startDisplayDensityPush(); }, context);
    const PresentationScale::Changes changes = m_scale.tick(platform);
    if (changes.typography) {
        Q_EMIT typographyChanged();
    }
    const auto route = QJniObject::callStaticObjectMethod(
                           kSupportClass, "audioRouteName", "(Landroid/content/Context;I)Ljava/lang/String;",
                           context.object(), static_cast<jint>(dsd_audio_output_device_id()))
                           .toString();
    if (route != m_audio_route) {
        m_audio_route = route;
        Q_EMIT audioRouteChanged();
    }
    if (changes.keyboard) {
        Q_EMIT keyboardChanged();
    }
    const int requests = QJniObject::callStaticMethod<jint>(kSupportClass, "takeBackRequests", "()I");
    for (int i = 0; i < requests; ++i) {
        Q_EMIT backRequested();
    }
}

void
DecoderHostAndroid::startDisplayDensityPush() {
    // The continuation runs on this host's thread, and not at all once the host is gone.
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread(&push_display_density)
        .then(this, [this](const QVariant& result) {
            const QVariantList fields = result.toList();
            if (fields.size() != 2) {
                return;
            }
            // Only the diagnostic is used here; no push starts from a finished one.
            AndroidPresentationPlatform platform(nullptr, android_context());
            m_scale.pushFinished(static_cast<DensityPush>(fields[0].toInt()), fields[1].toDouble(), platform);
        });
}

qreal
DecoderHostAndroid::fontPixelSize(qreal sp) const {
    if (!std::isfinite(sp) || sp <= 0) {
        return 0;
    }
    qreal pixels = 0;
    if (m_scale.fontPixelSize(sp, &pixels)) {
        return pixels;
    }
    const auto context = android_context();
    if (!context.isValid()) {
        return sp;
    }
    const QScreen* screen = QGuiApplication::primaryScreen();
    const qreal ratio = screen != nullptr ? screen->devicePixelRatio() : 1;
    const qreal live =
        QJniObject::callStaticMethod<jfloat>(kSupportClass, "fontPixels", "(Landroid/content/Context;F)F",
                                             context.object(), static_cast<jfloat>(sp))
        / ratio;
    // A failed call reads 0: fall back to the unscaled size rather than a font with no size.
    return std::isfinite(live) && live > 0 ? live : sp;
}

void
DecoderHostAndroid::setDarkAppearance(bool dark) {
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([dark]() -> QVariant {
        const auto context = android_context();
        if (context.isValid()) {
            QJniObject::callStaticMethod<void>(kSupportClass, "setDarkAppearance", "(Landroid/app/Activity;Z)V",
                                               context.object(), static_cast<jboolean>(dark));
        }
        return {};
    });
}

void
DecoderHostAndroid::refreshNotificationPermission() {
    const auto context = android_context();
    if (!context.isValid()) {
        return;
    }
    // Must match AppSupport.kt: @JvmStatic fun takeNotificationPermissionRefresh(): Boolean.
    if (QJniObject::callStaticMethod<jboolean>(kSupportClass, "takeNotificationPermissionRefresh", "()Z") != JNI_TRUE) {
        return;
    }
    // Must match AppSupport.kt: @JvmStatic fun notificationPermissionNeeded(context: Context): Boolean.
    const bool needed = QJniObject::callStaticMethod<jboolean>(kSupportClass, "notificationPermissionNeeded",
                                                               "(Landroid/content/Context;)Z", context.object())
                        == JNI_TRUE;
    if (needed != m_notification_permission_needed) {
        m_notification_permission_needed = needed;
        Q_EMIT notificationPermissionChanged();
    }
}

void
DecoderHostAndroid::requestNotificationPermission() {
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([]() -> QVariant {
        const auto context = android_context();
        if (context.isValid()) {
            QJniObject::callStaticMethod<void>(kSupportClass, "ensureNotificationPermission",
                                               "(Landroid/app/Activity;)V", context.object());
        }
        return {};
    });
}

void
DecoderHostAndroid::hostDiagnostic(const QString& line) {
    dsd_qt::DiagnosticsLog::submitHostDiagnostic(line);
}

// WP-F5: share content only, never a caller-selected file path.
void
DecoderHostAndroid::shareDiagnostics(const QString& text, const QString& title) {
    QStringList lines;
    for (const auto& line : text.split(QLatin1Char('\n'))) {
        lines.append(dsd_qt::DiagnosticsLog::redact(line));
    }
    const auto safeText = lines.join(QLatin1Char('\n'));
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([safeText, title]() -> QVariant {
        const auto context = android_context();
        if (!context.isValid()) {
            return {};
        }
        QJniObject::callStaticMethod<void>("io/github/arancormonk/dsdneo/DiagnosticsShare", "share",
                                           "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)V",
                                           context.object(), QJniObject::fromString(safeText).object(),
                                           QJniObject::fromString(title).object());
        return {};
    });
}

void
DecoderHostAndroid::platformRequestAccess() {
    QJniObject context = android_context();
    if (!context.isValid()) {
        return;
    }
    /* The permission dialog answers asynchronously; the poll tick picks the result
     * up through refresh(). */
    QJniObject::callStaticMethod<void>(kUsbClass, "requestAccess", "(Landroid/content/Context;)V", context.object());
}

void
DecoderHostAndroid::noteStartAttempt(const QString& sourceType) {
    noteAttempt(sourceType, dsd_qt::DiagnosticsLog::AttemptStage::Start);
}

void
DecoderHostAndroid::noteAttempt(const QString& sourceType, dsd_qt::DiagnosticsLog::AttemptStage stage) {
    QJniObject kind = QJniObject::fromString(sourceType);
    const QJniObject context = QJniObject::callStaticObjectMethod(
        kFactsClass, "attemptContext", "(Ljava/lang/String;)Ljava/lang/String;", kind.object());
    dsd_qt::DiagnosticsLog::instance().noteSessionAttempt(context.isValid() ? context.toString() : sourceType, stage);
}

void
DecoderHostAndroid::platformRequestAccessForSource(const QString& source, const QString& serial) {
    QJniObject context = android_context();
    if (!context.isValid()) {
        return;
    }
    QJniObject kind = QJniObject::fromString(source);
    QJniObject requested = QJniObject::fromString(serial);
    QJniObject::callStaticMethod<void>(kUsbClass, "requestAccessForSource",
                                       "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)V",
                                       context.object(), kind.object(), requested.object());
}

void
DecoderHostAndroid::platformAcknowledgeReceiverEnding(uint64_t session) {
    QJniObject::callStaticMethod<void>(kServiceClass, "acknowledgeReceiverEnding", "(J)V", static_cast<jlong>(session));
}

void
DecoderHostAndroid::setScreenPolicy(int mode, int delaySeconds) {
    /* ScreenSupport owns the screen state and touches the Activity's window, so it
     * runs on the Android main thread, not the Qt one. */
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([mode, delaySeconds]() -> QVariant {
        QJniObject context = android_context();
        if (!context.isValid()) {
            return {};
        }
        // Must match ScreenSupport.kt: @JvmStatic fun configure(context: Context, mode: Int, delaySeconds: Int)
        QJniObject::callStaticMethod<void>(kScreenClass, "configure", "(Landroid/content/Context;II)V",
                                           context.object(), static_cast<jint>(mode), static_cast<jint>(delaySeconds));
        return {};
    });
}

QString
DecoderHostAndroid::platformStartDecoder(const QStringList& argv, uint64_t attempt) {
    QJniObject context = android_context();
    if (!context.isValid()) {
        return QStringLiteral("No Android context");
    }

    QJniEnvironment env;
    jobjectArray args = to_java_string_array(env, argv);
    if (args == nullptr) {
        return QStringLiteral("Could not marshal arguments");
    }

    // Must match DecoderService.kt: @JvmStatic fun startDecoder(context: Context, args: Array<String>, attempt: Long)
    QJniObject::callStaticMethod<void>(kServiceClass, "startDecoder",
                                       "(Landroid/content/Context;[Ljava/lang/String;J)V", context.object(), args,
                                       static_cast<jlong>(attempt));
    env->DeleteLocalRef(args);
    return {};
}

bool
DecoderHostAndroid::platformRetireAttempts(uint64_t through) {
    // Must match DecoderService.kt: @JvmStatic fun retireAttempts(through: Long): Boolean. Short and lock-only, like
    // stopDecoder()'s synchronous part. A failed call answers false: nothing says the service took the attempt.
    QJniEnvironment env;
    const jboolean taken =
        QJniObject::callStaticMethod<jboolean>(kServiceClass, "retireAttempts", "(J)Z", static_cast<jlong>(through));
    if (env.checkAndClearExceptions()) {
        return false;
    }
    return taken == JNI_TRUE;
}

bool
DecoderHostAndroid::moveToBackground() {
    /* Finishing the Activity would terminate the Qt process, and with it the service
     * that owns the engine. Backgrounding keeps both alive. An Activity that will not
     * go back is reported as such, so the caller lets the close proceed rather than
     * leaving a window with no way out. */
    QJniObject activity = android_context();
    if (!activity.isValid()) {
        return false;
    }
    return activity.callMethod<jboolean>("moveTaskToBack", "(Z)Z", JNI_TRUE) == JNI_TRUE;
}

bool
DecoderHostAndroid::quitApp() {
    if (!QNativeInterface::QAndroidApplication::isActivityContext()) {
        return false;
    }
    /* Not waited for: finishing runs on the Android main thread, and its onDestroy waits for this thread's event
     * loop to end. */
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([]() -> QVariant {
        const auto context = android_context();
        if (context.isValid()) {
            // Must match AppSupport.kt: @JvmStatic fun finishApp(context: Context).
            QJniObject::callStaticMethod<void>(kSupportClass, "finishApp", "(Landroid/content/Context;)V",
                                               context.object());
        }
        return {};
    });
    return true;
}

/*
 * The JNI transport of the document copies (decoder_host_android_documents.cpp), on BackupModel's worker thread.
 * QJniObject attaches that thread to the VM and loads the app classes through Qt's class loader, and its call frames
 * release their local references; the calls use the held ContentResolver. Neither waits on another thread.
 */
bool
DecoderHostAndroid::platformHoldsResolver() {
    return g_content_resolver.load() != nullptr;
}

int
DecoderHostAndroid::platformCopyDocumentToFile(const QString& reference, const QString& destPath, qint64 maxBytes) {
    const jobject resolver = g_content_resolver.load();
    if (resolver == nullptr) {
        return DocumentLocalFailed; // the app never held its ContentResolver: nothing to do with the document
    }
    const QJniObject uri = QJniObject::fromString(reference);
    const QJniObject dest = QJniObject::fromString(destPath);
    // Must match DocumentCopies.kt: @JvmStatic fun copyDocumentToFile(ContentResolver, String, String, Long): Int.
    return QJniObject::callStaticMethod<jint>(
        kDocumentsClass, "copyDocumentToFile",
        "(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;J)I", resolver, uri.object(),
        dest.object(), static_cast<jlong>(maxBytes));
}

bool
DecoderHostAndroid::platformCopyFileToDocument(const QString& sourcePath, const QString& reference) {
    const jobject resolver = g_content_resolver.load();
    if (resolver == nullptr) {
        return false;
    }
    const QJniObject source = QJniObject::fromString(sourcePath);
    const QJniObject uri = QJniObject::fromString(reference);
    // Must match DocumentCopies.kt: @JvmStatic fun copyFileToDocument(ContentResolver, String, String): Boolean.
    return QJniObject::callStaticMethod<jboolean>(
               kDocumentsClass, "copyFileToDocument",
               "(Landroid/content/ContentResolver;Ljava/lang/String;Ljava/lang/String;)Z", resolver, source.object(),
               uri.object())
           == JNI_TRUE;
}

/*
 * The JNI transport of the import copies (#832; DecoderHostAndroid::ImportTransport in
 * decoder_host_android_imports.cpp), on the import worker thread, which QJniObject attaches. Only the copy uses the
 * held ContentResolver; the rest read or change DocumentCopies' request table under its lock. None waits on another
 * thread.
 */
int
DecoderHostAndroid::platformCopyImportDocument(qint64 request, const QString& reference, const QString& destPath,
                                               qint64 maxBytes) {
    const jobject resolver = g_content_resolver.load();
    if (resolver == nullptr) {
        return DocumentLocalFailed; // the app never held its ContentResolver: nothing to do with the document
    }
    const QJniObject uri = QJniObject::fromString(reference);
    const QJniObject dest = QJniObject::fromString(destPath);
    // Must match DocumentCopies.kt:
    // @JvmStatic fun copyImportDocument(ContentResolver, Long, String, String, Long): Int.
    return QJniObject::callStaticMethod<jint>(
        kDocumentsClass, "copyImportDocument",
        "(Landroid/content/ContentResolver;JLjava/lang/String;Ljava/lang/String;J)I", resolver,
        static_cast<jlong>(request), uri.object(), dest.object(), static_cast<jlong>(maxBytes));
}

void
DecoderHostAndroid::platformCancelImport(qint64 request) {
    // Must match DocumentCopies.kt: @JvmStatic fun cancelImport(request: Long).
    QJniObject::callStaticMethod<void>(kDocumentsClass, "cancelImport", "(J)V", static_cast<jlong>(request));
}

void
DecoderHostAndroid::platformImportProgress(qint64 request, qint64& bytes, qint64& total) {
    bytes = -1;
    total = -1;
    // Must match DocumentCopies.kt: @JvmStatic fun importProgress(request: Long): LongArray ([copied, declared]).
    const QJniObject answer =
        QJniObject::callStaticObjectMethod(kDocumentsClass, "importProgress", "(J)[J", static_cast<jlong>(request));
    if (!answer.isValid()) {
        return;
    }
    QJniEnvironment env;
    const auto values = static_cast<jlongArray>(answer.object());
    jlong pair[2] = {-1, -1};
    if (env->GetArrayLength(values) < 2) {
        return;
    }
    env->GetLongArrayRegion(values, 0, 2, pair);
    if (env.checkAndClearExceptions()) {
        return;
    }
    bytes = pair[0];
    total = pair[1];
}

QString
DecoderHostAndroid::platformImportName(qint64 request) {
    // Must match DocumentCopies.kt: @JvmStatic fun importName(request: Long): String?.
    const QJniObject name = QJniObject::callStaticObjectMethod(kDocumentsClass, "importName", "(J)Ljava/lang/String;",
                                                               static_cast<jlong>(request));
    return name.isValid() ? name.toString() : QString();
}

bool
DecoderHostAndroid::platformImportNoSpace(qint64 request) {
    // Must match DocumentCopies.kt: @JvmStatic fun importNoSpace(request: Long): Boolean.
    return QJniObject::callStaticMethod<jboolean>(kDocumentsClass, "importNoSpace", "(J)Z", static_cast<jlong>(request))
           == JNI_TRUE;
}

void
DecoderHostAndroid::platformForgetImport(qint64 request) {
    // Must match DocumentCopies.kt: @JvmStatic fun forgetImport(request: Long).
    QJniObject::callStaticMethod<void>(kDocumentsClass, "forgetImport", "(J)V", static_cast<jlong>(request));
}

/*
 * Call recordings (#708; the dispatch is decoder_host_android_recordings.cpp). Roots, playback and share are asked on
 * the Qt main thread and only post or read there; the transfer runs on the host's transfer worker, which QJniObject
 * attaches, with the held ContentResolver. RecordingShare's chooser starts on the Android main thread.
 */
QStringList
DecoderHostAndroid::platformRecordingRoots() {
    const QJniObject context = android_context();
    if (!context.isValid()) {
        return {};
    }
    // Must match RecordingShare.kt: @JvmStatic fun roots(context: Context): Array<String>.
    return java_strings(QJniObject::callStaticObjectMethod(
        kRecordingShareClass, "roots", "(Landroid/content/Context;)[Ljava/lang/String;", context.object()));
}

int
DecoderHostAndroid::platformPlayRecording(const QString& path) {
    const QJniObject context = android_context();
    if (!context.isValid()) {
        return 0;
    }
    const QJniObject file = QJniObject::fromString(path);
    // Must match RecordingPlayback.kt: @JvmStatic fun play(context: Context, path: String): Int.
    return QJniObject::callStaticMethod<jint>(kPlaybackClass, "play", "(Landroid/content/Context;Ljava/lang/String;)I",
                                              context.object(), file.object());
}

void
DecoderHostAndroid::platformPausePlayback() {
    QJniObject::callStaticMethod<void>(kPlaybackClass, "pause", "()V");
}

void
DecoderHostAndroid::platformResumePlayback() {
    QJniObject::callStaticMethod<void>(kPlaybackClass, "resume", "()V");
}

void
DecoderHostAndroid::platformSeekPlayback(qint64 positionMs) {
    QJniObject::callStaticMethod<void>(kPlaybackClass, "seek", "(J)V", static_cast<jlong>(positionMs));
}

void
DecoderHostAndroid::platformStopPlayback() {
    QJniObject::callStaticMethod<void>(kPlaybackClass, "stop", "()V");
}

QString
DecoderHostAndroid::platformPlaybackStatus() {
    const QJniObject status = QJniObject::callStaticObjectMethod(kPlaybackClass, "status", "()Ljava/lang/String;");
    return status.isValid() ? status.toString() : QString();
}

QStringList
DecoderHostAndroid::platformTransferRecordings(qint64 id, const QStringList& paths, const QStringList& names,
                                               const QString& reference, bool zip) {
    const jobject resolver = g_content_resolver.load();
    if (resolver == nullptr) {
        return {};
    }
    /* This worker thread has no Java frame to reclaim local references, so each one goes by hand. */
    QJniEnvironment env;
    jobjectArray files = to_java_string_array(env, paths);
    jobjectArray entries = files == nullptr ? nullptr : to_java_string_array(env, names);
    if (entries == nullptr) {
        if (files != nullptr) {
            env->DeleteLocalRef(files);
        }
        return {};
    }
    const QJniObject uri = QJniObject::fromString(reference);
    // Must match RecordingExport.kt: @JvmStatic fun transfer(ContentResolver, Long, Array<String>, Array<String>,
    // String, Boolean): Array<String>. The descriptor stays one literal, which tools/check_android_r8.py reads whole.
    // clang-format off
    const QJniObject answer = QJniObject::callStaticObjectMethod(kExportClass, "transfer",
        "(Landroid/content/ContentResolver;J[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Z)[Ljava/lang/String;",
        resolver, static_cast<jlong>(id), files, entries, uri.object(), static_cast<jboolean>(zip));
    // clang-format on
    env->DeleteLocalRef(entries);
    env->DeleteLocalRef(files);
    return java_strings(answer);
}

qint64
DecoderHostAndroid::platformRecordingTransferProgress(qint64 id) {
    return QJniObject::callStaticMethod<jlong>(kExportClass, "progress", "(J)J", static_cast<jlong>(id));
}

void
DecoderHostAndroid::platformCancelRecordingTransfer(qint64 id) {
    QJniObject::callStaticMethod<void>(kExportClass, "cancel", "(J)V", static_cast<jlong>(id));
}

bool
DecoderHostAndroid::platformCanShareRecording(const QString& path) {
    const QJniObject context = android_context();
    if (!context.isValid()) {
        return false;
    }
    // Must match RecordingShare.kt: @JvmStatic fun canShare(context: Context, path: String): Boolean.
    return QJniObject::callStaticMethod<jboolean>(kRecordingShareClass, "canShare",
                                                  "(Landroid/content/Context;Ljava/lang/String;)Z", context.object(),
                                                  QJniObject::fromString(path).object())
           == JNI_TRUE;
}

bool
DecoderHostAndroid::platformShareRecording(const QString& path, const QString& title) {
    const QJniObject context = android_context();
    if (!context.isValid()) {
        return false;
    }
    // Must match RecordingShare.kt: @JvmStatic fun stage(context: Context, path: String): String.
    const QJniObject copy = QJniObject::callStaticObjectMethod(
        kRecordingShareClass, "stage", "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
        context.object(), QJniObject::fromString(path).object());
    if (!copy.isValid()) {
        return false;
    }
    const QString copyPath = copy.toString();
    if (copyPath.isEmpty()) {
        return true; // RecordingShare queued the failure
    }
    /* Not waited for; a share that fails from here on is queued in RecordingShare for refreshRecordingShares(). The
     * context goes along (a global reference), so the offer always runs and always reports. */
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([context, copyPath, path, title]() -> QVariant {
        /* Must match RecordingShare.kt: @JvmStatic fun offer(context: Context, copy: String, path: String,
         * title: String): Boolean. */
        (void)QJniObject::callStaticMethod<jboolean>(
            kRecordingShareClass, "offer",
            "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z", context.object(),
            QJniObject::fromString(copyPath).object(), QJniObject::fromString(path).object(),
            QJniObject::fromString(title).object());
        return {};
    });
    return true;
}

void
DecoderHostAndroid::platformCleanRecordingShares() {
    const QJniObject context = android_context();
    if (context.isValid()) {
        // Must match RecordingShare.kt: @JvmStatic fun clean(context: Context).
        QJniObject::callStaticMethod<void>(kRecordingShareClass, "clean", "(Landroid/content/Context;)V",
                                           context.object());
    }
}

QString
DecoderHostAndroid::platformTakeRecordingShareFailure() {
    // Must match RecordingShare.kt: @JvmStatic fun takeFailure(): String.
    const QJniObject path =
        QJniObject::callStaticObjectMethod(kRecordingShareClass, "takeFailure", "()Ljava/lang/String;");
    return path.isValid() ? path.toString() : QString();
}

void
DecoderHostAndroid::abortDocumentTransfers() {
    /* Must match DocumentCopies.kt: @JvmStatic fun abort(). Cancels the provider calls in flight, stops the copies at
     * their next chunk and refuses new ones. */
    QJniObject::callStaticMethod<void>(kDocumentsClass, "abort", "()V");
    dsd_qt::DecoderHost::abortDocumentTransfers();
}

// WP-D3: Kotlin owns permissions, timeout and cancellation; polling emits on the Qt thread.
void
DecoderHostAndroid::requestCurrentLocation(qint64 requestId) {
    const auto context = android_context();
    if (!context.isValid()) {
        Q_EMIT locationResult(requestId, false, 0, 0, 0, 0, false, {}, {}, QStringLiteral("No Android context"));
        return;
    }
    QJniObject::callStaticMethod<void>(kLocationClass, "requestCurrentLocation", "(Landroid/app/Activity;J)V",
                                       context.object(), static_cast<jlong>(requestId));
}

void
DecoderHostAndroid::requestSiteLocation(qint64 requestId) {
    const auto context = android_context();
    if (!context.isValid()) {
        Q_EMIT locationResult(requestId, false, 0, 0, 0, 0, false, {}, {}, QStringLiteral("No Android context"));
        return;
    }
    QJniObject::callStaticMethod<void>(kLocationClass, "requestSiteLocation", "(Landroid/app/Activity;J)V",
                                       context.object(), static_cast<jlong>(requestId));
}

void
DecoderHostAndroid::cancelLocationRequest(qint64 requestId) {
    QJniObject::callStaticMethod<void>(kLocationClass, "cancelLocationRequest", "(J)V", static_cast<jlong>(requestId));
}

/* The sign-in form for password managers. Each must match SignInAutofill.kt's @JvmStatic entry point of that name. */
void
DecoderHostAndroid::signInAutofillBegin(int token) {
    QJniEnvironment env;
    QJniObject::callStaticMethod<void>(kSignInClass, "begin", "(I)V", static_cast<jint>(token));
    env.checkAndClearExceptions();
}

void
DecoderHostAndroid::signInAutofillField(int token, int field, const QRect& windowPixels, bool visible, bool focused,
                                        const QString& structureValue) {
    QJniEnvironment env;
    jstring value = new_java_string(env, structureValue);
    if (value == nullptr) {
        return;
    }
    QJniObject::callStaticMethod<void>(kSignInClass, "field", "(IIIIIIZZLjava/lang/String;)V", static_cast<jint>(token),
                                       static_cast<jint>(field), static_cast<jint>(windowPixels.x()),
                                       static_cast<jint>(windowPixels.y()), static_cast<jint>(windowPixels.width()),
                                       static_cast<jint>(windowPixels.height()), static_cast<jboolean>(visible),
                                       static_cast<jboolean>(focused), value);
    env.checkAndClearExceptions();
    env->DeleteLocalRef(value);
}

void
DecoderHostAndroid::signInAutofillRequest(int token, int field, const QRect& windowPixels) {
    QJniEnvironment env;
    QJniObject::callStaticMethod<void>(kSignInClass, "request", "(IIIIII)V", static_cast<jint>(token),
                                       static_cast<jint>(field), static_cast<jint>(windowPixels.x()),
                                       static_cast<jint>(windowPixels.y()), static_cast<jint>(windowPixels.width()),
                                       static_cast<jint>(windowPixels.height()));
    env.checkAndClearExceptions();
}

void
DecoderHostAndroid::signInAutofillCommit(int token, const QString& username, const QString& password) {
    QJniEnvironment env;
    jstring user = new_java_string(env, username);
    jstring pass = user != nullptr ? new_java_string(env, password) : nullptr;
    if (pass != nullptr) {
        QJniObject::callStaticMethod<void>(kSignInClass, "commit", "(ILjava/lang/String;Ljava/lang/String;)V",
                                           static_cast<jint>(token), user, pass);
        env.checkAndClearExceptions();
        env->DeleteLocalRef(pass);
    }
    if (user != nullptr) {
        env->DeleteLocalRef(user);
    }
}

void
DecoderHostAndroid::signInAutofillEnd(int token) {
    QJniEnvironment env;
    QJniObject::callStaticMethod<void>(kSignInClass, "end", "(I)V", static_cast<jint>(token));
    env.checkAndClearExceptions();
}

void
DecoderHostAndroid::refreshLocation() {
    // WP-D3: drain a single terminal location result, independently of decoder state.
    const auto locationRecord =
        QJniObject::callStaticObjectMethod(kLocationClass, "pollResult", "()Ljava/lang/String;");
    deliver_location_record(*this, QJsonDocument::fromJson(locationRecord.toString().toUtf8()).object());
}

/* #829: DocumentPicker posts each launch to the Android main thread itself, so these return at once. */
bool
DecoderHostAndroid::platformOpenDocument(qint64 id) {
    if (!QNativeInterface::QAndroidApplication::isActivityContext()) {
        return false;
    }
    const QJniObject activity = android_context();
    if (!activity.isValid()) {
        return false;
    }
    // Must match DocumentPicker.kt: @JvmStatic fun open(activity: Activity, id: Long): Boolean.
    return QJniObject::callStaticMethod<jboolean>(kPickerClass, "open", "(Landroid/app/Activity;J)Z", activity.object(),
                                                  static_cast<jlong>(id))
           == JNI_TRUE;
}

bool
DecoderHostAndroid::platformSaveDocument(qint64 id, const QString& suggestedName, const QString& mimeType) {
    if (!QNativeInterface::QAndroidApplication::isActivityContext()) {
        return false;
    }
    const QJniObject activity = android_context();
    if (!activity.isValid()) {
        return false;
    }
    const QJniObject title = QJniObject::fromString(suggestedName);
    const QJniObject mime = QJniObject::fromString(mimeType);
    // Must match DocumentPicker.kt: @JvmStatic fun create(activity: Activity, id: Long, title: String, mime: String):
    // Boolean. The descriptor stays one literal, which tools/check_android_r8.py reads whole.
    // clang-format off
    return QJniObject::callStaticMethod<jboolean>(kPickerClass, "create",
        "(Landroid/app/Activity;JLjava/lang/String;Ljava/lang/String;)Z",
        activity.object(), static_cast<jlong>(id), title.object(), mime.object()) == JNI_TRUE;
    // clang-format on
}

void
DecoderHostAndroid::platformCancelDocument(qint64 id) {
    // Must match DocumentPicker.kt: @JvmStatic fun cancel(id: Long).
    QJniObject::callStaticMethod<void>(kPickerClass, "cancel", "(J)V", static_cast<jlong>(id));
}

QString
DecoderHostAndroid::platformPollDocumentPick() {
    // Must match DocumentPicker.kt: @JvmStatic fun pollResult(): String.
    const QJniObject record = QJniObject::callStaticObjectMethod(kPickerClass, "pollResult", "()Ljava/lang/String;");
    return record.isValid() ? record.toString() : QString();
}

/* #829: DownloadsSave runs every MediaStore and grant call on its own thread; these only hand work over or read memory,
 * so the Qt thread and the transfer workers call them alike. */
void
DecoderHostAndroid::platformStartDownloads() {
    const jobject resolver = g_content_resolver.load();
    if (resolver != nullptr) {
        // Must match DownloadsSave.kt: @JvmStatic fun start(resolver: ContentResolver).
        QJniObject::callStaticMethod<void>(kDownloadsClass, "start", "(Landroid/content/ContentResolver;)V", resolver);
    }
}

QString
DecoderHostAndroid::platformSavedDocumentName(const QString& reference) {
    // Must match DownloadsSave.kt: @JvmStatic fun publishedName(uriText: String): String.
    const QJniObject name =
        QJniObject::callStaticObjectMethod(kDownloadsClass, "publishedName", "(Ljava/lang/String;)Ljava/lang/String;",
                                           QJniObject::fromString(reference).object());
    return name.isValid() ? name.toString() : QString();
}

void
DecoderHostAndroid::platformDiscardPendingDownload(const QString& reference) {
    // Must match DownloadsSave.kt: @JvmStatic fun discard(uriText: String).
    QJniObject::callStaticMethod<void>(kDownloadsClass, "discard", "(Ljava/lang/String;)V",
                                       QJniObject::fromString(reference).object());
}

/* #830: FileBrowser runs every read on its own worker threads and every launch on the Android main thread, so these
 * return at once; accessState() makes StorageManager and package-manager reads only, never a filesystem call. */
void
DecoderHostAndroid::platformStartFileBrowser() {
    const QJniObject context = android_context();
    if (context.isValid()) {
        // Must match FileBrowser.kt: @JvmStatic fun start(context: Context).
        QJniObject::callStaticMethod<void>(kFileBrowserClass, "start", "(Landroid/content/Context;)V",
                                           context.object());
    }
}

QString
DecoderHostAndroid::platformFileAccessState() {
    // Must match FileBrowser.kt: @JvmStatic fun accessState(): String.
    const QJniObject state =
        QJniObject::callStaticObjectMethod(kFileBrowserClass, "accessState", "()Ljava/lang/String;");
    return state.isValid() ? state.toString() : QString();
}

bool
DecoderHostAndroid::platformTakeFileAccessRefresh() {
    // Must match FileBrowser.kt: @JvmStatic fun takeAccessRefresh(): Boolean.
    return QJniObject::callStaticMethod<jboolean>(kFileBrowserClass, "takeAccessRefresh", "()Z") == JNI_TRUE;
}

bool
DecoderHostAndroid::platformListRoots(qint64 id, qint64 session) {
    // Must match FileBrowser.kt: @JvmStatic fun listRoots(id: Long, session: Long): Boolean.
    return QJniObject::callStaticMethod<jboolean>(kFileBrowserClass, "listRoots", "(JJ)Z", static_cast<jlong>(id),
                                                  static_cast<jlong>(session))
           == JNI_TRUE;
}

bool
DecoderHostAndroid::platformListFolder(qint64 id, qint64 session, const QString& rootKey, qint64 generation,
                                       const QString& path, const QString& kind, bool showAll) {
    const QJniObject key = QJniObject::fromString(rootKey);
    const QJniObject folder = QJniObject::fromString(path);
    const QJniObject filter = QJniObject::fromString(kind);
    // Must match FileBrowser.kt: @JvmStatic fun listFolder(id: Long, session: Long, rootKey: String, generation: Long,
    // path: String, kind: String, showAll: Boolean): Boolean. The descriptor stays one literal, which
    // tools/check_android_r8.py reads whole.
    // clang-format off
    return QJniObject::callStaticMethod<jboolean>(kFileBrowserClass, "listFolder",
        "(JJLjava/lang/String;JLjava/lang/String;Ljava/lang/String;Z)Z",
        static_cast<jlong>(id), static_cast<jlong>(session), key.object(), static_cast<jlong>(generation),
        folder.object(), filter.object(), showAll ? JNI_TRUE : JNI_FALSE) == JNI_TRUE;
    // clang-format on
}

bool
DecoderHostAndroid::platformListMore(qint64 id, qint64 session, qint64 snapshot, int offset) {
    // Must match FileBrowser.kt: @JvmStatic fun listMore(id: Long, session: Long, snapshot: Long, offset: Int):
    // Boolean.
    return QJniObject::callStaticMethod<jboolean>(kFileBrowserClass, "listMore", "(JJJI)Z", static_cast<jlong>(id),
                                                  static_cast<jlong>(session), static_cast<jlong>(snapshot),
                                                  static_cast<jint>(offset))
           == JNI_TRUE;
}

bool
DecoderHostAndroid::platformCheckHandoff(qint64 id, qint64 session, const QString& rootKey, qint64 generation,
                                         const QString& reference) {
    const QJniObject key = QJniObject::fromString(rootKey);
    const QJniObject file = QJniObject::fromString(reference);
    // Must match FileBrowser.kt: @JvmStatic fun checkHandoff(id: Long, session: Long, rootKey: String,
    // generation: Long, reference: String): Boolean.
    // clang-format off
    return QJniObject::callStaticMethod<jboolean>(kFileBrowserClass, "checkHandoff",
        "(JJLjava/lang/String;JLjava/lang/String;)Z",
        static_cast<jlong>(id), static_cast<jlong>(session), key.object(), static_cast<jlong>(generation),
        file.object()) == JNI_TRUE;
    // clang-format on
}

bool
DecoderHostAndroid::platformRequestFileAccess(qint64 id) {
    if (!QNativeInterface::QAndroidApplication::isActivityContext()) {
        return false;
    }
    const QJniObject activity = android_context();
    if (!activity.isValid()) {
        return false;
    }
    // Must match FileBrowser.kt: @JvmStatic fun requestAccess(activity: Activity, id: Long): Boolean.
    return QJniObject::callStaticMethod<jboolean>(kFileBrowserClass, "requestAccess", "(Landroid/app/Activity;J)Z",
                                                  activity.object(), static_cast<jlong>(id))
           == JNI_TRUE;
}

void
DecoderHostAndroid::platformCancelFileRequest(qint64 id) {
    // Must match FileBrowser.kt: @JvmStatic fun cancel(id: Long).
    QJniObject::callStaticMethod<void>(kFileBrowserClass, "cancel", "(J)V", static_cast<jlong>(id));
}

void
DecoderHostAndroid::platformCloseFileSession(qint64 session) {
    // Must match FileBrowser.kt: @JvmStatic fun closeSession(session: Long).
    QJniObject::callStaticMethod<void>(kFileBrowserClass, "closeSession", "(J)V", static_cast<jlong>(session));
}

QString
DecoderHostAndroid::platformPollFileBrowser() {
    // Must match FileBrowser.kt: @JvmStatic fun pollRecord(): String.
    const QJniObject record =
        QJniObject::callStaticObjectMethod(kFileBrowserClass, "pollRecord", "()Ljava/lang/String;");
    return record.isValid() ? record.toString() : QString();
}

void
DecoderHostAndroid::refreshLocalDevice() {
    // WP-D5: readiness, name and failure classification belong to one USB record.
    const auto usb_record = QJniObject::callStaticObjectMethod(kUsbClass, "deviceStatus", "()Ljava/lang/String;");
    const auto usb = QJsonDocument::fromJson(usb_record.toString().toUtf8()).object();
    m_usb.apply(*this, usb);
    // Read off the bus with no permission involved, so the UI can preselect the receiver actually plugged in.
    const auto attached = QJniObject::callStaticObjectMethod(kUsbClass, "attachedSource", "()Ljava/lang/String;");
    const QString attachedSource = attached.isValid() ? attached.toString() : QString();
    if (attachedSource != m_attached_source) {
        m_attached_source = attachedSource;
        Q_EMIT localDeviceChanged();
    }
    // WP-S2: consuming also preserves the cold-start attachment on the first poll.
    const QJniObject attach = QJniObject::callStaticObjectMethod(kUsbClass, "takeAttachment", "()Ljava/lang/String;");
    if (attach.isValid() && !attach.toString().isEmpty()) {
        Q_EMIT localDeviceAttached(attach.toString());
    }
}

} // namespace dsd_android

extern "C" {

JNIEXPORT void JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeHostDiagnostic(JNIEnv* env, jclass clazz, jstring line) {
    (void)env;
    (void)clazz;
    dsd_qt::DiagnosticsLog::submitHostDiagnostic(QJniObject(line).toString());
}

/** @brief Re-arms the diagnostics startup window at a device boundary (USB open, service start). */
JNIEXPORT void JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeArmStartupDiagnostics(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;
    // Called on the main thread, before the service's nativeInit: never waits for the log's construction.
    dsd_qt::DiagnosticsLog::armStartupDiagnostics();
}

/**
 * @brief Starts a flush of the persistent diagnostics before the process may go (Activity teardown), without waiting
 * for it: main() makes the teardown's records durable with the one bounded flush wait, once Qt's teardown is done
 * (#782).
 */
JNIEXPORT void JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeFlushDiagnostics(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;
    (void)dsd_qt::DiagnosticsLog::flushDiagnostics(0);
}

/**
 * @brief Asks the Qt event loop to quit, so main() can return.
 *
 * Called from DsdNeoActivity.onDestroy() before Qt's own teardown runs, and it has to
 * be: QtActivityBase.onDestroy() calls QtNative.terminateQtNativeApplication(), which
 * waits on a semaphore that is only posted once main() has returned — but the quit that
 * would make it return is raised by Qt only when the Android event dispatcher is already
 * stopped:
 *
 *     if (QAndroidEventDispatcherStopper::instance()->stopped()) {
 *         QAndroidEventDispatcherStopper::instance()->startAll();
 *         QCoreApplication::quit();
 *         ...
 *     }
 *     if (startQtAndroidPluginCalled.loadAcquire())
 *         sem_wait(&m_terminateSemaphore);
 *
 * With the dispatcher still running — which is the state a swipe out of recents leaves
 * behind — no quit is sent and the wait never ends. Without a foreground service the
 * process is an empty-process kill candidate and Android reaps it before anyone notices;
 * with the decoder's service up the process is retained, so the block persists and every
 * later main-thread delivery, the service's own included, times out into an ANR.
 *
 * Safe before Qt exists — the Activity can be destroyed after a failed start, when there
 * is no QCoreApplication instance to end.
 */
JNIEXPORT void JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeQuitUi(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;

    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        return;
    }
    /* exit(), not quit(). Since Qt 6, quit() first asks every top-level window to close
     * and abandons the quit if any of them refuses -- and Main.qml's onClosing refuses
     * every time, because that handler is what turns a window close into
     * moveToBackground() so a decode session survives the user leaving the app. Calling
     * quit() here therefore did nothing at all: the handler declined, the loop carried on
     * and the teardown went on waiting. exit() leaves the event loop directly, without
     * consulting windows, which is what a teardown that cannot be declined needs.
     *
     * Queued so the loop unwinds on its own thread; this runs on the Android main thread. */
    QMetaObject::invokeMethod(app, []() { QCoreApplication::exit(0); }, Qt::QueuedConnection);
}

} // extern "C"

namespace dsd_android {
QString
DecoderHostAndroid::serviceFailureText() const {
    const auto reason = QJniObject::callStaticObjectMethod(kServiceClass, "lastError", "()Ljava/lang/String;");
    return reason.isValid() ? reason.toString() : QString();
}

void
DecoderHostAndroid::requestServiceStop() {
    QJniObject context = android_context();
    // The service can cancel a live run without an Activity context; only its deferred
    // notification bookkeeping needs one. It diagnoses an unavailable dispatch itself.
    QJniObject::callStaticMethod<void>(kServiceClass, "stopDecoder", "(Landroid/content/Context;)V", context.object());
    setStatus(QStringLiteral("Stopping…"));
}

QJsonObject
DecoderHostAndroid::readLifecycleStatus() const {
    const auto record = QJniObject::callStaticObjectMethod(kServiceClass, "lifecycleStatus", "()Ljava/lang/String;");
    return record.isValid() ? QJsonDocument::fromJson(record.toString().toUtf8()).object() : QJsonObject();
}

bool
DecoderHostAndroid::readEngineRunning() {
    return engine_is_running();
}

uint64_t
DecoderHostAndroid::readRealtimeMs() {
    return dsd_realtime_mono_ms();
}

QByteArray
DecoderHostAndroid::readServiceState() {
    // Must match DecoderService.kt: @JvmStatic fun stateName(): String. A static read; it starts nothing.
    const auto name = QJniObject::callStaticObjectMethod(kServiceClass, "stateName", "()Ljava/lang/String;");
    return name.isValid() ? name.toString().toUtf8() : QByteArray();
}
} // namespace dsd_android
