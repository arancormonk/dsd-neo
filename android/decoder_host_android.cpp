// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "decoder_host_android.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>

#include "dsdneo_jni.h"

namespace dsd_android {

namespace {

constexpr const char* kServiceClass = "io/github/arancormonk/dsdneo/DecoderService";
constexpr const char* kSupportClass = "io/github/arancormonk/dsdneo/AppSupport";
constexpr const char* kUsbClass = "io/github/arancormonk/dsdneo/UsbSourceManager";

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

void
DecoderHostAndroid::moveToBackground() {
    /* Finishing the Activity would terminate the Qt process, and with it the service
     * that owns the engine. Backgrounding keeps both alive. */
    QJniObject activity = android_context();
    if (activity.isValid()) {
        (void)activity.callMethod<jboolean>("moveTaskToBack", "(Z)Z", JNI_TRUE);
    }
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
