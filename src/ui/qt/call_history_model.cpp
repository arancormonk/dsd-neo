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
#include <QRegularExpression>

#include <dsd-neo/core/state.h>

#include "call_history_merge.h"
#include "json_store.h"

namespace dsd_qt {

namespace {

constexpr const char kStoreFileName[] = "call_history.json";
constexpr const char kSeenStoreFileName[] = "call_history_seen.json";
constexpr const char kSessionLabelKey[] = "callHistory/sessionLabel";
constexpr const char kClearedThroughKey[] = "callHistory/clearedThrough";
constexpr int kMaxRows = 1000;
/* Bound on the persisted seen map. The ring holds at most DSD_EVENT_HISTORY_LEN-1
 * rows per slot, so anything beyond the newest ~4x that can no longer be
 * re-ingested and is dead weight in the store. */
constexpr int kMaxSeenEntries = 2048;
constexpr int kSaveDelayMs = 3000;

/** @brief End of a logged row on the shared timeline; unknown durations count as 0. */
qint64
row_end_secs(const CallHistoryModel::Row& row) {
    return row.when + qMax(row.durationSecs, 0);
}

/**
 * @brief The dedup key for one ring row, from its stable identity.
 *
 * slot+seq name the physical ring row for the service session's whole life (the
 * push stamp never changes, unlike the row's index). `when` and `tg` guard the
 * one hole seq leaves: a restarted service counts push_seq from zero again, and
 * without content in the key its early rows would collide with the previous
 * session's persisted entries. `when` is stable in turn because the core stamps
 * a row's start once and merges never move it.
 */
QString
seen_key(int slot, qulonglong seq, qint64 when, qulonglong tg, int kind) {
    return QStringLiteral("%1|%2|%3|%4|%5").arg(slot).arg(seq).arg(when).arg(tg).arg(kind);
}

/** @brief "TODAY" / "YESTERDAY" / "MON 3 AUG" for the list's day sections. */
QString
day_label(qint64 when) {
    const QDate day = QDateTime::fromSecsSinceEpoch(when).date();
    const QDate today = QDate::currentDate();
    if (day == today) {
        return QStringLiteral("TODAY");
    }
    if (day == today.addDays(-1)) {
        return QStringLiteral("YESTERDAY");
    }
    return QLocale().toString(day, QStringLiteral("ddd d MMM")).toUpper();
}

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
    connect(&m_saveTimer, &QTimer::timeout, this, [this]() { startAsyncSave(); });
    /* One worker: saves must not overlap (two writers racing on the same
     * QSaveFile target), and a second thread would buy nothing for two files. */
    m_savePool.setMaxThreadCount(1);
    /* Day sections are derived from the current date at read time; when midnight
     * passes, every "TODAY" on screen is wrong until the rows are re-read. */
    m_dayTimer.setSingleShot(true);
    m_dayTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_dayTimer, &QTimer::timeout, this, [this]() {
        if (!m_rows.isEmpty()) {
            Q_EMIT dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1), {DayLabelRole});
        }
        scheduleDayRollover();
    });
    scheduleDayRollover();
    load();
}

void
CallHistoryModel::scheduleDayRollover() {
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime nextMidnight = QDate::currentDate().addDays(1).startOfDay();
    /* A second past the boundary, so a coarse timer that fires marginally early
     * cannot re-derive the very labels it was meant to retire. */
    m_dayTimer.start(static_cast<int>(qMin<qint64>(now.msecsTo(nextMidnight) + 1000, 86400000)));
}

