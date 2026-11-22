// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "talkgroup_list_model.h"
#include "heard_talkgroup_source.h"
#include "heard_talkgroup_store.h"

#include <QByteArray>
#include <QDebug>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <algorithm>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_label.h>
#include <dsd-neo/core/id_pattern.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/tuner_owner.h>
#include <iterator>
#include <limits>
#include <string.h>
#include <utility>

namespace dsd_qt {

/* Coverage of heard talkgroups by the listed rows, which may overlap: ranges sorted by start, each carrying the largest
 * end of every row up to it, so a talkgroup is covered when the last row starting at or below it reaches it (a binary
 * search per heard talkgroup instead of a pass over every listed row), or when a pattern row matches it. */
class TalkgroupCoverage {
  public:
    void
    addRange(uint32_t start, uint32_t end) {
        m_spans.push_back({start, end});
    }

    void
    addPattern(const QString& text) {
        dsd_id_pattern parsed;
        if (dsd_id_pattern_parse(text.toLatin1().constData(), &parsed) == 0) {
            m_patterns.push_back(parsed);
        }
    }

    void
    finish() {
        std::sort(m_spans.begin(), m_spans.end(), [](const Span& a, const Span& b) { return a.start < b.start; });
        for (qsizetype i = 1; i < m_spans.size(); ++i) {
            m_spans[i].reach = std::max(m_spans[i].reach, m_spans[i - 1].reach);
        }
    }

    bool
    covers(uint32_t tg) const {
        const auto after = std::upper_bound(m_spans.cbegin(), m_spans.cend(), tg,
                                            [](uint32_t id, const Span& span) { return id < span.start; });
        if (after != m_spans.cbegin() && std::prev(after)->reach >= tg) {
            return true;
        }
        return std::any_of(m_patterns.cbegin(), m_patterns.cend(),
                           [tg](const dsd_id_pattern& pattern) { return dsd_id_pattern_match(&pattern, tg) != 0; });
    }

  private:
    struct Span {
        uint32_t start;
        uint32_t reach;
    };

    QVector<Span> m_spans;
    QVector<dsd_id_pattern> m_patterns;
};

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

/* List order: numeric rows by their IDs, a table's equal bounds in table order, then pattern rows in table order. A
 * heard card (table index -1) never shares its bounds with a listed row, which would cover it. */
template <typename Row>
bool
rowListedBefore(const Row& a, const Row& b) {
    if (a.isPattern() != b.isPattern()) {
        return b.isPattern();
    }
    if (a.isPattern()) {
        return a.tableIndex < b.tableIndex;
    }
    if (a.idStart != b.idStart) {
        return a.idStart < b.idStart;
    }
    return a.idEnd != b.idEnd ? a.idEnd < b.idEnd : a.tableIndex < b.tableIndex;
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

/* A row's identity across two readings of the list (TalkgroupListModel::Row::sameRow()), and which of the rows of that
 * identity it is, in list order: a table can list the same bounds twice. */
template <typename Row>
QString
rowIdentity(const Row& row, QHash<QString, int>& seen) {
    const QString key = QStringLiteral("%1|%2|%3|").arg(row.kind).arg(row.idStart).arg(row.idEnd) + row.pattern;
    const int occurrence = seen.value(key, 0);
    seen.insert(key, occurrence + 1);
    return key + QLatin1Char('#') + QString::number(occurrence);
}

/* The most heard cards one refresh inserts or removes one by one; more (a target hop, a session's start with a long
 * remembered list) goes through the whole-list compare, which signals each run of rows once. */
constexpr int kHeardPatchLimit = 32;

} // namespace

TalkgroupListModel::TalkgroupListModel(QAbstractItemModel* history, QObject* parent)
    : QAbstractListModel(parent), m_history(history), m_coverage(std::make_unique<TalkgroupCoverage>()) {
    if (history == nullptr) {
        return;
    }
    const auto roles = history->roleNames();
    m_tgRole = roles.key("tg", -1);
    m_sessionRole = roles.key("session", -1);
    m_kindRole = roles.key("kind", -1);
    m_giRole = roles.key("gi", -1);
    m_heardSource = qobject_cast<HeardTalkgroupSource*>(history);
    const auto dirty = [this]() { m_transientDirty = true; };
    connect(history, &QAbstractItemModel::rowsInserted, this, dirty);
    connect(history, &QAbstractItemModel::rowsRemoved, this, dirty);
    connect(history, &QAbstractItemModel::dataChanged, this, dirty);
    connect(history, &QAbstractItemModel::modelReset, this, dirty);
    connect(history, &QObject::destroyed, this, dirty);
}

TalkgroupListModel::~TalkgroupListModel() = default;

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
        case KindRole: return rowKindText(kind);
        case PatternRole: return pattern;
        case LastHeardRole: return QVariant::fromValue(lastHeard);
        case RememberedRole: return remembered;
        default: return policyValue(role);
    }
}

