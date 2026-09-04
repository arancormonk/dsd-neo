// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "decoder_host_android.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QVariant>

#include <jni.h>

#include "dsdneo_jni.h"

namespace dsd_android {

namespace {

constexpr const char* kServiceClass = "io/github/arancormonk/dsdneo/DecoderService";
constexpr const char* kSupportClass = "io/github/arancormonk/dsdneo/AppSupport";
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
QString
phase_text(SessionPhase phase) {
    switch (phase) {
        case kSessionStarting: return QStringLiteral("Starting…");
        case kSessionRunning: return QStringLiteral("Decoding");
        case kSessionStopping: return QStringLiteral("Stopping…");
        case kSessionFailed: return QStringLiteral("Start failed");
        default: return QStringLiteral("Idle");
    }
}

} // namespace

DecoderHostAndroid::DecoderHostAndroid(QObject* parent) : dsd_qt::DecoderHost(parent) {
    QJniObject context = android_context();
    if (context.isValid()) {
        QJniObject::callStaticMethod<void>(kSupportClass, "ensureNotificationPermission", "(Landroid/app/Activity;)V",
                                           context.object());
    }
}

DecoderHostAndroid::~DecoderHostAndroid() = default;

bool
DecoderHostAndroid::isRunning() const {
    return m_running;
}

QString
DecoderHostAndroid::statusText() const {
    return m_status;
}

dsd_qt::DecoderHost::SessionState
DecoderHostAndroid::sessionState() const {
    return static_cast<SessionState>(m_published_phase);
}

QString
DecoderHostAndroid::failureText() const {
    return m_failure;
}

bool
DecoderHostAndroid::localDeviceReady() const {
    return m_usb_ready;
}

QString
DecoderHostAndroid::localDeviceStatus() const {
    return m_usb_status;
}

void
DecoderHostAndroid::requestLocalDeviceAccess() {
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
    /* Clears the previous attempt's reason; setSessionPhase publishes that. */
    m_phase.note_start_requested();

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

void
DecoderHostAndroid::stop() {
    QJniObject context = android_context();
    if (!context.isValid()) {
        return;
    }
    QJniObject::callStaticMethod<void>(kServiceClass, "stopDecoder", "(Landroid/content/Context;)V", context.object());
    setStatus(QStringLiteral("Stopping…"));
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

void
DecoderHostAndroid::refresh() {
    const bool running = engine_is_running();
    if (running != m_running) {
        m_running = running;
        Q_EMIT runningChanged();
    }

    /* The service owns the transitions; native running-state alone cannot tell
     * "starting" from "idle", nor a failed start from one that never happened. */
    QJniObject state = QJniObject::callStaticObjectMethod(kServiceClass, "stateName", "()Ljava/lang/String;");
    const QByteArray name = state.isValid() ? state.toString().toUtf8() : QByteArrayLiteral("IDLE");

    const SessionPhase phase = m_phase.update(name.constData(), running);
    setSessionPhase(phase);
    setStatus(phase_text(phase));

    const bool usb_ready = QJniObject::callStaticMethod<jboolean>(kUsbClass, "isReady", "()Z") != JNI_FALSE;
    QJniObject usb_status = QJniObject::callStaticObjectMethod(kUsbClass, "statusText", "()Ljava/lang/String;");
    setLocalDeviceState(usb_ready, usb_status.isValid() ? usb_status.toString() : QString());
}

void
DecoderHostAndroid::setStatus(const QString& text) {
    if (m_status == text) {
        return;
    }
    m_status = text;
    Q_EMIT statusTextChanged();
}

void
DecoderHostAndroid::setSessionPhase(SessionPhase phase, const QString& reason) {
    QString failure;
    if (phase == kSessionFailed) {
        /* A reason the host produced itself wins: the service never saw that attempt,
         * so its own record would be stale. */
        failure = reason.isEmpty() ? m_failure : reason;
        if (failure.isEmpty()) {
            QJniObject service_reason =
                QJniObject::callStaticObjectMethod(kServiceClass, "lastError", "()Ljava/lang/String;");
            if (service_reason.isValid()) {
                failure = service_reason.toString();
            }
        }
        if (failure.isEmpty()) {
            failure = QStringLiteral("The decoder could not be started. Check the input settings.");
        }
    }

    if (phase == m_published_phase && failure == m_failure) {
        return;
    }
    m_published_phase = phase;
    m_failure = failure;
    Q_EMIT sessionStateChanged();
}

void
DecoderHostAndroid::setLocalDeviceState(bool ready, const QString& text) {
    if (m_usb_ready == ready && m_usb_status == text) {
        return;
    }
    m_usb_ready = ready;
    m_usb_status = text;
    Q_EMIT localDeviceChanged();
}

} // namespace dsd_android

extern "C" {

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
