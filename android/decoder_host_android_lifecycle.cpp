// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <stdint.h>
#include <utility>
#include "decoder_host.h"
#include "decoder_host_android.h"
#include "diagnostics_log.h"
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

/**
 * A new start attempt's id (#782): increasing for the life of the process, which DecoderService's record of retired
 * attempts shares, so a host recreated with the Activity never reuses an id. Qt thread only.
 */
uint64_t
next_attempt_id() {
    static uint64_t last = 0;
    return ++last;
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

DecoderHostAndroid::~DecoderHostAndroid() { closeRecordingWork(); }

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
    if (m_device_error == 0 || !m_usb.ready || m_usb.source != m_device_error_source) {
        return false;
    }
    // #782: the connection the session took. A blank or shared serial cannot tell two dongles apart, and a
    // replacement connection to the same dongle is a fresh one: the error is shown only while that connection is
    // the ready one.
    if (m_device_error_connection != 0) {
        return m_usb.connection == m_device_error_connection && m_usb.device == m_device_error_device
               && m_usb.epoch == m_device_error_epoch;
    }
    return m_usb.serial == m_device_error_serial;
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

bool
DecoderHostAndroid::recordOfOtherAttempt(const QJsonObject& status) const {
    // Records before #782 name no attempt; a host that has started none (a recreated Activity, a dismissed attempt)
    // follows whatever the service runs.
    if (m_attempt == 0 || !status.contains(QStringLiteral("attemptId"))) {
        return false;
    }
    return static_cast<uint64_t>(status.value(QStringLiteral("attemptId")).toInteger()) != m_attempt;
}

void
DecoderHostAndroid::applyLifecycleStatus(const QJsonObject& status, bool running) {
    const bool first_poll = !m_primed;
    if (running != m_running) {
        m_running = running;
        Q_EMIT runningChanged();
    }

    const uint64_t session = static_cast<uint64_t>(status.value(QStringLiteral("sessionId")).toInteger());
    /* A record that could not be read names no service state. With the engine running it is not IDLE: the engine's
     * run opens and closes the call recording journal (#708), and the recording store may rewrite a folder's journal
     * only once the session reads inactive. */
    const QString fallback = status.isEmpty() && running ? QStringLiteral("RUNNING") : QStringLiteral("IDLE");
    const QByteArray name = status.value(QStringLiteral("state")).toString(fallback).toUtf8();
    const uint64_t now_ms = readRealtimeMs();
    /* Gate r5 L1: a record of another attempt than the current one (one this host retired or superseded, or the
     * previous session's while the start waits) is none of the current attempt's start, run or end. It leaves the
     * phase, the run result, the receiver health and device error, and the published state as they are; only the
     * current start's grace goes on (SessionPhaseTracker::update_other_attempt()). */
    if (!first_poll && recordOfOtherAttempt(status)) {
        (void)m_phase.update_other_attempt(name.constData(), now_ms);
        (void)noteStartExpiry(name, now_ms);
        publishPhase(m_phase.phase(), m_failure);
        m_primed = true;
        return;
    }

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
        // An ending a previous host (an Activity since recreated) acknowledged stays acknowledged here.
        m_health_cleared_session =
            qMax(m_health_cleared_session,
                 static_cast<uint64_t>(status.value(QStringLiteral("endingAcknowledged")).toInteger()));
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
    SessionPhase phase = status.isEmpty() ? m_phase.update(name.constData(), running, now_ms)
                                          : m_phase.update(name.constData(), running, session, reason, now_ms);
    if (noteStartExpiry(name, now_ms)) {
        phase = m_phase.phase();
    }
    // The service's own failure of the attempt on screen, before its configure (#782). Older records never set one.
    const auto record_attempt = static_cast<uint64_t>(status.value(QStringLiteral("attemptId")).toInteger());
    const AttemptFailureKind service_kind =
        attempt_failure_from_wire(status.value(QStringLiteral("failureKind")).toString().toUtf8().constData());
    if (!m_attempt_failed && m_attempt != 0 && record_attempt == m_attempt && service_kind != kAttemptNone) {
        setAttemptFailure(service_kind, status.value(QStringLiteral("lastError")).toString());
    }
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
        // #782: the connection the session took, from its own record; a result arriving after a replacement
        // connection keeps naming the old one.
        m_device_error_device = status.value(QStringLiteral("usbDevice")).toString();
        m_device_error_epoch = status.value(QStringLiteral("usbEpoch")).toInteger();
        m_device_error_connection = status.value(QStringLiteral("usbConnection")).toInteger();
        device_changed = true;
    }
    if (device_changed) {
        Q_EMIT localDeviceChanged();
    }
    if (!first_poll && status.value(QStringLiteral("initialized")).toBool() && session > m_initialized_session) {
        m_initialized_session = session;
        Q_EMIT sessionInitialized();
    }
    publishPhase(phase, first_poll || adopted_idle ? QString() : status.value(QStringLiteral("lastError")).toString());
    if (m_phase.take_stop_expired()) {
        hostDiagnostic(QStringLiteral("Stop was not acknowledged by the decoder service within 2 s; showing the "
                                      "session as running again"));
    }

    // Attachment delivery can synchronously call start() through QML.
    m_primed = true;
    if (first_poll) {
        Q_EMIT sessionStateKnownChanged();
    }
}