QVariant
TalkgroupListModel::Row::policyValue(int role) const {
    switch (role) {
        case NameRole: return name;
        case TagsRole: return tags;
        case ListeningRole: return listening;
        case ListedRole: return listed;
        case PriorityRole: return priority;
        case PreemptRole: return preempt;
        case TableIndexRole: return tableIndex;
        case TemporaryAvoidCountRole: return QVariant::fromValue(temporaryAvoidCount);
        case RecordRole: return record;
        default: return {};
    }
}

bool
TalkgroupListModel::Row::fieldsDiffer(const Row& other) const {
    return name != other.name || tags != other.tags || listening != other.listening || listed != other.listed
           || priority != other.priority || preempt != other.preempt || record != other.record
           || tableIndex != other.tableIndex || temporaryAvoidCount != other.temporaryAvoidCount
           || lastHeard != other.lastHeard || remembered != other.remembered;
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
            {PatternRole, "pattern"},
            {LastHeardRole, "lastHeard"},
            {RememberedRole, "remembered"}};
}

void
TalkgroupListModel::setHistorySession(qint64 session) {
    if (session == m_historySession) {
        return;
    }
    m_historySession = session;
    m_transientDirty = true;
    // What a forget or clear hid was this session's: the next one's fragments are all new.
    m_forgotten.clear();
    m_clearedThrough.clear();
    Q_EMIT historySessionChanged();
}

void
TalkgroupListModel::setHeardStore(HeardTalkgroupStore* store) {
    if (m_store == store) {
        return;
    }
    if (m_store != nullptr) {
        disconnect(m_store, nullptr, this, nullptr);
    }
    m_store = store;
    m_rememberedDirty = true;
    if (store == nullptr) {
        return;
    }
    connect(store, &HeardTalkgroupStore::ownerChanged, this, [this](const QString& owner) {
        if (owner == m_viewedOwner) {
            m_rememberedDirty = true;
        }
    });
    connect(store, &QObject::destroyed, this, [this]() { m_rememberedDirty = true; });
}

QString
TalkgroupListModel::ownerOnAir(const dsd_opts* opts_snapshot, const dsd_state* snapshot) const {
    if (m_heardSource == nullptr || m_history == nullptr) {
        return {};
    }
    // The stamps a call heard now takes (watchdog_event_stamp_tuner_owner(), dsd_channel_label_current()), in the
    // session its ring is bound to.
    char label[DSD_CHANNEL_LABEL_SIZE] = {0};
    (void)dsd_channel_label_current(opts_snapshot, snapshot, label, sizeof(label));
    const bool scanMember = opts_snapshot->scanner_mode == 1 || dsd_trunk_scan_owns_tuner(opts_snapshot, snapshot) != 0;
    const quint64 ring =
        snapshot->event_history_s != nullptr ? static_cast<quint64>(snapshot->event_history_s[0].instance) : 0U;
    return m_heardSource->heardOwnerOnAir(
        ring, snapshot->operator_tuned != 0U, scanMember,
        QString::fromUtf8(label, static_cast<qsizetype>(qstrnlen(label, sizeof(label)))));
}

void
TalkgroupListModel::setViewedOwner(const QString& owner) {
    if (owner != m_viewedOwner) {
        m_viewedOwner = owner;
        m_rememberedDirty = true;
        m_transientDirty = true;
        Q_EMIT viewedOwnerChanged();
    }
    const bool learning = m_store != nullptr && m_store->remembering(m_viewedOwner);
    if (learning != m_remembering) {
        m_remembering = learning;
        Q_EMIT rememberingChanged();
    }
}

