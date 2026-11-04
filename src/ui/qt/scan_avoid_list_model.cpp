// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "scan_avoid_list_model.h"

#include <QByteArray>
#include <QLatin1Char>
#include <QMap>
#include <QSet>
#include <QVariant>
#include <QtGlobal>
#include <algorithm>
#include <cstring>
#include <dsd-neo/app_control/commands.h>
#include <utility>

namespace dsd_qt {

ScanAvoidListModel::ScanAvoidListModel(QObject* parent) : QAbstractListModel(parent) {}

int
ScanAvoidListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant
ScanAvoidListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Entry& row = m_rows.at(index.row());
    if (role == KeyRole) {
        return row.key;
    }
    if (role == LabelRole || role == Qt::DisplayRole) {
        return row.label;
    }
    return {};
}

QHash<int, QByteArray>
ScanAvoidListModel::roleNames() const {
    return {{KeyRole, "key"}, {LabelRole, "label"}};
}

QString
ScanAvoidListModel::fieldText(const char* field, size_t size) {
    const void* end = std::memchr(field, '\0', size);
    const size_t length = end != nullptr ? static_cast<size_t>(static_cast<const char*>(end) - field) : size;
    return QString::fromUtf8(field, static_cast<qsizetype>(length));
}

QString
ScanAvoidListModel::keyOf(const dsd_app_scan_avoid_remove_payload& identity) {
    if (identity.kind == DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN) {
        // Concatenated, not arg()'d: an id may hold "%1".
        const QString id = fieldText(identity.target_id, sizeof identity.target_id);
        return QStringLiteral("trunk:") + QString::number(identity.session) + QLatin1Char(':') + id;
    }
    return QStringLiteral("lcn:%1:%2:%3:%4")
        .arg(identity.instance)
        .arg(identity.list_rev)
        .arg(identity.row)
        .arg(identity.frequency_hz);
}

/* From the end, so each run's rows keep their positions until it is removed: one remove per run of rows whose key
   left. */
void
ScanAvoidListModel::removeLeftRows(const QHash<QString, qsizetype>& nextIndex) {
    qsizetype end = m_rows.size();
    while (end > 0) {
        if (nextIndex.contains(m_rows.at(end - 1).key)) {
            --end;
            continue;
        }
        qsizetype first = end - 1;
        while (first > 0 && !nextIndex.contains(m_rows.at(first - 1).key)) {
            --first;
        }
        beginRemoveRows(QModelIndex(), static_cast<int>(first), static_cast<int>(end - 1));
        m_rows.remove(first, end - first);
        endRemoveRows();
        end = first;
    }
}

void
ScanAvoidListModel::takeEntry(qsizetype pos, const Entry& next) {
    Entry& row = m_rows[pos];
    row.identity = next.identity;
    if (row.label != next.label) {
        row.label = next.label;
        const QModelIndex at = index(static_cast<int>(pos));
        Q_EMIT dataChanged(at, at, {LabelRole});
    }
}

/* A run of new keys from @p pos, inserted in one step; returns where the run ends. */
qsizetype
ScanAvoidListModel::insertNewRun(const QList<Entry>& next, qsizetype pos, const QSet<QString>& kept) {
    qsizetype end = pos + 1;
    while (end < next.size() && !kept.contains(next.at(end).key)) {
        ++end;
    }
    beginInsertRows(QModelIndex(), static_cast<int>(pos), static_cast<int>(end - 1));
    QList<Entry> tail = m_rows.mid(pos);
    m_rows.resize(pos);
    m_rows.append(next.mid(pos, end - pos));
    m_rows.append(std::move(tail));
    endInsertRows();
    return end;
}

/* A kept row further down belongs at @p pos (the target list was reordered): move it up with the rows that follow it
   in both lists. The rows before @p pos already match, so it is found after @p pos. */
void
ScanAvoidListModel::moveKeptRun(const QList<Entry>& next, qsizetype pos) {
    qsizetype from = pos + 1;
    while (m_rows.at(from).key != next.at(pos).key) {
        ++from;
    }
    qsizetype run = 1;
    while (from + run < m_rows.size() && pos + run < next.size()
           && m_rows.at(from + run).key == next.at(pos + run).key) {
        ++run;
    }
    beginMoveRows(QModelIndex(), static_cast<int>(from), static_cast<int>(from + run - 1), QModelIndex(),
                  static_cast<int>(pos));
    std::rotate(m_rows.begin() + pos, m_rows.begin() + from, m_rows.begin() + from + run);
    endMoveRows();
}

void
ScanAvoidListModel::apply(QList<Entry> next) {
    QHash<QString, qsizetype> nextIndex;
    nextIndex.reserve(next.size());
    for (qsizetype i = 0; i < next.size(); ++i) {
        nextIndex.insert(next.at(i).key, i);
    }
    if (nextIndex.size() != next.size()) {
        // Two entries with one key break the walk below; the contract rules it out, so start over rather than guess.
        beginResetModel();
        m_rows = std::move(next);
        endResetModel();
        reindex();
        return;
    }
    removeLeftRows(nextIndex);
    // Every row left has a key of next. Walk next and bring each position in line with it.
    QSet<QString> kept;
    kept.reserve(m_rows.size());
    for (const Entry& row : m_rows) {
        kept.insert(row.key);
    }
    qsizetype pos = 0;
    while (pos < next.size()) {
        if (pos < m_rows.size() && m_rows.at(pos).key == next.at(pos).key) {
            takeEntry(pos, next.at(pos));
            ++pos;
        } else if (!kept.contains(next.at(pos).key)) {
            pos = insertNewRun(next, pos, kept);
        } else {
            // The moved rows now match; the next turn takes their labels.
            moveKeptRun(next, pos);
        }
    }
    reindex();
}

void
ScanAvoidListModel::clear() {
    apply({});
}

void
ScanAvoidListModel::reindex() {
    m_index.clear();
    m_index.reserve(m_rows.size());
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        m_index.insert(m_rows.at(i).key, i);
    }
}

QVariantMap
ScanAvoidListModel::entry(const QString& key) const {
    const auto found = m_index.constFind(key);
    if (found == m_index.constEnd()) {
        return {};
    }
    const Entry& row = m_rows.at(found.value());
    const dsd_app_scan_avoid_remove_payload& id = row.identity;
    return {
        {"key", row.key},
        {"label", row.label},
        {"kind", static_cast<int>(id.kind)},
        {"instance", QString::number(id.instance)},
        {"listRev", QString::number(id.list_rev)},
        {"frequencyHz", QString::number(id.frequency_hz)},
        {"row", static_cast<int>(id.row)},
        {"session", static_cast<qulonglong>(id.session)},
        {"targetId", fieldText(id.target_id, sizeof id.target_id)},
    };
}

} // namespace dsd_qt
