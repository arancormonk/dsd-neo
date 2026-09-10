// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <stdint.h>
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

QString
DecoderHostAndroid::localDeviceStatus() const {
    // WP-D5: only a native claim error may suggest another app owns the device.
    if (m_usb.ready && m_usb.failureKind == NoDeviceFailure && m_device_error) {
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
    return m_device_error == -6 ? DeviceBusy : (m_device_error ? DeviceOpenFailed : NoDeviceFailure);
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
    m_failure = failure;
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
    if (first_poll) {
        m_initialized_session = session;
        if (name == "IDLE") {
            m_adopted_idle_session = session;
        }
    }
    const SessionPhase phase = status.isEmpty() ? m_phase.update(name.constData(), running)
                                                : m_phase.update(name.constData(), running, session, reason);
    const bool adopted_idle = session != 0 && session == m_adopted_idle_session;
    m_device_error_session = session;
    const int device_error =
        adopted_idle || session == m_retried_device_session ? 0 : status.value(QStringLiteral("deviceError")).toInt();
    if (m_device_error != device_error) {
        m_device_error = device_error;
        Q_EMIT localDeviceChanged();
    }
    if (!first_poll && status.value(QStringLiteral("initialized")).toBool() && session > m_initialized_session) {
        m_initialized_session = session;
        Q_EMIT sessionInitialized();
    }
    setSessionPhase(phase,
                    first_poll || adopted_idle ? QString() : status.value(QStringLiteral("lastError")).toString());
    setStatus(phase_text(phase));

    // Attachment delivery can synchronously call start() through QML.
    m_primed = true;
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
    requestServiceStop();
}

void
DecoderHostAndroid::refresh() {
    refreshLocation();
    const bool running = readEngineRunning();
    const auto status = readLifecycleStatus();
    applyLifecycleStatus(status, running);
    refreshLocalDevice();
}
} // namespace dsd_android
