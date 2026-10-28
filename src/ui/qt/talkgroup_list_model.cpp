// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "talkgroup_list_model.h"
#include "heard_talkgroup_source.h"

#include <QByteArray>
#include <QDebug>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <algorithm>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/id_pattern.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <iterator>
#include <limits>
#include <string.h>
#include <utility>

namespace dsd_qt {

namespace {

/* Whether @p index names one of @p rows rows in the list's single column. */
bool
listIndexValid(const QModelIndex& index, int rows) {
    return index.isValid() && static_cast<unsigned int>(index.row()) < static_cast<unsigned int>(rows)
           && index.column() == 0;
}

/* The kind role's text for a dsd_tg_policy_row_kind. */
QString
rowKindText(uint8_t kind) {
    switch (kind) {
        case DSD_TG_POLICY_ROW_PATTERN: return QStringLiteral("pattern");
        case DSD_TG_POLICY_ROW_RANGE: return QStringLiteral("range");
        default: return QStringLiteral("exact");
    }
}

/* List order: numeric rows by their IDs, then pattern rows in list order. */
template <typename Row>
bool
rowListedBefore(const Row& a, const Row& b) {
    if (a.isPattern() != b.isPattern()) {
        return b.isPattern();
    }
    if (a.isPattern()) {
        return a.tableIndex < b.tableIndex;
    }
    return a.idStart < b.idStart || (a.idStart == b.idStart && a.idEnd < b.idEnd);
}

/* Each row's count of session avoids: the IDs its bounds cover, or for a pattern row the IDs its pattern matches. */
template <typename Rows>
void
fillTemporaryAvoidCounts(Rows& rows, const dsd_state* snapshot) {
    for (auto& row : rows) {
        row.temporaryAvoidCount =
            row.isPattern() ? dsd_tg_policy_session_avoid_count_pattern(snapshot, row.pattern.toLatin1().constData())
                            : dsd_tg_policy_session_avoid_count(snapshot, row.idStart, row.idEnd);
    }
}

} // namespace

TalkgroupListModel::TalkgroupListModel(QAbstractItemModel* history, QObject* parent)
    : QAbstractListModel(parent), m_history(history) {
    if (history == nullptr) {
        return;
    }
    const auto roles = history->roleNames();
    m_tgRole = roles.key("tg", -1);
    m_sessionRole = roles.key("session", -1);
    m_kindRole = roles.key("kind", -1);
    m_heardSource = qobject_cast<HeardTalkgroupSource*>(history);
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
    if (!listIndexValid(index, count())) {
        return {};
    }
    return m_rows.at(index.row()).value(role);
}

QVariant
TalkgroupListModel::Row::value(int role) const {
    switch (role) {
        case IdStartRole: return QVariant::fromValue(static_cast<qulonglong>(idStart));
        case IdEndRole: return QVariant::fromValue(static_cast<qulonglong>(idEnd));
        case IdTextRole: return idText();
        case NameRole: return name;
        case TagsRole: return tags;
        case ListeningRole: return listening;
        case ListedRole: return listed;
        case PriorityRole: return priority;
        case PreemptRole: return preempt;
        case TableIndexRole: return tableIndex;
        case TemporaryAvoidCountRole: return QVariant::fromValue(temporaryAvoidCount);
        case RecordRole: return record;
        case KindRole: return rowKindText(kind);
        case PatternRole: return pattern;
        default: return {};
    }
}

bool
TalkgroupListModel::Row::fieldsDiffer(const Row& other) const {
    return name != other.name || tags != other.tags || listening != other.listening || listed != other.listed
           || priority != other.priority || preempt != other.preempt || record != other.record
           || tableIndex != other.tableIndex || temporaryAvoidCount != other.temporaryAvoidCount;
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
            {TableIndexRole, "tableIndex"},
            {TemporaryAvoidCountRole, "temporaryAvoidCount"},
            {RecordRole, "record"},
            {KindRole, "kind"},
            {PatternRole, "pattern"}};
}

