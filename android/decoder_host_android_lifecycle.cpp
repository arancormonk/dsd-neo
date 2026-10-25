// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>
#include <stdint.h>
#include <utility>
#include "decoder_host.h"
#include "decoder_host_android.h"
#include "local_device_state.h"
#include "run_status.h"
#include "session_state_map.h"

namespace dsd_android {
namespace {
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

/** receiverHealth()'s source for a record's ReceiverSource. */
QString
receiver_source_name(int source) {
    switch (source) {
        case kReceiverRtl: return QStringLiteral("usb");
        case kReceiverAirspy: return QStringLiteral("airspy");
        default: return QString();
    }
}

} // namespace

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
    return m_usb.ready;
}

bool
DecoderHostAndroid::deviceErrorApplies() const {
    return m_device_error != 0 && m_usb.ready && m_usb.source == m_device_error_source
           && m_usb.serial == m_device_error_serial;
}

QString
DecoderHostAndroid::localDeviceStatus() const {
    // WP-D5: only a native claim error may suggest another app owns the device.
    if (m_usb.failureKind == NoDeviceFailure && deviceErrorApplies()) {
        if (m_device_error == -6) {
            return tr("Android could not claim %1. It may be held by another SDR app, or the OTG port may be "
                      "under-powered — try a powered hub.")
                .arg(m_usb.name);
        }
        return tr("Android could not open or claim %1 (code %2).").arg(m_usb.name).arg(m_device_error);
    }
    return m_usb.text;
}

int
DecoderHostAndroid::localDeviceFailureKind() const {
    // libusb's BUSY value survives librtlsdr's claim failure; other native codes
    // remain in the terminal result for diagnostics without guessing a cause.
    if (m_usb.failureKind != NoDeviceFailure || !m_usb.ready) {
        return m_usb.failureKind;
    }
    if (!deviceErrorApplies()) {
        return NoDeviceFailure;
    }
    return m_device_error == -6 ? DeviceBusy : DeviceOpenFailed;
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
    QString failure = reason;
    if (phase == kSessionFailed) {
        /* A reason the host produced itself wins: the service never saw that attempt,
         * so its own record would be stale. */
        failure = reason.isEmpty() ? m_failure : reason;
        if (failure.isEmpty()) {
            failure = serviceFailureText();
        }
        if (failure.isEmpty()) {
            failure = QStringLiteral("The decoder could not be started. Check the input settings.");
        }
    }

    if (phase == m_published_phase && failure == m_failure) {
        return;
    }
    m_published_phase = phase;
    m_failure = std::move(failure);
    Q_EMIT sessionStateChanged();
}