void
TalkgroupListModel::observeDecryption(const dsd_state* snapshot) {
    // Issue #903: a call on the air now is the viewed owner's (heardOwnerOnAir()), and its encryption is looked up there.
    QHash<quint32, QVariantMap>* observed = nullptr;
    for (uint8_t slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; ++slot) {
        dsd_call_snapshot call = {};
        if (dsd_call_state_get(snapshot, slot, &call) > 0 && call.kind == DSD_CALL_KIND_GROUP_VOICE
            && call.ota_target_id && call.ota_target_id <= UINT32_MAX) {
            const bool dmr = DSD_SYNC_IS_DMR(call.protocol);
            if (observed == nullptr) {
                observed = &m_observedCrypto[m_viewedOwner];
            }
            observed->insert(static_cast<quint32>(call.ota_target_id),
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
    setViewedOwner(ownerOnAir(opts_snapshot, snapshot));
    observeDecryption(snapshot);
    const bool versionChanged = contextId != m_contextId || generation != m_generation;
    if (versionChanged || allowListChanged) {
        m_listedDirty = true;
    }
    // An idle tick: nothing the list shows moved, so nothing is read.
    if (!m_listedDirty && !m_rememberedDirty && !m_transientDirty) {
        return;
    }

    // Another policy context than the rows show is another table (another target's or system's): a new list, shown from
    // the top (#782). A hop that keeps the table (invalidateForTarget() forgets only the version read) keeps the view.
    const bool newTable = m_listedDirty && contextId != m_shownContextId;
    m_contextId = contextId;
    m_generation = generation;
    if (versionChanged) {
        Q_EMIT policyChanged();
    }
    readDirty(snapshot, allowListedOnly, newTable);
}

void
TalkgroupListModel::readDirty(const dsd_state* snapshot, bool allowListedOnly, bool newTable) {
    const int oldCount = count();
    const bool listedRead = m_listedDirty;
    if (m_listedDirty) {
        m_shownContextId = m_contextId;
        QSet<QString> categoryTags;
        readListed(snapshot, allowListedOnly, categoryTags);
        updateCategories(categoryTags);
    }
    if (m_rememberedDirty) {
        readRemembered();
    }
    if (m_transientDirty) {
        readTransient();
    }
    QHash<uint32_t, Heard> heard = mergeHeard();
    // A policy change can cover or uncover any card: the whole list is compared. Otherwise only the heard cards move.
    if (listedRead || !patchHeard(heard, snapshot)) {
        replaceRows(wholeList(heard, snapshot), newTable);
    }
    m_heard = std::move(heard);
    updateCounts(oldCount);
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

void
TalkgroupListModel::readListed(const dsd_state* snapshot, bool allowListedOnly, QSet<QString>& categoryTags) {
    m_listed = listedRows(snapshot, categoryTags);
    fillTemporaryAvoidCounts(m_listed, snapshot);
    std::sort(m_listed.begin(), m_listed.end(), rowListedBefore<Row>);
    /* A pattern row covers the heard talkgroups it matches, never by its 0/0 bounds. The `*` catch-all is the list's
     * "every other talkgroup" and covers none: a heard talkgroup keeps its card, so it can still be named, tuned or
     * prioritised on its own (issue #773). The last `*` row (as equal patterns go) decides those talkgroups, so their
     * cards show its listening and record and a first tap changes what the decoder does. */
    auto coverage = std::make_unique<TalkgroupCoverage>();
    const Row* catchAll = nullptr;
    for (const Row& row : m_listed) {
        if (!row.isPattern()) {
            coverage->addRange(row.idStart, row.idEnd);
        } else if (row.pattern == QStringLiteral("*")) {
            catchAll = &row;
        } else {
            coverage->addPattern(row.pattern);
        }
    }
    coverage->finish();
    m_coverage = std::move(coverage);
    m_heardListening = catchAll != nullptr ? catchAll->listening : !allowListedOnly;
    /* A heard talkgroup without a row records whenever it would be tuned, as a row added for it would. */
    m_heardRecord = catchAll != nullptr ? catchAll->listening && catchAll->record : m_heardListening;
    m_listedDirty = false;
#ifdef DSD_NEO_TEST_HOOKS
    ++m_work.listedReads;
#endif
}

QList<HeardTalkgroupRow>
TalkgroupListModel::heardFromRoles() const {
    QList<HeardTalkgroupRow> heard;
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
        // A private call's target is a radio, never a talkgroup heard (issue #800); not knowing stays as before.
        if (m_giRole >= 0 && m_history->data(idx, m_giRole).toInt() == 1) {
            continue;
        }
        const auto tg = static_cast<uint32_t>(target);
        if (!seen.contains(tg)) {
            seen.insert(tg);
            HeardTalkgroupRow row;
            row.id = tg;
            heard.append(row);
        }
    }
    return heard;
}

void
TalkgroupListModel::readRemembered() {
    m_remembered.clear();
    if (m_store != nullptr && !m_viewedOwner.isEmpty()) {
        // One card per talkgroup number: the variants of a number (and Type-D's prefixes) are one card.
        const QVector<HeardTalkgroupStore::Entry> entries = m_store->entriesFor(m_viewedOwner);
        m_remembered.reserve(entries.size());
        for (const HeardTalkgroupStore::Entry& entry : entries) {
            const uint32_t id = HeardTalkgroupStore::policyId(entry.variant, entry.id);
            if (id == 0U) {
                continue;
            }
            qint64& last = m_remembered[id];
            last = qMax(last, entry.last);
        }
    }
    m_rememberedDirty = false;
#ifdef DSD_NEO_TEST_HOOKS
    ++m_work.rememberedReads;
#endif
}

void
TalkgroupListModel::readTransient() {
    m_transient.clear();
    if (m_history != nullptr) {
        const QList<HeardTalkgroupRow> heard = m_heardSource != nullptr
                                                   ? m_heardSource->heardTalkgroups(m_historySession, m_viewedOwner)
                                                   : heardFromRoles();
        m_transient.reserve(heard.size());
        for (const HeardTalkgroupRow& row : heard) {
            m_transient.insert(row.id, qMakePair(row.last, row.serial));
        }
    }
    m_transientDirty = false;
#ifdef DSD_NEO_TEST_HOOKS
    ++m_work.historyReads;
#endif
}

QHash<uint32_t, TalkgroupListModel::Heard>
TalkgroupListModel::mergeHeard() const {
    QHash<uint32_t, Heard> heard;
    heard.reserve(m_remembered.size() + m_transient.size());
    for (auto it = m_remembered.cbegin(); it != m_remembered.cend(); ++it) {
        heard.insert(it.key(), Heard{it.value(), true});
    }
    // A forget or clear hid the fragments it found: only one ingested after it shows the number again.
    const auto forgotten = m_forgotten.constFind(m_viewedOwner);
    const auto cleared = m_clearedThrough.constFind(m_viewedOwner);
    for (auto it = m_transient.cbegin(); it != m_transient.cend(); ++it) {
        const quint64 serial = it.value().second;
        if (cleared != m_clearedThrough.cend() && serial <= cleared.value()) {
            continue;
        }
        if (forgotten != m_forgotten.cend()) {
            const auto hidden = forgotten->constFind(it.key());
            if (hidden != forgotten->cend() && serial <= hidden.value()) {
                continue;
            }
        }
        Heard& card = heard[it.key()];
        card.last = qMax(card.last, it.value().first);
    }
    return heard;
}

TalkgroupListModel::Row
TalkgroupListModel::heardRow(uint32_t id, const Heard& heard, const dsd_state* snapshot) const {
    Row row;
    row.idStart = id;
    row.idEnd = id;
    row.listening = m_heardListening;
    row.record = m_heardRecord;
    row.lastHeard = heard.last;
    row.remembered = heard.remembered;
    row.temporaryAvoidCount = snapshot != nullptr ? dsd_tg_policy_session_avoid_count(snapshot, id, id) : 0U;
    return row;
}

QVector<TalkgroupListModel::Row>
TalkgroupListModel::wholeList(const QHash<uint32_t, Heard>& heard, const dsd_state* snapshot) const {
    QVector<Row> rows = m_listed;
    rows.reserve(m_listed.size() + heard.size());
    for (auto it = heard.cbegin(); it != heard.cend(); ++it) {
        if (!m_coverage->covers(it.key())) {
            rows.push_back(heardRow(it.key(), it.value(), snapshot));
        }
    }
    std::sort(rows.begin() + m_listed.size(), rows.end(), rowListedBefore<Row>);
    std::inplace_merge(rows.begin(), rows.begin() + m_listed.size(), rows.end(), rowListedBefore<Row>);
    return rows;
}

int
TalkgroupListModel::heardRowIndex(uint32_t id) const {
    Row key;
    key.idStart = id;
    key.idEnd = id;
    const auto at = std::lower_bound(m_rows.cbegin(), m_rows.cend(), key, rowListedBefore<Row>);
    if (at == m_rows.cend() || at->isPattern() || at->listed || at->idStart != id || at->idEnd != id) {
        return -1;
    }
    return static_cast<int>(at - m_rows.cbegin());
}

void
TalkgroupListModel::removeRowAt(int row) {
    beginRemoveRows(QModelIndex(), row, row);
    m_rows.remove(row);
    endRemoveRows();
}

bool
TalkgroupListModel::patchHeard(const QHash<uint32_t, Heard>& heard, const dsd_state* snapshot) {
    QVector<uint32_t> gone;
    QVector<uint32_t> added;
    QVector<uint32_t> changed;
    for (auto it = m_heard.cbegin(); it != m_heard.cend(); ++it) {
        if (!heard.contains(it.key()) && !m_coverage->covers(it.key())) {
            gone.push_back(it.key());
        }
    }
    for (auto it = heard.cbegin(); it != heard.cend(); ++it) {
        const auto old = m_heard.constFind(it.key());
        const bool isNew = old == m_heard.cend();
        if ((isNew || !(old.value() == it.value())) && !m_coverage->covers(it.key())) {
            (isNew ? added : changed).push_back(it.key());
        }
    }
    if (gone.size() + added.size() > kHeardPatchLimit) {
        return false;
    }
    for (const uint32_t id : gone) {
        const int row = heardRowIndex(id);
        if (row >= 0) {
            removeRowAt(row);
        }
    }
    for (const uint32_t id : added) {
        Row row = heardRow(id, heard.value(id), snapshot);
        const int at = static_cast<int>(std::lower_bound(m_rows.cbegin(), m_rows.cend(), row, rowListedBefore<Row>)
                                        - m_rows.cbegin());
        beginInsertRows(QModelIndex(), at, at);
        m_rows.insert(at, std::move(row));
        endInsertRows();
    }
    for (const uint32_t id : changed) {
        const int row = heardRowIndex(id);
        if (row >= 0) {
            m_rows[row].lastHeard = heard.value(id).last;
            m_rows[row].remembered = heard.value(id).remembered;
            Q_EMIT dataChanged(index(row, 0), index(row, 0), {LastHeardRole, RememberedRole});
        }
    }
    return true;
}

bool
TalkgroupListModel::matchRows(const QVector<Row>& rows, QVector<int>& match) const {
    QHash<QString, int> oldAt;
    QHash<QString, int> seen;
    for (int i = 0; i < count(); ++i) {
        oldAt.insert(rowIdentity(m_rows.at(i), seen), i);
    }
    seen.clear();
    match.fill(-1, rows.size());
    int lastOld = -1;
    for (int j = 0; j < rows.size(); ++j) {
        const auto old = oldAt.constFind(rowIdentity(rows.at(j), seen));
        if (old == oldAt.cend()) {
            continue;
        }
        if (old.value() < lastOld) {
            return false;
        }
        lastOld = old.value();
        match[j] = old.value();
    }
    return true;
}

void
TalkgroupListModel::dropRows(const QVector<bool>& drop) {
    for (int i = count() - 1; i >= 0;) {
        if (!drop.at(i)) {
            --i;
            continue;
        }
        const int last = i;
        while (i >= 0 && drop.at(i)) {
            --i;
        }
        beginRemoveRows(QModelIndex(), i + 1, last);
        m_rows.remove(i + 1, last - i);
        endRemoveRows();
    }
}

bool
TalkgroupListModel::diffRows(QVector<Row>& rows) {
    // Each new row's old row, by identity and in order; the rows both readings have must keep their order.
    QVector<int> match;
    if (!matchRows(rows, match)) {
        return false;
    }
    // The rows that went, a run at a time from the end; then the rows that came, a run at a time from the start.
    QVector<bool> drop(count(), true);
    for (const int old : match) {
        if (old >= 0) {
            drop[old] = false;
        }
    }
    dropRows(drop);
    for (int j = 0; j < rows.size();) {
        if (match.at(j) >= 0) {
            ++j;
            continue;
        }
        const int first = j;
        while (j < rows.size() && match.at(j) < 0) {
            ++j;
        }
        beginInsertRows(QModelIndex(), first, j - 1);
        for (int k = first; k < j; ++k) {
            m_rows.insert(k, rows.at(k));
        }
        endInsertRows();
    }
    for (int j = 0; j < rows.size(); ++j) {
        if (match.at(j) >= 0 && m_rows.at(j).fieldsDiffer(rows.at(j))) {
            m_rows[j] = std::move(rows[j]);
            Q_EMIT dataChanged(index(j, 0), index(j, 0));
        }
    }
    return true;
}

void
TalkgroupListModel::replaceRows(QVector<Row> rows, bool reset) {
#ifdef DSD_NEO_TEST_HOOKS
    ++m_work.wholeListUpdates;
#endif
    if (reset || !diffRows(rows)) {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }
}

void
TalkgroupListModel::updateCounts(int oldCount) {
    const int notTuned =
        static_cast<int>(std::count_if(m_rows.cbegin(), m_rows.cend(), [](const Row& row) { return !row.listening; }));
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
TalkgroupListModel::forgetTalkgroup(quint32 id) {
    forgetTalkgroupOf(m_viewedOwner, id);
}

void
TalkgroupListModel::forgetTalkgroupOf(const QString& owner, quint32 id) {
    if (id == 0U) {
        return;
    }
    if (m_store != nullptr && !owner.isEmpty()) {
        m_store->forget(owner, id);
    }
    // Read after the store's forget, whose before-edit hook may fold what the background capture held into History:
    // those fragments came before the forget, so they are hidden too.
    m_forgotten[owner].insert(id, m_heardSource != nullptr && m_history != nullptr ? m_heardSource->heardSerial() : 0U);
    dropHeard(owner, {id}, false);
}

void
TalkgroupListModel::clearRemembered() {
    clearRememberedOf(m_viewedOwner);
}

void
TalkgroupListModel::clearRememberedOf(const QString& owner) {
    if (m_store != nullptr && !owner.isEmpty()) {
        m_store->clear(owner);
    }
    m_clearedThrough.insert(owner,
                            m_heardSource != nullptr && m_history != nullptr ? m_heardSource->heardSerial() : 0U);
    m_forgotten.remove(owner);
    dropHeard(owner, {}, true);
}

void
TalkgroupListModel::dropHeard(const QString& owner, const QSet<uint32_t>& ids, bool all) {
    if (owner != m_viewedOwner) {
        return;
    }
    // The next refresh reads both again; the cards go now, so the sheet that asked closes on a list without them.
    m_rememberedDirty = true;
    m_transientDirty = true;
    const int oldCount = count();
    QVector<bool> drop(count(), false);
    for (int i = 0; i < count(); ++i) {
        const Row& row = m_rows.at(i);
        drop[i] = !row.listed && !row.isPattern() && (all || ids.contains(row.idStart));
    }
    dropRows(drop);
    if (all) {
        m_heard.clear();
    } else {
        for (const uint32_t id : ids) {
            m_heard.remove(id);
        }
    }
    updateCounts(oldCount);
}

void
TalkgroupListModel::invalidateForTarget() {
    m_observedCrypto.clear();
    m_contextId = 0;
    m_generation = 0;
    m_listedDirty = true;
    m_rememberedDirty = true;
    m_transientDirty = true;
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
    m_shownContextId = 0;
    m_listed.clear();
    m_coverage = std::make_unique<TalkgroupCoverage>();
    m_remembered.clear();
    m_transient.clear();
    m_heard.clear();
    setViewedOwner(QString());
    m_listedDirty = true;
    m_rememberedDirty = true;
    m_transientDirty = true;
}

} // namespace dsd_qt
