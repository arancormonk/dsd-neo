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
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
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
    if (!index.isValid() || static_cast<unsigned int>(index.row()) >= static_cast<unsigned int>(count())
        || index.column() != 0) {
        return {};
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
        case IdStartRole: return QVariant::fromValue(static_cast<qulonglong>(row.idStart));
        case IdEndRole: return QVariant::fromValue(static_cast<qulonglong>(row.idEnd));
        case IdTextRole: return row.idText();
        case NameRole: return row.name;
        case TagsRole: return row.tags;
        case ListeningRole: return row.listening;
        case ListedRole: return row.listed;
        case PriorityRole: return row.priority;
        case PreemptRole: return row.preempt;
        case PolicyIndexRole: return row.policyIndex;
        case TemporaryAvoidCountRole: return QVariant::fromValue(row.temporaryAvoidCount);
        default: return {};
    }
}

QHash<int, QByteArray>
TalkgroupListModel::roleNames() const {
    return {{IdStartRole, "idStart"},
            {IdEndRole, "idEnd"},
            {IdTextRole, "idText"},
            {NameRole, "name"},
            {TagsRole, "tags"},
            {ListeningRole, "listening"},
            {ListedRole, "listed"},
            {PriorityRole, "priority"},
            {PreemptRole, "preempt"},
            {PolicyIndexRole, "policyIndex"},
            {TemporaryAvoidCountRole, "temporaryAvoidCount"}};
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
TalkgroupListModel::observeDecryption(const dsd_state* snapshot) {
    for (uint8_t slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; ++slot) {
        dsd_call_snapshot call = {};
        if (dsd_call_state_get(snapshot, slot, &call) > 0 && call.kind == DSD_CALL_KIND_GROUP_VOICE
            && call.ota_target_id && call.ota_target_id <= UINT32_MAX) {
            const bool dmr = DSD_SYNC_IS_DMR(call.protocol);
            m_observedCrypto.insert(
                static_cast<quint32>(call.ota_target_id),
                {{"algorithm", call.crypto >= DSD_CALL_CRYPTO_ENCRYPTED_PENDING
                                   ? QString::number(call.algid, 16).toUpper()
                                   : QString()},
                 {"crypto", static_cast<int>(call.crypto)},
                 {"keyId", call.crypto >= DSD_CALL_CRYPTO_ENCRYPTED_PENDING && (!dmr || call.kid <= 255)
                               ? QString::number(call.kid, 16).toUpper()
                               : QString()},
                 {"dmr", dmr},
                 {"profileRef", QString::fromUtf8(call.key_selection.profile_ref)},
                 {"lastObserved", true}});
        }
    }
}

void
TalkgroupListModel::updatePersistence(const dsd_opts* opts_snapshot, const dsd_state* snapshot, bool allowListChanged,
                                      bool allowListedOnly) {
    const bool hasGroupFile = opts_snapshot->group_in_file[0] != '\0';
    const QString scope = (dsd_scan_mode_option_fields(snapshot) & DSD_SCAN_OPT_GROUP) ? QStringLiteral("scan")
                          : hasGroupFile                                               ? QStringLiteral("file")
                                                                                       : QStringLiteral("session");
    if (scope != m_persistenceScope) {
        m_persistenceScope = scope;
        Q_EMIT policyChanged();
    }
    if (allowListChanged || hasGroupFile != m_persistent) {
        m_allowListMode = allowListedOnly;
        m_persistent = hasGroupFile;
        Q_EMIT policyChanged();
    }
}

void
TalkgroupListModel::refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot) {
    if (opts_snapshot == nullptr || snapshot == nullptr) {
        clear();
        return;
    }
    const bool allowListedOnly = opts_snapshot->trunk_use_allow_list == 1;
    const bool allowListChanged = allowListedOnly != m_allowListMode;
    updatePersistence(opts_snapshot, snapshot, allowListChanged, allowListedOnly);
    uint64_t contextId = 0;
    unsigned int generation = 0;
    dsd_tg_policy_table_version(snapshot, &contextId, &generation);
    if (contextId != m_contextId) {
        m_observedCrypto.clear();
    }
    observeDecryption(snapshot);
    if (contextId == m_contextId && generation == m_generation && !m_heardDirty && !allowListChanged) {
        return;
    }

    const bool versionChanged = contextId != m_contextId || generation != m_generation;
    m_contextId = contextId;
    m_generation = generation;
    if (versionChanged) {
        Q_EMIT policyChanged();
    }
    QSet<QString> categoryTags;
    QVector<Row> rows = listedRows(snapshot, categoryTags);
    appendHeardRows(rows, allowListedOnly);
    for (Row& row : rows) {
        row.temporaryAvoidCount = dsd_tg_policy_session_avoid_count(snapshot, row.idStart, row.idEnd);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.idStart < b.idStart || (a.idStart == b.idStart && a.idEnd < b.idEnd);
    });
    replaceRows(std::move(rows));
    updateCategories(categoryTags);
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
                        strcmp(entry.mode, "B") != 0 && strcmp(entry.mode, "DE") != 0, true, entry.priority,
                        entry.preempt != 0, static_cast<int>(i)});
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
                || old.listed != row.listed || old.priority != row.priority || old.preempt != row.preempt
                || old.policyIndex != row.policyIndex || old.temporaryAvoidCount != row.temporaryAvoidCount) {
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
    m_observedCrypto.clear();
    m_contextId = 0;
    m_generation = 0;
    m_heardDirty = true;
}

void
TalkgroupListModel::clear() {
    m_observedCrypto.clear();
    m_persistenceScope = QStringLiteral("session");
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
    const bool versionChanged = m_contextId != 0 || m_generation != 0;
    m_contextId = 0;
    m_generation = 0;
    if (m_allowListMode || m_persistent || versionChanged) {
        m_allowListMode = false;
        m_persistent = false;
        Q_EMIT policyChanged();
    }
    m_heardDirty = true;
}

} // namespace dsd_qt