CallHistoryModel::~CallHistoryModel() {
    /* A debounced or coalesced save may still be owed; the in-flight one (if any)
     * already carries the current stores. Wait it out, then flush what remains
     * synchronously — the worker must never outlive this object. */
    const bool owedSave = m_saveTimer.isActive() || m_saveDirty;
    m_saveTimer.stop();
    m_savePool.waitForDone();
    if (owedSave) {
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
        case DayLabelRole: return day_label(row.when);
        case TimeTextRole: return QDateTime::fromSecsSinceEpoch(row.when).toString(QStringLiteral("HH:mm"));
        case KindRole: return row.kind;
        case DetailRole: return row.detail;
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
    roles.insert(KindRole, QByteArrayLiteral("kind"));
    roles.insert(DetailRole, QByteArrayLiteral("detail"));
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

/**
 * @brief Which display kind a committed ring item ingests as, or -1 to skip it.
 *
 * Voice rows need a nameable target; notices need any payload at all. STATUS and
 * SYSTEM rows (per-frame churn, the startup banner) are not log material.
 */
int
ring_item_display_kind(const Event_History* item) {
    if (item->category == DSD_EVENT_CATEGORY_VOICE) {
        // Any nameable target will do: a numeric talkgroup, a textual target
        // (M17/D-STAR callsigns), or an imported label alone — a row whose only
        // identity is its CSV name is still a call the operator heard.
        if (item->target_id == 0U && item->tgt_str[0] == '\0' && item->t_name[0] == '\0') {
            return -1;
        }
        return CallHistoryModel::KindVoice;
    }
    if (item->category != DSD_EVENT_CATEGORY_DATA && item->category != DSD_EVENT_CATEGORY_CONTROL) {
        return -1;
    }
    if (item->event_string[0] == '\0' && item->text_message[0] == '\0' && item->gps_s[0] == '\0') {
        return -1;
    }
    return CallHistoryModel::KindNotice;
}

/** @brief The notice text minus the "YYYY-MM-DD HH:MM:SS " prefix the emitter stamps. */
QString
notice_summary(const Event_History* item) {
    static const QRegularExpression datePrefix(QStringLiteral("^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2} "));
    QString text = QString::fromUtf8(item->event_string);
    return text.remove(datePrefix).trimmed();
}

/** @brief One committed ring item as a display row. */
CallHistoryModel::Row
row_from_item(const Event_History* item, const QString& sessionLabel, int slot, qulonglong seq) {
    CallHistoryModel::Row row;
    row.slot = slot;
    row.seq = seq;
    row.tg = static_cast<qulonglong>(item->target_id);
    row.src = static_cast<qulonglong>(item->source_id);
    row.enc = item->enc != 0U;
    row.kind = item->category == DSD_EVENT_CATEGORY_VOICE ? CallHistoryModel::KindVoice : CallHistoryModel::KindNotice;
    if (row.kind == CallHistoryModel::KindNotice) {
        /* The emitter's summary line names what happened ("SMS from 1234",
         * "LRRP position"); the payload — the decoded message or GPS string —
         * rides in detail so the delegate can show both. */
        row.name = notice_summary(item);
        if (item->text_message[0] != '\0') {
            row.detail = QString::fromUtf8(item->text_message);
        } else if (item->gps_s[0] != '\0') {
            row.detail = QString::fromUtf8(item->gps_s);
        }
        if (row.name.isEmpty()) {
            row.name = !row.detail.isEmpty() ? row.detail : QStringLiteral("Data message");
        }
    } else if (item->t_name[0] != '\0') {
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
    if (row.kind == CallHistoryModel::KindVoice) {
        row.durationSecs = call_history_duration_secs(start, end);
    }
    return row;
}

} // namespace

int
CallHistoryModel::noteSeen(const QString& key, qint64 when, qint64 end, qulonglong src, bool enc, bool voice) {
    auto seen = m_seen.find(key);
    if (seen == m_seen.end()) {
        m_seen.insert(key, SeenState{when, end, src, enc});
        return SeenNew;
    }
    // Seen is not final: the core merges a reacquired segment into its committed
    // row in place — the end extends, src fills 0 -> real, the crypto verdict can
    // flip on — and the key does not change when it does. Re-read the row as an
    // update whenever it advanced. Notices are immutable, so only voice re-reads.
    if (!voice) {
        return SeenUnchanged;
    }
    int64_t storedEnd = seen->end;
    uint64_t storedSrc = seen->src;
    bool storedEnc = seen->enc;
    if (!call_history_seen_absorb(&storedEnd, &storedSrc, &storedEnc, end, src, enc)) {
        return SeenUnchanged;
    }
    seen->end = storedEnd;
    seen->src = storedSrc;
    seen->enc = storedEnc;
    return SeenAdvanced;
}

QList<CallHistoryModel::FreshRow>
CallHistoryModel::collectFresh(const dsd_state* snapshot, const bool scan[2]) {
    QList<FreshRow> fresh;
    for (int slot = 0; slot < 2; slot++) {
        if (!scan[slot]) {
            continue;
        }
        const qulonglong pushSeq = static_cast<qulonglong>(snapshot->event_history_s[slot].push_seq);
        // Index 0 is the still-active staged row; only committed rows are finished
        // calls that belong in a log.
        for (int idx = 1; idx < DSD_EVENT_HISTORY_LEN; idx++) {
            const Event_History* item = &snapshot->event_history_s[slot].Event_History_Items[idx];
            const int kind = ring_item_display_kind(item);
            if (kind < 0) {
                continue;
            }
            const qint64 start = static_cast<qint64>(item->event_start_time);
            const qint64 end = static_cast<qint64>(item->event_time);
            const qint64 when = (start > 0) ? start : end;
            if (qMax(when, end) <= m_clearedThrough) {
                // Cleared by the user; the ring still holds the row (and will until
                // the session ends), so it must stay invisible even after m_seen is
                // rebuilt by a relaunched UI. Judged by the row's end, not its
                // start: a call still airing when Clear was tapped commits later
                // and is new activity, not part of what was wiped.
                continue;
            }
            const bool voice = kind == KindVoice;
            const qulonglong src = static_cast<qulonglong>(item->source_id);
            // The push stamp this row was committed at — its stable ring identity.
            const qulonglong seq =
                pushSeq >= static_cast<qulonglong>(idx - 1) ? pushSeq - static_cast<qulonglong>(idx - 1) : 0ULL;
            // Must match keyFor() on the equivalent Row, or a relaunched UI would
            // re-ingest every row its predecessor already logged.
            const QString key = seen_key(slot, seq, when, item->target_id, kind);
            const int verdict = noteSeen(key, when, end, src, item->enc != 0U, voice);
            if (verdict == SeenUnchanged) {
                continue;
            }
            fresh.append(FreshRow{row_from_item(item, m_sessionLabel, slot, seq), verdict == SeenAdvanced});
        }
    }
    return fresh;
}

namespace {

/** @brief Whether two voice rows are one conversation the merge may fold together. */
bool
rows_mergeable(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (existing.kind != CallHistoryModel::KindVoice) {
        return false;
    }
    // A zero source id is "not yet learned", not a distinct unit: late-entry
    // fragments commit before the ring learns the src, and refusing to absorb
    // them would leave one conversation split across two rows.
    const bool srcCompatible = existing.src == row.src || existing.src == 0 || row.src == 0;
    if (existing.tg != row.tg || !srcCompatible || existing.systemName != row.systemName) {
        return false;
    }
    // Textual targets (M17/D-STAR/YSF callsigns, dPMR dial strings) all share
    // tg == 0, so the numeric check above cannot tell two destinations apart;
    // the name — built from the target text — is their identity.
    if (existing.tg == 0 && existing.name != row.name) {
        return false;
    }
    // Matched sources get the full retune window; a src-unknown pairing gets
    // the tight one, or two distinct back-to-back calls on a busy talkgroup
    // collapse into one row (the wildcard above cannot tell units apart).
    const bool srcKnownMatch = existing.src != 0 && existing.src == row.src;
    return call_history_merge_within_window(existing.when, row_end_secs(existing), row.when, row_end_secs(row),
                                            srcKnownMatch);
}

} // namespace

int
CallHistoryModel::tryMerge(const Row& row) {
    if (row.kind != KindVoice) {
        // Notices are discrete deliveries: two SMS a second apart are two
        // messages, never fragments of one.
        return -1;
    }
    for (int i = 0; i < m_rows.size() && i < 32; i++) {
        Row& existing = m_rows[i];
        if (!rows_mergeable(existing, row)) {
            continue;
        }
        const qint64 start = qMin(existing.when, row.when);
        const qint64 span = qMax(row_end_secs(existing), row_end_secs(row)) - start;
        existing.when = start;
        if ((existing.durationSecs >= 0 || row.durationSecs >= 0) && span <= kCallHistoryMaxPlausibleDurationSecs) {
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
    // Derived from the persisted row so it survives an Activity restart: the
    // Android service outlives the Activity, and a relaunched UI must not
    // re-ingest ring rows it already logged. Excludes everything a reacquisition
    // merge can still refine in place (end stamp, src, enc) — those changes must
    // read back as updates to the same key, never as a brand-new row.
    return seen_key(row.slot, row.seq, row.when, row.tg, row.kind);
}

bool
CallHistoryModel::ingestRow(const Row& row, bool isUpdate) {
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
    if (isUpdate) {
        // A seen row that advanced refines a call this model already logged; if
        // its row cannot be found (absorbed and trimmed, or deeper than the merge
        // scan), inserting it would mint the duplicate the seen map exists to
        // prevent. Drop it instead.
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

    // Gated on commit_rev, not revision: an active call re-renders its staged row
    // at the poll rate, and each render bumps revision without there being
    // anything new below index 0. Only actual commits, merges and enrichment move
    // commit_rev, so the ring walk runs exactly when it can find something.
    bool scan[2];
    bool anyChanged = false;
    for (int slot = 0; slot < 2; slot++) {
        const quint64 commitRev = static_cast<quint64>(snapshot->event_history_s[slot].commit_rev);
        scan[slot] = !m_seeded || commitRev != m_commitRev[slot];
        m_commitRev[slot] = commitRev;
        anyChanged = anyChanged || scan[slot];
    }
    m_seeded = true;
    if (!anyChanged) {
        return;
    }

    const QList<FreshRow> fresh = collectFresh(snapshot, scan);

    if (fresh.isEmpty()) {
        return;
    }

    bool rowsChanged = false;
    for (const FreshRow& item : fresh) {
        rowsChanged = ingestRow(item.row, item.isUpdate) || rowsChanged;
    }
    // Trimmed rows keep their seen entry: the ring may still hold them, and
    // forgetting the key would re-ingest (and re-trim) each one every tick. The
    // map itself is bounded below instead.
    while (m_rows.size() > kMaxRows) {
        const int last = static_cast<int>(m_rows.size()) - 1;
        beginRemoveRows(QModelIndex(), last, last);
        m_rows.removeLast();
        endRemoveRows();
        rowsChanged = true;
    }
    pruneSeen();
    if (rowsChanged) {
        Q_EMIT countChanged();
    }
    scheduleSave();
}

void
CallHistoryModel::pruneSeen() {
    // Drop the oldest entries once the map is well past what the ring could
    // still resurrect (at most 254 committed rows per slot). Oldest-first by the
    // stamp inside the value, so keys stay opaque.
    if (m_seen.size() <= static_cast<qsizetype>(kMaxSeenEntries) * 2) {
        return;
    }
    QList<qint64> stamps;
    stamps.reserve(m_seen.size());
    for (const SeenState& state : m_seen) {
        stamps.append(state.when);
    }
    std::sort(stamps.begin(), stamps.end());
    const qint64 cutoff = stamps.at(stamps.size() - kMaxSeenEntries);
    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (it->when < cutoff) {
            it = m_seen.erase(it);
        } else {
            ++it;
        }
    }
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
    // The watermark above is what makes the clear durable (QSettings writes it
    // through); the emptied stores can follow on the worker without a stall here.
    startAsyncSave();
}

void
CallHistoryModel::load() {
    QList<Row> rows;
    const QJsonArray array = json_store_load_array(QLatin1String(kStoreFileName));
    for (const auto& value : array) {
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
        row.kind = obj.value(QLatin1String("kind")).toInt(KindVoice);
        row.detail = obj.value(QLatin1String("detail")).toString();
        row.slot = obj.value(QLatin1String("slot")).toInt();
        row.seq = obj.value(QLatin1String("seq")).toVariant().toULongLong();
        rows.append(row);
    }
    beginResetModel();
    m_rows.clear();
    // Oldest first through the same merge the ingest path uses, so a log written
    // before fragment-coalescing existed collapses on its first load.
    for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
        m_seen.insert(keyFor(*it), SeenState{it->when, it->when + qMax(it->durationSecs, 0), it->src, it->enc});
        if (tryMerge(*it) < 0) {
            m_rows.prepend(*it);
        }
    }
    std::stable_sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.when > b.when; });
    // The persisted seen map wins over what the merged rows imply: it still holds
    // the keys of every absorbed fragment, which exist nowhere in the rows above,
    // and without them a service ring that outlived this process would re-ingest
    // each fragment as a duplicate conversation.
    const QJsonArray seenArray = json_store_load_array(QLatin1String(kSeenStoreFileName));
    for (const auto& value : seenArray) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        const QString key = obj.value(QLatin1String("key")).toString();
        if (key.isEmpty()) {
            continue;
        }
        SeenState state;
        state.when = obj.value(QLatin1String("when")).toVariant().toLongLong();
        state.end = obj.value(QLatin1String("end")).toVariant().toLongLong();
        state.src = obj.value(QLatin1String("src")).toVariant().toULongLong();
        state.enc = obj.value(QLatin1String("enc")).toBool();
        m_seen.insert(key, state);
    }
    endResetModel();
    Q_EMIT countChanged();
}

