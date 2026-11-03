// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "scan_list_toggle_model.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QtGlobal>
#include <algorithm>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_list_rows.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/core/safe_api.h>
#include <iterator>
#include <string.h>
#include <utility>

namespace dsd_qt {

ScanListToggleModel::Backend
ScanListToggleModel::appControlBackend() {
    Backend backend;
    backend.requestNext = [] { return dsd_app_scan_list_request_next(); };
    backend.submit = [](const dsd_app_scan_list_set_payload& payload) {
        return dsd_app_command_scan_list_set(&payload);
    };
    backend.resultsSince = [](uint64_t after, dsd_app_scan_list_result* out, int cap) {
        return dsd_app_scan_list_results_since(after, out, cap);
    };
    backend.latestSequence = [] {
        dsd_app_scan_list_result latest{};
        return dsd_app_scan_list_result_get(&latest) ? latest.sequence : uint64_t{0};
    };
    return backend;
}

ScanListToggleModel::ScanListToggleModel(QObject* parent)
    : QAbstractListModel(parent), m_backend(appControlBackend()) {}

int
ScanListToggleModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QHash<int, QByteArray>
ScanListToggleModel::roleNames() const {
    return {{NumberRole, "number"}, {NameRole, "name"},       {OnRole, "on"},      {TargetsRole, "targets"},
            {OnAirRole, "onAir"},   {PendingRole, "pending"}, {NoteRole, "reason"}};
}

QVariant
ScanListToggleModel::data(const QModelIndex& index, int role) const {
    const int i = index.row();
    if (index.parent().isValid() || i < 0 || i >= count()) {
        return {};
    }
    const dsd_app_scan_list_entry& entry = m_view.lists[i];
    switch (role) {
        case NumberRole: return entry.number;
        case NameRole:
            return QString::fromUtf8(entry.name, static_cast<qsizetype>(strnlen(entry.name, sizeof entry.name)));
        case OnRole: return rowOn(i);
        case TargetsRole: return entry.targets;
        case OnAirRole: return entry.has_active != 0U;
        case PendingRole: return m_rows[i].request != 0U;
        case NoteRole: return noteText(m_rows[i]);
        default: return {};
    }
}

void
ScanListToggleModel::setBackend(Backend backend) {
    m_backend = std::move(backend);
    m_cursorSet = false;
}

/* While a switch waits, the state it asked for; then what it left the list in until the view shows its lists; then
   the view. */
bool
ScanListToggleModel::rowOn(int i) const {
    const dsd_app_scan_list_row& row = m_rows[i];
    if (row.request != 0U) {
        return row.want != 0U;
    }
    return dsd_app_scan_list_row_on(&row, &m_view, i) != 0;
}

QString
ScanListToggleModel::listName(int number) const {
    return data(index(number - 1), NameRole).toString();
}

int
ScanListToggleModel::listsOn() const {
    int on = 0;
    for (int i = 0; i < count(); ++i) {
        on += rowOn(i) ? 1 : 0;
    }
    return on;
}

int
ScanListToggleModel::targetsOn() const {
    int targets = 0;
    for (int i = 0; i < count(); ++i) {
        targets += rowOn(i) ? m_view.lists[i].targets : 0;
    }
    return targets;
}

QString
ScanListToggleModel::noteText(const dsd_app_scan_list_row& row) const {
    if (row.request != 0U || row.note == DSD_APP_SCAN_LIST_NOTE_NONE) {
        return {};
    }
    if (row.note == DSD_APP_SCAN_LIST_NOTE_NOT_SENT) {
        return tr("Not sent. Try again.");
    }
    switch (row.code) {
        case DSD_APP_SCAN_LIST_SET_OK:
        case DSD_APP_SCAN_LIST_SET_UNCHANGED: return {};
        case DSD_APP_SCAN_LIST_SET_REFUSED_LAST: return tr("Nothing else to scan. Switch on another list first.");
        case DSD_APP_SCAN_LIST_SET_BUSY: return tr("The scan was busy. Try again.");
        case DSD_APP_SCAN_LIST_SET_PAUSED: return tr("The scan is paused. Resume it first.");
        case DSD_APP_SCAN_LIST_SET_STALE: return tr("The scan changed. Nothing was switched.");
        case DSD_APP_SCAN_LIST_SET_UNAVAILABLE: return tr("The trunk scan is not running.");
        case DSD_APP_SCAN_LIST_SET_SEARCH: return tr("Stop Signal search first.");
        case DSD_APP_SCAN_LIST_SET_DROPPED: return tr("Not applied. Try again.");
        default: return tr("Not applied.");
    }
}

QList<QList<QVariant>>
ScanListToggleModel::shownRows() const {
    /* The sheet-wide readings first, then one entry per row. */
    QList<QList<QVariant>> rows{{switchable(), paused(), listsOn(), targetsOn()}};
    for (int i = 0; i < count(); ++i) {
        const QModelIndex at = index(i);
        QList<QVariant> shown;
        for (int role = NumberRole; role <= NoteRole; ++role) {
            shown << data(at, role);
        }
        rows << shown;
    }
    return rows;
}

void
ScanListToggleModel::announce(const QList<QList<QVariant>>& before, int beforeCount) {
    if (count() != beforeCount) {
        Q_EMIT countChanged();
    }
    const auto after = shownRows();
    for (int i = 0; i < count() && i + 1 < before.size(); ++i) {
        if (after.at(i + 1) != before.at(i + 1)) {
            Q_EMIT dataChanged(index(i), index(i));
        }
    }
    if (after.constFirst() != before.constFirst()) {
        Q_EMIT stateChanged();
    }
}

void
ScanListToggleModel::refresh(const dsd_app_scan_list_view& view) {
    const bool newSession = view.session != m_view.session;
    const bool reshaped = newSession || view.list_count != m_view.list_count;
    const auto before = shownRows();
    const int beforeCount = count();
    if (reshaped) {
        beginResetModel();
    }
    if (newSession) {
        /* A request made of the last session is never this one's: its rows and reasons go with it. */
        std::fill(std::begin(m_rows), std::end(m_rows), dsd_app_scan_list_row{});
        m_cursorSet = false;
    }
    m_view = view;
    if (!m_cursorSet && m_view.session != 0U && m_backend.latestSequence) {
        m_after = m_backend.latestSequence();
        m_cursorSet = true;
    }
    takeResults();
    dsd_app_scan_list_rows_expire(m_rows, count(), &m_view);
    if (reshaped) {
        endResetModel();
    }
    announce(before, beforeCount);
}

void
ScanListToggleModel::clear() {
    refresh(dsd_app_scan_list_view{});
}

bool
ScanListToggleModel::setEnabled(int number, bool on) {
    const int i = number - 1;
    if (i < 0 || i >= count() || !switchable() || m_rows[i].request != 0U || !m_backend.submit
        || !m_backend.requestNext) {
        return false;
    }
    const auto before = shownRows();
    dsd_app_scan_list_set_payload payload;
    DSD_MEMSET(&payload, 0, sizeof payload);
    payload.session = m_view.session;
    payload.request = m_backend.requestNext();
    payload.list = static_cast<int16_t>(number);
    payload.enabled = on ? 1U : 0U;
    const bool sent = m_backend.submit(payload) != DSD_APP_COMMAND_SUBMIT_REJECTED;
    if (sent) {
        dsd_app_scan_list_row_sent(&m_rows[i], payload.request, on ? 1 : 0, m_view.lists_seq);
    } else {
        /* Never queued, so no result will come: the row says so until the lists move or it is switched again. */
        dsd_app_scan_list_row_not_sent(&m_rows[i], m_view.lists_seq);
    }
    announce(before, count());
    return sent;
}

void
ScanListToggleModel::takeResults() {
    if (!m_cursorSet || !m_backend.resultsSince) {
        return;
    }
    const auto read = [](void* ctx, uint64_t after, dsd_app_scan_list_result* out, int cap) {
        return static_cast<const Backend*>(ctx)->resultsSince(after, out, cap);
    };
    dsd_app_scan_list_rows_read(m_rows, count(), count(), &m_after, read, &m_backend);
}

} // namespace dsd_qt
