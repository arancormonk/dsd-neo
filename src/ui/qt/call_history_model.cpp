// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "call_history_model.h"

#include <algorithm>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>

#include <dsd-neo/core/state.h>

#include "json_store.h"

namespace dsd_qt {

namespace {

constexpr const char kStoreFileName[] = "call_history.json";
constexpr const char kSessionLabelKey[] = "callHistory/sessionLabel";
constexpr const char kClearedThroughKey[] = "callHistory/clearedThrough";
constexpr int kMaxRows = 1000;
/* Sanity bound on the ring's own start/end stamps: a span longer than this is a
 * corrupt or clock-shifted row, not a measured call, so it renders as unknown. */
constexpr qint64 kMaxPlausibleDurationSecs = 3600;
constexpr int kSaveDelayMs = 3000;
/* Trunk-following mints a committed row per tune attempt, so one keyed-up
 * talkgroup can shed several near-simultaneous fragments. Within this window a
 * same-target row is the same conversation, not a new call. */
constexpr qint64 kMergeWindowSecs = 20;

} // namespace

CallHistoryModel::CallHistoryModel(QObject* parent) : QAbstractListModel(parent) {
    /* Restored before the first ingest: after an Activity restart the service's
     * session is still decoding, and its backlog lands before any start button is
     * pressed. Without the persisted label those rows would be attributed to "". */
    m_sessionLabel = m_settings.value(QLatin1String(kSessionLabelKey)).toString();
    /* Restored for the same reason: Clear must survive an Activity restart while
     * the service's ring still holds the cleared rows. */
    m_clearedThrough = m_settings.value(QLatin1String(kClearedThroughKey)).toLongLong();
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, [this]() { saveNow(); });
    load();
}

CallHistoryModel::~CallHistoryModel() {
    if (m_saveTimer.isActive()) {
        saveNow();
    }
}

int
CallHistoryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant
CallHistoryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
        case NameRole: return row.name;
        case TgRole: return row.tg;
        case SrcRole: return row.src;
        case EncRole: return row.enc;
        case WhenRole: return row.when;
        case DurationSecsRole: return row.durationSecs;
        case SystemNameRole: return row.systemName;
        case DayLabelRole: {
            const QDate day = QDateTime::fromSecsSinceEpoch(row.when).date();
            const QDate today = QDate::currentDate();
            if (day == today) {
                return QStringLiteral("TODAY");
            }
            if (day == today.addDays(-1)) {
                return QStringLiteral("YESTERDAY");
            }
            return QLocale().toString(day, QStringLiteral("ddd d MMM")).toUpper();
        }
        case TimeTextRole: return QDateTime::fromSecsSinceEpoch(row.when).toString(QStringLiteral("HH:mm"));
        default: return QVariant();
    }
}

QHash<int, QByteArray>
CallHistoryModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles.insert(NameRole, QByteArrayLiteral("name"));
    roles.insert(TgRole, QByteArrayLiteral("tg"));
    roles.insert(SrcRole, QByteArrayLiteral("src"));
    roles.insert(EncRole, QByteArrayLiteral("enc"));
    roles.insert(WhenRole, QByteArrayLiteral("when"));
    roles.insert(DurationSecsRole, QByteArrayLiteral("durationSecs"));
    roles.insert(SystemNameRole, QByteArrayLiteral("systemName"));
    roles.insert(DayLabelRole, QByteArrayLiteral("dayLabel"));
    roles.insert(TimeTextRole, QByteArrayLiteral("timeText"));
    return roles;
}

void
CallHistoryModel::setSessionLabel(const QString& label) {
    if (label == m_sessionLabel) {
        return;
    }
    m_sessionLabel = label;
    m_settings.setValue(QLatin1String(kSessionLabelKey), label);
    Q_EMIT sessionLabelChanged();
}

QStringList
CallHistoryModel::systemLabels() const {
    QStringList labels;
    for (const Row& row : m_rows) {
        if (!row.systemName.isEmpty() && !labels.contains(row.systemName)) {
            labels.append(row.systemName);
        }
    }
    return labels;
}

