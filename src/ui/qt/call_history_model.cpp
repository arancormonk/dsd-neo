// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "call_history_model.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>

#include <dsd-neo/core/state.h>

namespace dsd_qt {

namespace {

constexpr const char kStoreFileName[] = "call_history.json";
constexpr int kMaxRows = 1000;
/* A "duration" longer than this is really "the row predates this UI process", not a
 * measured call, so it renders as unknown instead. */
constexpr qint64 kMaxPlausibleDurationSecs = 3600;
constexpr int kSaveDelayMs = 3000;

} // namespace

CallHistoryModel::CallHistoryModel(QObject* parent) : QAbstractListModel(parent) {
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
    return parent.isValid() ? 0 : static_cast<int>(m_visible.size());
}

QVariant
CallHistoryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size()) {
        return QVariant();
    }
    const Row& row = m_rows.at(m_visible.at(index.row()));
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
    Q_EMIT sessionLabelChanged();
}

void
CallHistoryModel::setFilterText(const QString& text) {
    if (text == m_filterText) {
        return;
    }
    m_filterText = text;
    Q_EMIT filterChanged();
    beginResetModel();
    rebuildVisible();
    endResetModel();
    Q_EMIT countChanged();
}

void
CallHistoryModel::setFilterSystem(const QString& system) {
    if (system == m_filterSystem) {
        return;
    }
    m_filterSystem = system;
    Q_EMIT filterChanged();
    beginResetModel();
    rebuildVisible();
    endResetModel();
    Q_EMIT countChanged();
}

void
CallHistoryModel::setFilterKind(int kind) {
    if (kind == m_filterKind) {
        return;
    }
    m_filterKind = kind;
    Q_EMIT filterChanged();
    beginResetModel();
    rebuildVisible();
    endResetModel();
    Q_EMIT countChanged();
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

QString
CallHistoryModel::keyFor(const Row& row) {
    // Content-derived so it survives persistence: the Android service outlives the
    // Activity, and a relaunched UI must not re-ingest rows it already logged.
    return QStringLiteral("%1|%2|%3").arg(row.when).arg(row.tg).arg(row.src);
}

bool
CallHistoryModel::rowVisible(const Row& row) const {
    if (!m_filterSystem.isEmpty() && row.systemName != m_filterSystem) {
        return false;
    }
    if ((m_filterKind == 1 && row.enc) || (m_filterKind == 2 && !row.enc)) {
        return false;
    }
    if (m_filterText.isEmpty()) {
        return true;
    }
    return row.name.contains(m_filterText, Qt::CaseInsensitive) || QString::number(row.tg).contains(m_filterText)
           || QString::number(row.src).contains(m_filterText);
}

void
CallHistoryModel::rebuildVisible() {
    m_visible.clear();
    m_visible.reserve(m_rows.size());
    for (int i = 0; i < m_rows.size(); i++) {
        if (rowVisible(m_rows.at(i))) {
            m_visible.append(i);
        }
    }
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

    const qint64 now = QDateTime::currentSecsSinceEpoch();
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
            Row row;
            row.when = static_cast<qint64>(item->event_time);
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
            row.systemName = m_sessionLabel;
            const QString key = keyFor(row);
            if (m_seen.contains(key)) {
                continue;
            }
            // The ring pushes a row when the call finishes, and this tick is at most
            // one poll behind that, so "now minus start" approximates the duration.
            // Rows that were already in the ring when this process attached read as
            // hours long and render as unknown instead.
            const qint64 elapsed = now - row.when;
            row.durationSecs = (elapsed >= 0 && elapsed <= kMaxPlausibleDurationSecs) ? static_cast<int>(elapsed) : -1;
            m_seen.insert(key);
            fresh.append(row);
        }
    }

    if (fresh.isEmpty()) {
        return;
    }

    beginResetModel();
    m_rows.append(fresh);
    std::stable_sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.when > b.when; });
    while (m_rows.size() > kMaxRows) {
        m_seen.remove(keyFor(m_rows.last()));
        m_rows.removeLast();
    }
    rebuildVisible();
    endResetModel();
    Q_EMIT countChanged();
    scheduleSave();
}

void
CallHistoryModel::clearAll() {
    beginResetModel();
    m_rows.clear();
    m_visible.clear();
    // m_seen deliberately survives: the ring still holds the rows just cleared, and
    // forgetting the keys would let the next tick re-ingest every one of them.
    endResetModel();
    Q_EMIT countChanged();
    saveNow();
}

QString
CallHistoryModel::storePath() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QLatin1Char('/') + QLatin1String(kStoreFileName);
}

void
CallHistoryModel::load() {
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        return;
    }
    QList<Row> rows;
    const QJsonArray array = doc.array();
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
    m_rows = rows;
    for (const Row& row : m_rows) {
        m_seen.insert(keyFor(row));
    }
    rebuildVisible();
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
    const QString path = storePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
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
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
    (void)file.commit();
}

} // namespace dsd_qt