void
DecoderHostAndroid::applyLifecycleStatus(const QJsonObject& status, bool running) {
    const bool first_poll = !m_primed;
    if (running != m_running) {
        m_running = running;
        Q_EMIT runningChanged();
    }

    const uint64_t session = static_cast<uint64_t>(status.value(QStringLiteral("sessionId")).toInteger());
    const QByteArray name = status.value(QStringLiteral("state")).toString(QStringLiteral("IDLE")).toUtf8();
    const auto reason = static_cast<RunReason>(status.value(QStringLiteral("reason")).toInt());
    const int input_failure = status.value(QStringLiteral("inputFailure")).toInt();
    const int input_error = status.value(QStringLiteral("inputError")).toInt();
    const int end_cause = status.value(QStringLiteral("endCause")).toInt();
    const int receiver_source = status.value(QStringLiteral("receiverSource")).toInt();
    // A record that could not be read says nothing about the last session's receiver.
    if (!status.isEmpty()) {
        m_health_session = session;
        m_end_cause = end_cause;
        m_receiver_source = receiver_source;
    }
    const bool health_changed = updateReceiverHealth();
    if (m_input_failure != input_failure || m_input_error != input_error || m_terminal_reason != reason
        || health_changed) {
        m_input_failure = input_failure;
        m_input_error = input_error;
        m_terminal_reason = reason;
        Q_EMIT runResultChanged();
    }
    if (first_poll) {
        m_initialized_session = session;
        if (name == "IDLE") {
            m_adopted_idle_session = session;
        }
    }
    const uint64_t now_ms = readRealtimeMs();
    SessionPhase phase = status.isEmpty() ? m_phase.update(name.constData(), running, now_ms)
                                          : m_phase.update(name.constData(), running, session, reason, now_ms);
    // #647: the receiver ended this session; it is winding down, whatever the service still says.
    if (!status.isEmpty() && (end_cause == kEndDetached || end_cause == kEndReceiverFailed)
        && receiver_source != kReceiverNone && m_phase.note_receiver_end(session)) {
        phase = m_phase.phase();
    }
    const bool adopted_idle = session != 0 && session == m_adopted_idle_session;
    m_device_error_session = session;
    const int device_error =
        adopted_idle || session == m_retried_device_session ? 0 : status.value(QStringLiteral("deviceError")).toInt();
    bool device_changed = m_device_error != device_error;
    m_device_error = device_error;
    if (device_error != 0 && (device_changed || m_device_error_owner != session)) {
        // B11: the error belongs to the session's receiver, which is the connection ready while it ran.
        const QString source = receiver_source_name(receiver_source);
        m_device_error_owner = session;
        m_device_error_source = source.isEmpty() ? m_usb.source : source;
        m_device_error_serial = m_usb.source == m_device_error_source ? m_usb.serial : QString();
        device_changed = true;
    }
    if (device_changed) {
        Q_EMIT localDeviceChanged();
    }
    if (!first_poll && status.value(QStringLiteral("initialized")).toBool() && session > m_initialized_session) {
        m_initialized_session = session;
        Q_EMIT sessionInitialized();
    }
    setSessionPhase(phase,
                    first_poll || adopted_idle ? QString() : status.value(QStringLiteral("lastError")).toString());
    setStatus(phase_text(phase));
    if (m_phase.take_stop_expired()) {
        hostDiagnostic(QStringLiteral("Stop was not acknowledged by the decoder service within 2 s; showing the "
                                      "session as running again"));
    }

    // Attachment delivery can synchronously call start() through QML.
    m_primed = true;
}

bool
DecoderHostAndroid::updateReceiverHealth() {
    QVariantMap health = healthyReceiver();
    const bool acknowledged = m_health_session == 0 || m_health_session <= m_health_cleared_session;
    const QString state = m_end_cause == kEndDetached         ? QStringLiteral("lost")
                          : m_end_cause == kEndReceiverFailed ? QStringLiteral("failed")
                                                              : QString();
    // An ending with no local receiver (a detach during an rtl_tcp session) is no receiver's: nothing to name.
    if (!acknowledged && !state.isEmpty() && m_receiver_source != kReceiverNone) {
        health = {{QStringLiteral("state"), state},
                  {QStringLiteral("source"), receiver_source_name(m_receiver_source)},
                  {QStringLiteral("session"), static_cast<qlonglong>(m_health_session)}};
    }
    if (health == m_receiver_health) {
        return false;
    }
    m_receiver_health = health;
    return true;
}

void
DecoderHostAndroid::clearReceiverHealth(uint64_t session) {
    m_health_cleared_session = qMax(m_health_cleared_session, session);
    if (updateReceiverHealth()) {
        Q_EMIT runResultChanged();
    }
}

void
DecoderHostAndroid::acknowledgeReceiverEnding() {
    // Like the WP-D5 claim error: while the ended session still winds down its notice stays (the Monitor's LOST).
    if (!sessionActive() && m_receiver_health.value(QStringLiteral("state")).toString() != QStringLiteral("ok")) {
        const uint64_t ended = m_health_session;
        // A stream failure ends its run Failed, and that failure is this same ending: it is retired with the notice,
        // as stop() acknowledges a failure, or Listen would show a second card for one session. The failure goes
        // first, so the notice never gives way to the failure's own text in between.
        if (m_phase.phase() == kSessionFailed) {
            const QJsonObject status = readLifecycleStatus();
            if (static_cast<uint64_t>(status.value(QStringLiteral("sessionId")).toInteger()) == ended) {
                (void)acknowledgeFailure(status);
            }
        }
        clearReceiverHealth(ended);
    }
}