namespace {

/** @brief One committed ring item as a display row. */
CallHistoryModel::Row
row_from_item(const Event_History* item, const QString& sessionLabel) {
    CallHistoryModel::Row row;
    row.tg = static_cast<qulonglong>(item->target_id);
    row.src = static_cast<qulonglong>(item->source_id);
    row.enc = item->enc != 0U;
    if (item->t_name[0] != '\0') {
        row.name = QString::fromUtf8(item->t_name);
    } else if (item->tgt_str[0] != '\0') {
        row.name = QString::fromUtf8(item->tgt_str);
    } else {
        row.name = QStringLiteral("Talkgroup %1").arg(row.tg);
    }
    row.systemName = sessionLabel;
    /* The ring stamps both ends of the transmission: event_start_time when the
     * epoch began, event_time as its last render (its end, once committed). Their
     * difference is the measured duration — never this process's ingest lag, which
     * on Android can be most of an hour when the service outlives the Activity. */
    const qint64 start = static_cast<qint64>(item->event_start_time);
    const qint64 end = static_cast<qint64>(item->event_time);
    row.when = (start > 0) ? start : end;
    if (start > 0 && end >= start && end - start <= kMaxPlausibleDurationSecs) {
        row.durationSecs = static_cast<int>(end - start);
    }
    return row;
}

} // namespace

QList<CallHistoryModel::Row>
CallHistoryModel::collectFresh(const dsd_state* snapshot) {
    QList<Row> fresh;
    for (int slot = 0; slot < 2; slot++) {
        // Index 0 is the still-active staged row; only committed rows (1..254) are
        // finished calls that belong in a log.
        for (int idx = 1; idx < 255; idx++) {
            const Event_History* item = &snapshot->event_history_s[slot].Event_History_Items[idx];
            if (item->category != DSD_EVENT_CATEGORY_VOICE) {
                continue;
            }
            if (item->target_id == 0U && item->tgt_str[0] == '\0') {
                continue;
            }
            // Key from the raw fields before building the Row: rescans are frequent
            // (any staged-row render bumps the revision) and almost every committed
            // row is already seen, so the common path must not allocate row strings.
            const qint64 start = static_cast<qint64>(item->event_start_time);
            const qint64 end = static_cast<qint64>(item->event_time);
            const qint64 when = (start > 0) ? start : end;
            if (when <= m_clearedThrough) {
                // Cleared by the user; the ring still holds the row (and will until
                // the session ends), so it must stay invisible even after m_seen is
                // rebuilt by a relaunched UI.
                continue;
            }
            const QString key = QStringLiteral("%1|%2").arg(when).arg(item->target_id);
            if (m_seen.contains(key)) {
                continue;
            }
            m_seen.insert(key);
            fresh.append(row_from_item(item, m_sessionLabel));
        }
    }
    return fresh;
}

int
CallHistoryModel::tryMerge(const Row& row) {
    for (int i = 0; i < m_rows.size() && i < 32; i++) {
        Row& existing = m_rows[i];
        // A zero source id is "not yet learned", not a distinct unit: late-entry
        // fragments commit before the ring learns the src, and refusing to absorb
        // them would leave one conversation split across two rows.
        const bool srcCompatible = existing.src == row.src || existing.src == 0 || row.src == 0;
        if (existing.tg != row.tg || !srcCompatible || existing.systemName != row.systemName) {
            continue;
        }
        // The window extends off each fragment's end, not its start: one keyed-up
        // talkgroup sheds a fragment per retune, and a fixed window off the first
        // start would leak a duplicate row every window-length of activity.
        const qint64 existingEnd = existing.when + qMax(existing.durationSecs, 0);
        const qint64 rowEnd = row.when + qMax(row.durationSecs, 0);
        if (row.when > existingEnd + kMergeWindowSecs || existing.when > rowEnd + kMergeWindowSecs) {
            continue;
        }
        const qint64 start = qMin(existing.when, row.when);
        const qint64 span = qMax(existingEnd, rowEnd) - start;
        existing.when = start;
        if ((existing.durationSecs >= 0 || row.durationSecs >= 0) && span <= kMaxPlausibleDurationSecs) {
            existing.durationSecs = static_cast<int>(span);
        }
        existing.enc = existing.enc || row.enc;
        if (existing.src == 0) {
            existing.src = row.src;
        }
        return i;
    }
    return -1;
}

QString
CallHistoryModel::keyFor(const Row& row) {
    // Content-derived so it survives persistence: the Android service outlives the
    // Activity, and a relaunched UI must not re-ingest rows it already logged.
    // Deliberately excludes the source id: the ring fills a committed row's src in
    // place when a reacquisition merge learns it (0 -> real id), and a src-bearing
    // key would re-ingest that same row as a permanent duplicate.
    return QStringLiteral("%1|%2").arg(row.when).arg(row.tg);
}

