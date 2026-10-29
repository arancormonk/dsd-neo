// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "recording_status.h"

#include "decoder_host.h"

#include <memory>

#include <QLatin1String>
#include <QMap>
#include <QVariant>
#include <QtGlobal>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/recording_view.h>
#include <dsd-neo/core/wav_recording.h>

namespace dsd_qt {

namespace {

/* The failure kinds by dsd_wav_recording_error, NONE first. */
constexpr const char* k_error_names[DSD_WAV_RECORDING_ERROR_KINDS] = {"", "open", "write", "close", "publish"};

bool
same_view(const dsd_app_recording_view& a, const dsd_app_recording_view& b) {
    for (int kind = 0; kind < DSD_WAV_RECORDING_ERROR_KINDS; kind++) {
        if (a.failures[kind] != b.failures[kind]) {
            return false;
        }
    }
    return a.published == b.published && a.mode == b.mode && a.per_call == b.per_call && a.slot_files == b.slot_files
           && a.unknown_only == b.unknown_only && a.journal_state == b.journal_state
           && a.journal_named == b.journal_named && a.command_state == b.command_state && a.command_id == b.command_id
           && a.last_error == b.last_error;
}

} // namespace

RecordingStatus::RecordingStatus(QObject* parent) : QObject(parent) {}

void
RecordingStatus::refresh() {
    dsd_app_recording_view view{};
    (void)dsd_app_recording_view_get(&view);
    apply(view);
}

void
RecordingStatus::apply(const dsd_app_recording_view& view) {
    if (same_view(view, m_view)) {
        return;
    }
    m_view = view;
    Q_EMIT changed();
}

void
RecordingStatus::noteRefusal(const QString& reason) {
    m_refusal = reason;
    m_refusalCount++;
    Q_EMIT changed();
}

void
RecordingStatus::noteSent() {
    const bool moved = !m_refusal.isEmpty() || !m_sentThisSession;
    m_refusal.clear();
    m_sentThisSession = true;
    m_followUpId = 0;
    m_followUpOf = 0;
    dsd_app_recording_view view{};
    (void)dsd_app_recording_view_get(&view);
    if (!same_view(view, m_view)) {
        m_view = view;
        Q_EMIT changed();
    } else if (moved) {
        Q_EMIT changed();
    }
}

void
RecordingStatus::noteFollowUp(int noted, int followUp) {
    m_followUpId = followUp;
    m_followUpOf = noted;
}

void
RecordingStatus::setSessionActive(bool active) {
    if (active == m_sessionActive) {
        return;
    }
    m_sessionActive = active;
    m_refusal.clear();
    m_sentThisSession = false;
    m_followUpId = 0;
    m_followUpOf = 0;
    Q_EMIT changed();
}

bool
RecordingStatus::sessionActive() const {
    return m_sessionActive;
}

void
RecordingStatus::setRunUp(bool up) {
    m_runUp = up;
}

bool
RecordingStatus::runUp() const {
    return m_sessionActive && m_runUp;
}

void
recording_status_follow_host(RecordingStatus* status, DecoderHost* host) {
    /* Whether this session's run said it is up: its decoder opened the queue and published its recording view. A host
       that signals it (sessionInitialized(), which can come while Starting) says so once per session; a session the
       host was already running when it first knew its state (the UI recreated over it) was up long before. A host
       that does not signal it is up when Running. */
    struct Seen {
        bool known = false;
        bool initialized = false;
    };

    const auto seen = std::make_shared<Seen>();
    const auto follow = [status, host, seen]() {
        const bool active = host->sessionActive();
        if (!seen->known && host->sessionStateKnown()) {
            seen->known = true;
            seen->initialized = active;
        }
        if (!active) {
            seen->initialized = false;
        }
        status->setSessionActive(active);
        status->setRunUp(host->sessionState() == DecoderHost::Running
                         && (!host->signalsSessionInitialized() || seen->initialized));
    };
    follow();
    QObject::connect(host, &DecoderHost::sessionStateChanged, status, follow);
    QObject::connect(host, &DecoderHost::sessionStateKnownChanged, status, follow);
    QObject::connect(host, &DecoderHost::sessionInitialized, status, [seen, follow]() {
        seen->initialized = true;
        follow();
    });
}

bool
RecordingStatus::published() const {
    return m_view.published != 0U;
}

bool
RecordingStatus::perCall() const {
    return m_view.per_call != 0U;
}

int
RecordingStatus::slotFiles() const {
    return m_view.slot_files;
}

bool
RecordingStatus::unknownOnly() const {
    return m_view.unknown_only != 0U;
}

bool
RecordingStatus::staticWav() const {
    return m_view.mode == DSD_APP_RECORDING_MODE_STATIC;
}

bool
RecordingStatus::storageNamed() const {
    return m_view.journal_named != 0U;
}

QString
RecordingStatus::journal() const {
    switch (m_view.journal_state) {
        case DSD_APP_RECORDING_JOURNAL_ON: return QStringLiteral("on");
        case DSD_APP_RECORDING_JOURNAL_FAILED: return QStringLiteral("failed");
        default: return QStringLiteral("none");
    }
}

QString
RecordingStatus::command() const {
    if (!m_sentThisSession) {
        return {};
    }
    const int id = m_followUpOf != 0 && m_view.command_id == m_followUpId ? m_followUpOf : m_view.command_id;
    switch (id) {
        case DSD_APP_CMD_WAV_START: return QStringLiteral("start");
        case DSD_APP_CMD_WAV_STOP: return QStringLiteral("stop");
        case DSD_APP_CMD_WAV_TOGGLE: return QStringLiteral("toggle");
        case DSD_APP_CMD_WAV_STATIC_OPEN: return QStringLiteral("static");
        case DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET: return QStringLiteral("unknownOnly");
        case DSD_APP_CMD_CONFIG_APPLY: return QStringLiteral("config");
        default: return {};
    }
}

QString
RecordingStatus::commandState() const {
    if (!m_sentThisSession) {
        return QStringLiteral("none");
    }
    switch (m_view.command_state) {
        case DSD_APP_RECORDING_COMMAND_PENDING: return QStringLiteral("pending");
        case DSD_APP_RECORDING_COMMAND_APPLIED: return QStringLiteral("applied");
        case DSD_APP_RECORDING_COMMAND_FAILED: return QStringLiteral("failed");
        case DSD_APP_RECORDING_COMMAND_REFUSED: return QStringLiteral("refused");
        case DSD_APP_RECORDING_COMMAND_DROPPED: return QStringLiteral("dropped");
        default: return QStringLiteral("none");
    }
}

int
RecordingStatus::failures() const {
    qint64 total = 0;
    for (int kind = 0; kind < DSD_WAV_RECORDING_ERROR_KINDS; kind++) {
        total += m_view.failures[kind];
    }
    return static_cast<int>(qMin<qint64>(total, 0x7fffffff));
}

QVariantMap
RecordingStatus::failureCounts() const {
    QVariantMap counts;
    for (int kind = DSD_WAV_RECORDING_ERROR_NONE + 1; kind < DSD_WAV_RECORDING_ERROR_KINDS; kind++) {
        counts.insert(QLatin1String(k_error_names[kind]), static_cast<qint64>(m_view.failures[kind]));
    }
    return counts;
}

QString
RecordingStatus::lastError() const {
    const int kind = m_view.last_error;
    return kind > DSD_WAV_RECORDING_ERROR_NONE && kind < DSD_WAV_RECORDING_ERROR_KINDS
               ? QLatin1String(k_error_names[kind])
               : QString();
}

QString
RecordingStatus::refusal() const {
    return m_refusal;
}

int
RecordingStatus::refusalCount() const {
    return m_refusalCount;
}

} // namespace dsd_qt