bool
DecoderHostAndroid::noteStartExpiry(const QByteArray& service_state, uint64_t now_ms) {
    uint64_t waited_ms = 0;
    if (!m_phase.take_start_expired(&waited_ms)) {
        return false;
    }
    hostDiagnostic(QStringLiteral("Start was not delivered to the decoder service within %1 ms (service state %2)")
                       .arg(waited_ms)
                       .arg(QString::fromUtf8(service_state)));
    // The expiry was computed from the record read above; the retirement answers for now, under the lock the
    // service decides each start with. Only an attempt that can no longer run is reported as undelivered.
    if (platformRetireAttempts(m_attempt)) {
        hostDiagnostic(QStringLiteral("Start attempt %1 was taken by the decoder service after all").arg(m_attempt));
        m_phase.note_start_taken(now_ms);
    } else {
        setAttemptFailure(kAttemptUndelivered, QStringLiteral("The decoder service did not respond to the start"));
    }
    return true;
}

void
DecoderHostAndroid::publishPhase(SessionPhase phase, const QString& reason) {
    if (m_attempt_failed) {
        // The attempt's own failure outranks every record: they are older sessions', or say less.
        setSessionPhase(kSessionFailed, m_attempt_failure_text);
        setStatus(phase_text(kSessionFailed));
    } else {
        setSessionPhase(phase, reason);
        setStatus(phase_text(phase));
    }
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
    if (session > m_health_cleared_session) {
        m_health_cleared_session = session;
        platformAcknowledgeReceiverEnding(session);
    }
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
DecoderHostAndroid::setAttemptFailure(int kind, const QString& text) {
    const bool changed = !m_attempt_failed || m_attempt_failure_kind != kind;
    m_attempt_failed = true;
    m_attempt_failure_kind = kind;
    m_attempt_failure_text = text.isEmpty() ? QStringLiteral("The decoder could not be started.") : text;
    if (changed) {
        Q_EMIT runResultChanged();
    }
    setSessionPhase(kSessionFailed, m_attempt_failure_text);
    setStatus(phase_text(kSessionFailed));
}

void
DecoderHostAndroid::dismissAttemptFailure() {
    (void)platformRetireAttempts(m_attempt);
    m_attempt = 0;
    const bool was_failed = m_attempt_failed;
    const int kind = m_attempt_failure_kind;
    m_attempt_failed = false;
    m_attempt_failure_kind = 0;
    m_attempt_failure_text.clear();
    if (was_failed) {
        Q_EMIT runResultChanged();
    }
    // The tracker went on following the service underneath. A failure of its own (the expired grace, the attempt's
    // failed record) goes with the attempt's, as stop() acknowledges any failure; otherwise its phase is published.
    if (m_phase.phase() == kSessionFailed) {
        const QJsonObject status = readLifecycleStatus();
        if (acknowledgeFailure(status)) {
            return;
        }
        // An undelivered start the service took after all is still being given up: the retirement cancels it. Not
        // the expiry's stale failure, nor "Starting…" for a session that never runs, until its terminal record.
        if (kind == kAttemptUndelivered && !status.isEmpty()
            && status.value(QStringLiteral("state")).toString() != QStringLiteral("IDLE")) {
            m_phase.note_attempt_retired(readRealtimeMs());
        }
    }
    setSessionPhase(m_phase.phase());
    setStatus(phase_text(m_phase.phase()));
}

void
DecoderHostAndroid::dismissFailure() {
    if (m_attempt_failed) {
        dismissAttemptFailure();
        return;
    }
    // A failure the service reported: the attempt behind it is over, and retiring it only makes sure.
    if (m_attempt != 0 && !sessionActive()) {
        (void)platformRetireAttempts(m_attempt);
    }
}

void
DecoderHostAndroid::stop() {
    // The attempt's own failure is acknowledged like any failure: nothing of it runs that a stop could end.
    if (m_attempt_failed) {
        dismissAttemptFailure();
        return;
    }
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
DecoderHostAndroid::refuseStart(uint64_t attempt, const QByteArray& service_state) {
    hostDiagnostic(QStringLiteral("Start attempt %1 refused: the decoder service is %2")
                       .arg(attempt)
                       .arg(QString::fromUtf8(service_state)));
    // A run on screen, starting or running, stays as it is: the refusal is this start's, not that run's failure.
    if (m_published_phase == kSessionStarting || m_published_phase == kSessionRunning) {
        return false;
    }
    m_attempt = attempt;
    clearAttemptFailure();
    setAttemptFailure(kAttemptPreviousStopping, QStringLiteral("A previous session is still stopping"));
    return false;
}

bool
DecoderHostAndroid::start(const QStringList& argv) {
    if (!m_primed) {
        refresh();
    }
    const uint64_t attempt = next_attempt_id();
    /* #782: before the service state is read, so no earlier attempt (one whose grace ran out, or one a host before
     * this one asked for) can be taken by the service once this one is asked for, whatever the read says. Only from a
     * settled screen: a start that reaches here while one is on its way (an entry point no guard covers) must not
     * cancel that one before the service state is read, as it can be refused for it below; accepted, it supersedes
     * that one then. */
    const bool settled = m_published_phase == kSessionIdle || m_published_phase == kSessionFailed;
    if (settled) {
        (void)platformRetireAttempts(attempt - 1);
    }
    /* Clears the previous attempt's reason; setSessionPhase publishes that. */
    const QJsonObject status = readLifecycleStatus();
    const auto last_session = static_cast<uint64_t>(status.value(QStringLiteral("sessionId")).toInteger());
    const QByteArray service_state = status.value(QStringLiteral("state")).toString(QStringLiteral("IDLE")).toUtf8();
    const SessionPhaseTracker before = m_phase;
    if (!m_phase.note_start_requested(last_session, service_state.constData(), readRealtimeMs())) {
        return refuseStart(attempt, service_state);
    }
    /* Accepted while one was on its way: the service reads IDLE, so that one's START is not delivered yet. This one
     * supersedes it, or it would run under a host that follows this one, and this one would be refused for it. */
    if (!settled) {
        (void)platformRetireAttempts(attempt - 1);
        /* Gate r5 L1: the service may have taken that one between the read and the retirement. One still STARTING is
         * cancelled once its configure returns, and this one's START is handled after it. One already RUNNING (or
         * past it) runs on, and this one's START would only be rejected as busy: it is refused here, and the run on
         * screen goes on as that attempt's, followed as before the request. */
        const QByteArray now_state =
            readLifecycleStatus().value(QStringLiteral("state")).toString(QStringLiteral("IDLE")).toUtf8();
        if (now_state == "RUNNING" || now_state == "STOPPING") {
            m_phase = before;
            return refuseStart(attempt, now_state);
        }
    }
    m_attempt = attempt;
    clearAttemptFailure();
    m_initialized_session = last_session;
    // The new session replaces the last one, and with it any receiver ending, seen yet or not.
    clearReceiverHealth(qMax(last_session, m_health_session));

    const QString failure = platformStartDecoder(argv, attempt);
    if (!failure.isEmpty()) {
        return failStart(failure);
    }

    setSessionPhase(m_phase.phase());
    setStatus(QStringLiteral("Starting…"));
    return true;
}

void
DecoderHostAndroid::clearAttemptFailure() {
    if (m_attempt_failed) {
        m_attempt_failed = false;
        m_attempt_failure_kind = 0;
        m_attempt_failure_text.clear();
        Q_EMIT runResultChanged();
    }
}

bool
DecoderHostAndroid::failStart(const QString& reason) {
    m_phase.note_start_failed();
    /* The attempt's own failure, with no kind the UI words: the previous session's input failure is not this one's. */
    setAttemptFailure(0, reason);
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
    refreshPlayback();
    refreshRecordingTransfer();
}
} // namespace dsd_android
