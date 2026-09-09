// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "talkgroup_list_model.h"

#include <QByteArray>
#include <QDebug>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <algorithm>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <limits>
#include <string.h>
#include <utility>

namespace dsd_qt {

TalkgroupListModel::TalkgroupListModel(QAbstractItemModel* history, QObject* parent)
    : QAbstractListModel(parent), m_history(history) {
    if (history == nullptr) {
        return;
    }
    const auto roles = history->roleNames();
    m_tgRole = roles.key("tg", -1);
    m_whenRole = roles.key("when", -1);
    m_kindRole = roles.key("kind", -1);
    const auto dirty = [this]() { m_heardDirty = true; };
    connect(history, &QAbstractItemModel::rowsInserted, this, dirty);
    connect(history, &QAbstractItemModel::rowsRemoved, this, dirty);
    connect(history, &QAbstractItemModel::dataChanged, this, dirty);
    connect(history, &QAbstractItemModel::modelReset, this, dirty);
    connect(history, &QObject::destroyed, this, dirty);
}

int
TalkgroupListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant
TalkgroupListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count() || index.column() != 0) {
        return {};
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
        case IdStartRole: return QVariant::fromValue(static_cast<qulonglong>(row.idStart));
        case IdEndRole: return QVariant::fromValue(static_cast<qulonglong>(row.idEnd));
        case IdTextRole:
            return row.idStart == row.idEnd ? QString::number(row.idStart)
                                            : QStringLiteral("%1–%2").arg(row.idStart).arg(row.idEnd);
        case NameRole: return row.name;
        case TagsRole: return row.tags;
        case ListeningRole: return row.listening;
        case ListedRole: return row.listed;
        default: return {};
    }
}

QHash<int, QByteArray>
TalkgroupListModel::roleNames() const {
    return {{IdStartRole, "idStart"}, {IdEndRole, "idEnd"},         {IdTextRole, "idText"}, {NameRole, "name"},
            {TagsRole, "tags"},       {ListeningRole, "listening"}, {ListedRole, "listed"}};
}

void
TalkgroupListModel::setSinceWhen(qint64 when) {
    if (when == m_sinceWhen) {
        return;
    }
    m_sinceWhen = when;
    m_heardDirty = true;
    Q_EMIT sinceWhenChanged();
}

void
TalkgroupListModel::refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot) {
    if (opts_snapshot == nullptr || snapshot == nullptr) {
        clear();
        return;
    }
    const bool allowListedOnly = opts_snapshot->trunk_use_allow_list == 1;
    const bool allowListChanged = allowListedOnly != m_allowListMode;
    const bool hasGroupFile = opts_snapshot->group_in_file[0] != '\0';
    if (allowListChanged || hasGroupFile != m_persistent) {
        m_allowListMode = allowListedOnly;
        m_persistent = hasGroupFile;
        Q_EMIT policyChanged();
    }
    uint64_t contextId = 0;
    unsigned int generation = 0;
    dsd_tg_policy_table_version(snapshot, &contextId, &generation);
    if (contextId == m_contextId && generation == m_generation && !m_heardDirty && !allowListChanged) {
        return;
    }

    QSet<QString> categoryTags;
    QVector<Row> rows = listedRows(snapshot, categoryTags);
    appendHeardRows(rows, allowListedOnly);
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.idStart < b.idStart || (a.idStart == b.idStart && a.idEnd < b.idEnd);
    });
    replaceRows(std::move(rows));
    updateCategories(categoryTags);
    m_contextId = contextId;
    m_generation = generation;
    m_heardDirty = false;
}

QVector<TalkgroupListModel::Row>
TalkgroupListModel::listedRows(const dsd_state* snapshot, QSet<QString>& categoryTags) {
    QVector<Row> rows;
    const size_t entries = dsd_tg_policy_entry_count(snapshot);
    rows.reserve(static_cast<qsizetype>(entries));
    for (size_t i = 0; i < entries; ++i) {
        dsd_tg_policy_entry entry;
        if (!dsd_tg_policy_entry_at(snapshot, i, &entry) || entry.source == DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS
            || strcmp(entry.mode, "D") == 0) {
            continue;
        }
        rows.push_back({entry.id_start, entry.id_end, QString::fromUtf8(entry.name), QString::fromUtf8(entry.tags),
                        strcmp(entry.mode, "B") != 0 && strcmp(entry.mode, "DE") != 0, true});
        if (!rows.back().tags.isEmpty()) {
            categoryTags.insert(rows.back().tags);
        }
    }
    return rows;
}