bool
CallHistoryModel::ingestRow(const Row& row) {
    // Coalesce fragments into the call they belong to, with granular model
    // signals: a merge is a dataChanged on the absorbing row, a new call inserts
    // at its sorted (newest-first) position. Never a reset — delegates and the
    // reader's scroll position survive every ingest.
    static const QVector<int> mergeRoles = {WhenRole, SrcRole, EncRole, DurationSecsRole, DayLabelRole, TimeTextRole};
    const int merged = tryMerge(row);
    if (merged >= 0) {
        const QModelIndex idx = index(merged);
        Q_EMIT dataChanged(idx, idx, mergeRoles);
        // A merge can only pull the absorbing row's start earlier, which may
        // now sort below newer rows beneath it; restore the newest-first
        // invariant the insertion scan and day sections depend on.
        int newPos = merged;
        while (newPos + 1 < m_rows.size() && m_rows.at(newPos + 1).when > m_rows.at(merged).when) {
            newPos++;
        }
        if (newPos != merged) {
            beginMoveRows(QModelIndex(), merged, merged, QModelIndex(), newPos + 1);
            m_rows.move(merged, newPos);
            endMoveRows();
        }
        return false;
    }
    int pos = 0;
    while (pos < m_rows.size() && m_rows.at(pos).when > row.when) {
        pos++;
    }
    beginInsertRows(QModelIndex(), pos, pos);
    m_rows.insert(pos, row);
    endInsertRows();
    return true;
}

void
CallHistoryModel::refresh(const dsd_state* snapshot) {
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

    const QList<Row> fresh = collectFresh(snapshot);

    if (fresh.isEmpty()) {
        return;
    }

    bool rowsChanged = false;
    for (const Row& row : fresh) {
        rowsChanged = ingestRow(row) || rowsChanged;
    }
    while (m_rows.size() > kMaxRows) {
        const int last = static_cast<int>(m_rows.size()) - 1;
        beginRemoveRows(QModelIndex(), last, last);
        m_seen.remove(keyFor(m_rows.last()));
        m_rows.removeLast();
        endRemoveRows();
        rowsChanged = true;
    }
    if (rowsChanged) {
        Q_EMIT countChanged();
    }
    scheduleSave();
}

void
CallHistoryModel::clearAll() {
    beginResetModel();
    m_rows.clear();
    // m_seen deliberately survives: the ring still holds the rows just cleared, and
    // forgetting the keys would let the next tick re-ingest every one of them.
    // m_seen alone is not enough, though — it is in-memory only, and a relaunched
    // UI rebuilds it from the (now empty) persisted log while the service's ring
    // still holds every cleared row. The persisted watermark is what keeps a clear
    // effective across an Activity restart.
    m_clearedThrough = QDateTime::currentSecsSinceEpoch();
    m_settings.setValue(QLatin1String(kClearedThroughKey), m_clearedThrough);
    endResetModel();
    Q_EMIT countChanged();
    saveNow();
}

void
CallHistoryModel::load() {
    QList<Row> rows;
    const QJsonArray array = json_store_load_array(QLatin1String(kStoreFileName));
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        Row row;
        row.when = obj.value(QLatin1String("when")).toVariant().toLongLong();
        row.name = obj.value(QLatin1String("name")).toString();
        row.tg = obj.value(QLatin1String("tg")).toVariant().toULongLong();
        row.src = obj.value(QLatin1String("src")).toVariant().toULongLong();
        row.enc = obj.value(QLatin1String("enc")).toBool();
        row.durationSecs = obj.value(QLatin1String("durationSecs")).toInt(-1);
        row.systemName = obj.value(QLatin1String("systemName")).toString();
        rows.append(row);
    }
    beginResetModel();
    m_rows.clear();
    // Oldest first through the same merge the ingest path uses, so a log written
    // before fragment-coalescing existed collapses on its first load.
    for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
        m_seen.insert(keyFor(*it));
        if (tryMerge(*it) < 0) {
            m_rows.prepend(*it);
        }
    }
    std::stable_sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.when > b.when; });
    endResetModel();
    Q_EMIT countChanged();
}

void
CallHistoryModel::scheduleSave() {
    if (!m_saveTimer.isActive()) {
        m_saveTimer.start();
    }
}

void
CallHistoryModel::saveNow() const {
    QJsonArray array;
    for (const Row& row : m_rows) {
        QJsonObject obj;
        obj.insert(QLatin1String("when"), row.when);
        obj.insert(QLatin1String("name"), row.name);
        obj.insert(QLatin1String("tg"), static_cast<qint64>(row.tg));
        obj.insert(QLatin1String("src"), static_cast<qint64>(row.src));
        obj.insert(QLatin1String("enc"), row.enc);
        obj.insert(QLatin1String("durationSecs"), row.durationSecs);
        obj.insert(QLatin1String("systemName"), row.systemName);
        array.append(obj);
    }
    json_store_save_array(QLatin1String(kStoreFileName), array);
}

} // namespace dsd_qt