void
TalkgroupListModel::setHistorySession(qint64 session) {
    if (session == m_historySession) {
        return;
    }
    m_historySession = session;
    m_heardDirty = true;
    Q_EMIT historySessionChanged();
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
    fillTemporaryAvoidCounts(rows, snapshot);
    std::stable_sort(rows.begin(), rows.end(), rowListedBefore<Row>);
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
        Row row;
        row.idStart = entry.id_start;
        row.idEnd = entry.id_end;
        row.name = QString::fromUtf8(entry.name);
        row.tags = QString::fromUtf8(entry.tags);
        row.listening = strcmp(entry.mode, "B") != 0 && strcmp(entry.mode, "DE") != 0;
        row.listed = true;
        row.priority = entry.priority;
        row.preempt = entry.preempt != 0;
        row.record = entry.record != 0;
        row.tableIndex = static_cast<int>(i);
        row.kind = entry.kind;
        if (entry.kind == DSD_TG_POLICY_ROW_PATTERN) {
            row.pattern = QString::fromLatin1(entry.pattern,
                                              static_cast<qsizetype>(strnlen(entry.pattern, sizeof(entry.pattern))));
        }
        rows.push_back(std::move(row));
        if (!rows.back().tags.isEmpty()) {
            categoryTags.insert(rows.back().tags);
        }
    }
    return rows;
}

QList<uint32_t>
TalkgroupListModel::heardFromRoles() const {
    QList<uint32_t> heard;
    if (m_tgRole < 0 || m_sessionRole < 0 || m_kindRole < 0) {
        return heard;
    }
    QSet<uint32_t> seen;
    for (int i = 0; i < m_history->rowCount(); ++i) {
        const QModelIndex idx = m_history->index(i, 0);
        if (m_history->data(idx, m_kindRole).toInt() != 0
            || (m_historySession > 0 && m_history->data(idx, m_sessionRole).toLongLong() != m_historySession)) {
            continue;
        }
        const qulonglong target = m_history->data(idx, m_tgRole).toULongLong();
        if (target == 0 || target > std::numeric_limits<uint32_t>::max()) {
            continue;
        }
        const auto tg = static_cast<uint32_t>(target);
        if (!seen.contains(tg)) {
            seen.insert(tg);
            heard.append(tg);
        }
    }
    return heard;
}

void
TalkgroupListModel::appendHeardRows(QVector<Row>& rows, bool allowListedOnly) const {
    if (m_history == nullptr) {
        return;
    }
    const QList<uint32_t> heard =
        m_heardSource != nullptr ? m_heardSource->heardTalkgroups(m_historySession) : heardFromRoles();
    if (heard.isEmpty()) {
        return;
    }

    /* Coverage by the listed rows, which may overlap: sorted by start, each carrying the largest end
     * of every row up to it. A talkgroup is covered when the last row starting at or below it reaches
     * it, a binary search per heard talkgroup instead of a pass over every listed row. */
    struct Span {
        uint32_t start;
        uint32_t reach;
    };

    QVector<Span> spans;
    /* A pattern row covers the heard talkgroups it matches, never by its 0/0 bounds. */
    QVector<dsd_id_pattern> patterns;
    spans.reserve(rows.size());
    for (const Row& row : rows) {
        dsd_id_pattern parsed;
        if (!row.isPattern()) {
            spans.push_back({row.idStart, row.idEnd});
        } else if (dsd_id_pattern_parse(row.pattern.toLatin1().constData(), &parsed) == 0) {
            patterns.push_back(parsed);
        }
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.start < b.start; });
    for (qsizetype i = 1; i < spans.size(); ++i) {
        spans[i].reach = std::max(spans[i].reach, spans[i - 1].reach);
    }
    for (const uint32_t tg : heard) {
        const auto after = std::upper_bound(spans.cbegin(), spans.cend(), tg,
                                            [](uint32_t id, const Span& span) { return id < span.start; });
        const bool covered = (after != spans.cbegin() && std::prev(after)->reach >= tg)
                             || std::any_of(patterns.cbegin(), patterns.cend(), [tg](const dsd_id_pattern& pattern) {
                                    return dsd_id_pattern_match(&pattern, tg) != 0;
                                });
        if (!covered) {
            Row row;
            row.idStart = tg;
            row.idEnd = tg;
            row.listening = !allowListedOnly;
            /* A heard talkgroup has no row: it records whenever it would be tuned, as a row added for it would. */
            row.record = !allowListedOnly;
            rows.push_back(std::move(row));
        }
    }
}

void
TalkgroupListModel::replaceRows(QVector<Row> rows) {
    const int oldCount = count();
    const int notTuned =
        static_cast<int>(std::count_if(rows.cbegin(), rows.cend(), [](const Row& row) { return !row.listening; }));
    const bool sameRows = rows.size() == m_rows.size()
                          && std::equal(rows.cbegin(), rows.cend(), m_rows.cbegin(),
                                        [](const Row& a, const Row& b) { return a.sameRow(b); });
    if (sameRows) {
        for (int i = 0; i < count(); ++i) {
            const Row& row = rows.at(i);
            Row& old = m_rows[i];
            if (old.fieldsDiffer(row)) {
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