void
CallHistoryModel::scheduleSave() {
    if (!m_saveTimer.isActive()) {
        m_saveTimer.start();
    }
}

QJsonArray
CallHistoryModel::rowsToJson() const {
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
        obj.insert(QLatin1String("kind"), row.kind);
        if (!row.detail.isEmpty()) {
            obj.insert(QLatin1String("detail"), row.detail);
        }
        obj.insert(QLatin1String("slot"), row.slot);
        obj.insert(QLatin1String("seq"), static_cast<qint64>(row.seq));
        array.append(obj);
    }
    return array;
}

QJsonArray
CallHistoryModel::seenToJson() const {
    // Newest first and capped: the seen map only has to outlive what the ring can
    // still resurrect, not the whole persisted log.
    QList<QPair<QString, SeenState>> entries;
    entries.reserve(m_seen.size());
    for (auto it = m_seen.cbegin(); it != m_seen.cend(); ++it) {
        entries.append(qMakePair(it.key(), it.value()));
    }
    std::sort(entries.begin(), entries.end(),
              [](const QPair<QString, SeenState>& a, const QPair<QString, SeenState>& b) {
                  return a.second.when > b.second.when;
              });
    QJsonArray seenArray;
    for (qsizetype i = 0; i < entries.size() && i < kMaxSeenEntries; i++) {
        const SeenState& state = entries.at(i).second;
        QJsonObject obj;
        obj.insert(QLatin1String("key"), entries.at(i).first);
        obj.insert(QLatin1String("when"), state.when);
        obj.insert(QLatin1String("end"), state.end);
        obj.insert(QLatin1String("src"), static_cast<qint64>(state.src));
        obj.insert(QLatin1String("enc"), state.enc);
        seenArray.append(obj);
    }
    return seenArray;
}

void
CallHistoryModel::saveNow() const {
    json_store_save_array(QLatin1String(kStoreFileName), rowsToJson());
    json_store_save_array(QLatin1String(kSeenStoreFileName), seenToJson());
}

void
CallHistoryModel::startAsyncSave() {
    if (m_saveInFlight) {
        // The worker is mid-write with an older frame; remember that the stores
        // moved again rather than race a second writer onto the same files.
        m_saveDirty = true;
        return;
    }
    m_saveInFlight = true;
    m_saveDirty = false;
    const QJsonArray rows = rowsToJson();
    const QJsonArray seen = seenToJson();
    m_savePool.start([this, rows, seen]() {
        json_store_save_array(QLatin1String(kStoreFileName), rows);
        json_store_save_array(QLatin1String(kSeenStoreFileName), seen);
        // Back to the GUI thread; the destructor's waitForDone() keeps `this`
        // alive for the worker's whole run.
        QMetaObject::invokeMethod(this, &CallHistoryModel::onSaveFinished, Qt::QueuedConnection);
    });
}

void
CallHistoryModel::onSaveFinished() {
    m_saveInFlight = false;
    if (m_saveDirty) {
        m_saveDirty = false;
        scheduleSave();
    }
}

} // namespace dsd_qt
