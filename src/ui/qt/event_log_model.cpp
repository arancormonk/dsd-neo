// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "event_log_model.h"

#include <algorithm>

#include <dsd-neo/app_control/history.h>
#include <dsd-neo/core/state.h>

namespace dsd_qt {

namespace {

constexpr int kMaxRows = 200;
constexpr size_t kTextCapacity = 512U;

bool
item_has_content(const Event_History* item) {
    if (item == nullptr) {
        return false;
    }
    return item->event_string[0] != '\0' || item->text_message[0] != '\0' || item->alias[0] != '\0'
           || item->gps_s[0] != '\0' || item->internal_str[0] != '\0';
}

struct Ref {
    const Event_History* item;
    time_t sort_time;
    int slot;
};

} // namespace

EventLogModel::EventLogModel(QObject* parent) : QAbstractListModel(parent) {}

EventLogModel::~EventLogModel() = default;

int
EventLogModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant
EventLogModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
        case TextRole: return row.text;
        case SlotRole: return row.slot;
        case SeverityRole: return row.severity;
        default: return QVariant();
    }
}

QHash<int, QByteArray>
EventLogModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles.insert(TextRole, QByteArrayLiteral("text"));
    roles.insert(SlotRole, QByteArrayLiteral("slot"));
    roles.insert(SeverityRole, QByteArrayLiteral("severity"));
    return roles;
}

void
EventLogModel::refresh(const dsd_state* snapshot) {
    if (snapshot == nullptr || snapshot->event_history_s == nullptr) {
        return;
    }

    const quint64 revision[2] = {static_cast<quint64>(snapshot->event_history_s[0].revision),
                                 static_cast<quint64>(snapshot->event_history_s[1].revision)};
    if (m_seeded && revision[0] == m_revision[0] && revision[1] == m_revision[1]) {
        return;
    }
    m_revision[0] = revision[0];
    m_revision[1] = revision[1];
    m_seeded = true;

    QList<Ref> refs;
    refs.reserve(2 * 255);
    for (int slot = 0; slot < 2; slot++) {
        for (int idx = 1; idx < 255; idx++) {
            const Event_History* item = &snapshot->event_history_s[slot].Event_History_Items[idx];
            if (!item_has_content(item)) {
                continue;
            }
            Ref ref;
            ref.item = item;
            ref.sort_time = dsd_app_frontend_history_event_sort_time(item->event_string, item->event_time);
            ref.slot = slot;
            refs.append(ref);
        }
    }

    // Newest first: the list view shows the tail of a running system, not its history.
    std::stable_sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) { return a.sort_time > b.sort_time; });
    if (refs.size() > kMaxRows) {
        refs.resize(kMaxRows);
    }

    const int mode = dsd_app_frontend_history_get_mode();
    QList<Row> rows;
    rows.reserve(refs.size());
    for (const Ref& ref : refs) {
        char text[kTextCapacity];
        text[0] = '\0';
        (void)dsd_app_frontend_history_compact_event_text(text, sizeof text, ref.item->event_string, mode);

        Row row;
        row.text = QString::fromUtf8(text);
        if (row.text.isEmpty() && ref.item->internal_str[0] != '\0') {
            row.text = QString::fromUtf8(ref.item->internal_str);
        }
        row.slot = ref.slot;
        row.severity = ref.item->severity;
        rows.append(row);
    }

    beginResetModel();
    m_rows = rows;
    endResetModel();
    Q_EMIT countChanged();
}

void
EventLogModel::clear() {
    if (m_rows.isEmpty()) {
        return;
    }
    beginResetModel();
    m_rows.clear();
    endResetModel();
    Q_EMIT countChanged();
}

} // namespace dsd_qt