void
TalkgroupListModel::appendHeardRows(QVector<Row>& rows, bool allowListedOnly) const {
    if (m_history == nullptr || m_tgRole < 0 || m_whenRole < 0 || m_kindRole < 0) {
        return;
    }
    const qsizetype listedCount = rows.size();
    QSet<uint32_t> heard;
    for (int i = 0; i < m_history->rowCount(); ++i) {
        const QModelIndex idx = m_history->index(i, 0);
        if (m_history->data(idx, m_kindRole).toInt() != 0
            || m_history->data(idx, m_whenRole).toLongLong() < m_sinceWhen) {
            continue;
        }
        const qulonglong target = m_history->data(idx, m_tgRole).toULongLong();
        if (target == 0 || target > std::numeric_limits<uint32_t>::max()) {
            continue;
        }
        const auto tg = static_cast<uint32_t>(target);
        if (heard.contains(tg)) {
            continue;
        }
        heard.insert(tg);
        const bool covered = std::any_of(rows.cbegin(), rows.cbegin() + listedCount,
                                         [tg](const Row& row) { return tg >= row.idStart && tg <= row.idEnd; });
        if (!covered) {
            rows.push_back({tg, tg, {}, {}, !allowListedOnly, false});
        }
    }
}

void
TalkgroupListModel::replaceRows(QVector<Row> rows) {
    const int oldCount = count();
    const int notTuned =
        static_cast<int>(std::count_if(rows.cbegin(), rows.cend(), [](const Row& row) { return !row.listening; }));
    const bool sameBounds = rows.size() == m_rows.size()
                            && std::equal(rows.cbegin(), rows.cend(), m_rows.cbegin(), [](const Row& a, const Row& b) {
                                   return a.idStart == b.idStart && a.idEnd == b.idEnd;
                               });
    if (sameBounds) {
        for (int i = 0; i < count(); ++i) {
            const Row& row = rows.at(i);
            Row& old = m_rows[i];
            if (old.name != row.name || old.tags != row.tags || old.listening != row.listening
                || old.listed != row.listed) {
                old = std::move(rows[i]);
                Q_EMIT dataChanged(index(i, 0), index(i, 0));
            }
        }
    } else {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }
    if (oldCount != count() || m_notTunedCount != notTuned) {
        m_notTunedCount = notTuned;
        Q_EMIT countChanged();
    }
}

void
TalkgroupListModel::updateCategories(const QSet<QString>& categoryTags) {
    QStringList updatedCategories = categoryTags.values();
    std::sort(updatedCategories.begin(), updatedCategories.end(), [](const QString& a, const QString& b) {
        const int order = QString::compare(a, b, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : a < b;
    });
    if (updatedCategories != m_categories) {
        m_categories = std::move(updatedCategories);
        Q_EMIT categoriesChanged();
    }
}

void
TalkgroupListModel::invalidateForTarget() {
    m_contextId = 0;
    m_generation = 0;
    m_heardDirty = true;
}

void
TalkgroupListModel::clear() {
    const bool hadCounts = !m_rows.isEmpty() || m_notTunedCount != 0;
    if (!m_rows.isEmpty()) {
        beginResetModel();
        m_rows.clear();
        endResetModel();
    }
    m_notTunedCount = 0;
    if (hadCounts) {
        Q_EMIT countChanged();
    }
    if (!m_categories.isEmpty()) {
        m_categories.clear();
        Q_EMIT categoriesChanged();
    }
    if (m_allowListMode || m_persistent) {
        m_allowListMode = false;
        m_persistent = false;
        Q_EMIT policyChanged();
    }
    m_contextId = 0;
    m_generation = 0;
    m_heardDirty = true;
}

} // namespace dsd_qt
