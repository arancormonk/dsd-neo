// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "recording_status.h"

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
    if (reason == m_refusal) {
        return;
    }
    m_refusal = reason;
    Q_EMIT changed();
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
    switch (m_view.command_id) {
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

} // namespace dsd_qt