bool
DecoderHostAndroid::acknowledgeFailure(const QJsonObject& status) {
    const auto name = status.value(QStringLiteral("state")).toString().toUtf8();
    const auto session = static_cast<uint64_t>(status.value(QStringLiteral("sessionId")).toInteger());
    if (status.isEmpty() || !m_phase.acknowledge_failure(session, name.constData())) {
        return false;
    }
    m_adopted_idle_session = session;
    setSessionPhase(kSessionIdle);
    setStatus(phase_text(kSessionIdle));
    return true;
}

void
DecoderHostAndroid::stop() {
    if (m_phase.phase() == kSessionFailed && acknowledgeFailure(readLifecycleStatus())) {
        return;
    }
    /* #718: answer the tap now. The service reports STOPPING only after it has handled the intent and a later poll
     * has read it, and until then the button would sit enabled and unchanged. The tracker holds Stopping over the
     * attempt's records until its terminal one, so a poll in between cannot flip it back. */
    if (m_phase.note_stop_requested(readRealtimeMs())) {
        setSessionPhase(m_phase.phase());
        setStatus(phase_text(m_phase.phase()));
    }
    requestServiceStop();
}

bool
DecoderHostAndroid::start(const QStringList& argv) {
    if (!m_primed) {
        refresh();
    }
    /* Clears the previous attempt's reason; setSessionPhase publishes that. */
    const QJsonObject status = readLifecycleStatus();
    const auto last_session = static_cast<uint64_t>(status.value(QStringLiteral("sessionId")).toInteger());
    const QByteArray service_state = status.value(QStringLiteral("state")).toString(QStringLiteral("IDLE")).toUtf8();
    if (!m_phase.note_start_requested(last_session, service_state.constData())) {
        const QString reason = QStringLiteral("A previous session is still stopping");
        setSessionPhase(m_phase.phase(), reason);
        setStatus(reason);
        return false;
    }
    m_initialized_session = last_session;
    // The new session replaces the last one, and with it any receiver ending, seen yet or not.
    clearReceiverHealth(qMax(last_session, m_health_session));

    const QString failure = platformStartDecoder(argv);
    if (!failure.isEmpty()) {
        return failStart(failure);
    }

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
DecoderHostAndroid::requestLocalDeviceAccess() {
    // WP-D5: keep the service's run result intact, but acknowledge its old USB
    // diagnostic so Retry can reacquire access and the next Play can claim again.
    if (!sessionActive() && m_device_error) {
        m_retried_device_session = m_device_error_session;
        m_device_error = 0;
        Q_EMIT localDeviceChanged();
    }
    acknowledgeReceiverEnding();
    platformRequestAccess();
}

void
DecoderHostAndroid::requestLocalDeviceAccessForSource(const QString& source, const QString& serial) {
    noteAttempt(source, dsd_qt::DiagnosticsLog::AttemptStage::UsbAccess);
    acknowledgeReceiverEnding();
    platformRequestAccessForSource(source, serial);
}

bool
DecoderHostAndroid::engineBusy() const {
    /* The engine first: it only runs while the service is out of IDLE, so an IDLE read after a clear atomic means
     * nothing was starting or running at that read. The other order could miss a start that began between the two. */
    const bool running = readEngineRunning();
    return engine_busy(readServiceState().constData(), running);
}

void
DecoderHostAndroid::refresh() {
    refreshPresentation();
    refreshLocation();
    const bool running = readEngineRunning();
    const auto status = readLifecycleStatus();
    applyLifecycleStatus(status, running);
    refreshLocalDevice();
}
} // namespace dsd_android
