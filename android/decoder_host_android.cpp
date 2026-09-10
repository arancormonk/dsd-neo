// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QByteArray>
#include <QChar>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <Qt>
#include <functional>
#include <qcoreapplication_platform.h>
#include "decoder_host_android.h"
#include "diagnostics_log.h"
#include "run_status.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>

#include <jni.h>

#include "dsdneo_jni.h"

namespace dsd_android {

namespace {

constexpr const char* kServiceClass = "io/github/arancormonk/dsdneo/DecoderService";
constexpr const char* kSupportClass = "io/github/arancormonk/dsdneo/AppSupport";
constexpr const char* kLocationClass = "io/github/arancormonk/dsdneo/LocationSupport";
constexpr const char* kUsbClass = "io/github/arancormonk/dsdneo/UsbSourceManager";

/* android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON. Namespace scope,
 * not function-local: QJniObject::callMethod takes its arguments by forwarding
 * reference, which odr-uses the constant, and the keep-awake lambda below has
 * no capture-default to pick a local up with. */
constexpr int kFlagKeepScreenOn = 128;

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

/** @brief Prose for the toolbar and the platform notification. */

} // namespace

DecoderHostAndroid::DecoderHostAndroid(QObject* parent) : dsd_qt::DecoderHost(parent) {
    dsd_qt::DiagnosticsLog::installTap();
    QJniObject context = android_context();
    if (context.isValid()) {
        QJniObject::callStaticMethod<void>(kSupportClass, "ensureNotificationPermission", "(Landroid/app/Activity;)V",
                                           context.object());
    }
}

void
DecoderHostAndroid::hostDiagnostic(const QString& line) {
    dsd_qt::DiagnosticsLog::instance().submit(QStringLiteral("host"), QStringLiteral("info"), line);
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
DecoderHostAndroid::requestLocalDeviceAccess() {
    // WP-D5: keep the service's run result intact, but acknowledge its old USB
    // diagnostic so Retry can reacquire access and the next Play can claim again.
    if (!sessionActive() && m_device_error) {
        m_retried_device_session = m_device_error_session;
        m_device_error = 0;
        Q_EMIT localDeviceChanged();
    }
    QJniObject context = android_context();
    if (!context.isValid()) {
        return;
    }
    /* The permission dialog answers asynchronously; the poll tick picks the result
     * up through refresh(). */
    QJniObject::callStaticMethod<void>(kUsbClass, "requestAccess", "(Landroid/content/Context;)V", context.object());
}

void
DecoderHostAndroid::setKeepScreenAwake(bool on) {
    /* The flag belongs to the Activity's window and must be flipped on the Android
     * main thread, not the Qt one. */
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([on]() -> QVariant {
        QJniObject activity = android_context();
        if (!activity.isValid()) {
            return {};
        }
        QJniObject window = activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
        if (!window.isValid()) {
            return {};
        }
        if (on) {
            window.callMethod<void>("addFlags", "(I)V", kFlagKeepScreenOn);
        } else {
            window.callMethod<void>("clearFlags", "(I)V", kFlagKeepScreenOn);
        }
        return {};
    });
}

bool
DecoderHostAndroid::start(const QStringList& argv) {
    if (!m_primed) {
        refresh();
    }
    /* Clears the previous attempt's reason; setSessionPhase publishes that. */
    QJniObject record = QJniObject::callStaticObjectMethod(kServiceClass, "lifecycleStatus", "()Ljava/lang/String;");
    const QJsonObject status =
        record.isValid() ? QJsonDocument::fromJson(record.toString().toUtf8()).object() : QJsonObject();
    const auto last_session = static_cast<uint64_t>(status.value(QStringLiteral("sessionId")).toInteger());
    const QByteArray service_state = status.value(QStringLiteral("state")).toString(QStringLiteral("IDLE")).toUtf8();
    if (!m_phase.note_start_requested(last_session, service_state.constData())) {
        const QString reason = QStringLiteral("A previous session is still stopping");
        setSessionPhase(m_phase.phase(), reason);
        setStatus(reason);
        return false;
    }
    m_initialized_session = last_session;

    QJniObject context = android_context();
    if (!context.isValid()) {
        return failStart(QStringLiteral("No Android context"));
    }

    QJniEnvironment env;
    jobjectArray args = to_java_string_array(env, argv);
    if (args == nullptr) {
        return failStart(QStringLiteral("Could not marshal arguments"));
    }

    QJniObject::callStaticMethod<void>(kServiceClass, "startDecoder", "(Landroid/content/Context;[Ljava/lang/String;)V",
                                       context.object(), args);
    env->DeleteLocalRef(args);

    setSessionPhase(m_phase.phase());
    setStatus(QStringLiteral("Starting…"));
    return true;
}

bool
DecoderHostAndroid::failStart(const QString& reason) {
    m_phase.note_start_failed();
    /* m_failure is only ever written by setSessionPhase, so that it can still see the
     * previous reason and tell one failure from the next. */
    setSessionPhase(m_phase.phase(), reason);
    setStatus(reason);
    return false;
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

QString
DecoderHostAndroid::importContentUri(const QString& reference, const QString& fileName) {
    QJniObject context = android_context();
    if (!context.isValid()) {
        return QString();
    }
    QJniObject uri = QJniObject::fromString(reference);
    QJniObject name = QJniObject::fromString(fileName);
    QJniObject result = QJniObject::callStaticObjectMethod(
        kSupportClass, "copyContentUriToCache",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", context.object(),
        uri.object(), name.object());
    return result.isValid() ? result.toString() : QString();
}

QString
DecoderHostAndroid::importDocument(const QString& reference, const QString& fileName, const QString& replacePath) {
    QJniObject context = android_context();
    if (!context.isValid()) {
        return QString();
    }
    QJniObject uri = QJniObject::fromString(reference);
    QJniObject name = QJniObject::fromString(fileName);
    QJniObject replace = QJniObject::fromString(replacePath);
    QJniObject result = QJniObject::callStaticObjectMethod(
        kSupportClass, "importDocumentToFiles",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        context.object(), uri.object(), name.object(), replace.object());
    return result.isValid() ? result.toString() : QString();
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
DecoderHostAndroid::cancelLocationRequest(qint64 requestId) {
    QJniObject::callStaticMethod<void>(kLocationClass, "cancelLocationRequest", "(J)V", static_cast<jlong>(requestId));
}

void
DecoderHostAndroid::refreshLocation() {
    // WP-D3: drain a single terminal location result, independently of decoder state.
    const auto locationRecord =
        QJniObject::callStaticObjectMethod(kLocationClass, "pollResult", "()Ljava/lang/String;");
    const auto location = QJsonDocument::fromJson(locationRecord.toString().toUtf8()).object();
    if (!location.isEmpty()) {
        Q_EMIT locationResult(
            location.value(QStringLiteral("id")).toInteger(), location.value(QStringLiteral("fixOk")).toBool(),
            location.value(QStringLiteral("lat")).toDouble(), location.value(QStringLiteral("lon")).toDouble(),
            location.value(QStringLiteral("accuracyM")).toDouble(),
            location.value(QStringLiteral("fixAtMs")).toInteger(), location.value(QStringLiteral("geocodeOk")).toBool(),
            location.value(QStringLiteral("postalCode")).toString(),
            location.value(QStringLiteral("countryCode")).toString(),
            location.value(QStringLiteral("error")).toString());
    }
}

void
DecoderHostAndroid::refreshLocalDevice() {
    // WP-D5: readiness, name and failure classification belong to one USB record.
    const auto usb_record = QJniObject::callStaticObjectMethod(kUsbClass, "deviceStatus", "()Ljava/lang/String;");
    const auto usb = QJsonDocument::fromJson(usb_record.toString().toUtf8()).object();
    m_usb.apply(*this, usb);
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
    if (!context.isValid()) {
        return;
    }
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
} // namespace dsd_android
