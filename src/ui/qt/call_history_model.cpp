// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QChar>
#include <QDebug>
#include <dsd-neo/core/receive_types.h>
#include <type_traits>
#include "call_history_model.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QLocale>
#include <QMap>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QStringView>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <algorithm>
#include <dsd-neo/app_control/access_code_view.h>
#include <dsd-neo/app_control/site_identity_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/analog_mode.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/received_identity_site.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <iterator>
#include <limits>
#include <stddef.h>
#include <stdint.h>
#include <utility>

#ifdef DSD_NEO_TEST_HOOKS
#include <QThread>
#include <atomic>
#endif

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/analog_tones.h>
#include "call_history_heard.h"
#include "call_history_merge.h"
#include "call_history_payload.h"
#include "call_history_tap.h"
#include "call_recording_provider.h"
#include "dsd-neo/core/state_fwd.h"
#include "heard_talkgroup_sink.h"
#include "heard_talkgroup_store.h"
#include "json_store.h"
#include "local_day_watch.h"
#include "position_kind_text.h"
#include "teardown_trace.h"

namespace dsd_qt {

namespace {

constexpr const char kStoreFileName[] = "call_history.json";
constexpr const char kSeenStoreFileName[] = "call_history_seen.json";
/* What the background tap found while Qt was paused (issue #708, B1): one JSON line per row, appended. */
constexpr const char kSpillFileName[] = "call_history_spill.jsonl";
constexpr const char kNextUidKey[] = "callHistory/nextUid";
constexpr const char kSessionUidKey[] = "callHistory/sessionUid";
constexpr const char kSessionLabelKey[] = "callHistory/sessionLabel";
constexpr const char kSessionKey[] = "callHistory/session";
/* The clear mark: callHistory/clear/{session,pending,pushSeq0,pushSeq1,ring}, absent when none applies. */
constexpr const char kClearGroup[] = "callHistory/clear";
constexpr const char kClearSessionKey[] = "callHistory/clear/session";
constexpr const char kClearPendingKey[] = "callHistory/clear/pending";
constexpr const char kClearPushSeqKey[2][32] = {"callHistory/clear/pushSeq0", "callHistory/clear/pushSeq1"};
/* Absent in a mark written before marks named their ring, which then reads 0. */
constexpr const char kClearRingKey[] = "callHistory/clear/ring";
/* Issue #800: the session ordinals given to rings (SessionOrdinals): the next to give, and "ring:ordinal" pairs, the
   ring in hex (a nonce can use all 64 bits). */
constexpr const char kSessionOrdNextKey[] = "callHistory/sessionOrd/next";
constexpr const char kSessionOrdRingsKey[] = "callHistory/sessionOrd/rings";
/* The ordinals set aside for each start of the background tap: it gives one to each new ring it meets while the app is
   away, which is rarely more than one. Ordinals are 64-bit, so the unused rest of a block costs nothing. */
constexpr qint64 kTapOrdinalBlock = 1024;
/* Issue #903: who owns each recent session's calls and the rings bound to them (HeardOwners), as JSON text. */
constexpr const char kHeardOwnersKey[] = "callHistory/heardOwners";
/* Older builds persisted Clear as a decode-time watermark; read once to retire it. */
constexpr const char kLegacyClearedThroughKey[] = "callHistory/clearedThrough";
constexpr int kMaxRows = 1000;
/* How far past kMaxRows a spill fold lets the log grow before it trims. A long stretch in the background spills far
 * more rows than the log keeps; trimmed as they fold, the log never holds the whole backlog, each ingest scans no
 * more than a live one, and each trim's pass is shared by this many rows. */
constexpr int kSpillFoldTrimSlack = kMaxRows / 4;
/* Bound on the persisted seen map. The ring holds at most DSD_EVENT_HISTORY_LEN-1
 * rows per slot, so anything beyond the newest ~4x that can no longer be
 * re-ingested and is dead weight in the store. */
constexpr int kMaxSeenEntries = 2048;
constexpr int kSaveDelayMs = 3000;
/* Each save rewrites both stores whole, most of a megabyte at a full log. On a busy system a fixed
 * delay rewrites that every few seconds, about a gigabyte of flash an hour. The delay after a save
 * therefore stretches to what writing its bytes at this rate takes: a full log saves about once a
 * minute, while a small one keeps kSaveDelayMs. */
constexpr qint64 kSaveBudgetBytesPerSec = 16LL * 1024;
/* A save that failed (a full disk) is retried without waiting for another change, kSaveDelayMs after the first
 * failure and twice as long after each further one, up to this. */
constexpr int kSaveRetryMaxMs = 3 * 60 * 1000;
/* The `pv` a stored fragment part (row_parts_to_json()) carries when it holds its sender, call identity, label and
   message (issue #800); parts an earlier build stored have none. */
constexpr int kPartVersionSender = 2;

/* How far down the log a live fragment looks for the call it continues: fragments arrive while
 * their call is among the newest rows. */
constexpr int kMergeScanRows = 32;

#ifdef DSD_NEO_TEST_HOOKS
std::atomic<void (*)(const QThread*)> g_store_build_observer{nullptr};
/* The fragment parts the part lookups read or index (PartLookup, part_index(), fragment_withdrawn(),
 * recording_fragments()), for the cost of a fold and of the recording roles. */
std::atomic<qint64> g_part_lookup_steps{0};
/* The lines the next spill fold reads before its read fails (-1: it does not). */
std::atomic<int> g_spill_read_fail_after{-1};

void
note_store_build() {
    if (auto* observer = g_store_build_observer.load()) {
        observer(QThread::currentThread());
    }
}
#else
void
note_store_build() {}
#endif

/** @brief An analog row's name when nothing else names it (issue #667): its mode, "Analog" when not known. */
QString
analog_fallback_name(uint8_t mode) {
    return dsd_analog_mode_known(mode) != 0 ? QString::fromLatin1(dsd_analog_mode_name(mode))
                                            : QStringLiteral("Analog");
}

/** @brief End of a logged row on the shared timeline: its end when known, else its start. */
qint64
row_end_secs(const CallHistoryModel::Row& row) {
    return row.end > 0 ? row.end : row.when;
}

/**
 * @brief The end a merge matches a row by: its end while its span is plausible (a known duration), else its start.
 *
 * A span past the cap is a clock-shifted row or a chain of fragments longer than an hour. Matching by its stored end
 * would let a clock-shifted row absorb every later call of its talkgroup and source inside a span of hours, so it
 * matches from its start, as a row of unknown duration always has. A real chain longer than an hour therefore
 * continues in a new row once it crosses the cap. foldRow() still takes the real end (row_end_secs()), so the
 * row's duration stays unknown and the stored end stays real.
 */
qint64
row_merge_end_secs(const CallHistoryModel::Row& row) {
    return row.durationSecs >= 0 ? row_end_secs(row) : row.when;
}

/**
 * @brief Whether a (session, start) pair is older in retention order than another.
 *
 * What a full log gives up first, and what the seen map forgets first: the oldest session,
 * then within it the oldest start. The stamps alone cannot order the log. A replay's calls
 * are stamped with the capture's time, and ranking by them would drop the session running now.
 */
bool
retained_older(qint64 session, qint64 when, qint64 otherSession, qint64 otherWhen) {
    return session != otherSession ? session < otherSession : when < otherWhen;
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

/**
 * @brief "TODAY" / "YESTERDAY" / "MON 3 AUG" for the list's day sections, with the year outside @p today's ("SAT 21
 * NOV 2015"); "TIME UNKNOWN" for a row with no start.
 *
 * A deliberate real-time exception to the rule that decoded stamps are compared on the decode
 * clock. The log spans sessions, and its sections name the viewer's calendar day, which the shared
 * LocalDayWatch publishes and retires. On the decode clock a week-old replay's calls would read
 * "TODAY" and the labels would never roll over on time. The label is the section key, so one date
 * in two years needs the year to stay two sections.
 */
QString
day_label(qint64 when, QDate today) {
    if (when <= 0) {
        // No start stamp: the detail sheet and the export leave its time out, so the list makes up no date.
        return QStringLiteral("TIME UNKNOWN");
    }
    const QDate day = QDateTime::fromSecsSinceEpoch(when).date();
    if (day == today) {
        return QStringLiteral("TODAY");
    }
    if (day == today.addDays(-1)) {
        return QStringLiteral("YESTERDAY");
    }
    const QString format = day.year() == today.year() ? QStringLiteral("ddd d MMM") : QStringLiteral("ddd d MMM yyyy");
    return QLocale().toString(day, format).toUpper();
}

/** @brief "12:04" for a row's time in the list; empty for a row with no start, as day_label() names no day. */
QString
time_text(qint64 when) {
    return when > 0 ? QDateTime::fromSecsSinceEpoch(when).toString(QStringLiteral("HH:mm")) : QString();
}

/** @brief The wait before the next debounced save, once the last save wrote @p bytes. */
int
save_delay_ms(qint64 bytes) {
    const qint64 budgetMs = bytes * 1000 / kSaveBudgetBytesPerSec;
    return static_cast<int>(qMin<qint64>(qMax<qint64>(kSaveDelayMs, budgetMs), std::numeric_limits<int>::max()));
}

/** @brief The wait before retrying a save after @p failures failed in a row. */
int
save_retry_delay_ms(int failures) {
    qint64 delay = kSaveDelayMs;
    for (int i = 1; i < failures && delay < kSaveRetryMaxMs; i++) {
        delay *= 2;
    }
    return static_cast<int>(qMin<qint64>(delay, kSaveRetryMaxMs));
}

} // namespace

CallHistoryModel::CallHistoryModel(QObject* parent) : QAbstractListModel(parent) {
    /* Restored before the first ingest: after an Activity restart the service's
     * session is still decoding, and its backlog lands before any start button is
     * pressed. Without the persisted label those rows would be attributed to "". */
    m_sessionLabel = m_settings.value(QLatin1String(kSessionLabelKey)).toString();
    m_sessionUid = m_settings.value(QLatin1String(kSessionUidKey)).toString();
    /* Restored for the same reason: the relaunched UI rejoins the running session, and
     * Clear must survive an Activity restart while the service's ring still holds the
     * cleared rows. */
    loadSessionState();
    loadSessionOrdinals();
    // Before the spill folds below: its rows carry their owners, and the rings they bound go with them.
    m_heardOwners = heard_owners_from_text(m_settings.value(QLatin1String(kHeardOwnersKey)).toString());
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, [this]() { startAsyncSave(); });
    /* One worker: saves must not overlap (two writers racing on the same
     * QSaveFile target), and a second thread would buy nothing for two files. */
    m_savePool.setMaxThreadCount(1);
    /* The save delay can be a minute long, and these are the moments it may never run out: Android
     * can kill a process off screen without notice, and a quit ends the event loop the timer needs. */
    if (QCoreApplication* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, &CallHistoryModel::saveNow);
    }
    if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, &CallHistoryModel::onApplicationStateChanged);
    }
    /* A view following one row by its uid (the detail sheet) looks it up again whenever rows move. */
    connect(this, &QAbstractItemModel::rowsInserted, this, &CallHistoryModel::rowPositionsChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &CallHistoryModel::rowPositionsChanged);
    connect(this, &QAbstractItemModel::rowsMoved, this, &CallHistoryModel::rowPositionsChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &CallHistoryModel::rowPositionsChanged);
    m_spill = std::make_unique<CallHistorySpill>(json_store_path(QLatin1String(kSpillFileName)));
    /* Day sections are derived from the watch's day at read time; when it changes, every
     * "TODAY" on screen is wrong until the rows are re-read. The clock text goes with them: a
     * day change can be a zone change that moved midnight, and a row's section and time must
     * come from one zone. (No application, no watch: both are read as each is asked for.) */
    if (const LocalDayWatch* watch = LocalDayWatch::shared()) {
        connect(watch, &LocalDayWatch::dayChanged, this, [this]() {
            if (!m_rows.isEmpty()) {
                Q_EMIT dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1), {DayLabelRole, TimeTextRole});
            }
        });
    }
    load();
}

void
CallHistoryModel::loadSessionState() {
    m_session = qMax<qint64>(m_settings.value(QLatin1String(kSessionKey)).toLongLong(), 0);
    m_clear.active = m_settings.contains(QLatin1String(kClearSessionKey));
    if (m_clear.active) {
        m_clear.session = m_settings.value(QLatin1String(kClearSessionKey)).toLongLong();
        m_clear.pending = m_settings.value(QLatin1String(kClearPendingKey)).toBool();
        for (int slot = 0; slot < 2; slot++) {
            m_clear.pushSeq[slot] = m_settings.value(QLatin1String(kClearPushSeqKey[slot])).toULongLong();
        }
        m_clear.ring = m_settings.value(QLatin1String(kClearRingKey)).toULongLong();
    }
    /* A build that stamped Clear with the decode time left this watermark. Nothing it guarded
     * is left. The log rows it cleared were deleted from the store, and the ring rows it held
     * back died with the process an app update replaces. Carried over, it would hide every
     * later call stamped before it, such as a replay of an older capture. So it is removed,
     * not converted: a clear mark names positions in a running ring, and none of its is running. */
    if (m_settings.contains(QLatin1String(kLegacyClearedThroughKey))) {
        m_settings.remove(QLatin1String(kLegacyClearedThroughKey));
    }
}

qint64
CallHistoryModel::sessionOrdinal(quint64 ring) {
    const qint64 next = m_sessionOrdinals.next;
    const qint64 ordinal = sessionOrdinalFor(m_sessionOrdinals, ring);
    if (m_sessionOrdinals.next != next) {
        saveSessionOrdinals();
    }
    return ordinal;
}

bool
CallHistoryModel::noteSessionOrdinal(quint64 ring, qint64 ordinal) {
    if (ring == 0U || ordinal < 1 || m_sessionOrdinals.rings.contains(ring)) {
        return false;
    }
    m_sessionOrdinals.rings.insert(ring, ordinal);
    if (ordinal < std::numeric_limits<qint64>::max()) {
        m_sessionOrdinals.next = qMax(m_sessionOrdinals.next, ordinal + 1);
    }
    return true;
}

void
CallHistoryModel::adoptSessionOrdinals(const QHash<quint64, qint64>& ordinals) {
    bool changed = false;
    for (auto it = ordinals.constBegin(); it != ordinals.constEnd(); ++it) {
        changed = noteSessionOrdinal(it.key(), it.value()) || changed;
    }
    if (changed) {
        bound_session_ordinals(m_sessionOrdinals.rings);
        saveSessionOrdinals();
    }
}

void
CallHistoryModel::adoptSessionOrdinal(quint64 ring, qint64 ordinal) {
    if (noteSessionOrdinal(ring, ordinal)) {
        bound_session_ordinals(m_sessionOrdinals.rings);
        saveSessionOrdinals();
    }
}

void
CallHistoryModel::saveSessionOrdinals() {
    QStringList rings;
    for (auto it = m_sessionOrdinals.rings.constBegin(); it != m_sessionOrdinals.rings.constEnd(); ++it) {
        rings.append(QString::number(it.key(), 16) + QLatin1Char(':') + QString::number(it.value()));
    }
    m_settings.setValue(QLatin1String(kSessionOrdNextKey), m_sessionOrdinals.next);
    m_settings.setValue(QLatin1String(kSessionOrdRingsKey), rings);
}

void
CallHistoryModel::loadSessionOrdinals() {
    m_sessionOrdinals = SessionOrdinals();
    m_sessionOrdinals.next = qMax<qint64>(m_settings.value(QLatin1String(kSessionOrdNextKey)).toLongLong(), 1);
    const QStringList rings = m_settings.value(QLatin1String(kSessionOrdRingsKey)).toStringList();
    for (const QString& entry : rings) {
        const qsizetype colon = entry.indexOf(QLatin1Char(':'));
        bool ringOk = false;
        bool ordOk = false;
        const quint64 ring = colon > 0 ? entry.left(colon).toULongLong(&ringOk, 16) : 0U;
        const qint64 ordinal = colon > 0 ? entry.mid(colon + 1).toLongLong(&ordOk) : 0;
        if (ringOk && ordOk && ring != 0U && ordinal >= 1) {
            m_sessionOrdinals.rings.insert(ring, ordinal);
            if (ordinal < std::numeric_limits<qint64>::max()) {
                m_sessionOrdinals.next = qMax(m_sessionOrdinals.next, ordinal + 1);
            }
        }
    }
    bound_session_ordinals(m_sessionOrdinals.rings);
}

CallHistoryModel::~CallHistoryModel() {
    const TeardownStage stage("~CallHistoryModel");
    /* The tap writes what it holds to the spill as it stops; the next model folds it in. What it could not write (the
     * spill still unwritable) only this model has: it goes into the log before the last save below, as the handoff
     * takes it. */
    if (m_tap && m_tap->running()) {
        bool folded = false;
        (void)foldUnwritten(stopBackgroundCapture(), &folded);
        if (folded) {
            (void)trimToCapacity();
            pruneSeen();
            markChanged();
        }
    }
    /* A debounced or coalesced save may still be owed; the queued ones (if any)
     * already carry the stores they copied. Wait them out, then flush what remains
     * synchronously — the worker must never outlive this object. */
    const bool owedSave = m_saveTimer.isActive() || m_saveDirty;
    m_saveTimer.stop();
    m_savePool.waitForDone();
    if (owedSave) {
        saveSync();
    }
}

int
CallHistoryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

namespace {

/** Identity roles shared by the history views and their saved row details. */
QVariant
row_identity_value(const CallHistoryModel::Row& row, int role) {
    switch (role) {
        case CallHistoryModel::NameRole: return row.name;
        case CallHistoryModel::TgRole: return row.tg;
        case CallHistoryModel::GiRole: return static_cast<int>(row.gi);
        case CallHistoryModel::SrcRole: return row.src;
        case CallHistoryModel::SourceNameRole: return row.sourceName;
        case CallHistoryModel::SystemNameRole: return row.systemName;
        case CallHistoryModel::SystemUidRole: return row.systemUid;
        default: return QVariant();
    }
}

/** @brief Whether a kind and value fit the fields a ring row carries them in (uint8_t, uint16_t). */
bool
access_code_fits(qint64 kind, qint64 code) {
    return kind >= 0 && kind <= UINT8_MAX && code >= 0 && code <= UINT16_MAX;
}

/** @brief The row's site line (issue #799): its site in the Monitor's words, then the call's channel. */
QString
row_site_text(const CallHistoryModel::Row& row) {
    char text[DSD_APP_SITE_HISTORY_TEXT_SIZE];
    (void)dsd_app_site_history_text(&row.siteId, row.callChannelKind, row.callChannel, text, sizeof(text));
    return QString::fromUtf8(text);
}

/**
 * @brief A row's saved fix as the map's "Show on map" pin reads it (issue #800): {latitude, longitude, errorM,
 * errorLowerBound, validityUnknown, provisional, weakCheck, anonymous, emergency, targetGroup, kindText, receivedTime},
 * or an empty map when the row has none. targetGroup: the report named a talkgroup as its target (the row's target can
 * be a radio, a private call's). errorM and receivedTime are left out when not known (0). Degrees are for display only.
 */
QVariantMap
position_record(const CallHistoryModel::Position& pos) {
    QVariantMap out;
    if (!pos.valid) {
        return out;
    }
    const auto flag = [&pos](unsigned bit) { return (pos.flags & bit) != 0U; };
    out.insert(QStringLiteral("latitude"), static_cast<double>(pos.latE7) / 1.0e7);
    out.insert(QStringLiteral("longitude"), static_cast<double>(pos.lonE7) / 1.0e7);
    if (pos.errM > 0U) {
        out.insert(QStringLiteral("errorM"), pos.errM);
    }
    out.insert(QStringLiteral("errorLowerBound"), flag(DSD_POS_FLAG_ERR_LOWER_BOUND));
    out.insert(QStringLiteral("validityUnknown"), flag(DSD_POS_FLAG_VALIDITY_UNKNOWN));
    out.insert(QStringLiteral("provisional"), flag(DSD_POS_FLAG_FORMAT_PROVISIONAL));
    out.insert(QStringLiteral("weakCheck"), flag(DSD_POS_FLAG_WEAK_CHECK));
    out.insert(QStringLiteral("anonymous"),
               pos.anonymous || flag(DSD_POS_FLAG_SOURCE_HASHED) || flag(DSD_POS_FLAG_SOURCE_UNVERIFIED));
    out.insert(QStringLiteral("emergency"), flag(DSD_POS_FLAG_EMERGENCY));
    out.insert(QStringLiteral("targetGroup"), flag(DSD_POS_FLAG_TARGET_GROUP));
    out.insert(QStringLiteral("kindText"), position_kind_text(pos.kind));
    if (pos.rxTimeS > 0) {
        out.insert(QStringLiteral("receivedTime"), static_cast<double>(pos.rxTimeS));
    }
    return out;
}

/** @brief The frequency, access-code, site and position roles. Their texts are app-control's, spelled at read time. */
QVariant
row_carrier_value(const CallHistoryModel::Row& row, int role) {
    switch (role) {
        case CallHistoryModel::FreqHzRole: return row.freqHz;
        case CallHistoryModel::AccessCodeRole:
        case CallHistoryModel::AccessCodeTextRole: {
            // Only the text the role asks for: a delegate reads each role on its own.
            char text[sizeof(dsd_app_access_code::long_text)];
            const bool fits = access_code_fits(row.codeKind, row.code);
            const uint8_t kind = fits ? static_cast<uint8_t>(row.codeKind) : static_cast<uint8_t>(DSD_ACCESS_CODE_NONE);
            const int form =
                role == CallHistoryModel::AccessCodeRole ? DSD_APP_ACCESS_CODE_SHORT : DSD_APP_ACCESS_CODE_LONG;
            dsd_app_access_code_format(kind, fits ? static_cast<uint32_t>(row.code) : 0U, form, text, sizeof(text));
            return QString::fromLatin1(text);
        }
        case CallHistoryModel::SiteTextRole: return row_site_text(row);
        case CallHistoryModel::PositionRole: return position_record(row.pos);
        default: return row_identity_value(row, role);
    }
}

/** @brief The confirmed tone in the words every surface uses ("CTCSS 100.0 Hz", "DCS D023N / D047I"), else empty. */
QString
row_received_tone(const CallHistoryModel::Row& row) {
    constexpr int kToneSize = std::max(static_cast<int>(DSD_CTCSS_LABEL_SIZE), static_cast<int>(DSD_DCS_LABEL_SIZE));
    char tone[kToneSize] = "";
    if (row.receiveKnown && row.receiveKind == DSD_RECEIVE_CTCSS) {
        (void)dsd_ctcss_format_label(row.receiveValue, tone, sizeof tone);
    } else if (row.receiveKnown && row.receiveKind == DSD_RECEIVE_DCS) {
        // The stored value is the canonical code * 2 + polarity (receive_types.h).
        (void)dsd_dcs_format_label(row.receiveValue / 2, row.receiveValue % 2, tone, sizeof tone);
    }
    return QString::fromUtf8(tone);
}

/** @brief An analog row's mode as the analogMode role reads it: "NFM", "AM", or empty when unknown or not analog. */
QString
row_analog_mode_text(const CallHistoryModel::Row& row) {
    return row.analog && dsd_analog_mode_known(row.analogMode) != 0
               ? QString::fromLatin1(dsd_analog_mode_name(row.analogMode))
               : QString();
}

/** @brief One row's value for @p role; an unknown role reads as null. */
QVariant
row_role_value(const CallHistoryModel::Row& row, int role) {
    switch (role) {
        case CallHistoryModel::EmergencyRole: return row.emergency;
        case CallHistoryModel::EncRole: return row.enc;
        case CallHistoryModel::WhenRole: return row.when;
        case CallHistoryModel::DurationSecsRole: return row.durationSecs;
        case CallHistoryModel::DayLabelRole: return day_label(row.when, LocalDayWatch::current().date);
        case CallHistoryModel::TimeTextRole: return time_text(row.when);
        case CallHistoryModel::KindRole: return row.kind;
        case CallHistoryModel::DetailRole: return row_detail_text(row);
        case CallHistoryModel::ChannelRole: return row.channel;
        case CallHistoryModel::ServiceNameRole: return row.service;
        case CallHistoryModel::ReceivedToneRole: {
            return row_received_tone(row);
        }
        case CallHistoryModel::AnalogRole: return row.analog;
        case CallHistoryModel::AnalogModeRole: return row_analog_mode_text(row);
        case CallHistoryModel::SessionRole: return row.session;
        default: return row_carrier_value(row, role);
    }
}

/** @brief Whether fragment @p fragment's part in @p row is withdrawn (issue #800): its reading named another call than
 * the row's, so the row keeps its key only to know its later readings. For one lookup; recording_fragments() for all. */
bool
fragment_withdrawn(const CallHistoryModel::Row& row, const QString& fragment) {
    return std::any_of(row.parts.cbegin(), row.parts.cend(), [&fragment](const CallHistoryModel::FragmentPart& part) {
#ifdef DSD_NEO_TEST_HOOKS
        g_part_lookup_steps.fetch_add(1, std::memory_order_relaxed);
#endif
        return part.withdrawn && part.fragment == fragment;
    });
}

/** @brief The fragments whose recordings are @p row's: every fragment it holds but the withdrawn ones, whose
 * recordings belong to another call. Its parts are read once, whatever the number of fragments: both recording roles
 * ask on the GUI thread, for a long call of many short fragments too. */
QStringList
recording_fragments(const CallHistoryModel::Row& row) {
    QSet<QString> withdrawn;
    for (const CallHistoryModel::FragmentPart& part : row.parts) {
#ifdef DSD_NEO_TEST_HOOKS
        g_part_lookup_steps.fetch_add(1, std::memory_order_relaxed);
#endif
        if (part.withdrawn) {
            withdrawn.insert(part.fragment);
        }
    }
    if (withdrawn.isEmpty()) {
        return row.fragments;
    }
    QStringList fragments;
    fragments.reserve(row.fragments.size());
    for (const QString& fragment : row.fragments) {
        if (!withdrawn.contains(fragment)) {
            fragments.append(fragment);
        }
    }
    return fragments;
}

} // namespace

QVariant
CallHistoryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
        case UidRole: return row.uid;
        // A trunk-scan target's id is the app's uid for its entry: shown as the entry's name (setChannelNames()).
        case NameRole: return !row.channel.isEmpty() && row.name == row.channel ? channelDisplayName(row) : row.name;
        case ChannelRole: return channelDisplayName(row);
        case RecordingsRole: {
            const QStringList fragments = m_recordings != nullptr ? recording_fragments(row) : QStringList();
            return !fragments.isEmpty() ? m_recordings->recordingsForFragments(fragments) : QVariantList();
        }
        case HasRecordingRole: {
            const QStringList fragments = m_recordings != nullptr ? recording_fragments(row) : QStringList();
            return !fragments.isEmpty() && m_recordings->hasRecordingForFragments(fragments);
        }
        default: return row_role_value(row, role);
    }
}

int
CallHistoryModel::rowForUid(qint64 uid) const {
    if (uid <= 0) {
        return -1;
    }
    for (int i = 0; i < m_rows.size(); i++) {
        if (m_rows.at(i).uid == uid) {
            return i;
        }
    }
    return -1;
}

QVariantList
CallHistoryModel::recordingsForUid(qint64 uid) const {
    const int row = rowForUid(uid);
    return row >= 0 ? data(index(row), RecordingsRole).toList() : QVariantList();
}

double
CallHistoryModel::uidForFragment(const QString& fragment) const {
    const auto it = m_fragmentIndex.constFind(fragment);
    if (it == m_fragmentIndex.constEnd()) {
        return 0.0;
    }
    // A withdrawn fragment is another call's: the row only keeps its key, so its recording names no row.
    const int row = rowForUid(it.value());
    return row >= 0 && !fragment_withdrawn(m_rows.at(row), fragment) ? static_cast<double>(it.value()) : 0.0;
}

QVariantMap
CallHistoryModel::recordFor(const QString& uid) const {
    // The keys the detail sheet's entry points have always passed, each read through its role as their delegates did.
    static const struct {
        const char* key;
        int role;
    } kRecordRoles[] = {
        {"uid", UidRole},
        {"name", NameRole},
        {"when", WhenRole},
        {"systemName", SystemNameRole},
        {"systemUid", SystemUidRole},
        {"channel", ChannelRole},
        {"tg", TgRole},
        {"src", SrcRole},
        {"sourceName", SourceNameRole},
        {"emergency", EmergencyRole},
        {"enc", EncRole},
        {"durationSecs", DurationSecsRole},
        {"detail", DetailRole},
        {"freqHz", FreqHzRole},
        {"accessCode", AccessCodeRole},
        {"accessCodeText", AccessCodeTextRole},
        {"serviceName", ServiceNameRole},
        {"receivedTone", ReceivedToneRole},
        {"siteText", SiteTextRole},
        {"analog", AnalogRole},
        {"analogMode", AnalogModeRole},
        {"position", PositionRole},
        {"gi", GiRole},
    };

    bool parsed = false;
    const qint64 id = uid.toLongLong(&parsed);
    const int row = parsed ? rowForUid(id) : -1;
    QVariantMap record;
    if (row < 0) {
        return record;
    }
    const QModelIndex idx = index(row);
    for (const auto& entry : kRecordRoles) {
        record.insert(QLatin1String(entry.key), data(idx, entry.role));
    }
    return record;
}

namespace {

/** @brief The row at @p idx as an export writes it, every value through data(), recordings left to the caller. */
CallHistoryModel::ExportRow
export_row_at(const CallHistoryModel& model, const QModelIndex& idx) {
    const auto text = [&model, &idx](int role) { return model.data(idx, role).toString(); };
    CallHistoryModel::ExportRow row;
    row.when = model.data(idx, CallHistoryModel::WhenRole).toLongLong();
    row.durationSecs = model.data(idx, CallHistoryModel::DurationSecsRole).toInt();
    row.kind = model.data(idx, CallHistoryModel::KindRole).toInt();
    row.name = text(CallHistoryModel::NameRole);
    row.tg = model.data(idx, CallHistoryModel::TgRole).toULongLong();
    row.gi = model.data(idx, CallHistoryModel::GiRole).toInt();
    row.src = model.data(idx, CallHistoryModel::SrcRole).toULongLong();
    row.sourceName = text(CallHistoryModel::SourceNameRole);
    row.systemName = text(CallHistoryModel::SystemNameRole);
    row.channel = text(CallHistoryModel::ChannelRole);
    row.freqHz = model.data(idx, CallHistoryModel::FreqHzRole).toLongLong();
    row.accessCode = text(CallHistoryModel::AccessCodeRole);
    row.receivedTone = text(CallHistoryModel::ReceivedToneRole);
    row.siteText = text(CallHistoryModel::SiteTextRole);
    row.analog = model.data(idx, CallHistoryModel::AnalogRole).toBool();
    row.analogMode = text(CallHistoryModel::AnalogModeRole);
    row.enc = model.data(idx, CallHistoryModel::EncRole).toBool();
    row.emergency = model.data(idx, CallHistoryModel::EmergencyRole).toBool();
    row.detail = text(CallHistoryModel::DetailRole);
    return row;
}

} // namespace

QList<CallHistoryModel::ExportRow>
CallHistoryModel::exportRows(const QList<int>& sourceRows) const {
    QList<ExportRow> rows;
    QList<QStringList> fragments;
    rows.reserve(sourceRows.size());
    fragments.reserve(sourceRows.size());
    for (const int source : sourceRows) {
        if (source < 0 || source >= m_rows.size()) {
            continue;
        }
        rows.append(export_row_at(*this, index(source)));
        fragments.append(recording_fragments(m_rows.at(source)));
    }
    if (m_recordings == nullptr) {
        return rows;
    }
    // One read for every row: the store takes its lock once. An answer that does not cover every row names none.
    const QList<QStringList> names = m_recordings->recordingEntryNames(fragments);
    if (names.size() == rows.size()) {
        for (qsizetype i = 0; i < rows.size(); i++) {
            rows[i].recordingNames = names.at(i);
        }
    }
    return rows;
}

QList<CallHistoryModel::PlaylistRow>
CallHistoryModel::playlistRows(const QList<int>& sourceRows) const {
    QList<PlaylistRow> rows;
    QList<QStringList> fragments;
    rows.reserve(sourceRows.size());
    fragments.reserve(sourceRows.size());
    for (const int source : sourceRows) {
        if (source < 0 || source >= m_rows.size()) {
            continue;
        }
        const QModelIndex idx = index(source);
        PlaylistRow row;
        row.uid = m_rows.at(source).uid;
        row.name = data(idx, NameRole).toString();
        row.when = data(idx, WhenRole).toLongLong();
        rows.append(row);
        fragments.append(recording_fragments(m_rows.at(source)));
    }
    if (m_recordings == nullptr) {
        return rows;
    }
    // One read for every row: the store takes its lock once. An answer that does not cover every row lists none.
    const QList<QVariantList> recordings = m_recordings->recordingsForFragmentLists(fragments);
    if (recordings.size() == rows.size()) {
        for (qsizetype i = 0; i < rows.size(); i++) {
            rows[i].recordings = recordings.at(i);
        }
    }
    return rows;
}

void
CallHistoryModel::setRecordingProvider(const CallRecordingProvider* provider) {
    if (provider == m_recordings) {
        return;
    }
    m_recordings = provider;
    if (!m_rows.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1), {RecordingsRole, HasRecordingRole});
    }
}

void
CallHistoryModel::recordingFragmentsChanged(const QStringList& fragments) {
    notifyRecordingRows(fragments);
}

void
CallHistoryModel::notifyRecordingRows(const QStringList& fragments) {
    QSet<qint64> uids;
    for (const QString& fragment : fragments) {
        const auto it = m_fragmentIndex.constFind(fragment);
        if (it != m_fragmentIndex.constEnd()) {
            uids.insert(it.value());
        }
    }
    if (uids.isEmpty()) {
        // A recording whose row has not arrived yet: the row reads it when it does.
        return;
    }
    for (int i = 0; i < m_rows.size(); i++) {
        if (uids.contains(m_rows.at(i).uid)) {
            const QModelIndex idx = index(i);
            Q_EMIT dataChanged(idx, idx, {RecordingsRole, HasRecordingRole});
        }
    }
}

namespace {

/* Note one fragment of a heard talkgroup (its time and serial) in @p heard. */
void
note_heard_fragment(QHash<uint32_t, HeardTalkgroupRow>& heard, uint32_t tg, qint64 when, qint64 end, quint64 serial) {
    HeardTalkgroupRow& row = heard[tg];
    row.id = tg;
    row.last = qMax(row.last, end > 0 ? end : when);
    row.serial = qMax(row.serial, serial);
}

/* Whether @p row is a talkgroup call heard on scan target @p target's channel. A private call's target is a radio,
   never a talkgroup heard (issue #800); not knowing stays as before. A row keeps the scan target it was heard on, which
   no merge crosses (rows_mergeable()). */
bool
heard_talkgroup_row(const CallHistoryModel::Row& row, const HeardTarget& target) {
    return row.kind == CallHistoryModel::KindVoice && row.tg != 0U && row.tg <= UINT32_MAX && row.gi != 1
           && row.channel == target.channel;
}

/* Whether a fragment heard on -Y scan-list row @p scanListRow of list @p scanListRev (0 and 0: off any -Y row) was heard
   on @p target: a -Y row by the row itself, never its name or frequency alone, and off any -Y row only off one (issue
   #903). */
bool
heard_on_target(const HeardTarget& target, quint32 scanListRow, quint64 scanListRev) {
    return scanListRow == target.scanListRow && (scanListRow == 0U || scanListRev == target.scanListRev);
}

} // namespace

QList<HeardTalkgroupRow>
CallHistoryModel::heardTalkgroups(qint64 forSession, const QString& owner, const HeardTarget& target) const {
    QHash<uint32_t, HeardTalkgroupRow> heard;
    for (const Row& row : m_rows) {
        if (!heard_talkgroup_row(row, target)) {
            continue;
        }
        const auto tg = static_cast<uint32_t>(row.tg);
        // Issue #903: each fragment by its own session, owner and -Y row; a row of one fragment a reload gave is its own
        // part.
        if (row.parts.isEmpty()) {
            if ((forSession <= 0 || row.session == forSession) && row.viewOwner == owner
                && heard_on_target(target, row.scanListRow, row.scanListRev)) {
                note_heard_fragment(heard, tg, row.when, row.end, row.heardSerial);
            }
            continue;
        }
        for (const FragmentPart& part : row.parts) {
            if (!part.withdrawn && (forSession <= 0 || part.session == forSession) && part.viewOwner == owner
                && heard_on_target(target, part.scanListRow, part.scanListRev)) {
                note_heard_fragment(heard, tg, part.when, part.end, part.heardSerial);
            }
        }
    }
    return heard.values();
}

QString
CallHistoryModel::heardOwnerOnAir(quint64 ring, bool operatorTuned, bool scanMember, const QString& channel) const {
    // The session a call the ring commits now would be read in (walkRing()).
    return heard_owner_for(m_heardOwners, heard_owners_on_air_session(m_heardOwners, ring, m_session), operatorTuned,
                           scanMember, channel);
}

QHash<int, QByteArray>
CallHistoryModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles.insert(NameRole, QByteArrayLiteral("name"));
    roles.insert(TgRole, QByteArrayLiteral("tg"));
    roles.insert(GiRole, QByteArrayLiteral("gi"));
    roles.insert(SrcRole, QByteArrayLiteral("src"));
    roles.insert(SourceNameRole, QByteArrayLiteral("srcName"));
    roles.insert(EmergencyRole, QByteArrayLiteral("emergency"));
    roles.insert(EncRole, QByteArrayLiteral("enc"));
    roles.insert(WhenRole, QByteArrayLiteral("when"));
    roles.insert(DurationSecsRole, QByteArrayLiteral("durationSecs"));
    roles.insert(SystemNameRole, QByteArrayLiteral("systemName"));
    roles.insert(SystemUidRole, QByteArrayLiteral("systemUid"));
    roles.insert(DayLabelRole, QByteArrayLiteral("dayLabel"));
    roles.insert(TimeTextRole, QByteArrayLiteral("timeText"));
    roles.insert(KindRole, QByteArrayLiteral("kind"));
    roles.insert(DetailRole, QByteArrayLiteral("detail"));
    roles.insert(ChannelRole, QByteArrayLiteral("channel"));
    roles.insert(ServiceNameRole, QByteArrayLiteral("serviceName"));
    roles.insert(ReceivedToneRole, QByteArrayLiteral("receivedTone"));
    roles.insert(SessionRole, QByteArrayLiteral("session"));
    roles.insert(FreqHzRole, QByteArrayLiteral("freqHz"));
    roles.insert(AccessCodeRole, QByteArrayLiteral("accessCode"));
    roles.insert(AccessCodeTextRole, QByteArrayLiteral("accessCodeText"));
    roles.insert(UidRole, QByteArrayLiteral("uid"));
    roles.insert(RecordingsRole, QByteArrayLiteral("recordings"));
    roles.insert(HasRecordingRole, QByteArrayLiteral("hasRecording"));
    roles.insert(SiteTextRole, QByteArrayLiteral("siteText"));
    roles.insert(AnalogRole, QByteArrayLiteral("analog"));
    roles.insert(AnalogModeRole, QByteArrayLiteral("analogMode"));
    roles.insert(PositionRole, QByteArrayLiteral("position"));
    return roles;
}

void
CallHistoryModel::setSessionLabel(const QString& label) {
    changeSession(label, m_sessionUid, false);
}

void
CallHistoryModel::setSessionUid(const QString& uid) {
    changeSession(m_sessionLabel, uid, false);
}

void
CallHistoryModel::beginSession() {
    changeSession(m_sessionLabel, m_sessionUid, true);
}

void
CallHistoryModel::beginSessionOn(const QString& label, const QString& uid) {
    changeSession(label, uid, true);
}

void
CallHistoryModel::holdCaptureForStart() {
    (void)handOffBackgroundCaptureForSession();
    m_captureHeld = true;
    const quint64 hold = ++m_captureHoldRev;
    // A safety release: Main's start runs in one event-loop turn, and a start that began a session consumed the hold.
    QTimer::singleShot(0, this, [this, hold]() {
        if (m_captureHoldRev == hold) {
            releaseCaptureHold();
        }
    });
}

void
CallHistoryModel::releaseCaptureHold() {
    if (!m_captureHeld) {
        return;
    }
    m_captureHeld = false;
    ++m_captureHoldRev;
    resumeBackgroundCapture(m_appAway);
}

void
CallHistoryModel::beginSessionWithOwners(const QString& label, const QString& uid, const QString& singleOwnerUid,
                                         const QVariantMap& scanEntryOwner) {
    HeardSessionOwners who;
    who.single = singleOwnerUid;
    for (auto it = scanEntryOwner.constBegin(); it != scanEntryOwner.constEnd(); ++it) {
        const QString owner = it.value().toString();
        if (!it.key().isEmpty() && !owner.isEmpty()) {
            who.scan.insert(it.key(), owner);
        }
    }
    changeSession(label, uid, true, &who);
}

void
CallHistoryModel::setSessionSystem(const QString& label, const QString& uid) {
    changeSession(label, uid, false);
}

void
CallHistoryModel::changeSession(const QString& label, const QString& uid, bool begin,
                                const HeardSessionOwners* owners) {
    const bool labelMoved = label != m_sessionLabel;
    const bool uidMoved = uid != m_sessionUid;
    if (!labelMoved && !uidMoved && !begin) {
        return;
    }
    // One handoff for every field: a tap started again between them would attribute what it finds to half the change.
    // A start's hold (holdCaptureForStart()) already stopped the tap; the new session takes it over.
    bool resumeTap = handOffBackgroundCaptureForSession();
    if (begin && m_captureHeld) {
        m_captureHeld = false;
        ++m_captureHoldRev;
        resumeTap = m_appAway;
    }
    if (labelMoved) {
        m_sessionLabel = label;
        m_settings.setValue(QLatin1String(kSessionLabelKey), label);
    }
    if (uidMoved) {
        m_sessionUid = uid;
        m_settings.setValue(QLatin1String(kSessionUidKey), uid);
    }
    if (begin) {
        advanceSession();
        // Issue #903: the ring read last, where the start found it: a run that reuses it owns what it pushes after.
        const quint64 through[2] = {m_ringPushSeq[0], m_ringPushSeq[1]};
        heard_owners_mark_start(&m_heardOwners, m_session, m_ringPushSeqRing, through);
        if (owners != nullptr) {
            heard_owners_set_session(&m_heardOwners, m_session, *owners);
        }
        saveHeardOwners();
    }
    resumeBackgroundCapture(resumeTap);
    if (labelMoved) {
        Q_EMIT sessionLabelChanged();
    }
    if (uidMoved) {
        Q_EMIT sessionUidChanged();
    }
    if (begin) {
        Q_EMIT sessionChanged();
    }
}

void
CallHistoryModel::setChannelNames(const QVariantMap& names) {
    // Added to the names given before, never in place of them: another session's names leave the rows an earlier one
    // logged named.
    QHash<QString, QString> next = m_channelNames;
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        const QString name = it.value().toString();
        if (!it.key().isEmpty() && !name.isEmpty()) {
            next.insert(it.key(), name);
        }
    }
    if (next == m_channelNames) {
        return;
    }
    // A display name only: rows keep the target id they were heard on, which tells two targets of one name apart
    // (rows_mergeable()) and outlives a rename.
    m_channelNames = next;
    bool named = false;
    for (Row& row : m_rows) {
        named = nameChannel(row) || named;
    }
    if (named) {
        scheduleSave();
    }
    if (!m_rows.isEmpty()) {
        Q_EMIT dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1), {NameRole, ChannelRole});
    }
}

bool
CallHistoryModel::nameChannel(Row& row) const {
    if (row.channel.isEmpty() || !row.channelName.isEmpty()) {
        return false;
    }
    const auto it = m_channelNames.constFind(row.channel);
    if (it == m_channelNames.constEnd()) {
        return false;
    }
    row.channelName = it.value();
    return true;
}

QString
CallHistoryModel::channelDisplayName(const Row& row) const {
    return m_channelNames.value(row.channel, row.channelName.isEmpty() ? row.channel : row.channelName);
}

void
CallHistoryModel::advanceSession() {
    m_session++;
    m_settings.setValue(QLatin1String(kSessionKey), m_session);
    /* A start usually brings a fresh ring, but not always: an embedding host can run again on the
     * state its last run used (the Android service no longer does: it resets its objects for every
     * configure, android/session_objects.h), and that ring keeps its identity, its rows and its push count. So a clear
     * that names its ring is kept. It goes on covering that ring's cleared rows if the state was
     * reused, and the first read of any other ring drops it (settleClear()). A pending clear, or one
     * that names no ring, would bind to whatever ring comes next and could hide a fresh ring's first
     * rows, so those are dropped here. */
    if (m_clear.active && (m_clear.pending || m_clear.ring == 0U)) {
        m_clear = ClearMark();
        saveClear();
    }
    /* For the same reason the last read position stays: it names the ring it was read in, which a
     * clear before the next read then covers. That is right if the state was reused, and a fresh
     * ring's first read drops such a clear. Every ring is read at its first tick, even at the
     * commit_rev the last one stopped at (noteCommitRevs()), so a quiet session does not delay it.
     * Only a model that has read nothing yet takes the new ring to be at position zero on no ring:
     * a clear before its first read covers nothing, and that read drops it. */
    if (!m_ringRead) {
        m_ringPushSeq[0] = 0U;
        m_ringPushSeq[1] = 0U;
        m_ringPushSeqRing = 0U;
        m_ringRead = true;
    }
}

bool
CallHistoryModel::clearApplies() const {
    // Bound by its ring, whatever the session: a start that reuses the state keeps the ring the clear wiped.
    return m_clear.active && !m_clear.pending;
}

void
CallHistoryModel::saveClear() {
    m_settings.remove(QLatin1String(kClearGroup));
    if (!m_clear.active) {
        return;
    }
    m_settings.setValue(QLatin1String(kClearSessionKey), m_clear.session);
    m_settings.setValue(QLatin1String(kClearPendingKey), m_clear.pending);
    for (int slot = 0; slot < 2; slot++) {
        m_settings.setValue(QLatin1String(kClearPushSeqKey[slot]), m_clear.pushSeq[slot]);
    }
    m_settings.setValue(QLatin1String(kClearRingKey), static_cast<qulonglong>(m_clear.ring));
}

void
CallHistoryModel::settleClear(const qulonglong pushSeq[2], quint64 ring) {
    if (!m_clear.active) {
        return;
    }
    if (m_clear.pending) {
        // Cleared before this model had read the ring, as a relaunched UI can: what the
        // ring holds at the first read is what was wiped.
        m_clear.pending = false;
        m_clear.pushSeq[0] = pushSeq[0];
        m_clear.pushSeq[1] = pushSeq[1];
        m_clear.ring = ring;
        saveClear();
        return;
    }
    // The mark's position is one in the ring the clear was made on. Any other ring is one the clear never
    // saw, whatever its position: a ring replaced without a start (beginSession()) can already be past the
    // mark at its first read, and so can a fresh ring after a start that kept the mark. A mark that names
    // no ring covers no ring the model reads: one made before a model's first read after a start sits at
    // position zero and covers nothing, and one an older build wrote belongs to a ring that died with the
    // process an app update replaced.
    if (ring != m_clear.ring) {
        m_clear = ClearMark();
        saveClear();
    }
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
        // (M17/D-STAR callsigns), an imported label alone — a row whose only
        // identity is its CSV name is still a call the operator heard — or the
        // scan channel it was heard on, which is the only name encrypted
        // traffic on a conventional list ever gets.
        if (item->target_id == 0U && item->tgt_str[0] == '\0' && item->t_name[0] == '\0'
            && item->channel_label[0] == '\0' && item->service_label[0] == '\0' && item->systype != DSD_SYNC_ANALOG) {
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

/* "YYYY-MM-DD HH:MM:SS ", 'd' for a digit. */
constexpr char kEmitterStampShape[] = "dddd-dd-dd dd:dd:dd ";
constexpr int kEmitterStampLength = static_cast<int>(sizeof(kEmitterStampShape)) - 1;

/**
 * @brief Whether @p text starts with the stamp the emitter puts first.
 *
 * Matched by hand rather than with a shared QRegularExpression: the background tap builds rows on its own thread
 * (CallHistoryTap), and one regular expression object is not to be matched from two threads at once.
 */
bool
starts_with_emitter_stamp(const QString& text) {
    if (text.size() < kEmitterStampLength) {
        return false;
    }
    for (int i = 0; i < kEmitterStampLength; i++) {
        // ASCII digits only, as the regular expression this replaces matched (\d without Unicode properties).
        const char16_t c = text.at(i).unicode();
        if (kEmitterStampShape[i] == 'd' ? (c < u'0' || c > u'9') : c != static_cast<char16_t>(kEmitterStampShape[i])) {
            return false;
        }
    }
    return true;
}

/** @brief The notice text minus the "YYYY-MM-DD HH:MM:SS " prefix the emitter stamps,
 *  and minus the "[label] " the emitter adds for the scan channel the row was heard on.
 *  That label rides the row's own channel_label (and the system column), so only that
 *  exact prefix is removed: a summary that merely starts with a bracket keeps it. The
 *  row's own CRC/RAS marker, which the emitter puts ahead of the label, stays where it is.
 *  A notice a service name labels (issue #637) carries the service there instead, and
 *  keeps it: nothing else on the row shows it, and the title is what search, an elided
 *  row and the accessible name read. */
QString
notice_summary(const Event_History* item) {
    QString text = QString::fromUtf8(item->event_string);
    if (starts_with_emitter_stamp(text)) {
        text.remove(0, kEmitterStampLength);
    }
    if (item->channel_label[0] != '\0' && item->service_label[0] == '\0') {
        const QString marker = QString::fromUtf8(dsd_crc_marker(item->crc_invalid));
        const qsizetype at = text.startsWith(marker) ? marker.size() : 0;
        const QString labelPrefix = QLatin1Char('[') + QString::fromUtf8(item->channel_label) + QLatin1String("] ");
        if (text.mid(at).startsWith(labelPrefix)) {
            text.remove(at, labelPrefix.size());
        }
    }
    return text.trimmed();
}

/** @brief The digest of a ring item's raw identity (call_history_identity_digest()), read from its fields as they are:
 * the walk reads every committed row, and nothing is formatted here. */
uint32_t
item_identity_digest(const Event_History* item) {
    CallHistoryScope rx;
    rx.kind = item->rx_scope.scope.kind;
    rx.a = item->rx_scope.scope.a;
    rx.b = item->rx_scope.scope.b;
    rx.freqHz = item->rx_scope.identity_freq_hz;
    CallHistoryScope sender;
    sender.kind = item->pos.scope.kind;
    sender.a = item->pos.scope.a;
    sender.b = item->pos.scope.b;
    sender.freqHz = item->pos.identity_freq_hz;
    return call_history_identity_digest(item->systype, item->rx_scope.resolved != 0U, rx, sender, item->src_str,
                                        sizeof(item->src_str), item->tgt_str, sizeof(item->tgt_str));
}

/** @brief The frequency and access code a ring item was heard with; 0 for whatever is unknown. */
struct ItemCarrier {
    qint64 freqHz = 0;
    int codeKind = 0;
    int code = 0;
};

ItemCarrier
item_carrier(const Event_History* item) {
    ItemCarrier carrier;
    carrier.freqHz = call_history_freq_known(item->freq_hz) ? static_cast<qint64>(item->freq_hz) : 0;
    carrier.codeKind = static_cast<int>(item->access_code_kind);
    // The value means something only beside a kind (Event_History::access_code).
    carrier.code = call_history_access_code_known(carrier.codeKind) ? static_cast<int>(item->access_code) : 0;
    return carrier;
}

/** @brief Give @p row its item's frequency and access code, each from the row's own fragment. */
void
row_take_carrier(CallHistoryModel::Row& row, const Event_History* item) {
    const ItemCarrier carrier = item_carrier(item);
    row.freqHz = carrier.freqHz;
    row.freqWhen = row.when;
    row.freqSeq = row.seq;
    row.freqSlot = row.slot;
    row.codeKind = carrier.codeKind;
    row.code = carrier.code;
    row.acWhen = row.when;
    row.acSeq = row.seq;
    row.acSlot = row.slot;
}

/** @brief A ring item's call channel (issue #799): its number and numbering when it has a DMR one, else 0 and NONE. */
struct ItemChannel {
    uint32_t channel = 0U;
    uint8_t kind = DSD_CALL_CHANNEL_NONE;
};

ItemChannel
item_call_channel(const Event_History* item) {
    ItemChannel channel;
    if (call_history_call_channel_known(item->call_channel_kind, item->channel)) {
        channel.channel = item->channel;
        channel.kind = item->call_channel_kind;
    }
    return channel;
}

/** @brief Give a voice @p row its item's site and call channel, the channel from the row's own fragment. A notice names
 * no call's site. Pure, as everything the background tap's walk calls must be. */
void
row_take_site(CallHistoryModel::Row& row, const Event_History* item) {
    if (row.kind != CallHistoryModel::KindVoice) {
        return;
    }
    dsd_rxid_site_snapshot(&item->site_id, &row.siteId);
    const ItemChannel channel = item_call_channel(item);
    row.callChannel = channel.channel;
    row.callChannelKind = channel.kind;
    row.cchWhen = row.when;
    row.cchSeq = row.seq;
    row.cchSlot = row.slot;
}

/** @brief Give @p row the verified talkgroup proof and live-input latch @p item carries (issue #903). A proof is a
 * variant and a non-zero id together, or none. Pure, as everything the background tap's walk calls must be. */
void
row_take_heard(CallHistoryModel::Row& row, const Event_History* item) {
    const HeardProof proof = heard_item_proof(item);
    row.heardVariant = proof.variant;
    row.heardId = proof.id;
    row.liveInput = item->live_input != 0U;
}

/** @brief One committed ring item as a display row. */
/** @brief The name of a row with no other: who it was to, a talkgroup, a radio (a private call), or, not knowing which,
 * the number (issue #800). */
QString
target_fallback_name(const CallHistoryModel::Row& row) {
    if (row.gi == 0) {
        return QStringLiteral("Talkgroup %1").arg(row.tg);
    }
    if (row.gi == 1) {
        return QStringLiteral("Radio %1").arg(row.tg);
    }
    return QStringLiteral("Call to %1").arg(row.tg);
}

void
row_take_name(CallHistoryModel::Row& row, const Event_History* item) {
    if (row.kind == CallHistoryModel::KindNotice) {
        /* The emitter's summary line names what happened ("SMS from 1234",
         * "LRRP position"); the payload — the decoded message or GPS string —
         * rides in detail so the delegate can show both. */
        row.name = notice_summary(item);
        if (row.name.isEmpty()) {
            const QString detail = row_detail_text(row);
            row.name = !detail.isEmpty() ? detail : QStringLiteral("Data message");
        }
    } else if (item->service_label[0] != '\0') {
        row.name = QString::fromUtf8(item->service_label);
    } else if (item->t_name[0] != '\0') {
        row.name = QString::fromUtf8(item->t_name);
    } else if (item->channel_label[0] != '\0'
               && !dsd_call_target_text_is_name(item->systype, item->target_id, item->tgt_str)) {
        /* No identity of its own: the channel it was heard on is what the
         * operator recognises it by, and "Talkgroup 0" is not. So for an
         * NXDN Type-D group, whose "pp-iiii" text only spells its number;
         * text that names the call (a callsign) takes the branch below. */
        row.name = QString::fromUtf8(item->channel_label);
    } else if (item->tgt_str[0] != '\0') {
        row.name = QString::fromUtf8(item->tgt_str);
    } else {
        row.name =
            item->systype == DSD_SYNC_ANALOG ? analog_fallback_name(item->analog_mode) : target_fallback_name(row);
    }
}

CallHistoryModel::Row
row_from_item(const Event_History* item, const QString& sessionLabel, const QString& systemUid, int slot,
              qulonglong seq) {
    CallHistoryModel::Row row;
    row.slot = slot;
    row.seq = seq;
    row.tg = static_cast<qulonglong>(item->target_id);
    row.gi = item->gi == 0 || item->gi == 1 ? static_cast<qint8>(item->gi) : static_cast<qint8>(-1);
    row.src = static_cast<qulonglong>(item->source_id);
    row.sourceName = QString::fromUtf8(item->s_name[0] ? item->s_name : item->src_str);
    row.enc = item->enc != 0U;
    row.emergency = item->emergency != 0U;
    row.kind = item->category == DSD_EVENT_CATEGORY_VOICE ? CallHistoryModel::KindVoice : CallHistoryModel::KindNotice;
    /* The system is the saved entry this session runs, for every row alike, so
     * the system filter keeps listing what the Home screen lists. The scan channel
     * the row was heard on (a -Y row name or a trunk-scan target id) rides its own
     * role: the row's meta line shows it, search matches it, and a talkgroup heard
     * on two channels stays two rows.
     *
     * Who held the tuner is the row's own stamp, frozen at the call's first render (issue #671), never the session
     * label or scan flags when the UI reads the row: a call heard while the operator held the receiver (a paused scan,
     * a tuner release) is Exploring with no saved-system identity, and a call heard under a running scan never gains
     * the saved system's UID, even once the scan has stopped. A call the release ended but did not begin keeps the
     * session's identity. */
    if (item->operator_tuned != 0U) {
        row.systemName = CallHistoryModel::tr("Exploring");
    } else {
        row.systemName = sessionLabel;
        // A scan-labelled row was heard under a scan whichever flag says so.
        row.systemUid = item->scan_member == 0U && item->channel_label[0] == '\0' ? systemUid : QString();
    }
    row.channel = QString::fromUtf8(item->channel_label);
    // Issue #903: the -Y scan-list row is a pair, or none.
    row.scanListRow = item->scan_list_row;
    row.scanListRev = item->scan_list_row != 0U ? static_cast<quint64>(item->scan_list_rev) : 0U;
    row.service = QString::fromUtf8(item->service_label);
    row.analog = item->systype == DSD_SYNC_ANALOG;
    row.analogMode = row.analog && dsd_analog_mode_known(item->analog_mode) != 0
                         ? item->analog_mode
                         : static_cast<uint8_t>(DSD_ANALOG_MODE_UNKNOWN);
    row.receiveKind = item->receive_identity.kind;
    row.receiveValue = item->receive_identity.value;
    row.receiveKnown = item->receive_identity.known != 0;
    row.receiveSlot = item->receive_identity.slot;
    /* The ring stamps both ends of the transmission: event_start_time when the
     * epoch began, event_time as its last render (its end, once committed). Their
     * difference is the measured duration — never this process's ingest lag, which
     * on Android can be most of an hour when the service outlives the Activity. */
    const qint64 start = static_cast<qint64>(item->event_start_time);
    const qint64 end = static_cast<qint64>(item->event_time);
    row.when = (start > 0) ? start : end;
    row.sourceNameWhen = row.when;
    row.sourceNameSeq = seq;
    row.sourceNameSlot = slot;
    row_take_payload(row, item);
    row_take_name(row, item);
    row_take_carrier(row, item);
    row_take_site(row, item);
    row_take_heard(row, item);
    if (row.kind == CallHistoryModel::KindVoice) {
        row.durationSecs = call_history_duration_secs(start, end);
        /* The end only with the start it is measured from: a row with no start stamp (--playfiles) takes event_time
         * as its start, and an end kept there would turn its unknown duration into 0 at the next update. */
        row.end = (start > 0 && end >= start) ? end : 0;
    }
    return row;
}

} // namespace

// Fold the receive evidence and service name @p read gained into @p seen; returns whether it gained any (issue #637).
bool
CallHistoryModel::absorbSeenReceive(SeenState& seen, const SeenState& read) {
    const bool received = (read.receiveKnown && !seen.receiveKnown) || (read.receiveSlot != 0 && seen.receiveSlot == 0)
                          || (!read.service.isEmpty() && read.service != seen.service);
    if (received) {
        seen.receiveKnown = seen.receiveKnown || read.receiveKnown;
        seen.receiveSlot = seen.receiveSlot != 0 ? seen.receiveSlot : read.receiveSlot;
        seen.service = read.service.isEmpty() ? seen.service : read.service;
    }
    return received;
}

// Record the site parts, site text and call channel @p read gained in @p seen; returns whether it gained any (issue
// #799).
bool
CallHistoryModel::absorbSeenSite(SeenState& seen, const SeenState& read) {
    const bool sited = call_history_seen_absorb_site(&seen.siteMask, &seen.callChannel, &seen.callChannelKind,
                                                     read.siteMask, read.callChannel, read.callChannelKind);
    const bool texted =
        call_history_seen_absorb_site_text(&seen.siteText, &seen.siteTextKnown, read.siteText, read.siteTextKnown);
    return sited || texted;
}

// Fold the end, source, crypto verdict, emergency, source label, frequency and access code @p read gained into @p seen,
// each by its own ratchet; returns whether it gained any.
bool
CallHistoryModel::absorbSeenCall(SeenState& seen, const SeenState& read) {
    int64_t storedEnd = seen.end;
    uint64_t storedSrc = seen.src;
    bool storedEnc = seen.enc;
    const bool advanced = call_history_seen_absorb(&storedEnd, &storedSrc, &storedEnc, read.end, read.src, read.enc,
                                                   &seen.emergency, read.emergency);
    const bool labelAdvanced =
        !read.sourceName.isEmpty() && read.sourceName != seen.sourceName && read.src == storedSrc;
    int64_t storedFreqHz = seen.freqHz;
    int storedAcKind = seen.acKind;
    int storedAc = seen.ac;
    const bool filled =
        call_history_seen_absorb_fill(&storedFreqHz, &storedAcKind, &storedAc, read.freqHz, read.acKind, read.ac);
    if (labelAdvanced) {
        seen.sourceName = read.sourceName;
    }
    seen.end = storedEnd;
    seen.src = storedSrc;
    seen.enc = storedEnc;
    seen.freqHz = storedFreqHz;
    seen.acKind = storedAcKind;
    seen.ac = storedAc;
    return advanced || labelAdvanced || filled;
}

// Record the GPS text, text message, position and raw identity @p read carries in @p seen; returns whether any changed
// (issue #800). An identity digest not known on either side is no change (SeenState::identity).
bool
CallHistoryModel::absorbSeenTexts(SeenState& seen, const SeenState& read) {
    const bool changed =
        read.gpsText != seen.gpsText || read.messageText != seen.messageText || read.position != seen.position;
    seen.gpsText = read.gpsText;
    seen.messageText = read.messageText;
    seen.position = read.position;
    const bool resolved =
        call_history_seen_absorb_site_text(&seen.identity, &seen.identityKnown, read.identity, read.identityKnown);
    return changed || resolved;
}

// Record the verified talkgroup proof @p read gained, or the live-input latch it closed, in @p seen; returns whether
// either happened (issue #903). The core never retracts a proof nor reopens the latch, so neither direction back counts.
bool
CallHistoryModel::absorbSeenProof(SeenState& seen, const SeenState& read) {
    const bool proved = read.heardVariant != 0U && seen.heardVariant == 0U;
    const bool closed = seen.liveInput && !read.liveInput;
    if (proved) {
        seen.heardVariant = read.heardVariant;
        seen.heardId = read.heardId;
    }
    seen.liveInput = read.liveInput;
    return proved || closed;
}

int
CallHistoryModel::noteSeen(SeenMap& seenMap, qint64 session, const QString& key, quint64 ring, const SeenState& read,
                           bool voice) {
    // What a sighting records: the row as read, in this session, from this ring, with no twin yet.
    const auto sighting = [session, ring, &read]() {
        SeenState entry = read;
        entry.session = session;
        entry.ring = ring;
        entry.twin.clear();
        return entry;
    };
    auto seen = seenMap.find(key);
    if (seen == seenMap.end()) {
        seenMap.insert(key, sighting());
        return SeenNew;
    }
    if (seen->ring == 0U) {
        // An entry that does not know its ring: a single-fragment row's own entry, seeded by load() when the
        // seen store has lost it, or one from a store written before rings had an identity. It is taken to be
        // the reading ring's, as every entry was before: the row stays in the session that logged it,
        // never moved into the running one. A capture replayed again across such an entry is not known
        // as heard again.
        seen->ring = ring;
    } else if (seen->ring != ring) {
        // Another ring's row at the same push stamp, with the same start, target and kind: the same
        // capture decoded again in a fresh state, which pushes the same rows at the same stamps. It is a
        // call heard again, not the entry's own row read again, so it is taken in as the live path takes
        // a call heard again. A state an embedding host reuses keeps its ring, and the rows an earlier run
        // left in it stay this entry's (Event_History_I::instance).
        *seen = sighting();
        return SeenAgain;
    }
    seen->session = qMax(seen->session, session);
    // Seen is not final: the core merges a reacquired segment into its committed
    // row in place — the end extends, src fills 0 -> real, the crypto verdict can
    // flip on, an unknown frequency or access code fills — and the key does not
    // change when it does. Re-read the row as an update whenever it advanced.
    // Notices are immutable, so only voice re-reads.
    if (!voice) {
        return SeenUnchanged;
    }
    const bool advanced = absorbSeenCall(*seen, read);
    const bool received = absorbSeenReceive(*seen, read);
    const bool digestAdopted =
        (!seen->siteTextKnown && read.siteTextKnown) || (!seen->identityKnown && read.identityKnown);
    const bool sited = absorbSeenSite(*seen, read);
    const bool texted = absorbSeenTexts(*seen, read);
    const bool proved = absorbSeenProof(*seen, read);
    if (!advanced && !received && !sited && !texted && !proved) {
        return digestAdopted ? SeenSettled : SeenUnchanged;
    }
    return SeenAdvanced;
}

namespace {
// What a ring reading gives its row besides its texts and position (defined with the merge's fold below).
CallHistoryModel::FragmentPart fragment_part_of(const CallHistoryModel::Row& row, const QString& fragment);
} // namespace

CallHistoryModel::SeenState
CallHistoryModel::seenReadOf(const Event_History* item) {
    const qint64 start = static_cast<qint64>(item->event_start_time);
    SeenState read;
    read.end = static_cast<qint64>(item->event_time);
    read.when = (start > 0) ? start : read.end;
    read.src = static_cast<qulonglong>(item->source_id);
    read.emergency = item->emergency != 0U;
    read.enc = item->enc != 0U;
    read.sourceName = QString::fromUtf8(item->s_name[0] ? item->s_name : item->src_str);
    const ItemCarrier carrier = item_carrier(item);
    read.freqHz = carrier.freqHz;
    read.acKind = carrier.codeKind;
    read.ac = carrier.code;
    read.receiveKnown = item->receive_identity.known != 0;
    read.receiveSlot = item->receive_identity.slot;
    read.service = QString::fromUtf8(item->service_label);
    // A mask and a digest read only: the walk reads every committed row, and nothing is formatted here.
    read.siteMask = item->site_id.valid;
    read.siteText = call_history_site_text_digest(&item->site_id);
    read.siteTextKnown = true;
    const ItemChannel channel = item_call_channel(item);
    read.callChannel = channel.channel;
    read.callChannelKind = channel.kind;
    read.gpsText = call_history_text_digest(item->gps_s, sizeof(item->gps_s));
    read.messageText = call_history_text_digest(item->text_message, sizeof(item->text_message));
    read.position =
        call_history_position_digest(item_position_valid(item), item->pos.lat_e7, item->pos.lon_e7, item->pos.err_m,
                                     item->pos.flags, item->pos.kind, item->pos.rev, item->pos.proc_instance);
    read.identity = item_identity_digest(item);
    read.identityKnown = true;
    const HeardProof proof = heard_item_proof(item);
    read.heardVariant = proof.variant;
    read.heardId = proof.id;
    read.liveInput = item->live_input != 0U;
    return read;
}

void
CallHistoryModel::noteClearedProof(const Event_History* item, int slot, qulonglong seq, quint64 instance,
                                   const RingWalk& walk, qint64 session, QList<FreshRow>& fresh) {
    if (walk.heardOwners == nullptr || !walk.optsPublished || !heard_item_learnable(item)) {
        return;
    }
    const qint64 start = static_cast<qint64>(item->event_start_time);
    const qint64 end = static_cast<qint64>(item->event_time);
    const QString key = seen_key(slot, seq, start > 0 ? start : end, item->target_id, ring_item_display_kind(item));
    const auto seen = walk.seen->find(key);
    if (seen == walk.seen->end() || (seen->ring != 0U && seen->ring != instance) || seen->heardVariant != 0U) {
        return;
    }
    const HeardProof proof = heard_item_proof(item);
    seen->heardVariant = proof.variant;
    seen->heardId = proof.id;
    seen->liveInput = true;
    FreshRow sighting;
    sighting.clearedProof = true;
    sighting.key = key;
    sighting.seen = *seen;
    sighting.heardOwner = heard_owner_of(*walk.heardOwners, session, item);
    sighting.row.when = start > 0 ? start : end;
    sighting.row.end = start > 0 && end >= start ? end : 0;
    sighting.row.heardVariant = proof.variant;
    sighting.row.heardId = proof.id;
    if (!sighting.heardOwner.isEmpty()) {
        fresh.append(sighting);
    }
}

bool
CallHistoryModel::seenProven(const SeenMap& seen, const QString& key, quint64 ring) {
    const auto prior = seen.constFind(key);
    return prior != seen.constEnd() && prior->heardVariant != 0U && (prior->ring == 0U || prior->ring == ring);
}

QString
CallHistoryModel::freshHeardOwner(const RingWalk& walk, qint64 session, const Event_History* item, bool provenBefore) {
    if (walk.heardOwners == nullptr || !walk.optsPublished || provenBefore || !heard_item_learnable(item)) {
        return {};
    }
    return heard_owner_of(*walk.heardOwners, session, item);
}

void
CallHistoryModel::walkRing(const Event_History_I& ring, int slot, const RingWalk& walk, QList<FreshRow>& fresh) {
    const qulonglong pushSeq = static_cast<qulonglong>(ring.push_seq);
    const quint64 instance = static_cast<quint64>(ring.instance);
    // Issue #903: a ring's rows belong to the session of the generation they were pushed in: the session the ring was
    // first read in, or a later one whose run reused it. So a ring's late rows keep their session's owners.
    heard_owners_bind_ring(walk.heardOwners, instance, slot, static_cast<quint64>(pushSeq), walk.session);
    // Index 0 is the still-active staged row; only committed rows are finished
    // calls that belong in a log.
    for (int idx = 1; idx < DSD_EVENT_HISTORY_LEN; idx++) {
        const Event_History* item = &ring.Event_History_Items[idx];
        const int kind = ring_item_display_kind(item);
        if (kind < 0) {
            continue;
        }
        // The push stamp this row was committed at — its stable ring identity.
        const qulonglong seq =
            pushSeq >= static_cast<qulonglong>(idx - 1) ? pushSeq - static_cast<qulonglong>(idx - 1) : 0ULL;
        if (walk.cleared && seq <= walk.clearedThrough) {
            // Cleared by the user. The ring still holds the row until the session ends,
            // so it must stay hidden even after a relaunched UI rebuilds m_seen. Judged by
            // push order, never by stamps. Whatever the ring takes in after the clear is
            // new, however old its stamps (a replay's are the capture's). A call still
            // airing when Clear was tapped commits later, and so it is new activity.
            // Issue #903: a proof that came after the clear is still a talkgroup heard.
            noteClearedProof(item, slot, seq, instance, walk,
                             heard_owners_session_of(walk.heardOwners, instance, slot, seq, walk.session), fresh);
            continue;
        }
        const qint64 heardSession = heard_owners_session_of(walk.heardOwners, instance, slot, seq, walk.session);
        const SeenState read = seenReadOf(item);
        // Must match keyFor() on the equivalent Row, or a relaunched UI would
        // re-ingest every row its predecessor already logged.
        const QString key = seen_key(slot, seq, read.when, item->target_id, kind);
        // Whether an earlier reading of this fragment already carried its proof: only the first that does is handed on.
        const bool provenBefore = seenProven(*walk.seen, key, instance);
        const int verdict = noteSeen(*walk.seen, walk.session, key, instance, read, kind == KindVoice);
        if (verdict == SeenSettled && walk.seenSettled != nullptr) {
            *walk.seenSettled = true;
        }
        if (verdict == SeenSettled && walk.settledKeys != nullptr) {
            walk.settledKeys->append(key);
        }
        if (verdict == SeenUnchanged || verdict == SeenSettled) {
            continue;
        }
        FreshRow row;
        row.row = row_from_item(item, walk.sessionLabel, walk.systemUid, slot, seq);
        row.row.rxRing = instance;
        row.row.session = walk.session;
        row.row.gpsOrd = walk.sessionOrd;
        row.row.gpsVer = static_cast<quint64>(ring.commit_rev);
        row.row.fragments.append(call_history_fragment_key(instance, slot, seq));
        row.row.fragmentVers.insert(row.row.fragments.constLast(), static_cast<quint64>(ring.commit_rev));
        // Issue #903: who the Talkgroups screen shows the fragment for, whether or not its talkgroup may be learnt.
        if (walk.heardOwners != nullptr && walk.optsPublished) {
            row.row.viewOwner = heard_owner_of(*walk.heardOwners, heardSession, item);
        }
        if (kind == KindVoice) {
            row.row.parts.append(fragment_part_of(row.row, row.row.fragments.constLast()));
        }
        row.isUpdate = verdict == SeenAdvanced;
        row.again = verdict == SeenAgain;
        row.key = key;
        row.seen = walk.seen->value(key);
        row.heardOwner = freshHeardOwner(walk, heardSession, item, provenBefore);
        fresh.append(row);
    }
}

QList<CallHistoryModel::FreshRow>
CallHistoryModel::collectFresh(const dsd_state* snapshot, const bool scan[2], bool optsPublished, bool* seenSettled) {
    const auto ringsBound = m_heardOwners.rings;
    const HeardRingMark markBound = m_heardOwners.mark;
    QList<FreshRow> fresh;
    RingWalk walk;
    walk.seen = &m_seen;
    walk.heardOwners = &m_heardOwners;
    walk.optsPublished = optsPublished;
    walk.seenSettled = seenSettled;
    walk.session = m_session;
    walk.sessionLabel = m_sessionLabel;
    // Whether a row may carry the saved system's UID is its own stamp (row_from_item()), which covers scans started
    // from a saved system's extra arguments as well as the list UI. A snapshot read without its options cannot
    // establish identity.
    walk.systemUid = optsPublished ? m_sessionUid : QString();
    walk.cleared = clearApplies();
    for (int slot = 0; slot < 2; slot++) {
        if (!scan[slot]) {
            continue;
        }
        walk.clearedThrough = m_clear.pushSeq[slot];
        walk.sessionOrd = sessionOrdinal(static_cast<quint64>(snapshot->event_history_s[slot].instance));
        walkRing(snapshot->event_history_s[slot], slot, walk, fresh);
    }
    if (m_heardOwners.rings != ringsBound || !(m_heardOwners.mark == markBound)) {
        // A ring read for the first time, or again by a run that reused it, is bound to this session (issue #903).
        saveHeardOwners();
    }
    return fresh;
}

namespace {

/* Fold the identity evidence @p row carries into @p existing, which tryMerge() matched to it: a confirmed code the row
   holds and the row lacks, and a slot the row has proven, each on its own. Returns 1 when the row's service label,
   now the better-founded one, should name @p existing. */
bool
receive_adopt_identity(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    bool adopt = false;
    if (!existing.receiveKnown && row.receiveKnown) {
        existing.receiveKnown = true;
        existing.receiveKind = row.receiveKind;
        existing.receiveValue = row.receiveValue;
        adopt = true;
    }
    if (existing.receiveSlot == 0 && row.receiveSlot != 0) {
        existing.receiveSlot = row.receiveSlot;
        /* A slot name names only a row no confirmed code has named: a code's name outranks it
           (dsd_receive_service_name()). */
        adopt = adopt || (!row.service.isEmpty() && (existing.service.isEmpty() || !existing.receiveKnown));
    }
    /* A row no service named takes the name a later fragment of the same service carries. */
    return adopt || (existing.service.isEmpty() && !row.service.isEmpty());
}

/* The textual target a row with no talkgroup names (an M17/D-STAR/YSF callsign, a dPMR dial string), or empty when
   its name is only a label: the service the receive policy named it, the channel it was heard on, or the placeholder
   (row_take_name()). A label changes as the call is confirmed (issue #637); a textual target does not. */
QString
row_textual_destination(const CallHistoryModel::Row& row) {
    if ((!row.service.isEmpty() && row.name == row.service) || (!row.channel.isEmpty() && row.name == row.channel)
        || row.name == QStringLiteral("Talkgroup 0")) {
        return QString();
    }
    return row.name;
}

/**
 * @brief A row's destination as a merge compares it: the target text as received (issue #800, B4), else its textual
 * destination by name (rows older builds stored keep no target text).
 */
QString
row_destination(const CallHistoryModel::Row& row) {
    const QString raw = row.targetText.simplified();
    return !raw.isEmpty() ? raw : row_textual_destination(row);
}

/** @brief Whether two rows with no talkgroup (tg == 0) name the same destination; any rows with one do. */
bool
target_less_rows_same_destination(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (existing.tg != 0) {
        return true;
    }
    // Textual targets (M17/D-STAR/YSF callsigns, dPMR dial strings) all share
    // tg == 0, so the numeric check cannot tell two destinations apart;
    // the target text is their identity. A name that is only a label (a service
    // or channel name) is no destination of its own, but the target text a row
    // keeps beside it still is (issue #800, B4: one caller's two M17 destinations
    // under one named CAN). Analog rows never get here (rows_mergeable()).
    const QString existingTarget = row_destination(existing);
    const QString rowTarget = row_destination(row);
    if (!existingTarget.isEmpty() && !rowTarget.isEmpty() && existingTarget != rowTarget) {
        return false;
    }
    // A digital row with no IDs at all (issue #637) is named only by where it was heard:
    // two such transmissions on different frequencies are two calls, whatever the gap between them. Trunked fragments
    // keep a talkgroup, so their retunes still fold.
    return existing.src != 0 || row.src != 0 || existing.freqHz == 0 || row.freqHz == 0
           || existing.freqHz == row.freqHz;
}

/** @brief Whether two rows were heard on one site under one channel numbering (issue #799) and their known identities
 * agree (rows_identity_compatible()). */
bool
rows_systems_compatible(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    return dsd_call_site_compatible(&existing.siteId, existing.callChannelKind, &row.siteId, row.callChannelKind) != 0
           && rows_identity_compatible(existing, row);
}

/** @brief Whether two voice rows are one conversation the merge may fold together. */
bool
rows_mergeable(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    // Each analog reception is a row of its own (issue #667): the tracker already joined what one reception holds
    // (its hangover, the pause hold), so two analog rows, or an analog and a digital row, are two calls.
    if (existing.kind != CallHistoryModel::KindVoice || existing.analog || row.analog) {
        return false;
    }
    if (!receive_services_compatible(existing, row)) {
        return false;
    }
    // A zero source id is "not yet learned", not a distinct unit: late-entry
    // fragments commit before the ring learns the src, and refusing to absorb
    // them would leave one conversation split across two rows.
    const bool srcCompatible = existing.src == row.src || existing.src == 0 || row.src == 0;
    if (existing.tg != row.tg || !srcCompatible || existing.systemName != row.systemName
        || existing.systemUid != row.systemUid || existing.channel != row.channel) {
        return false;
    }
    // Two sites, or two channel numberings, are two calls (issue #799): a radio that roams calls again on another site.
    // So are two systems, protocols or callsign callers (issue #800).
    if (!rows_systems_compatible(existing, row)) {
        return false;
    }
    if (!target_less_rows_same_destination(existing, row)) {
        return false;
    }
    // Matched sources get the full retune window; a src-unknown pairing gets
    // the tight one, or two distinct back-to-back calls on a busy talkgroup
    // collapse into one row (the wildcard above cannot tell units apart).
    const bool srcKnownMatch = existing.src != 0 && existing.src == row.src;
    return call_history_merge_within_window(existing.when, row_merge_end_secs(existing), row.when,
                                            row_merge_end_secs(row), srcKnownMatch);
}

/**
 * @brief Whether analog row @p row is the reception @p existing logged, read again (issue #667).
 *
 * Exactly the same: its start, its tone (or the same lack of one), its frequency, and the system and channel it was
 * logged under. A replay in a fresh ring pushes that, and so does the ring an update of a row an older build stored
 * (with no fragments) comes from.
 */
bool
rows_same_reception(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (existing.kind != CallHistoryModel::KindVoice || row.kind != CallHistoryModel::KindVoice || !existing.analog
        || !row.analog) {
        return false;
    }
    const bool sameTone =
        existing.receiveKnown == row.receiveKnown
        && (!row.receiveKnown
            || (existing.receiveKind == row.receiveKind && existing.receiveValue == row.receiveValue));
    // One system's reception never stands for another's, as rows_mergeable() keeps two systems apart: the same capture
    // replayed under another saved system (or channel) is that one's.
    const bool sameSystem =
        existing.systemName == row.systemName && existing.systemUid == row.systemUid && existing.channel == row.channel;
    return existing.when == row.when && existing.freqHz == row.freqHz && sameTone && sameSystem;
}

/** @brief The ring a call_history_fragment_key() names: the instance before its first ':'. */
QStringView
fragment_ring(const QString& fragment) {
    const qsizetype colon = fragment.indexOf(QLatin1Char(':'));
    return colon > 0 ? QStringView(fragment).left(colon) : QStringView();
}

/** @brief Whether @p existing already holds a fragment, other than @p row's own, of a ring @p row comes from. */
bool
row_holds_ring_of(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    return std::any_of(row.fragments.cbegin(), row.fragments.cend(), [&existing](const QString& own) {
        const QStringView ring = fragment_ring(own);
        return !ring.isEmpty()
               && std::any_of(existing.fragments.cbegin(), existing.fragments.cend(),
                              [&own, ring](const QString& held) { return held != own && fragment_ring(held) == ring; });
    });
}

/**
 * @brief The row of the first @p scanRows that analog row @p row is the reception of, read again, or -1.
 *
 * One to one: a row's start is a whole second, so two receptions on one frequency and tone can share it. A row that
 * already took another reception of @p row's ring is that reception's, never this one's; among the rest, a row that
 * also ends when @p row does (a replay pushes the same span) comes before one that does not (an update ran on).
 */
int
same_reception_row(const QList<CallHistoryModel::Row>& rows, const CallHistoryModel::Row& row, int scanRows) {
    int first = -1;
    for (int i = 0; i < rows.size() && i < scanRows; i++) {
        const CallHistoryModel::Row& existing = rows.at(i);
        if (!rows_same_reception(existing, row) || row_holds_ring_of(existing, row)) {
            continue;
        }
        if (row_merge_end_secs(existing) == row_merge_end_secs(row)) {
            return i;
        }
        if (first < 0) {
            first = i;
        }
    }
    return first;
}

/**
 * @brief Adopt @p row's source label when it is the later of the two.
 *
 * Fragments reach the merge in whichever order a refresh happens to collect them --
 * a backlog walks newest-first, a live session arrives oldest-first -- so the label
 * has to be chosen by the fragment it came from rather than by arrival
 * (call_history_fold_adopts()). The push sequence and slot break a same-second tie;
 * identical provenance is the fragment enriching its own label, which must still land.
 */
void
merge_source_label(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (!call_history_fold_adopts(!existing.sourceName.isEmpty(),
                                  provenance(existing.sourceNameWhen, existing.sourceNameSeq, existing.sourceNameSlot),
                                  !row.sourceName.isEmpty(),
                                  provenance(row.sourceNameWhen, row.sourceNameSeq, row.sourceNameSlot))) {
        return;
    }
    existing.sourceName = row.sourceName;
    existing.sourceNameWhen = row.sourceNameWhen;
    existing.sourceNameSeq = row.sourceNameSeq;
    existing.sourceNameSlot = row.sourceNameSlot;
}

void
take_freq(CallHistoryModel::Row& to, const CallHistoryModel::Row& from) {
    to.freqHz = from.freqHz;
    to.freqWhen = from.freqWhen;
    to.freqSeq = from.freqSeq;
    to.freqSlot = from.freqSlot;
}

void
take_access_code(CallHistoryModel::Row& to, const CallHistoryModel::Row& from) {
    to.codeKind = from.codeKind;
    to.code = from.code;
    to.acWhen = from.acWhen;
    to.acSeq = from.acSeq;
    to.acSlot = from.acSlot;
}

/**
 * @brief Fold @p row's frequency and access code into @p existing, each by the fragment it came from.
 *
 * A trunked call lands on several voice channels, and its fragments still merge (rows_mergeable() never
 * looks at the frequency), so the merged row shows the newest fragment's known frequency, whatever order
 * the fragments arrive in. The code pair folds the same way with its own provenance.
 */
void
fold_carrier(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (call_history_fold_adopts(call_history_freq_known(existing.freqHz),
                                 provenance(existing.freqWhen, existing.freqSeq, existing.freqSlot),
                                 call_history_freq_known(row.freqHz),
                                 provenance(row.freqWhen, row.freqSeq, row.freqSlot))) {
        take_freq(existing, row);
    }
    if (call_history_fold_adopts(call_history_access_code_known(existing.codeKind),
                                 provenance(existing.acWhen, existing.acSeq, existing.acSlot),
                                 call_history_access_code_known(row.codeKind),
                                 provenance(row.acWhen, row.acSeq, row.acSlot))) {
        take_access_code(existing, row);
    }
}

/** @brief Give @p to the call channel of @p from, with the fragment it came from. */
void
take_call_channel(CallHistoryModel::Row& to, const CallHistoryModel::Row& from) {
    to.callChannel = from.callChannel;
    to.callChannelKind = from.callChannelKind;
    to.cchWhen = from.cchWhen;
    to.cchSeq = from.cchSeq;
    to.cchSlot = from.cchSlot;
}

/**
 * @brief Fold @p row's site and call channel into @p existing (issue #799).
 *
 * The site is the union of both fragments' parts; the channel is the newest fragment's known one, as the frequency is.
 * A site that conflicts with the row's (an update of a fragment the row already holds, filled by the core with another
 * site) is discarded whole, its channel with it (call_history_site_fold()).
 */
void
fold_site(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (!call_history_site_fold(&existing.siteId, existing.callChannelKind, &row.siteId, row.callChannelKind)) {
        return;
    }
    if (call_history_fold_adopts(call_history_call_channel_known(existing.callChannelKind, existing.callChannel),
                                 provenance(existing.cchWhen, existing.cchSeq, existing.cchSlot),
                                 call_history_call_channel_known(row.callChannelKind, row.callChannel),
                                 provenance(row.cchWhen, row.cchSeq, row.cchSlot))) {
        take_call_channel(existing, row);
    }
}

/** @brief Give @p to the site and call channel of @p from, with the fragment the channel came from. */
void
take_site(CallHistoryModel::Row& to, const CallHistoryModel::Row& from) {
    to.siteId = from.siteId;
    take_call_channel(to, from);
}

using FragmentPart = CallHistoryModel::FragmentPart;

/** @brief Fold @p row's span and its crypto and emergency flags into @p existing, as a merge does. The row keeps its
 * end, so one plausibility rule decides on every fold and update: a span past the cap reads unknown, and a later update
 * of an earlier fragment cannot rebuild a duration from that fragment alone. With neither end known the duration stays
 * unknown; a side whose end is unknown still counts from its start, so both ends are read before the start moves (a
 * start-less row's end is its start). */
void
fold_span(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    const qint64 start = qMin(existing.when, row.when);
    const qint64 end = qMax(row_end_secs(existing), row_end_secs(row));
    const bool endKnown = existing.end > 0 || row.end > 0;
    existing.when = start;
    if (endKnown) {
        existing.end = end;
        existing.durationSecs = call_history_duration_secs(start, end);
    }
    existing.enc = existing.enc || row.enc;
    existing.emergency = existing.emergency || row.emergency;
}

/** @brief Fold the receive evidence and service of @p row into @p existing, and the service name it now earns. */
void
fold_receive(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (receive_adopt_identity(existing, row)) {
        existing.service = row.service;
        /* A service name outranks every other name a row has (row_take_name()). */
        if (!row.service.isEmpty()) {
            existing.name = row.service;
        }
    }
}

/** @brief Give @p part the text message of @p row, with the fragment it came from. */
void
take_part_message(FragmentPart& part, const CallHistoryModel::Row& row) {
    part.messageText = row.messageText;
    part.msgWhen = row.msgWhen;
    part.msgSeq = row.msgSeq;
    part.msgSlot = row.msgSlot;
}

/** @brief Give @p to the sender, call identity, source label and text message of part @p from. */
void
take_part_sender(FragmentPart& to, const FragmentPart& from) {
    to.protocol = from.protocol;
    to.tg = from.tg;
    to.gi = from.gi;
    to.src = from.src;
    to.sourceText = from.sourceText;
    to.targetText = from.targetText;
    to.sourceScope = from.sourceScope;
    to.sourceName = from.sourceName;
    to.sourceNameWhen = from.sourceNameWhen;
    to.sourceNameSeq = from.sourceNameSeq;
    to.sourceNameSlot = from.sourceNameSlot;
    to.messageText = from.messageText;
    to.msgWhen = from.msgWhen;
    to.msgSeq = from.msgSeq;
    to.msgSlot = from.msgSlot;
}

/** @brief What a one-fragment reading @p row gives its row (Row::parts), as fragment @p fragment. */
FragmentPart
fragment_part_of(const CallHistoryModel::Row& row, const QString& fragment) {
    FragmentPart part;
    part.fragment = fragment;
    part.enc = row.enc;
    part.emergency = row.emergency;
    part.when = row.when;
    part.end = row.end;
    part.durationSecs = row.durationSecs;
    part.receiveKnown = row.receiveKnown;
    part.receiveKind = row.receiveKind;
    part.receiveValue = row.receiveValue;
    part.receiveSlot = row.receiveSlot;
    part.freqHz = row.freqHz;
    part.freqWhen = row.freqWhen;
    part.freqSeq = row.freqSeq;
    part.freqSlot = row.freqSlot;
    part.codeKind = row.codeKind;
    part.code = row.code;
    part.acWhen = row.acWhen;
    part.acSeq = row.acSeq;
    part.acSlot = row.acSlot;
    part.siteId = row.siteId;
    part.callChannel = row.callChannel;
    part.callChannelKind = row.callChannelKind;
    part.cchWhen = row.cchWhen;
    part.cchSeq = row.cchSeq;
    part.cchSlot = row.cchSlot;
    part.rxScope = row.rxScope;
    part.rxRing = row.rxRing;
    part.rxGen = row.rxGen;
    part.rxGenKnown = row.rxGenKnown;
    part.rxHz = row.rxHz;
    part.name = row.name;
    part.service = row.service;
    part.session = row.session;
    part.protocol = row.protocol;
    part.tg = row.tg;
    part.gi = row.gi;
    part.src = row.src;
    part.sourceText = row.sourceText;
    part.targetText = row.targetText;
    part.sourceScope = row.sourceScope;
    part.sourceName = row.sourceName;
    part.sourceNameWhen = row.sourceNameWhen;
    part.sourceNameSeq = row.sourceNameSeq;
    part.sourceNameSlot = row.sourceNameSlot;
    take_part_message(part, row);
    part.heardVariant = row.heardVariant;
    part.heardId = row.heardId;
    part.liveInput = row.liveInput;
    part.viewOwner = row.viewOwner;
    part.heardSerial = row.heardSerial;
    part.scanListRow = row.scanListRow;
    part.scanListRev = row.scanListRev;
    return part;
}

/** @brief A voice row holding only @p part's fields, so the merge's own rules fold it. */
CallHistoryModel::Row
row_of_part(const FragmentPart& part) {
    CallHistoryModel::Row row;
    row.enc = part.enc;
    row.emergency = part.emergency;
    row.when = part.when;
    row.end = part.end;
    row.durationSecs = part.durationSecs;
    row.receiveKnown = part.receiveKnown;
    row.receiveKind = part.receiveKind;
    row.receiveValue = part.receiveValue;
    row.receiveSlot = part.receiveSlot;
    row.freqHz = part.freqHz;
    row.freqWhen = part.freqWhen;
    row.freqSeq = part.freqSeq;
    row.freqSlot = part.freqSlot;
    row.codeKind = part.codeKind;
    row.code = part.code;
    row.acWhen = part.acWhen;
    row.acSeq = part.acSeq;
    row.acSlot = part.acSlot;
    row.siteId = part.siteId;
    row.callChannel = part.callChannel;
    row.callChannelKind = part.callChannelKind;
    row.cchWhen = part.cchWhen;
    row.cchSeq = part.cchSeq;
    row.cchSlot = part.cchSlot;
    row.rxScope = part.rxScope;
    row.rxRing = part.rxRing;
    row.rxGen = part.rxGen;
    row.rxGenKnown = part.rxGenKnown;
    row.rxHz = part.rxHz;
    row.name = part.name;
    row.service = part.service;
    row.session = part.session;
    row.protocol = part.protocol;
    row.tg = part.tg;
    row.gi = part.gi;
    row.src = part.src;
    row.sourceText = part.sourceText;
    row.targetText = part.targetText;
    row.sourceScope = part.sourceScope;
    row.sourceName = part.sourceName;
    row.sourceNameWhen = part.sourceNameWhen;
    row.sourceNameSeq = part.sourceNameSeq;
    row.sourceNameSlot = part.sourceNameSlot;
    row.messageText = part.messageText;
    row.msgWhen = part.msgWhen;
    row.msgSeq = part.msgSeq;
    row.msgSlot = part.msgSlot;
    row.heardVariant = part.heardVariant;
    row.heardId = part.heardId;
    row.liveInput = part.liveInput;
    row.viewOwner = part.viewOwner;
    row.heardSerial = part.heardSerial;
    row.scanListRow = part.scanListRow;
    row.scanListRev = part.scanListRev;
    return row;
}

/** @brief The fields a fragment's part folds into its row (FragmentPart), from @p from: the reception scope by
 * strength, only from a fragment heard on the row's reception (reception_learns()), the receive evidence and service,
 * the frequency and access code, the site and channel, the span and flags.
 */
void
fold_part_fields(CallHistoryModel::Row& existing, const CallHistoryModel::Row& from) {
    if (reception_learns(existing, from)) {
        take_reception(existing, from);
    }
    fold_receive(existing, from);
    fold_carrier(existing, from);
    fold_site(existing, from);
    fold_span(existing, from);
    // A call heard again in a later session is part of that session too: its views show the merged row, and a full
    // log keeps it as that session's.
    existing.session = qMax(existing.session, from.session);
    // Issue #903: the row shows its first proven part's proof, and is live only while every part is; each part keeps
    // its own.
    if (existing.heardVariant == 0U && from.heardVariant != 0U) {
        existing.heardVariant = from.heardVariant;
        existing.heardId = from.heardId;
    }
    existing.liveInput = existing.liveInput && from.liveInput;
    // The row shows its first part's owner and its parts' newest serial; each part keeps its own.
    existing.heardSerial = qMax(existing.heardSerial, from.heardSerial);
}

/** @brief The index of fragment @p fragment's part in @p row, or -1. For one lookup; PartLookup for many. */
qsizetype
part_index(const CallHistoryModel::Row& row, const QString& fragment) {
    for (qsizetype i = 0; i < row.parts.size(); i++) {
#ifdef DSD_NEO_TEST_HOOKS
        g_part_lookup_steps.fetch_add(1, std::memory_order_relaxed);
#endif
        if (row.parts.at(i).fragment == fragment) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Many lookups of fragments' parts in one row's parts: an index by fragment key, built once, when both the
 * lookups and the parts are many, so a fold that looks every fragment of a long call up walks its parts once rather
 * than once per fragment (a call of N short fragments would otherwise cost N^3 over its folds); a plain scan for a few,
 * which costs less than building the index.
 */
class PartLookup {
  public:
    PartLookup(const QList<FragmentPart>& parts, qsizetype lookups) : m_parts(parts) {
        if (lookups > kScanLimit && parts.size() > kScanLimit) {
            m_index.reserve(parts.size());
            for (qsizetype i = 0; i < parts.size(); i++) {
                m_index.insert(parts.at(i).fragment, i);
            }
            m_indexed = true;
#ifdef DSD_NEO_TEST_HOOKS
            g_part_lookup_steps.fetch_add(parts.size(), std::memory_order_relaxed);
#endif
        }
    }

    /** @brief The index of fragment @p fragment's part, or -1. */
    qsizetype
    find(const QString& fragment) const {
        if (m_indexed) {
            return m_index.value(fragment, -1);
        }
        for (qsizetype i = 0; i < m_parts.size(); i++) {
#ifdef DSD_NEO_TEST_HOOKS
            g_part_lookup_steps.fetch_add(1, std::memory_order_relaxed);
#endif
            if (m_parts.at(i).fragment == fragment) {
                return i;
            }
        }
        return -1;
    }

    /** @brief Note that fragment @p fragment's part now sits at @p at of the parts this lookup reads. */
    void
    added(const QString& fragment, qsizetype at) {
        if (m_indexed) {
            m_index.insert(fragment, at);
        }
    }

  private:
    static constexpr qsizetype kScanLimit = 8;
    const QList<FragmentPart>& m_parts;
    QHash<QString, qsizetype> m_index;
    bool m_indexed = false;
};

/** @brief Whether every fragment of @p row has its part, so the row can be rebuilt from them. */
bool
parts_complete(const CallHistoryModel::Row& row) {
    if (row.fragments.isEmpty()) {
        return false;
    }
    const PartLookup lookup(row.parts, row.fragments.size());
    return std::all_of(row.fragments.cbegin(), row.fragments.cend(),
                       [&lookup](const QString& fragment) { return lookup.find(fragment) >= 0; });
}

/** @brief Put the parts of @p row into @p existing: a fragment's part is replaced whole by a reading that is not older
 * (Row::fragmentVers), and by any reading once withdrawn (a reading that agrees again starts afresh), else it is added.
 * A reading is the fragment's whole ring entry as it now stands, so nothing of an earlier reading outlives it: a
 * message, sender, destination or name it no longer carries is gone from the part (issue #800, fail closed). */
void
merge_parts(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    QList<FragmentPart> parts = existing.parts;
    PartLookup lookup(parts, row.parts.size());
    for (const FragmentPart& part : row.parts) {
        const qsizetype held = lookup.find(part.fragment);
        if (held < 0) {
            lookup.added(part.fragment, parts.size());
            parts.append(part);
        } else if (parts.at(held).withdrawn
                   || row.fragmentVers.value(part.fragment, 0U) >= existing.fragmentVers.value(part.fragment, 0U)) {
            parts[held] = part;
        }
    }
    existing.parts = std::move(parts);
}

/** @brief Whether a value whose provenance has push stamp @p seq and slot @p slot is fragment @p fragment's, by the slot
 * and push stamp its key (call_history_fragment_key()) holds. */
bool
fragment_stamp_matches(const QString& fragment, qulonglong seq, int slot) {
    const QStringList parts = fragment.split(QLatin1Char(':'));
    bool slotOk = false;
    bool seqOk = false;
    return parts.size() == 3 && parts.at(1).toInt(&slotOk) == slot && slotOk && parts.at(2).toULongLong(&seqOk) == seq
           && seqOk;
}

/** @brief The key of the part standing for a row an older build stored without fragments: `legacy|` and the row's seen
 * key (seen_key()), which no fragment key (call_history_fragment_key(), `:`-separated) can be. */
QString
legacy_part_key(const CallHistoryModel::Row& row) {
    return QStringLiteral("legacy|") + seen_key(row.slot, row.seq, row.when, row.tg, row.kind);
}

/** @brief Whether @p fragment is a legacy_part_key(). */
bool
is_legacy_part_key(const QString& fragment) {
    return fragment.startsWith(QLatin1String("legacy|"));
}

/**
 * @brief The part a row an older build stored gives fragment @p fragment (ensure_parts()): the row's values, except
 * that a value carrying a fragment's provenance (frequency, access code, call channel, source label, text message) is
 * this fragment's only when the provenance names it (its slot and push stamp). Nothing else tells which fragment gave
 * what, so a withdrawal of such a fragment takes back only those. The part of a row with no fragments (a
 * legacy_part_key(), or an empty key) keeps every value.
 */
FragmentPart
legacy_part_of(const CallHistoryModel::Row& row, const QString& fragment) {
    FragmentPart part = fragment_part_of(row, fragment);
    if (fragment.isEmpty() || is_legacy_part_key(fragment)) {
        return part;
    }
    if (!fragment_stamp_matches(fragment, row.freqSeq, row.freqSlot)) {
        part.freqHz = 0;
    }
    if (!fragment_stamp_matches(fragment, row.acSeq, row.acSlot)) {
        part.codeKind = 0;
        part.code = 0;
    }
    if (!fragment_stamp_matches(fragment, row.cchSeq, row.cchSlot)) {
        part.callChannel = 0U;
        part.callChannelKind = DSD_CALL_CHANNEL_NONE;
    }
    if (!fragment_stamp_matches(fragment, row.sourceNameSeq, row.sourceNameSlot)) {
        part.sourceName.clear();
    }
    if (!fragment_stamp_matches(fragment, row.msgSeq, row.msgSlot)) {
        part.messageText.clear();
    }
    return part;
}

/** @brief Give a voice row that lacks them its parts (legacy_part_of()): one per fragment it holds without one, or,
 * for a row with no fragments and no parts, one standing for all it holds, under the row's own key
 * (legacy_part_key()); a part an earlier build stored with no key takes that key. On load (row_from_json()) and before
 * every fold, so the row is always the fold of its parts, and two such rows folded together are two parts. */
void
ensure_parts(CallHistoryModel::Row& row) {
    if (row.kind != CallHistoryModel::KindVoice) {
        return;
    }
    if (row.fragments.isEmpty()) {
        if (row.parts.isEmpty()) {
            row.parts.append(legacy_part_of(row, legacy_part_key(row)));
        } else if (part_index(row, QString()) >= 0) {
            QList<FragmentPart> parts = row.parts;
            parts[part_index(row, QString())].fragment = legacy_part_key(row);
            row.parts = std::move(parts);
        }
        return;
    }
    PartLookup lookup(row.parts, row.fragments.size());
    for (const QString& fragment : row.fragments) {
        if (lookup.find(fragment) < 0) {
            lookup.added(fragment, row.parts.size());
            row.parts.append(legacy_part_of(row, fragment));
        }
    }
}

/** @brief Clear @p row's sender: its source number, text and system, its source label and its text message. */
void
clear_sender(CallHistoryModel::Row& row) {
    row.src = 0;
    row.sourceText.clear();
    row.sourceScope = CallHistoryScope();
    row.sourceName.clear();
    row.messageText.clear();
}

/** @brief Whose sender a rebuild keeps (rebuild_from_parts()): the radio the row names, while a part still names it. */
struct SenderRule {
    /* The part naming the row's radio, as a row; null when none does. */
    const CallHistoryModel::Row* anchor = nullptr;
    /* The row named a radio no part names any longer: a part naming another radio gives no sender. */
    bool radioGone = false;
};

/** @brief Whether part @p from may give @p rebuilt its sender, source label and message under @p rule: it names the
 * anchor's radio; with the row's radio gone, it names none; with no radio known, it agrees with the row. */
bool
part_gives_sender(const CallHistoryModel::Row& rebuilt, const CallHistoryModel::Row& from, const SenderRule& rule) {
    if (rule.anchor != nullptr) {
        return row_names_source(from) && fragment_identity_compatible(*rule.anchor, from)
               && fragment_identity_compatible(rebuilt, from);
    }
    return (!rule.radioGone || !row_names_source(from)) && fragment_identity_compatible(rebuilt, from);
}

/**
 * @brief Fold the call identity part @p from gives into @p rebuilt, whoever sent it: the sync type, the numeric target
 * and the raw destination the row does not know yet (fragment_call_compatible() already found them agreeing), and with
 * a target or destination the name it gave the part (row_take_name(): its alias, target text or placeholder) when the
 * row's own name is only a label (a channel or a placeholder, not its service). A target or destination is the call's,
 * not the sender's: the core fills a committed call's unknown target in place (watchdog_event_merge_identity_fields()),
 * and a senderless fragment's keeps a later fragment naming another destination off the row (rows_mergeable()).
 */
void
fold_call_identity(CallHistoryModel::Row& rebuilt, const CallHistoryModel::Row& from) {
    if (call_history_protocol_family(rebuilt.protocol) < 0) {
        rebuilt.protocol = from.protocol;
    }
    const bool nameIsLabel = rebuilt.service.isEmpty() || rebuilt.name != rebuilt.service;
    if (rebuilt.tg == 0U && from.tg != 0U) {
        rebuilt.tg = from.tg;
        if (nameIsLabel) {
            rebuilt.name = from.name;
        }
    }
    // Who the call was to: a known kind wins (the core's merge does the same); the two never disagree here
    // (fragment_call_compatible()).
    if (rebuilt.gi < 0 && from.gi >= 0) {
        rebuilt.gi = from.gi;
        // A name that was only "Call to N", for not knowing the kind, takes the name the kind gave (row_take_name()).
        if (nameIsLabel && rebuilt.tg == from.tg && rebuilt.name == QStringLiteral("Call to %1").arg(rebuilt.tg)) {
            rebuilt.name = from.name;
        }
    }
    const QString destination = from.targetText.simplified();
    if (destination.isEmpty() || !rebuilt.targetText.simplified().isEmpty()) {
        return;
    }
    rebuilt.targetText = from.targetText;
    if (nameIsLabel && from.name.simplified() == destination) {
        rebuilt.name = from.name;
    }
}

/**
 * @brief Fold part @p from into @p rebuilt by the merge's rules: nothing from a part that names another call than the
 * parts before it; its call identity always (fold_call_identity()); its sender, source label and message only when
 * @p rule lets it (part_gives_sender()); its other fields always.
 */
void
fold_part(CallHistoryModel::Row& rebuilt, const CallHistoryModel::Row& from, const SenderRule& rule) {
    if (!fragment_call_compatible(rebuilt, from)) {
        return;
    }
    fold_call_identity(rebuilt, from);
    if (part_gives_sender(rebuilt, from, rule)) {
        merge_source_label(rebuilt, from);
        fold_message(rebuilt, from);
        fill_identity(rebuilt, from);
        if (rebuilt.src == 0) {
            rebuilt.src = from.src;
        }
    }
    fold_part_fields(rebuilt, from);
}

/** @brief Give @p existing the fields its parts give it (FragmentPart), from @p rebuilt. */
void
take_part_fields(CallHistoryModel::Row& existing, const CallHistoryModel::Row& rebuilt) {
    existing.enc = rebuilt.enc;
    existing.emergency = rebuilt.emergency;
    existing.when = rebuilt.when;
    existing.end = rebuilt.end;
    existing.durationSecs = rebuilt.durationSecs;
    existing.receiveKnown = rebuilt.receiveKnown;
    existing.receiveKind = rebuilt.receiveKind;
    existing.receiveValue = rebuilt.receiveValue;
    existing.receiveSlot = rebuilt.receiveSlot;
    take_freq(existing, rebuilt);
    take_access_code(existing, rebuilt);
    take_site(existing, rebuilt);
    take_reception(existing, rebuilt);
    existing.name = rebuilt.name;
    existing.service = rebuilt.service;
    existing.session = rebuilt.session;
    existing.protocol = rebuilt.protocol;
    existing.tg = rebuilt.tg;
    existing.gi = rebuilt.gi;
    existing.src = rebuilt.src;
    existing.sourceText = rebuilt.sourceText;
    existing.targetText = rebuilt.targetText;
    existing.sourceScope = rebuilt.sourceScope;
    existing.sourceName = rebuilt.sourceName;
    existing.sourceNameWhen = rebuilt.sourceNameWhen;
    existing.sourceNameSeq = rebuilt.sourceNameSeq;
    existing.sourceNameSlot = rebuilt.sourceNameSlot;
    take_message(existing, rebuilt);
    existing.heardVariant = rebuilt.heardVariant;
    existing.heardId = rebuilt.heardId;
    existing.liveInput = rebuilt.liveInput;
    existing.viewOwner = rebuilt.viewOwner;
    existing.heardSerial = rebuilt.heardSerial;
    existing.scanListRow = rebuilt.scanListRow;
    existing.scanListRev = rebuilt.scanListRev;
}

/** @brief The index of the first part not withdrawn in @p parts that names a source, and the radio @p radio names when
 * it is not null; -1 when none does. */
qsizetype
anchor_part(const QList<FragmentPart>& parts, const CallHistoryModel::Row* radio) {
    for (qsizetype i = 0; i < parts.size(); i++) {
        const CallHistoryModel::Row from = row_of_part(parts.at(i));
        if (!parts.at(i).withdrawn && row_names_source(from)
            && (radio == nullptr || fragment_identity_compatible(*radio, from))) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Rebuild what @p existing's fragments gave it (FragmentPart) from the parts not withdrawn, folded in their
 * order by the merge's rules (fold_part()). The row keeps the radio @p radio names (the row's as it was; null for none):
 * only parts naming it give the sender, label and message, the first of them first. When no part names it any longer,
 * the row names no radio, and no part naming another one gives it one: a fragment of another radio gave the row
 * nothing of its sender, and does not now. With @p radio null, the first part naming a radio gives it. False, changing
 * nothing, when a fragment has no part or none is left.
 */
bool
rebuild_from_parts(CallHistoryModel::Row& existing, const CallHistoryModel::Row* radio) {
    if (!parts_complete(existing) && !existing.fragments.isEmpty()) {
        return false;
    }
    const bool keepRadio = radio != nullptr && row_names_source(*radio);
    const qsizetype anchorAt = anchor_part(existing.parts, keepRadio ? radio : nullptr);
    const CallHistoryModel::Row anchor =
        anchorAt >= 0 ? row_of_part(existing.parts.at(anchorAt)) : CallHistoryModel::Row();
    SenderRule rule;
    rule.anchor = anchorAt >= 0 ? &anchor : nullptr;
    rule.radioGone = keepRadio && anchorAt < 0;
    CallHistoryModel::Row rebuilt;
    bool any = false;
    for (qsizetype i = 0; i < existing.parts.size(); i++) {
        if (existing.parts.at(i).withdrawn) {
            continue;
        }
        const CallHistoryModel::Row from = row_of_part(existing.parts.at(i));
        if (any) {
            fold_part(rebuilt, from, rule);
            continue;
        }
        rebuilt = from;
        any = true;
        // The first part keeps its sender only when it may give one: the anchor, or any part with no radio known.
        if (i != anchorAt && !part_gives_sender(CallHistoryModel::Row(), from, rule)) {
            clear_sender(rebuilt);
        }
    }
    if (!any) {
        return false;
    }
    take_part_fields(existing, rebuilt);
    return true;
}

/** @brief The keys of the parts reading @p reading gives (ensure_parts() gave it one per fragment, or its legacy
 * part), in its order. */
QStringList
reading_part_keys(const CallHistoryModel::Row& reading) {
    QStringList keys;
    keys.reserve(reading.parts.size());
    for (const FragmentPart& part : reading.parts) {
        keys.append(part.fragment);
    }
    return keys;
}

/** @brief Fold the parts @p existing holds under keys @p keys, a reading's (merge_parts() put them there), into
 * @p existing, which holds what the row's other parts gave (rest_of_row()), as a merge folds a fragment: the row's
 * radio, when it names one, stays its own. Every part the reading gave folds, a legacy row's own part with the rest. */
void
fold_reading_parts(CallHistoryModel::Row& existing, const QStringList& keys) {
    const PartLookup lookup(existing.parts, keys.size());
    for (const QString& fragment : keys) {
        const qsizetype held = lookup.find(fragment);
        if (held < 0 || existing.parts.at(held).withdrawn) {
            continue;
        }
        const FragmentPart part = existing.parts.at(held);
        const CallHistoryModel::Row anchor = existing;
        SenderRule rule;
        rule.anchor = row_names_source(anchor) ? &anchor : nullptr;
        fold_part(existing, row_of_part(part), rule);
    }
}

/** @brief Mark the parts of fragments @p fragments in @p existing withdrawn (their reading names another call than the
 * row's), adding a withdrawn marker for a fragment that has none: each is kept so the row knows the fragment, and folds
 * nothing until a reading agrees again (merge_parts()). One pass over the parts, whatever the number of fragments. */
void
withdraw_parts(CallHistoryModel::Row& existing, const QStringList& fragments) {
    QSet<QString> missing(fragments.cbegin(), fragments.cend());
    QList<FragmentPart> parts = existing.parts;
    for (FragmentPart& part : parts) {
#ifdef DSD_NEO_TEST_HOOKS
        g_part_lookup_steps.fetch_add(1, std::memory_order_relaxed);
#endif
        if (missing.remove(part.fragment)) {
            part.withdrawn = true;
        }
    }
    for (const QString& fragment : fragments) {
        if (missing.remove(fragment)) {
            FragmentPart marker;
            marker.fragment = fragment;
            marker.withdrawn = true;
            parts.append(marker);
        }
    }
    existing.parts = std::move(parts);
}

/** @brief Make @p rest what its parts but those under keys @p own gave it (rebuild_from_parts(), keeping the row's
 * radio): every part a reading gives again is left out, so no earlier reading of it stays in what the reading is
 * judged against or folded onto. False when no other part is left, so nothing of the row disagrees with the reading. */
bool
rest_of_row(CallHistoryModel::Row& rest, const QStringList& own) {
    // One pass over the parts, whatever the number of keys: a stored row of a long call folds as one reading at load.
    const QSet<QString> keys(own.cbegin(), own.cend());
    QList<FragmentPart> parts = rest.parts;
    for (FragmentPart& part : parts) {
#ifdef DSD_NEO_TEST_HOOKS
        g_part_lookup_steps.fetch_add(1, std::memory_order_relaxed);
#endif
        if (keys.contains(part.fragment)) {
            part.withdrawn = true;
        }
    }
    rest.parts = std::move(parts);
    const CallHistoryModel::Row radio = rest;
    return rebuild_from_parts(rest, &radio);
}

/**
 * @brief Fill what @p existing does not know of its frequency and access code from @p row; keep what it knows.
 *
 * For a notice heard again: the same delivery, so it can only fill in. Across rings the push stamps say nothing
 * about which delivery is newer.
 */
void
fill_carrier(CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (!call_history_freq_known(existing.freqHz) && call_history_freq_known(row.freqHz)) {
        take_freq(existing, row);
    }
    if (!call_history_access_code_known(existing.codeKind) && call_history_access_code_known(row.codeKind)) {
        take_access_code(existing, row);
    }
}

/** @brief Whether @p row is a notice an older build stored: one detail (legacyDetail), no text message or GPS text. */
bool
notice_is_legacy(const CallHistoryModel::Row& row) {
    return row.messageText.isEmpty() && row.gpsText.isEmpty() && !row.legacyDetail.isEmpty();
}

/** @brief The one detail an older build stored for notice @p row: its text message when it has one, else its GPS text
 * (that build's row_take_name()). */
QString
legacy_notice_detail(const CallHistoryModel::Row& row) {
    return row.messageText.isEmpty() ? row.gpsText : row.messageText;
}

/** @brief The name an older build gave notice @p row: its summary, or with none its one detail
 * (legacy_notice_detail()) where this build names it by its whole detail line. */
QString
legacy_notice_name(const CallHistoryModel::Row& row) {
    const QString detail = row_detail_text(row);
    return !detail.isEmpty() && row.name == detail ? legacy_notice_detail(row) : row.name;
}

/**
 * @brief Whether notices @p a and @p b carry one name and detail (issue #800, B2). Two rows of this build compare their
 * whole detail line; against a row an older build stored, the other row's text is what that build would have stored
 * (legacy_notice_detail(), legacy_notice_name()), so a replay after an upgrade finds the stored row and gives it its
 * parts (absorbRepeatedNotice()).
 */
bool
notice_texts_same(const CallHistoryModel::Row& a, const CallHistoryModel::Row& b) {
    const bool aLegacy = notice_is_legacy(a);
    const bool bLegacy = notice_is_legacy(b);
    if (aLegacy == bLegacy) {
        return a.name == b.name && row_detail_text(a) == row_detail_text(b);
    }
    const CallHistoryModel::Row& legacy = aLegacy ? a : b;
    const CallHistoryModel::Row& current = aLegacy ? b : a;
    return legacy.name == legacy_notice_name(current) && legacy.legacyDetail == legacy_notice_detail(current);
}

/**
 * @brief Whether notice @p row is the delivery the logged notice @p existing is, decoded again: field for field, the
 * fix included (issue #800: the same text with another position is another delivery); the texts as notice_texts_same()
 * compares them.
 */
bool
notices_same_delivery(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    return existing.when == row.when && existing.tg == row.tg && existing.src == row.src
           && notice_texts_same(existing, row) && positions_same_fix(existing.pos, row.pos)
           && existing.systemName == row.systemName && existing.systemUid == row.systemUid
           && existing.channel == row.channel && receive_services_compatible(existing, row)
           && rows_identity_compatible(existing, row) && !raw_texts_differ(existing.targetText, row.targetText);
}

/**
 * @brief Whether @p row is a reading of one fragment that is older than a reading of it @p existing already folded in
 * (Row::fragmentVers): the ring's commit_rev when each was read orders two readings of one fragment.
 */
bool
reading_is_stale(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (row.fragments.size() != 1) {
        return false;
    }
    const auto read = row.fragmentVers.constFind(row.fragments.constFirst());
    const auto held = existing.fragmentVers.constFind(row.fragments.constFirst());
    return read != row.fragmentVers.constEnd() && held != existing.fragmentVers.constEnd()
           && held.value() > read.value();
}

/** @brief The newest reading's version of each fragment in @p held or @p read. Built aside, as the fragment list is
 * (unionFragments()). */
QHash<QString, quint64>
newest_fragment_versions(const QHash<QString, quint64>& held, const QHash<QString, quint64>& read) {
    QHash<QString, quint64> versions = held;
    for (auto it = read.constBegin(); it != read.constEnd(); ++it) {
        quint64& version = versions[it.key()];
        version = qMax(version, it.value());
    }
    return versions;
}

/** @brief The fragment key of @p row's anchor: its first part not withdrawn, whose sync type and reception the rebuilt
 * row takes (rebuild_from_parts()); empty when none. */
QString
anchor_fragment(const CallHistoryModel::Row& row) {
    const auto anchor =
        std::find_if(row.parts.cbegin(), row.parts.cend(), [](const FragmentPart& part) { return !part.withdrawn; });
    return anchor != row.parts.cend() ? anchor->fragment : QString();
}

/**
 * @brief Give @p row the sync type and reception its parts give it in their order, the anchor first (rebuild_from_parts(),
 * fold_part_fields()): the anchor's, with a stronger scope of the same reception a later part learned
 * (reception_learns()). A rebuild that folds a new reading of the anchor after the rest (what the row held stays ahead
 * of an update) would otherwise leave the row where the rest alone was heard (issue #800).
 */
void
take_anchor_reception(CallHistoryModel::Row& row) {
    qsizetype anchorAt = -1;
    CallHistoryModel::Row anchored;
    for (qsizetype i = 0; i < row.parts.size(); i++) {
        if (row.parts.at(i).withdrawn) {
            continue;
        }
        const CallHistoryModel::Row from = row_of_part(row.parts.at(i));
        if (anchorAt < 0) {
            anchorAt = i;
            anchored = from;
        } else if (reception_learns(anchored, from)) {
            take_reception(anchored, from);
        }
    }
    if (anchorAt >= 0) {
        row.protocol = anchored.protocol;
        take_reception(row, anchored);
    }
}

/**
 * @brief Take from @p row every GPS unit a fragment gave that was not heard where the row now is (issue #800, fail
 * closed): a rebuild can change the row's sync type or reception (its anchor withdrawn, or another one first), and a
 * fix kept from a fragment of the other P25 phase or another reception is then not the row's. The anchor's own unit
 * stays; a fragment withdrawn from the row already took its unit back. A unit is its fragment's by the fragment's run
 * (its ring's session ordinal in @p ordinals), slot and push stamp. One pass over the parts.
 */
void
requalify_row_gps(CallHistoryModel::Row& row, const QHash<quint64, qint64>& ordinals) {
    const QString anchor = anchor_fragment(row);
    if (anchor.isEmpty() || !row_gps_known(row)) {
        return;
    }
    QSet<QString> elsewhere;
    for (const FragmentPart& part : row.parts) {
        if (part.withdrawn || part.fragment == anchor) {
            continue;
        }
        const QStringList key = part.fragment.split(QLatin1Char(':'));
        bool ringOk = false;
        bool slotOk = false;
        bool seqOk = false;
        const quint64 ring = key.size() == 3 ? key.at(0).toULongLong(&ringOk, 16) : 0U;
        const int slot = key.size() == 3 ? key.at(1).toInt(&slotOk) : 0;
        const qulonglong seq = key.size() == 3 ? key.at(2).toULongLong(&seqOk) : 0U;
        if (ringOk && slotOk && seqOk && !fragment_same_reception(row, row_of_part(part))) {
            elsewhere.insert(gps_fragment_stamp(ordinals.value(ring, 0), slot, seq));
        }
    }
    drop_gps_units_of_stamps(row, elsewhere);
}

/**
 * @brief Rebuild @p existing from @p rest (rest_of_row(): what its parts but the reading's, keys @p own, gave) and then
 * the reading's parts, which merge_parts() or withdraw_parts() put in @p existing: what the row held stays ahead of
 * what an update now says (a conflicting site is discarded, the row's radio stays its own), and a withdrawn reading
 * (@p callOk false) leaves the rest alone. A new reading of the row's anchor (@p readsAnchor) leaves the row where the
 * anchor was heard, not where the rest alone was (take_anchor_reception()).
 */
void
fold_reading_after_rest(CallHistoryModel::Row& existing, CallHistoryModel::Row& rest, const QStringList& own,
                        bool callOk, bool readsAnchor) {
    rest.parts = existing.parts;
    if (callOk) {
        fold_reading_parts(rest, own);
    }
    take_part_fields(existing, rest);
    if (readsAnchor && callOk) {
        take_anchor_reception(existing);
    }
}

} // namespace

int
CallHistoryModel::tryMerge(const Row& row, int scanRows, bool sameReception) {
    if (row.kind != KindVoice) {
        // Notices are discrete deliveries: two SMS a second apart are two
        // messages, never fragments of one.
        return -1;
    }
    if (row.analog) {
        // An analog row never merges (rows_mergeable()); it lands only on the reception it is, read again.
        const int same = sameReception ? same_reception_row(m_rows, row, scanRows) : -1;
        if (same >= 0) {
            foldRow(m_rows[same], row);
        }
        return same;
    }
    for (int i = 0; i < m_rows.size() && i < scanRows; i++) {
        if (rows_mergeable(m_rows.at(i), row)) {
            foldRow(m_rows[i], row);
            return i;
        }
    }
    return -1;
}

void
CallHistoryModel::foldRow(Row& existing, const Row& row) {
    // A reading of a fragment older than one the row already folded (a spill line replayed over a newer save) knows
    // nothing the newer one did not, and what it says otherwise is older: it changes nothing.
    if (reading_is_stale(existing, row)) {
        unionFragments(existing, row.fragments);
        return;
    }
    // The row is the fold of its fragments' parts (FragmentPart), each what one fragment's readings gave, keyed by its
    // ring identity: so whatever a fragment gave (its sender, label, message, destination and call fields) is that
    // fragment's, and goes or changes with it alone.
    ensure_parts(existing);
    Row reading = row;
    ensure_parts(reading);
    const bool namedBefore = row_names_source(existing);
    // A reading is judged against what the row's other parts gave, never against its own earlier readings (an update
    // found by its fragment, ingestRow(), is not merged again). One that names another call (another system, or another
    // raw destination) gives nothing and takes back what its earlier readings gave; a later reading that agrees again
    // gives its part back (merge_parts()). Every row field a part gives is rebuilt from the parts on every fold.
    const QStringList own = reading_part_keys(reading);
    Row rest = existing;
    const bool restKnown = rest_of_row(rest, own);
    const bool callOk = !restKnown || fragment_call_compatible(rest, reading);
    if (callOk) {
        merge_parts(existing, reading);
    } else {
        withdraw_parts(existing, own);
    }
    // The row's anchor (its first fragment still folded in) gives it its sync type and reception: a new reading of the
    // anchor is the row's own reception read again, so an update of its span or alias leaves the row where it was heard.
    // The anchor is the one the parts give once this reading is in (issue #800, fail closed): a withdrawn fragment
    // restored at its own place ahead of the others (merge_parts()) is the row's anchor again, so the row goes back to
    // where it was heard and its fix with it.
    const QString anchor = anchor_fragment(existing);
    const bool readsAnchor = !anchor.isEmpty() && own.contains(anchor);
    // A fix lands only where it was heard (issue #800, fail closed): on the row's reception, by one protocol class
    // (fragment_same_reception()). For any fragment but the anchor that is the reception the row's other fragments,
    // the anchor first, were heard on, read as they were before this reading folds in anything (a stronger scope it
    // brings is the reading's own). A conversation may still merge across carriers or both P25 phases; its fixes do
    // not.
    const bool positionHere = !restKnown || readsAnchor || fragment_same_reception(rest, reading);
    if (restKnown) {
        fold_reading_after_rest(existing, rest, own, callOk, readsAnchor);
    } else {
        (void)rebuild_from_parts(existing, nullptr);
    }
    // The GPS units are kept per fragment beside the parts (Row::gpsOthers): a reading gives its own only when it names
    // the row's radio (fragment_identity_compatible(): an anonymous fragment never enriches a named row, and one naming
    // another radio keeps its fix to itself) and was heard where the row was (positionHere), else its earlier unit goes.
    fold_gps(existing, reading, callOk && positionHere && fragment_identity_compatible(existing, reading),
             !namedBefore && row_names_source(existing));
    // A rebuild may have moved the row to another sync type or reception: every fix it keeps is one heard there.
    requalify_row_gps(existing, m_sessionOrdinals.rings);
    // The same analog reception read again: a row an older build stored learns the mode it did not keep.
    if (existing.analog && dsd_analog_mode_known(existing.analogMode) == 0) {
        existing.analogMode = row.analogMode;
    }
    drop_anonymous_gps_of_named(existing);
    // The fragment's recordings are the merged row's now (none while it is withdrawn: recording_fragments()).
    unionFragments(existing, row.fragments);
    existing.fragmentVers = newest_fragment_versions(existing.fragmentVers, row.fragmentVers);
}

int
CallHistoryModel::rowHoldingFragment(const Row& row) const {
    if (row.kind != KindVoice) {
        return -1;
    }
    for (const QString& fragment : row.fragments) {
        const auto held = m_fragmentIndex.constFind(fragment);
        const int at = held != m_fragmentIndex.constEnd() ? rowForUid(held.value()) : -1;
        if (at >= 0 && m_rows.at(at).kind == KindVoice) {
            return at;
        }
    }
    return -1;
}

int
CallHistoryModel::findRepeatedNotice(const Row& row) const {
    for (int i = 0; i < m_rows.size(); i++) {
        const Row& existing = m_rows.at(i);
        if (existing.kind == KindNotice && notices_same_delivery(existing, row)
            && !m_noticeTwinsTaken.contains(keyFor(existing))) {
            return i;
        }
    }
    return -1;
}

bool
CallHistoryModel::absorbRepeatedNotice(const Row& row) {
    const int repeated = findRepeatedNotice(row);
    if (repeated < 0) {
        return false;
    }
    const QString twin = keyFor(m_rows.at(repeated));
    m_noticeTwinsTaken.insert(twin);
    // Kept on the notice's own seen entry, which the seen store persists: a relaunched model rebuilds
    // the ring's taken twins from it, and its next identical notice then promotes the other twin.
    const auto seen = m_seen.find(keyFor(row));
    if (seen != m_seen.end()) {
        seen->twin = twin;
    }
    Row& logged = m_rows[repeated];
    logged.session = qMax(logged.session, row.session);
    // A notice logged before rows carried a frequency and access code gains them here, when a fresh ring
    // delivers it again (a replay after an upgrade).
    fill_carrier(logged, row);
    // A notice an older build stored keeps its detail as one text: the repeat (the same detail) gives it its parts, and
    // the name this build gives the delivery (notice_texts_same() matched the older one against it): an unsummarised
    // notice is named by its whole detail line now, so a later replay, neither row legacy any more, compares the two
    // as rows of this build and finds it.
    if (logged.messageText.isEmpty() && !row_gps_known(logged)) {
        take_message(logged, row);
        take_gps(logged, row);
        if (!notice_is_legacy(logged)) {
            logged.name = row.name;
            logged.legacyDetail.clear();
        }
    }
    fill_identity(logged, row);
    drop_anonymous_gps_of_named(logged);
    // Its fragment is the logged notice's now, so a spill folded twice finds it there (foldSpill()).
    unionFragments(logged, row.fragments);
    (void)receive_adopt_identity(logged, row);
    const QModelIndex idx = index(repeated);
    Q_EMIT dataChanged(
        idx, idx,
        {SessionRole, FreqHzRole, AccessCodeRole, AccessCodeTextRole, ReceivedToneRole, DetailRole, NameRole});
    return true;
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

void
CallHistoryModel::settleMerged(int merged) {
    static const QVector<int> mergeRoles = {
        WhenRole,           TgRole,          SrcRole,          SourceNameRole, EncRole,        DurationSecsRole,
        DayLabelRole,       TimeTextRole,    EmergencyRole,    SessionRole,    FreqHzRole,     AccessCodeRole,
        AccessCodeTextRole, ServiceNameRole, ReceivedToneRole, NameRole,       RecordingsRole, HasRecordingRole,
        SiteTextRole,       AnalogModeRole,  DetailRole,       GiRole};
    (void)nameChannel(m_rows[merged]);
    const QModelIndex idx = index(merged);
    Q_EMIT dataChanged(idx, idx, mergeRoles);
    // A merge pulls the absorbing row's start earlier, which may now sort below newer rows beneath it; a fragment taken
    // back (another call's, withdraw_parts()) can move it later, above older rows. Restore the
    // newest-first invariant the insertion scan and day sections depend on.
    int newPos = merged;
    while (newPos + 1 < m_rows.size() && m_rows.at(newPos + 1).when > m_rows.at(merged).when) {
        newPos++;
    }
    if (newPos == merged) {
        while (newPos > 0 && m_rows.at(newPos - 1).when < m_rows.at(merged).when) {
            newPos--;
        }
    }
    if (newPos > merged) {
        beginMoveRows(QModelIndex(), merged, merged, QModelIndex(), newPos + 1);
        m_rows.move(merged, newPos);
        endMoveRows();
    } else if (newPos < merged) {
        beginMoveRows(QModelIndex(), merged, merged, QModelIndex(), newPos);
        m_rows.move(merged, newPos);
        endMoveRows();
    }
}

bool
CallHistoryModel::ingestRow(const Row& row, bool isUpdate, bool again) {
    // Coalesce fragments into the call they belong to, with granular model
    // signals: a merge is a dataChanged on the absorbing row, a new call inserts
    // at its sorted (newest-first) position. Never a reset — delegates and the
    // reader's scroll position survive every ingest.
    // A call heard again overlaps the logged call it repeats, which the merge folds it into as it
    // folds any overlapping fragment, so that row joins the running session and no new row is logged.
    // Only the search reaches further: a replay's calls sort by the capture's stamps, anywhere in the
    // log, where a live fragment's call is among the newest rows. An update (its end extending, its
    // source learned, its crypto verdict) searches as far, or it would miss a replayed call's row under
    // newer ones, whether it was a first sighting or heard again. Newest first, so a live update still
    // lands on the row the newest rows hold for it. A first sighting still searches the newest rows only,
    // so a call the first replay logged as two rows, its fragments landing apart under newer rows, stays
    // two, and the repeat folds into the newer of them.
    // An analog row's first sighting searches as far (issue #667): a replay whose receptions took other push stamps (a
    // tone filter that leaves an earlier one out) is read as new, yet it is a reception already logged, matched exactly
    // and one to one (same_reception_row()), so never one a live reception or a same-ring neighbour could be.
    const bool wholeLog = again || isUpdate || row.analog;
    // An update refines the row that already holds its fragment, found by the fragment (issue #799, H1). Merged again
    // by its content it could miss that row once it brings what the row's other fragments disagree with (a site the
    // core filled in later), and be dropped. A row an older build stored keeps no fragments, and is found by merging.
    // A reading the seen store takes for new is still that row's when its fragment is held (issue #800, 2.7): the
    // fragment's ring identity is stable, while its seen key holds its start, which the core can fill in or move later
    // (watchdog_event_merge_stamps()); merged by content it could land on another row or log a second one.
    int merged = rowHoldingFragment(row);
    if (merged >= 0) {
        foldRow(m_rows[merged], row);
    } else {
        merged = tryMerge(row, wholeLog ? static_cast<int>(m_rows.size()) : kMergeScanRows, wholeLog);
    }
    if (merged >= 0) {
        settleMerged(merged);
        return false;
    }
    // Notices never merge, but one decoded again is the same delivery, not a second one: the logged
    // notice joins the running session.
    if (again && !isUpdate && row.kind == KindNotice && absorbRepeatedNotice(row)) {
        return false;
    }
    if (isUpdate) {
        // A seen row that advanced refines a call this model already logged; if
        // its row cannot be found (trimmed, or cleared), inserting it would mint
        // the duplicate the seen map exists to prevent. Drop it instead.
        return false;
    }
    int pos = 0;
    while (pos < m_rows.size() && m_rows.at(pos).when > row.when) {
        pos++;
    }
    Row logged = row;
    adoptNewRow(logged);
    (void)nameChannel(logged);
    beginInsertRows(QModelIndex(), pos, pos);
    m_rows.insert(pos, logged);
    endInsertRows();
#ifdef DSD_NEO_TEST_HOOKS
    m_peakRowsForTest = qMax(m_peakRowsForTest, static_cast<int>(m_rows.size()));
#endif
    return true;
}

bool
CallHistoryModel::fragmentProven(const Row& row) const {
    if (row.fragments.size() != 1) {
        return false;
    }
    const int at = rowHoldingFragment(row);
    if (at < 0) {
        return false;
    }
    const qsizetype part = part_index(m_rows.at(at), row.fragments.constFirst());
    return part >= 0 && m_rows.at(at).parts.at(part).heardVariant != 0U;
}

bool
CallHistoryModel::ingestReading(const Row& row, bool isUpdate, bool again, const QString& heardOwner) {
    // Issue #903: handed on before the fold, whatever the log then does with the reading (a trimmed or cleared row
    // drops an update), and before any trim; once per fragment: a spill line folded again finds its fragment's proof
    // in the log. Its own time, never the conversation's it joins.
    // A proof that reaches a fragment taken in before a Forget or Clear of its number is not handed on: it would undo
    // the edit. A fragment taken in after it is a new call, and is.
    if (!heardOwner.isEmpty() && row.heardVariant != 0U && !fragmentProven(row)
        && !heardForgottenSince(row, isUpdate, heardOwner)) {
        noteHeard(heardOwner, row.heardVariant, row.heardId, row.end > 0 ? row.end : row.when);
    }
    // The Talkgroups screen's order of what was heard (heardSerial()): a fragment taken in as new gets the next serial,
    // an update of one keeps the serial it had, so a Forget there hides what came before it and nothing after.
    Row reading = row;
    reading.heardSerial = isUpdate ? heldSerial(row) : ++m_heardSerial;
    for (FragmentPart& part : reading.parts) {
        part.heardSerial = reading.heardSerial;
    }
    return ingestRow(reading, isUpdate, again);
}

bool
CallHistoryModel::heardForgottenSince(const Row& row, bool isUpdate, const QString& owner) const {
    // A fragment the log does not hold, read as new, is ingested now: after any edit. An update of one the log no
    // longer holds (trimmed or cleared) reads serial 0, before every edit.
    if (!isUpdate && rowHoldingFragment(row) < 0) {
        return false;
    }
    const quint64 serial = heldSerial(row);
    const auto cleared = m_heardClearedThrough.constFind(owner);
    if (cleared != m_heardClearedThrough.cend() && serial <= cleared.value()) {
        return true;
    }
    const auto forgotten = m_heardForgotten.constFind(owner);
    if (forgotten == m_heardForgotten.cend()) {
        return false;
    }
    const auto number = forgotten->constFind(HeardTalkgroupStore::policyId(row.heardVariant, row.heardId));
    return number != forgotten->cend() && serial <= number.value();
}

void
CallHistoryModel::heardForgotten(const QString& owner, quint32 number) {
    if (number == 0U) {
        m_heardClearedThrough.insert(owner, m_heardSerial);
        m_heardForgotten.remove(owner);
        return;
    }
    m_heardForgotten[owner].insert(number, m_heardSerial);
}

quint64
CallHistoryModel::heldSerial(const Row& row) const {
    const int at = rowHoldingFragment(row);
    if (at < 0 || row.fragments.isEmpty()) {
        return 0U;
    }
    const Row& held = m_rows.at(at);
    const qsizetype part = part_index(held, row.fragments.constFirst());
    return part >= 0 ? held.parts.at(part).heardSerial : held.heardSerial;
}

void
CallHistoryModel::noteHeard(const QString& owner, int variant, quint32 id, qint64 whenSecs) {
    if (m_heardSink != nullptr) {
        m_heardSink->observe(owner, variant, id, whenSecs);
        return;
    }
    heard_pending_note(&m_heardPending, owner, variant, id, whenSecs);
}

void
CallHistoryModel::setHeardSink(HeardTalkgroupSink* sink) {
    m_heardSink = sink;
    if (m_heardSink == nullptr) {
        return;
    }
    // What the spill folded at construction gave: its earliest and latest sighting of each talkgroup.
    const HeardPending pending = std::exchange(m_heardPending, {});
    for (const auto& owner : pending) {
        for (const HeardSighting& sighting : owner) {
            m_heardSink->observe(sighting.owner, sighting.variant, sighting.id, sighting.first);
            if (sighting.last != sighting.first) {
                m_heardSink->observe(sighting.owner, sighting.variant, sighting.id, sighting.last);
            }
        }
    }
}

void
CallHistoryModel::setSessionOwners(qint64 sessionId, const QString& singleOwnerUid,
                                   const QHash<QString, QString>& scanEntryOwner) {
    // The running background tap holds a copy of the owners: it hands off what it found and starts again on these.
    const bool resumeTap = handOffBackgroundCaptureForSession();
    HeardSessionOwners who;
    who.single = singleOwnerUid;
    who.scan = scanEntryOwner;
    heard_owners_set_session(&m_heardOwners, sessionId, who);
    saveHeardOwners();
    resumeBackgroundCapture(resumeTap);
}

bool
CallHistoryModel::settleSpill() {
    const bool resumeTap = handOffBackgroundCaptureForSession();
    bool settled = true;
    // A spill no fold read whole (a read that failed, or lines a failed truncate left) is read again first: a line
    // folded twice adds nothing.
    if (m_spillFoldedBytes < 0 && !m_spill->empty() && foldSpill()) {
        Q_EMIT countChanged();
    }
    if (m_spillFoldedBytes >= 0) {
        // One writer at a time, as the destructor's last save: the queued saves finish first.
        m_saveTimer.stop();
        m_savePool.waitForDone();
        m_saveDirty = false;
        settled = saveSync();
        if (!settled) {
            m_saveFailures++;
            m_saveDirty = true;
            if (!m_saveFailing) {
                qWarning("call history: saving the log failed; a remembered talkgroup edit waits for it");
            }
            scheduleSave();
        } else {
            m_saveFailures = 0;
        }
        setSaveFailing(!settled);
    }
    // Settled only once nothing is left to replay: a spill the save could not empty (its truncate failed, or a read
    // failed again) would bring the edit's entries back at the next fold.
    if (settled && !m_spill->empty()) {
        settled = false;
        qWarning("call history: the background spill could not be emptied; a remembered talkgroup edit waits for it");
    }
    resumeBackgroundCapture(resumeTap);
    return settled;
}

std::function<bool()>
CallHistoryModel::spillSettler() {
    return [history = QPointer<CallHistoryModel>(this)]() { return history == nullptr || history->settleSpill(); };
}

void
CallHistoryModel::saveHeardOwners() {
    m_settings.setValue(QLatin1String(kHeardOwnersKey), heard_owners_to_text(m_heardOwners));
}

void
CallHistoryModel::adoptNewRow(Row& row) {
    if (row.uid <= 0) {
        row.uid = m_nextUid++;
    } else {
        m_nextUid = qMax(m_nextUid, row.uid + 1);
    }
    for (const QString& fragment : row.fragments) {
        m_fragmentIndex.insert(fragment, row.uid);
    }
}

void
CallHistoryModel::unionFragments(Row& existing, const QStringList& fragments) {
    // Built aside and assigned rather than appended in place: a save worker may still hold the list
    // (queueSave()), and the row only lets go of it.
    QStringList merged = existing.fragments;
    bool added = false;
    for (const QString& fragment : fragments) {
        if (!merged.contains(fragment)) {
            merged.append(fragment);
            added = true;
        }
        // A row load() is still folding has no uid yet; rebuildFragmentIndex() indexes it then.
        if (existing.uid > 0) {
            m_fragmentIndex.insert(fragment, existing.uid);
        }
    }
    if (added) {
        existing.fragments = std::move(merged);
    }
}

void
CallHistoryModel::rebuildFragmentIndex() {
    m_fragmentIndex.clear();
    for (const Row& row : m_rows) {
        for (const QString& fragment : row.fragments) {
            m_fragmentIndex.insert(fragment, row.uid);
        }
    }
}

bool
CallHistoryModel::noteCommitRevs(const dsd_state* snapshot, bool scan[2]) {
    // Gated on commit_rev, not revision: an active call re-renders its staged row
    // at the poll rate, and each render bumps revision without there being
    // anything new below index 0. Only actual commits, merges and enrichment move
    // commit_rev, so the ring walk runs exactly when it can find something.
    // A new ring (Event_History_I::instance) is walked whatever its commit_rev: a capture replayed in a
    // fresh state can be read first at the very count the last ring stopped at.
    bool anyChanged = false;
    bool ringChanged = false;
    for (int slot = 0; slot < 2; slot++) {
        const quint64 commitRev = static_cast<quint64>(snapshot->event_history_s[slot].commit_rev);
        const quint64 ring = static_cast<quint64>(snapshot->event_history_s[slot].instance);
        ringChanged = ringChanged || ring != m_ringInstance[slot];
        scan[slot] = !m_seeded || commitRev != m_commitRev[slot] || ring != m_ringInstance[slot];
        m_commitRev[slot] = commitRev;
        m_ringInstance[slot] = ring;
        anyChanged = anyChanged || scan[slot];
    }
    if (ringChanged) {
        // The twins notices heard again have taken belong to the ring that heard them.
        restoreNoticeTwinsTaken(m_ringInstance[0], m_ringInstance[1]);
    }
    m_seeded = true;
    return anyChanged;
}

void
CallHistoryModel::restoreNoticeTwinsTaken(quint64 a, quint64 b) {
    // A new ring has taken none yet. A ring read before, by this model or by the one a relaunch replaced,
    // has taken exactly the twins its notices' seen entries record: an entry read again from another ring
    // starts over with no twin.
    m_noticeTwinsTaken.clear();
    for (const SeenState& state : m_seen) {
        if (!state.twin.isEmpty() && state.ring != 0U && (state.ring == a || state.ring == b)) {
            m_noticeTwinsTaken.insert(state.twin);
        }
    }
}

bool
CallHistoryModel::trimToCapacity() {
    // Trimmed rows keep their seen entry: the ring may still hold them, and
    // forgetting the key would re-ingest (and re-trim) each one every tick. The
    // map itself is bounded by pruneSeen() instead.
    const qsizetype excess = m_rows.size() - kMaxRows;
    if (excess <= 0) {
        return false;
    }
#ifdef DSD_NEO_TEST_HOOKS
    m_trimRowsScannedForTest += m_rows.size();
#endif
    // The oldest session's oldest rows, not the bottom of the list. A replay's calls
    // sort below every live one by their stamps, and trimming the bottom would drop
    // them as they land. Of rows alike in both, the lower one goes first. One pass picks
    // every row that goes: the same rows as taking the oldest one at a time.
    QList<int> order(m_rows.size());
    for (int i = 0; i < static_cast<int>(order.size()); i++) {
        order[i] = i;
    }
    const auto goesFirst = [this](int a, int b) {
        const Row& ra = m_rows.at(a);
        const Row& rb = m_rows.at(b);
        if (retained_older(ra.session, ra.when, rb.session, rb.when)) {
            return true;
        }
        return !retained_older(rb.session, rb.when, ra.session, ra.when) && a > b;
    };
    std::nth_element(order.begin(), order.begin() + (excess - 1), order.end(), goesFirst);
    QList<bool> victim(m_rows.size(), false);
    for (qsizetype i = 0; i < excess; i++) {
        victim[order.at(i)] = true;
    }
    // Each run of adjacent victims goes in one removal, from the bottom up so the indices above stay valid.
    for (int last = static_cast<int>(m_rows.size()) - 1; last >= 0; last--) {
        if (!victim.at(last)) {
            continue;
        }
        int first = last;
        while (first > 0 && victim.at(first - 1)) {
            first--;
        }
        for (int i = first; i <= last; i++) {
            for (const QString& fragment : m_rows.at(i).fragments) {
                m_fragmentIndex.remove(fragment);
            }
        }
        beginRemoveRows(QModelIndex(), first, last);
        m_rows.remove(first, last - first + 1);
        endRemoveRows();
        last = first;
    }
    return true;
}

void
CallHistoryModel::refresh(const dsd_state* snapshot, const dsd_opts* opts_snapshot, bool bindNewRings) {
    // The first ingest after the app returns takes over from the background tap: what the tap found is folded in
    // before this read, so the walk below finds those rows seen, and anything newer it finds itself.
    if (m_handoffOwed) {
        handOffBackgroundCapture();
    }
    if (snapshot == nullptr || snapshot->event_history_s == nullptr) {
        return;
    }

    // Issue #903: before the session that owns it, nothing of the starting run's ring is read, its commit revision
    // included: the next read takes it in under the session that begins. What a ring a session owns pushed meanwhile
    // is read, and no one's.
    if (!bindNewRings && !readBeforeSession(snapshot)) {
        return;
    }
    bool scan[2];
    if (!noteCommitRevs(snapshot, scan)) {
        return;
    }
    // Only a snapshot that moved says where the ring is. Right after a start the old
    // session's last snapshot is still the latest, unchanged, and says nothing new; the
    // position last read stays, naming the ring it was read in (beginSession()).
    const qulonglong pushSeq[2] = {static_cast<qulonglong>(snapshot->event_history_s[0].push_seq),
                                   static_cast<qulonglong>(snapshot->event_history_s[1].push_seq)};
    // initState() draws one identity for both slots' rings.
    const quint64 ring = static_cast<quint64>(snapshot->event_history_s[0].instance);
    m_ringPushSeq[0] = pushSeq[0];
    m_ringPushSeq[1] = pushSeq[1];
    m_ringPushSeqRing = ring;
    m_ringRead = true;
    settleClear(pushSeq, ring);

    bool seenSettled = false;
    const QList<FreshRow> fresh = collectFresh(snapshot, scan, opts_snapshot != nullptr, &seenSettled);

    if (fresh.isEmpty()) {
        if (seenSettled) {
            // Only the seen store moved: save it, so an older build's entry takes its digest once.
            markChanged();
        }
        return;
    }

    bool rowsChanged = false;
    for (const FreshRow& item : fresh) {
        if (item.clearedProof) {
            noteHeard(item.heardOwner, item.row.heardVariant, item.row.heardId,
                      item.row.end > 0 ? item.row.end : item.row.when);
            continue;
        }
        rowsChanged = ingestReading(item.row, item.isUpdate, item.again, item.heardOwner) || rowsChanged;
    }
    rowsChanged = trimToCapacity() || rowsChanged;
    pruneSeen();
    if (rowsChanged) {
        Q_EMIT countChanged();
    }
    markChanged();
}

bool
CallHistoryModel::readBeforeSession(const dsd_state* snapshot) {
    const quint64 ring = static_cast<quint64>(snapshot->event_history_s[0].instance);
    if (!heard_owners_ring_bound(m_heardOwners, ring)) {
        return false;
    }
    // Issue #903 (fail closed): what a ring a session owns pushed after the last read, found between the host's accept
    // and the new session, is the old run's last or the new run's first on the ring it reused. Nothing tells which: it
    // is no one's, and so are its later revisions. A ring not read last is taken whole.
    const bool readLast = m_ringRead && m_ringPushSeqRing == ring;
    const quint64 after[2] = {readLast ? static_cast<quint64>(m_ringPushSeq[0]) : 0U,
                              readLast ? static_cast<quint64>(m_ringPushSeq[1]) : 0U};
    const quint64 through[2] = {static_cast<quint64>(snapshot->event_history_s[0].push_seq),
                                static_cast<quint64>(snapshot->event_history_s[1].push_seq)};
    if (heard_owners_note_unowned(&m_heardOwners, ring, after, through)) {
        saveHeardOwners();
    }
    return true;
}

void
CallHistoryModel::pruneSeenMap(SeenMap& seen) {
    // Drop the oldest entries once the map is well past what the ring could
    // still resurrect (at most 254 committed rows per slot). Oldest-first in
    // retention order (session, then stamp) from the value, so keys stay opaque.
    // By the stamp alone, a replay's entries would go first and its ring rows
    // would come back as new calls on the next scan.
    if (seen.size() <= static_cast<qsizetype>(kMaxSeenEntries) * 2) {
        return;
    }
    QList<QPair<qint64, qint64>> stamps;
    stamps.reserve(seen.size());
    for (const SeenState& state : seen) {
        stamps.append(qMakePair(state.session, state.when));
    }
    std::sort(stamps.begin(), stamps.end());
    const QPair<qint64, qint64> cutoff = stamps.at(stamps.size() - kMaxSeenEntries);
    for (auto it = seen.begin(); it != seen.end();) {
        if (retained_older(it->session, it->when, cutoff.first, cutoff.second)) {
            it = seen.erase(it);
        } else {
            ++it;
        }
    }
}

void
CallHistoryModel::clearAll() {
    // A clear while the background tap runs (the app hidden but its loop running) covers what the tap read too: the
    // tap is handed off (stopped, its read position joining the model's, its spill and what it could not write folded
    // in) before the rows go, and it starts again behind the new mark. Everything it found is cleared rows; folded
    // first, their verified talkgroups still reach the remembered list (issue #903), and the rings it bound stay bound.
    // A tap the app's return already owes a handoff (no ingest since) stays stopped in the foreground.
    const bool tapWasRunning = m_tap && m_tap->running();
    const bool resumeTap = tapWasRunning && !m_handoffOwed;
    if (tapWasRunning) {
        handOffBackgroundCapture();
    }
    m_handoffOwed = false;
    m_clearRev++;
    // Removed rather than reset: a ListView that is still building a row ahead of the
    // viewport (its cache buffer) cancels that row by index on a reset, after the model
    // is already empty, and the cancel misses; a removal it applies in step.
    const bool had_rows = !m_rows.isEmpty();
    if (had_rows) {
        beginRemoveRows(QModelIndex(), 0, static_cast<int>(m_rows.size()) - 1);
    }
    m_rows.clear();
    m_fragmentIndex.clear();
    // The uids of the rows just cleared are never handed out again.
    m_settings.setValue(QLatin1String(kNextUidKey), m_nextUid);
    m_spill->clear();
    m_spillFoldedBytes = -1;
    // m_seen deliberately survives: the ring still holds the rows just cleared, and
    // forgetting the keys would let the next tick re-ingest every one of them.
    // m_seen alone is not enough, though. Its store is written later, on the worker,
    // and is bounded. A relaunched UI can rebuild it without the cleared rows while the
    // service's ring still holds every one. The persisted clear mark is what keeps a
    // clear effective across an Activity restart. It names the cleared rows by this
    // session's ring position, never by a stamp or a clock reading. A replay's rows are
    // stamped with the capture's time, and a watermark would hide every later call
    // stamped before it.
    m_clear.active = true;
    m_clear.session = m_session;
    m_clear.pending = !m_ringRead;
    m_clear.pushSeq[0] = m_ringRead ? m_ringPushSeq[0] : 0U;
    m_clear.pushSeq[1] = m_ringRead ? m_ringPushSeq[1] : 0U;
    m_clear.ring = m_ringRead ? m_ringPushSeqRing : 0U;
    saveClear();
    if (had_rows) {
        endRemoveRows();
    }
    Q_EMIT countChanged();
    // The mark above is what makes the clear durable (QSettings writes it
    // through); the emptied stores can follow on the worker without a stall here.
    m_generation++;
    startAsyncSave();
    if (resumeTap) {
        startBackgroundCapture();
    }
}

namespace {

/**
 * @brief A stored access code as {kind, value}, or {0, 0} when it is missing or does not fit the ring's fields.
 *
 * Stores older builds wrote have no code: a missing key means unknown.
 */
QPair<int, int>
json_access_code(const QJsonObject& obj, const char* kindKey, const char* codeKey) {
    const qint64 kind = json_i64_or(obj, kindKey, DSD_ACCESS_CODE_NONE);
    const qint64 code = json_i64_or(obj, codeKey, 0);
    if (!access_code_fits(kind, code) || !call_history_access_code_known(static_cast<int>(kind))) {
        return qMakePair(0, 0);
    }
    return qMakePair(static_cast<int>(kind), static_cast<int>(code));
}

/* Issue #799: a row's site is stored by its parts (`site`), each key only with its part. The DMR texts are stored as
   the record holds them, so a reloaded row shows what it showed; no compare reads them. */
constexpr uint32_t kSiteDmrParts =
    DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND | DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE;
constexpr uint32_t kSiteEdacsParts = DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_AREA | DSD_RXID_EDACS_SYSTEM;

/** @brief One stored value of a site: written and read while any of @c parts is valid. */
template <typename T>
struct SiteField {
    uint32_t parts;
    const char* key;
    T dsd_received_identity::* member;
};

const SiteField<uint32_t> kSiteFields32[] = {
    {DSD_RXID_P25_WACN, "wacn", &dsd_received_identity::p25_wacn},
    {DSD_RXID_NXDN_LOCATION, "nxdn_sys", &dsd_received_identity::nxdn_sys},
    {DSD_RXID_NXDN_TYPED_SYSTEM, "nxdn_typed_sys", &dsd_received_identity::nxdn_typed_sys},
};

const SiteField<uint16_t> kSiteFields16[] = {
    {DSD_RXID_P25_SYSID, "sysid", &dsd_received_identity::p25_sysid},
    {DSD_RXID_NXDN_LOCATION, "nxdn_site", &dsd_received_identity::nxdn_site},
    {DSD_RXID_DMR_T3_SYSCODE, "dmr_t3_syscode", &dsd_received_identity::dmr_t3_syscode},
    {DSD_RXID_DMR_NET, "dmr_net", &dsd_received_identity::dmr_net},
    {DSD_RXID_DMR_SITE, "dmr_site", &dsd_received_identity::dmr_site},
    {DSD_RXID_EDACS_SITE, "edacs_site", &dsd_received_identity::edacs_site},
    {DSD_RXID_EDACS_AREA, "edacs_area", &dsd_received_identity::edacs_area},
    {DSD_RXID_EDACS_SYSTEM, "edacs_system", &dsd_received_identity::edacs_system},
    {DSD_RXID_TETRA_NET, "tetra_mcc", &dsd_received_identity::tetra_mcc},
    {DSD_RXID_TETRA_NET, "tetra_mnc", &dsd_received_identity::tetra_mnc},
    {DSD_RXID_TETRA_LA, "tetra_la", &dsd_received_identity::tetra_la},
};

const SiteField<uint8_t> kSiteFields8[] = {
    {DSD_RXID_P25_SITE, "rfss", &dsd_received_identity::p25_rfss},
    {DSD_RXID_P25_SITE, "site", &dsd_received_identity::p25_site},
    {DSD_RXID_P25_LRA, "lra", &dsd_received_identity::p25_lra},
    {DSD_RXID_NXDN_LOCATION, "nxdn_category", &dsd_received_identity::nxdn_category},
    {DSD_RXID_NXDN_IDAS_AREA, "idas_area", &dsd_received_identity::nxdn_idas_area},
    {DSD_RXID_NXDN_IDAS_SITE, "idas_site_type", &dsd_received_identity::nxdn_idas_site_type},
    {DSD_RXID_NXDN_IDAS_SITE, "idas_site_code", &dsd_received_identity::nxdn_idas_site_code},
    {DSD_RXID_NXDN_TYPED_SYSTEM, "nxdn_typed_site", &dsd_received_identity::nxdn_typed_site},
    {kSiteDmrParts, "dmr_trunk", &dsd_received_identity::dmr_trunk},
    {DSD_RXID_DMR_SITE, "dmr_t3_model", &dsd_received_identity::dmr_t3_model},
    {DSD_RXID_TETRA_NET, "tetra_colour", &dsd_received_identity::tetra_colour},
};

const SiteField<int8_t> kSiteFieldsSigned8[] = {
    {kSiteEdacsParts, "edacs_ea_mode", &dsd_received_identity::edacs_ea_mode},
};

template <typename T, size_t N>
void
site_fields_to_json(const dsd_received_identity& id, const SiteField<T> (&fields)[N], QJsonObject& obj) {
    for (const SiteField<T>& field : fields) {
        if ((id.valid & field.parts) != 0U) {
            obj.insert(QLatin1String(field.key), static_cast<qint64>(id.*field.member));
        }
    }
}

/** @brief Read every value of @p fields a part @p id holds valid has; false when one is missing or does not fit. */
template <typename T, size_t N>
bool
site_fields_from_json(const QJsonObject& obj, const SiteField<T> (&fields)[N], dsd_received_identity& id) {
    return std::all_of(std::begin(fields), std::end(fields), [&obj, &id](const SiteField<T>& field) {
        return (id.valid & field.parts) == 0U || json_field_fits(obj, field.key, &(id.*field.member));
    });
}

/** @brief A text the record holds in @p capacity bytes, copied into zeroed @p out; false when it does not fit. */
bool
json_text_fits(const QJsonObject& obj, const char* key, char* out, size_t capacity) {
    const QJsonValue value = obj.value(QLatin1String(key));
    const QByteArray text = value.isString() ? value.toString().toUtf8() : QByteArray();
    if (!value.isString() || static_cast<size_t>(text.size()) >= capacity || text.contains('\0')) {
        return false;
    }
    DSD_MEMCPY(out, text.constData(), static_cast<size_t>(text.size()));
    return true;
}

/**
 * @brief The length of the first @p len bytes of @p text without a UTF-8 character its field cut short.
 *
 * The decoder bounds a text to its field, which can cut a character. Decoded, the cut bytes read as U+FFFD, three
 * bytes, so the stored text could no longer fit its field and the whole site would be refused on reload.
 */
size_t
utf8_whole_length(const char* text, size_t len) {
    size_t start = len;
    while (start > 0U && len - start < 3U && (static_cast<unsigned char>(text[start - 1U]) & 0xC0U) == 0x80U) {
        start--;
    }
    if (start == 0U) {
        return len;
    }
    const unsigned char lead = static_cast<unsigned char>(text[start - 1U]);
    size_t need = 1U;
    if ((lead & 0xE0U) == 0xC0U) {
        need = 2U;
    } else if ((lead & 0xF0U) == 0xE0U) {
        need = 3U;
    } else if ((lead & 0xF8U) == 0xF0U) {
        need = 4U;
    }
    return len - (start - 1U) < need ? start - 1U : len;
}

QString
site_text_value(const char* text, size_t capacity) {
    return QString::fromUtf8(text, static_cast<qsizetype>(utf8_whole_length(text, qstrnlen(text, capacity))));
}

QJsonObject
site_to_json(const dsd_received_identity& id) {
    QJsonObject site;
    site.insert(QLatin1String("valid"), static_cast<qint64>(id.valid));
    site.insert(QLatin1String("family"), id.family);
    site_fields_to_json(id, kSiteFields32, site);
    site_fields_to_json(id, kSiteFields16, site);
    site_fields_to_json(id, kSiteFields8, site);
    site_fields_to_json(id, kSiteFieldsSigned8, site);
    if ((id.valid & DSD_RXID_DMR_SITE_TEXT) != 0U) {
        site.insert(QLatin1String("dmr_site_text"), site_text_value(id.dmr_site_text, sizeof(id.dmr_site_text)));
    }
    if ((id.valid & DSD_RXID_DMR_BRAND) != 0U) {
        site.insert(QLatin1String("dmr_brand"), site_text_value(id.dmr_brand, sizeof(id.dmr_brand)));
    }
    return site;
}

/**
 * @brief A stored site, or none when it is missing (a store an older build wrote) or is no site a record could hold.
 *
 * Every value must fit its field, and the whole must pass the record's own rules (dsd_rxid_site_consistent(): the
 * family's parts only, the value ranges, the cross parts, the NXDN codes the standard reserves): a corrupt or
 * hand-edited site loads as no site rather than as one nobody received.
 */
dsd_received_identity
site_from_json(const QJsonObject& obj) {
    dsd_received_identity id = {};
    dsd_received_identity checked = {};
    const QJsonValue value = obj.value(QLatin1String("site"));
    if (!value.isObject()) {
        return checked;
    }
    const QJsonObject site = value.toObject();
    const bool read = json_field_fits(site, "valid", &id.valid) && json_field_fits(site, "family", &id.family)
                      && site_fields_from_json(site, kSiteFields32, id)
                      && site_fields_from_json(site, kSiteFields16, id) && site_fields_from_json(site, kSiteFields8, id)
                      && site_fields_from_json(site, kSiteFieldsSigned8, id)
                      && ((id.valid & DSD_RXID_DMR_SITE_TEXT) == 0U
                          || json_text_fits(site, "dmr_site_text", id.dmr_site_text, sizeof(id.dmr_site_text)))
                      && ((id.valid & DSD_RXID_DMR_BRAND) == 0U
                          || json_text_fits(site, "dmr_brand", id.dmr_brand, sizeof(id.dmr_brand)));
    if (read && dsd_rxid_site_consistent(&id) != 0) {
        dsd_rxid_site_snapshot(&id, &checked);
    }
    return checked;
}

/**
 * @brief Read a stored row's site and call channel (issue #799), and the fragment the channel came from.
 *
 * A channel in no DMR numbering, wider than its field, or in another numbering than its site's trunking system's is
 * no channel. A channel written without its provenance takes the row's own, as the frequency's does.
 */
void
row_site_from_json(CallHistoryModel::Row& row, const QJsonObject& obj) {
    row.siteId = site_from_json(obj);
    uint32_t channel = 0U;
    uint8_t kind = DSD_CALL_CHANNEL_NONE;
    if (json_field_fits(obj, "cch", &channel) && json_field_fits(obj, "cchKind", &kind)
        && call_history_call_channel_known(kind, channel)
        && dsd_call_site_compatible(&row.siteId, kind, nullptr, DSD_CALL_CHANNEL_NONE) != 0) {
        row.callChannel = channel;
        row.callChannelKind = kind;
    }
    row.cchWhen = json_i64_or(obj, "cchWhen", row.when);
    row.cchSeq = static_cast<qulonglong>(json_i64_or(obj, "cchSeq", static_cast<qint64>(row.seq)));
    row.cchSlot = static_cast<int>(json_i64_or(obj, "cchSlot", row.slot));
}

/** @brief Write a row's site by its parts and its call channel with the fragment it came from; nothing unknown. */
void
row_site_to_json(const CallHistoryModel::Row& row, QJsonObject& obj) {
    if (row.siteId.valid != 0U) {
        obj.insert(QLatin1String("site"), site_to_json(row.siteId));
    }
    if (call_history_call_channel_known(row.callChannelKind, row.callChannel)) {
        obj.insert(QLatin1String("cch"), static_cast<qint64>(row.callChannel));
        obj.insert(QLatin1String("cchKind"), row.callChannelKind);
        obj.insert(QLatin1String("cchWhen"), row.cchWhen);
        obj.insert(QLatin1String("cchSeq"), static_cast<qint64>(row.cchSeq));
        obj.insert(QLatin1String("cchSlot"), row.cchSlot);
    }
}

/**
 * @brief Read a stored row's frequency and access code, and the fragment each came from.
 *
 * Unknown values are never written, so a missing key reads as unknown. A known value written without its
 * provenance takes the row's own identity, as the source label's does.
 */
void
row_carrier_from_json(CallHistoryModel::Row& row, const QJsonObject& obj) {
    row.freqHz = qMax<qint64>(json_i64_or(obj, "freqHz", 0), 0);
    row.freqWhen = json_i64_or(obj, "freqWhen", row.when);
    row.freqSeq = static_cast<qulonglong>(json_i64_or(obj, "freqSeq", static_cast<qint64>(row.seq)));
    row.freqSlot = static_cast<int>(json_i64_or(obj, "freqSlot", row.slot));
    const QPair<int, int> code = json_access_code(obj, "acKind", "ac");
    row.codeKind = code.first;
    row.code = code.second;
    row.acWhen = json_i64_or(obj, "acWhen", row.when);
    row.acSeq = static_cast<qulonglong>(json_i64_or(obj, "acSeq", static_cast<qint64>(row.seq)));
    row.acSlot = static_cast<int>(json_i64_or(obj, "acSlot", row.slot));
}

/**
 * @brief Read a stored analog row's mode (issue #667), and name a row stored with no name by it.
 *
 * A store older builds wrote keeps no mode: its rows read unknown, never a mode guessed from their name (which every
 * analog row had as "NFM"), and keep the name they were stored with.
 */
void
row_analog_from_json(CallHistoryModel::Row& row, const QJsonObject& obj) {
    const qint64 mode = json_i64_or(obj, "analogMode", DSD_ANALOG_MODE_UNKNOWN);
    row.analogMode =
        row.analog && mode >= 0 && mode <= UINT8_MAX && dsd_analog_mode_known(static_cast<uint8_t>(mode)) != 0
            ? static_cast<uint8_t>(mode)
            : static_cast<uint8_t>(DSD_ANALOG_MODE_UNKNOWN);
    if (row.analog && row.kind == CallHistoryModel::KindVoice && row.name.isEmpty()) {
        row.name = analog_fallback_name(row.analogMode);
    }
}

/**
 * @brief A stored voice row's end (B1): `end` when stored; a row an older build stored, which kept none, ends where its
 * known duration says, else not known. An end before the row's start is not known, and a notice keeps none.
 */
void
row_end_from_json(CallHistoryModel::Row& row, const QJsonObject& obj) {
    const qint64 derived = row.durationSecs >= 0 ? row.when + row.durationSecs : 0;
    const qint64 end = json_i64_or(obj, "end", derived);
    row.end = row.kind == CallHistoryModel::KindVoice && row.when > 0 && end >= row.when ? end : 0;
}

/** @brief A verified talkgroup proof and the live-input latch it was read with (issue #903), as rows, parts and seen
 * entries store them. */
struct StoredHeard {
    uint8_t variant = 0U;
    quint32 id = 0U;
    bool live = false;
};

/** @brief Store @p heard: the proof (`hv`, `hid`) and the latch (`live`), each only when set. */
void
heard_to_json(const StoredHeard& heard, QJsonObject& obj) {
    if (heard.variant != 0U) {
        obj.insert(QLatin1String("hv"), static_cast<int>(heard.variant));
        obj.insert(QLatin1String("hid"), static_cast<qint64>(heard.id));
    }
    if (heard.live) {
        obj.insert(QLatin1String("live"), true);
    }
}

/** @brief What heard_to_json() stored: a proof is a variant and a non-zero id that fit their fields together, or none.
 * Absent in stores older builds wrote: no proof, not live. */
StoredHeard
heard_from_json(const QJsonObject& obj) {
    StoredHeard heard;
    uint8_t variant = 0U;
    quint32 id = 0U;
    if (json_field_fits(obj, "hv", &variant) && json_field_fits(obj, "hid", &id) && variant != 0U && id != 0U) {
        heard.variant = variant;
        heard.id = id;
    }
    heard.live = obj.value(QLatin1String("live")).toBool();
    return heard;
}

/** @brief A row's or part's proof and latch (heard_to_json()), its owner (`vo`) when it has one, and its -Y scan-list
 * row (`slr`, and its list's identity `sll` as hex text, which can use all 64 bits) when on one. */
void
row_heard_to_json(const CallHistoryModel::Row& row, QJsonObject& obj) {
    heard_to_json(StoredHeard{row.heardVariant, row.heardId, row.liveInput}, obj);
    if (!row.viewOwner.isEmpty()) {
        obj.insert(QLatin1String("vo"), row.viewOwner);
    }
    if (row.scanListRow != 0U) {
        obj.insert(QLatin1String("slr"), static_cast<qint64>(row.scanListRow));
        obj.insert(QLatin1String("sll"), QString::number(row.scanListRev, 16));
    }
}

void
row_heard_from_json(CallHistoryModel::Row& row, const QJsonObject& obj) {
    const StoredHeard heard = heard_from_json(obj);
    row.heardVariant = heard.variant;
    row.heardId = heard.id;
    row.liveInput = heard.live;
    row.viewOwner = obj.value(QLatin1String("vo")).toString();
    // A row whose pair does not read whole is on none.
    bool revOk = false;
    const quint64 rev = obj.value(QLatin1String("sll")).toString().toULongLong(&revOk, 16);
    quint32 scanListRow = 0U;
    if (json_field_fits(obj, "slr", &scanListRow) && scanListRow != 0U && revOk) {
        row.scanListRow = scanListRow;
        row.scanListRev = rev;
    }
}

/** @brief One stored fragment part (row_parts_to_json()), read as the row's own fields are. A part stored without its
 * sender (`pv` missing: an earlier build of #800's parts) takes it from @p row as legacy_part_of() gives it. */
FragmentPart
part_from_json(const QJsonObject& entry, const QString& fragment, const CallHistoryModel::Row& row) {
    CallHistoryModel::Row fields;
    fields.when = json_i64_or(entry, "when", 0);
    const qint64 end = json_i64_or(entry, "end", 0);
    fields.end = fields.when > 0 && end >= fields.when ? end : 0;
    fields.durationSecs = static_cast<int>(qBound<qint64>(-1, json_i64_or(entry, "durationSecs", -1),
                                                          static_cast<qint64>(std::numeric_limits<int>::max())));
    fields.enc = entry.value(QLatin1String("enc")).toBool();
    fields.emergency = entry.value(QLatin1String("em")).toBool();
    fields.name = entry.value(QLatin1String("name")).toString();
    fields.service = entry.value(QLatin1String("service")).toString();
    fields.receiveKind = entry.value(QLatin1String("receiveKind")).toInt(DSD_RECEIVE_NONE);
    fields.receiveValue = entry.value(QLatin1String("receiveValue")).toInt();
    fields.receiveKnown = entry.value(QLatin1String("receiveKnown")).toBool();
    fields.receiveSlot = entry.value(QLatin1String("receiveSlot")).toInt();
    fields.session = json_i64_or(entry, "session", row.session);
    row_carrier_from_json(fields, entry);
    row_site_from_json(fields, entry);
    row_heard_from_json(fields, entry);
    if (entry.value(QLatin1String("pv")).toInt() < kPartVersionSender) {
        fields.rxScope = stored_scope_from_json(entry, "rx");
        FragmentPart part = fragment_part_of(fields, fragment);
        take_part_sender(part, legacy_part_of(row, fragment));
        return part;
    }
    // The sender, call identity and message, as a row's (`proto`, `srcText`, `tgtText`, `sscope`, `rx`, `msg*`).
    row_payload_from_json(fields, entry);
    fields.src = static_cast<qulonglong>(qMax<qint64>(json_i64_or(entry, "src", 0), 0));
    // A part an earlier build of this one stored keeps no target: every part of a row then had the row's.
    fields.tg = static_cast<qulonglong>(qMax<qint64>(json_i64_or(entry, "tg", static_cast<qint64>(row.tg)), 0));
    fields.gi = stored_gi(json_i64_or(entry, "gi", row.gi));
    fields.sourceName = entry.value(QLatin1String("srcName")).toString();
    fields.sourceNameWhen = json_i64_or(entry, "srcNameWhen", fields.when);
    fields.sourceNameSeq = static_cast<qulonglong>(json_i64_or(entry, "srcNameSeq", 0));
    fields.sourceNameSlot = static_cast<int>(json_i64_or(entry, "srcNameSlot", 0));
    return fragment_part_of(fields, fragment);
}

/** @brief A stored voice row's fragment parts: one per fragment it holds, the first stored for each; a row of one
 * fragment with none stored is its own part. A part of a fragment the row does not hold is dropped. */
QList<FragmentPart>
row_parts_from_json(const CallHistoryModel::Row& row, const QJsonObject& obj) {
    QList<FragmentPart> parts;
    if (row.kind != CallHistoryModel::KindVoice) {
        return parts;
    }
    for (const auto& value : obj.value(QLatin1String("fp")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString fragment = entry.value(QLatin1String("fr")).toString();
        // A legacy part (or one an earlier build stored with no key) stands for what a row stored without fragments
        // held (ensure_parts()).
        if ((!fragment.isEmpty() && !is_legacy_part_key(fragment) && !row.fragments.contains(fragment))
            || std::any_of(parts.cbegin(), parts.cend(),
                           [&fragment](const FragmentPart& held) { return held.fragment == fragment; })) {
            continue;
        }
        FragmentPart part;
        if (entry.value(QLatin1String("x")).toBool()) {
            part.fragment = fragment;
            part.withdrawn = true;
        } else {
            part = part_from_json(entry, fragment, row);
        }
        parts.append(part);
    }
    if (parts.isEmpty() && row.fragments.size() == 1) {
        parts.append(fragment_part_of(row, row.fragments.constFirst()));
    }
    return parts;
}

/** @brief A call kind (Row::gi) into @p obj as `gi`, only when known: an unknown one is left out (it reads -1). */
void
put_gi(QJsonObject& obj, qint8 gi) {
    if (gi >= 0) {
        obj.insert(QLatin1String("gi"), static_cast<int>(gi));
    }
}

/** @brief One stored row. */
CallHistoryModel::Row
row_from_json(const QJsonObject& obj) {
    CallHistoryModel::Row row;
    row.when = obj.value(QLatin1String("when")).toVariant().toLongLong();
    row.name = obj.value(QLatin1String("name")).toString();
    row.tg = obj.value(QLatin1String("tg")).toVariant().toULongLong();
    row.gi = stored_gi(json_i64_or(obj, "gi", -1));
    row.src = obj.value(QLatin1String("src")).toVariant().toULongLong();
    row.sourceName = obj.value(QLatin1String("srcName")).toString();
    row.enc = obj.value(QLatin1String("enc")).toBool();
    row.emergency = obj.value(QLatin1String("em")).toBool();
    row.durationSecs = obj.value(QLatin1String("durationSecs")).toInt(-1);
    row.systemName = obj.value(QLatin1String("systemName")).toString();
    row.systemUid = obj.value(QLatin1String("systemUid")).toString();
    row.kind = obj.value(QLatin1String("kind")).toInt(CallHistoryModel::KindVoice);
    row_end_from_json(row, obj);
    row.channel = obj.value(QLatin1String("channel")).toString();
    row.channelName = obj.value(QLatin1String("channelName")).toString();
    row.service = obj.value(QLatin1String("service")).toString();
    row.analog = obj.value(QLatin1String("analog")).toBool();
    row_analog_from_json(row, obj);
    row.receiveKind = obj.value(QLatin1String("receiveKind")).toInt(DSD_RECEIVE_NONE);
    row.receiveValue = obj.value(QLatin1String("receiveValue")).toInt();
    row.receiveKnown = obj.value(QLatin1String("receiveKnown")).toBool();
    row.receiveSlot = obj.value(QLatin1String("receiveSlot")).toInt();
    row.slot = obj.value(QLatin1String("slot")).toInt();
    row.seq = obj.value(QLatin1String("seq")).toVariant().toULongLong();
    // Older stores have no label provenance; their surviving row identity
    // is the best available starting point.
    row.sourceNameWhen = obj.contains(QLatin1String("srcNameWhen"))
                             ? obj.value(QLatin1String("srcNameWhen")).toVariant().toLongLong()
                             : row.when;
    row.sourceNameSeq = obj.contains(QLatin1String("srcNameSeq"))
                            ? obj.value(QLatin1String("srcNameSeq")).toVariant().toULongLong()
                            : row.seq;
    row.sourceNameSlot = obj.value(QLatin1String("srcNameSlot")).toInt(row.slot);
    row.session = obj.value(QLatin1String("session")).toVariant().toLongLong();
    row_carrier_from_json(row, obj);
    if (row.kind == CallHistoryModel::KindVoice) {
        row_site_from_json(row, obj);
    }
    row_payload_from_json(row, obj);
    drop_anonymous_gps_of_named(row);
    // Issue #708; absent in stores older builds wrote (a uid is then assigned at load).
    row.uid = qMax<qint64>(json_i64_or(obj, "uid", 0), 0);
    for (const auto& fragment : obj.value(QLatin1String("fr")).toArray()) {
        const QString key = fragment.toString();
        if (!key.isEmpty() && !row.fragments.contains(key)) {
            row.fragments.append(key);
        }
    }
    // Issue #800: the newest reading's version of each of those fragments; absent in stores older builds wrote.
    const QJsonObject versions = obj.value(QLatin1String("frv")).toObject();
    for (auto it = versions.constBegin(); it != versions.constEnd(); ++it) {
        const qint64 version = it.value().toInteger(0);
        if (version > 0 && row.fragments.contains(it.key())) {
            row.fragmentVers.insert(it.key(), static_cast<quint64>(version));
        }
    }
    row_heard_from_json(row, obj);
    row.parts = row_parts_from_json(row, obj);
    // A row an older build stored without parts is converted into the same model now (issue #800): a load merge folds
    // it as a part of its own, as every later fold does.
    ensure_parts(row);
    return row;
}

/** @brief Write a row's frequency and access code with the fragment each came from; nothing for an unknown one. */
void
row_carrier_to_json(const CallHistoryModel::Row& row, QJsonObject& obj) {
    if (call_history_freq_known(row.freqHz)) {
        obj.insert(QLatin1String("freqHz"), row.freqHz);
        obj.insert(QLatin1String("freqWhen"), row.freqWhen);
        obj.insert(QLatin1String("freqSeq"), static_cast<qint64>(row.freqSeq));
        obj.insert(QLatin1String("freqSlot"), row.freqSlot);
    }
    if (call_history_access_code_known(row.codeKind)) {
        obj.insert(QLatin1String("acKind"), row.codeKind);
        obj.insert(QLatin1String("ac"), row.code);
        obj.insert(QLatin1String("acWhen"), row.acWhen);
        obj.insert(QLatin1String("acSeq"), static_cast<qint64>(row.acSeq));
        obj.insert(QLatin1String("acSlot"), row.acSlot);
    }
}

/** @brief Store a voice row's fragment parts (`fp`, Row::parts) when it has two or more; each part as the row's own
 * fields are stored, under its fragment key (`fr`), with `x` when withdrawn, and `pv` for a part that holds its
 * sender. */
void
row_parts_to_json(const CallHistoryModel::Row& row, QJsonObject& obj) {
    if (row.kind != CallHistoryModel::KindVoice || row.parts.size() < 2) {
        return;
    }
    QJsonArray stored;
    for (const FragmentPart& part : row.parts) {
        QJsonObject entry;
        entry.insert(QLatin1String("fr"), part.fragment);
        if (part.withdrawn) {
            entry.insert(QLatin1String("x"), true);
            stored.append(entry);
            continue;
        }
        const CallHistoryModel::Row fields = row_of_part(part);
        entry.insert(QLatin1String("when"), fields.when);
        if (fields.end > 0) {
            entry.insert(QLatin1String("end"), fields.end);
        }
        entry.insert(QLatin1String("durationSecs"), fields.durationSecs);
        if (fields.enc) {
            entry.insert(QLatin1String("enc"), true);
        }
        if (fields.emergency) {
            entry.insert(QLatin1String("em"), true);
        }
        entry.insert(QLatin1String("name"), fields.name);
        if (!fields.service.isEmpty()) {
            entry.insert(QLatin1String("service"), fields.service);
        }
        if (fields.receiveKnown) {
            entry.insert(QLatin1String("receiveKind"), fields.receiveKind);
            entry.insert(QLatin1String("receiveValue"), fields.receiveValue);
            entry.insert(QLatin1String("receiveKnown"), true);
        }
        if (fields.receiveSlot != 0) {
            entry.insert(QLatin1String("receiveSlot"), fields.receiveSlot);
        }
        row_carrier_to_json(fields, entry);
        row_site_to_json(fields, entry);
        row_payload_to_json(fields, entry);
        entry.insert(QLatin1String("tg"), static_cast<qint64>(fields.tg));
        put_gi(entry, fields.gi);
        if (fields.src != 0U) {
            entry.insert(QLatin1String("src"), static_cast<qint64>(fields.src));
        }
        if (!fields.sourceName.isEmpty()) {
            entry.insert(QLatin1String("srcName"), fields.sourceName);
            entry.insert(QLatin1String("srcNameWhen"), fields.sourceNameWhen);
            entry.insert(QLatin1String("srcNameSeq"), static_cast<qint64>(fields.sourceNameSeq));
            entry.insert(QLatin1String("srcNameSlot"), fields.sourceNameSlot);
        }
        entry.insert(QLatin1String("session"), fields.session);
        row_heard_to_json(fields, entry);
        entry.insert(QLatin1String("pv"), kPartVersionSender);
        stored.append(entry);
    }
    obj.insert(QLatin1String("fp"), stored);
}

/** @brief One row as the rows store and the spill write it. */
QJsonObject
row_to_json(const CallHistoryModel::Row& row) {
    QJsonObject obj;
    obj.insert(QLatin1String("when"), row.when);
    obj.insert(QLatin1String("name"), row.name);
    obj.insert(QLatin1String("tg"), static_cast<qint64>(row.tg));
    put_gi(obj, row.gi);
    obj.insert(QLatin1String("src"), static_cast<qint64>(row.src));
    obj.insert(QLatin1String("srcName"), row.sourceName);
    obj.insert(QLatin1String("srcNameWhen"), row.sourceNameWhen);
    obj.insert(QLatin1String("srcNameSeq"), static_cast<qint64>(row.sourceNameSeq));
    obj.insert(QLatin1String("srcNameSlot"), row.sourceNameSlot);
    obj.insert(QLatin1String("enc"), row.enc);
    if (row.emergency) {
        obj.insert(QLatin1String("em"), true);
    }
    obj.insert(QLatin1String("durationSecs"), row.durationSecs);
    if (row.end > 0) {
        obj.insert(QLatin1String("end"), row.end);
    }
    obj.insert(QLatin1String("systemName"), row.systemName);
    obj.insert(QLatin1String("systemUid"), row.systemUid);
    obj.insert(QLatin1String("kind"), row.kind);
    if (!row.channel.isEmpty()) {
        obj.insert(QLatin1String("channel"), row.channel);
    }
    if (!row.channelName.isEmpty()) {
        obj.insert(QLatin1String("channelName"), row.channelName);
    }
    if (!row.service.isEmpty()) {
        obj.insert(QLatin1String("service"), row.service);
    }
    if (row.analog) {
        obj.insert(QLatin1String("analog"), true);
    }
    if (row.analog && dsd_analog_mode_known(row.analogMode) != 0) {
        obj.insert(QLatin1String("analogMode"), static_cast<int>(row.analogMode));
    }
    if (row.receiveKnown) {
        obj.insert(QLatin1String("receiveKind"), row.receiveKind);
        obj.insert(QLatin1String("receiveValue"), row.receiveValue);
        obj.insert(QLatin1String("receiveKnown"), true);
    }
    if (row.receiveSlot != 0) {
        obj.insert(QLatin1String("receiveSlot"), row.receiveSlot);
    }
    obj.insert(QLatin1String("slot"), row.slot);
    obj.insert(QLatin1String("seq"), static_cast<qint64>(row.seq));
    obj.insert(QLatin1String("session"), row.session);
    row_carrier_to_json(row, obj);
    row_site_to_json(row, obj);
    row_payload_to_json(row, obj);
    row_heard_to_json(row, obj);
    if (row.uid > 0) {
        obj.insert(QLatin1String("uid"), row.uid);
    }
    if (!row.fragments.isEmpty()) {
        obj.insert(QLatin1String("fr"), QJsonArray::fromStringList(row.fragments));
    }
    if (!row.fragmentVers.isEmpty()) {
        QJsonObject versions;
        for (auto it = row.fragmentVers.constBegin(); it != row.fragmentVers.constEnd(); ++it) {
            versions.insert(it.key(), static_cast<qint64>(it.value()));
        }
        obj.insert(QLatin1String("frv"), versions);
    }
    row_parts_to_json(row, obj);
    return obj;
}

} // namespace

void
CallHistoryModel::load() {
    QList<Row> rows;
    const QJsonArray array = json_store_load_array(QLatin1String(kStoreFileName));
    for (const auto& value : array) {
        if (!value.isObject()) {
            continue;
        }
        const Row row = row_from_json(value.toObject());
        // Rows carry their session with them, so the session numbering can never fall back
        // onto rows already in the log, even if the settings that hold it were lost.
        m_session = qMax(m_session, row.session);
        m_nextUid = qMax(m_nextUid, row.uid + 1);
        // Likewise the session ordinals (issue #800): never given again to a later run, even if the settings were lost.
        if (row.gpsOrd < std::numeric_limits<qint64>::max()) {
            m_sessionOrdinals.next = qMax(m_sessionOrdinals.next, row.gpsOrd + 1);
        }
        rows.append(row);
    }
    m_nextUid = qMax(m_nextUid, m_settings.value(QLatin1String(kNextUidKey)).toLongLong());
    beginResetModel();
    m_rows.clear();
    // Oldest first through the same merge the ingest path uses, so a log written
    // before fragment-coalescing existed collapses on its first load.
    for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
        // A row does not keep the ring it was read from, so its seed knows none (ring 0). The persisted
        // seen store below replaces it with the real entry; should the store have lost that, the next read
        // takes the seed to be the reading ring's (noteSeen()) and leaves the row in its session. That holds
        // for a row of one fragment only. A merged row keeps one fragment's slot and seq but the earliest
        // fragment's start, so its seed's key is no ring row's, and a merged call whose seen entries were
        // evicted or lost is logged again as new rows, one per fragment.
        // The seed's site text digest is the text the row shows, as a ring walk reads it: a relaunch re-reads nothing.
        // So are its GPS text, message and position digests (issue #800).
        m_seen.insert(keyFor(*it), SeenState{it->when,
                                             row_end_secs(*it),
                                             it->src,
                                             it->emergency,
                                             it->enc,
                                             it->sourceName,
                                             it->freqHz,
                                             it->codeKind,
                                             it->code,
                                             it->receiveKnown,
                                             it->receiveSlot,
                                             it->service,
                                             it->siteId.valid,
                                             it->callChannel,
                                             it->callChannelKind,
                                             call_history_site_text_digest(&it->siteId),
                                             true,
                                             row_text_digest(it->gpsText),
                                             row_text_digest(it->messageText),
                                             row_position_digest(it->pos),
                                             it->session,
                                             0U,
                                             QString()});
        if (tryMerge(*it, kMergeScanRows, false) < 0) {
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
        m_seen.insert(key, seenEntryFromJson(obj));
    }
    // Rows older builds wrote get their uid now; rows a load merge folded away gave theirs up.
    for (Row& row : m_rows) {
        if (row.uid <= 0) {
            row.uid = m_nextUid++;
        }
    }
    rebuildFragmentIndex();
    endResetModel();
    // What a background tap spilled and no acknowledged save has covered: rows a process killed in the background
    // never folded, or folded but did not get to the disk.
    (void)foldSpill();
    Q_EMIT countChanged();
}

CallHistoryModel::SeenState
CallHistoryModel::seenEntryFromJson(const QJsonObject& obj) {
    SeenState state;
    state.when = obj.value(QLatin1String("when")).toVariant().toLongLong();
    state.end = obj.value(QLatin1String("end")).toVariant().toLongLong();
    state.src = obj.value(QLatin1String("src")).toVariant().toULongLong();
    state.enc = obj.value(QLatin1String("enc")).toBool();
    state.emergency = obj.value(QLatin1String("em")).toBool();
    state.sourceName = obj.value(QLatin1String("srcName")).toString();
    // Absent when unknown, and in stores older builds wrote.
    state.freqHz = qMax<qint64>(json_i64_or(obj, "freqHz", 0), 0);
    const QPair<int, int> code = json_access_code(obj, "acKind", "ac");
    state.acKind = code.first;
    state.ac = code.second;
    state.receiveKnown = obj.value(QLatin1String("rxKnown")).toBool();
    state.receiveSlot = obj.value(QLatin1String("rxSlot")).toInt();
    state.service = obj.value(QLatin1String("svc")).toString();
    // Absent when nothing was read, and in stores older builds wrote; a value that does not fit reads as nothing.
    (void)json_field_fits(obj, "sm", &state.siteMask);
    // Absent in stores older builds wrote, which kept no digest: not known, so the first read takes one (SeenState).
    state.siteTextKnown = json_field_fits(obj, "stx", &state.siteText);
    if (!state.siteTextKnown) {
        state.siteText = 0U;
    }
    // Issue #800: absent when there is none, and in stores older builds wrote; a value that does not fit reads as none.
    const auto digest = [&obj](const char* key) {
        uint32_t value = 0U;
        return json_field_fits(obj, key, &value) ? value : 0U;
    };
    state.gpsText = digest("gpx");
    state.messageText = digest("msx");
    state.position = digest("psx");
    // Absent in stores older builds wrote: not known, so the first read takes one (SeenState::identity).
    state.identityKnown = json_field_fits(obj, "idx", &state.identity);
    if (!state.identityKnown) {
        state.identity = 0U;
    }
    if (!json_field_fits(obj, "cch", &state.callChannel) || !json_field_fits(obj, "cchKind", &state.callChannelKind)
        || !call_history_call_channel_known(state.callChannelKind, state.callChannel)) {
        state.callChannel = 0U;
        state.callChannelKind = DSD_CALL_CHANNEL_NONE;
    }
    state.session = obj.value(QLatin1String("session")).toVariant().toLongLong();
    // Hex text: a nonce can use all 64 bits, more than a JSON number holds exactly.
    bool ringOk = false;
    state.ring = obj.value(QLatin1String("ring")).toString().toULongLong(&ringOk, 16);
    if (!ringOk) {
        state.ring = 0U;
    }
    state.twin = obj.value(QLatin1String("twin")).toString();
    // Issue #903: absent in stores older builds wrote, and in an entry with no proof or a closed latch.
    const StoredHeard heard = heard_from_json(obj);
    state.heardVariant = heard.variant;
    state.heardId = heard.id;
    state.liveInput = heard.live;
    return state;
}

void
CallHistoryModel::markChanged() {
    m_generation++;
    scheduleSave();
}

void
CallHistoryModel::scheduleSave() {
    if (!m_saveTimer.isActive()) {
        m_saveTimer.start();
    }
}

#ifdef DSD_NEO_TEST_HOOKS
void
CallHistoryModel::setStoreBuildObserverForTest(void (*observer)(const QThread* thread)) {
    g_store_build_observer.store(observer);
}

void
CallHistoryModel::failNextSpillReadAfterLinesForTest(int lines) {
    g_spill_read_fail_after.store(lines);
}

void
CallHistoryModel::failSpillTruncatesForTest(bool fail) {
    CallHistorySpill::setTruncateFailsForTest(fail);
}

qint64
CallHistoryModel::takePartLookupStepsForTest() {
    return g_part_lookup_steps.exchange(0);
}
#endif

QJsonArray
CallHistoryModel::rowsToJson(const QList<Row>& rows) {
    note_store_build();
    QJsonArray array;
    for (const Row& row : rows) {
        array.append(row_to_json(row));
    }
    return array;
}

QJsonArray
CallHistoryModel::seenToJson(const QHash<QString, SeenState>& seen) {
    // Newest first and capped: the seen map only has to outlive what the ring can
    // still resurrect, not the whole persisted log. Newest in retention order, so the
    // running session's entries, which are all the ring can resurrect, are the ones kept.
    note_store_build();
    QList<QPair<QString, SeenState>> entries;
    entries.reserve(seen.size());
    for (auto it = seen.cbegin(); it != seen.cend(); ++it) {
        entries.append(qMakePair(it.key(), it.value()));
    }
    std::sort(entries.begin(), entries.end(),
              [](const QPair<QString, SeenState>& a, const QPair<QString, SeenState>& b) {
                  return retained_older(b.second.session, b.second.when, a.second.session, a.second.when);
              });
    QJsonArray seenArray;
    for (qsizetype i = 0; i < entries.size() && i < kMaxSeenEntries; i++) {
        QJsonObject obj = seenEntryToJson(entries.at(i).second);
        obj.insert(QLatin1String("key"), entries.at(i).first);
        seenArray.append(obj);
    }
    return seenArray;
}

QJsonObject
CallHistoryModel::seenEntryToJson(const SeenState& state) {
    QJsonObject obj;
    obj.insert(QLatin1String("when"), state.when);
    obj.insert(QLatin1String("end"), state.end);
    obj.insert(QLatin1String("src"), static_cast<qint64>(state.src));
    obj.insert(QLatin1String("srcName"), state.sourceName);
    obj.insert(QLatin1String("enc"), state.enc);
    if (state.emergency) {
        obj.insert(QLatin1String("em"), true);
    }
    if (call_history_freq_known(state.freqHz)) {
        obj.insert(QLatin1String("freqHz"), state.freqHz);
    }
    if (call_history_access_code_known(state.acKind)) {
        obj.insert(QLatin1String("acKind"), state.acKind);
        obj.insert(QLatin1String("ac"), state.ac);
    }
    if (state.receiveKnown) {
        obj.insert(QLatin1String("rxKnown"), true);
    }
    if (state.receiveSlot != 0) {
        obj.insert(QLatin1String("rxSlot"), state.receiveSlot);
    }
    if (!state.service.isEmpty()) {
        obj.insert(QLatin1String("svc"), state.service);
    }
    if (state.siteMask != 0U) {
        obj.insert(QLatin1String("sm"), static_cast<qint64>(state.siteMask));
    }
    // Written even when 0 (no text read): only its absence means an older build's entry.
    if (state.siteTextKnown) {
        obj.insert(QLatin1String("stx"), static_cast<qint64>(state.siteText));
    }
    if (state.callChannel != 0U || state.callChannelKind != DSD_CALL_CHANNEL_NONE) {
        obj.insert(QLatin1String("cch"), static_cast<qint64>(state.callChannel));
        obj.insert(QLatin1String("cchKind"), state.callChannelKind);
    }
    const auto digest = [&obj](const char* key, uint32_t value) {
        if (value != 0U) {
            obj.insert(QLatin1String(key), static_cast<qint64>(value));
        }
    };
    digest("gpx", state.gpsText);
    digest("msx", state.messageText);
    digest("psx", state.position);
    if (state.identityKnown) {
        obj.insert(QLatin1String("idx"), static_cast<qint64>(state.identity));
    }
    obj.insert(QLatin1String("session"), state.session);
    obj.insert(QLatin1String("ring"), QString::number(state.ring, 16));
    if (!state.twin.isEmpty()) {
        obj.insert(QLatin1String("twin"), state.twin);
    }
    heard_to_json(StoredHeard{state.heardVariant, state.heardId, state.liveInput}, obj);
    return obj;
}

bool
CallHistoryModel::saveSync() {
    const bool rowsCommitted = json_store_save_array(QLatin1String(kStoreFileName), rowsToJson(m_rows));
    const bool seenCommitted = json_store_save_array(QLatin1String(kSeenStoreFileName), seenToJson(m_seen));
    if (rowsCommitted && seenCommitted) {
        acknowledgeSave(m_generation);
        return true;
    }
    return false;
}

void
CallHistoryModel::startAsyncSave() {
    if (m_savesInFlight > 0) {
        // The worker is mid-write with an older frame; remember that the stores
        // moved again rather than race a second writer onto the same files.
        m_saveDirty = true;
        return;
    }
    m_saveDirty = false;
    queueSave();
}

void
CallHistoryModel::queueSave() {
    m_savesInFlight++;
    m_settings.setValue(QLatin1String(kNextUidKey), m_nextUid);
    // Snapshots the worker owns outright. Detached here, on the GUI thread, so the GUI thread's later
    // edits of m_rows and m_seen and the worker's reads never touch one container block: an implicitly
    // shared copy would leave the GUI thread deciding whether to detach from a relaxed reference count
    // the worker drops. That holds for the containers nested in a row too: each row's fragment list is
    // detached as well, since a merge appends to the GUI's copy (unionFragments()). Copying the containers
    // costs a fraction of a millisecond at a full log; building their JSON costs tens. The strings stay
    // shared, and nothing edits a string in place.
    QList<Row> rows = m_rows;
    rows.detach();
    for (Row& row : rows) {
        row.fragments.detach();
    }
    QHash<QString, SeenState> seen = m_seen;
    seen.detach();
    const quint64 generation = m_generation;
    m_savePool.start([this, rows = std::move(rows), seen = std::move(seen), generation]() {
        qint64 rowBytes = 0;
        qint64 seenBytes = 0;
        SaveResult result;
        result.generation = generation;
        // Each store reports whether it reached the disk: a write or commit can fail (a full disk), and the
        // log stays owed until both did (B6).
        result.rowsCommitted = json_store_save_array(QLatin1String(kStoreFileName), rowsToJson(rows), &rowBytes);
        result.seenCommitted = json_store_save_array(QLatin1String(kSeenStoreFileName), seenToJson(seen), &seenBytes);
        result.bytes = rowBytes + seenBytes;
        // Back to the GUI thread; the destructor's waitForDone() keeps `this`
        // alive for the worker's whole run.
        QMetaObject::invokeMethod(this, [this, result]() { onSaveFinished(result); }, Qt::QueuedConnection);
    });
}

void
CallHistoryModel::saveNow() {
    // Owed is what the destructor flushes too. Otherwise the files, or the save in
    // flight, already hold the stores, and a rewrite would only cost flash.
    if (!m_saveTimer.isActive() && !m_saveDirty) {
        return;
    }
    m_saveTimer.stop();
    m_saveDirty = false;
    // Snapshot now and queue behind any save in flight. The pool's one thread runs them in
    // order, so the writes never overlap, and this one starts as the last finishes, with no
    // GUI event loop in between: Qt pauses it on Android while the app is in the background.
    queueSave();
}

void
CallHistoryModel::onSaveFinished(const SaveResult& result) {
    m_savesInFlight--;
    // A debounced save already waiting restarts with it, so the delay counts from this write.
    m_saveTimer.setInterval(save_delay_ms(result.bytes));
    if (result.rowsCommitted && result.seenCommitted) {
        m_saveFailures = 0;
        acknowledgeSave(result.generation);
        setSaveFailing(false);
    } else {
        // Not on the disk, or only half of it: the log is still owed, and is retried on a growing delay even if
        // nothing changes again. The rows store and the seen store are rewritten together.
        m_saveFailures++;
        m_saveDirty = true;
        m_saveTimer.setInterval(qMax(m_saveTimer.interval(), save_retry_delay_ms(m_saveFailures)));
        if (!m_saveFailing) {
            qWarning("call history: saving the log failed (rows %s, seen %s); retrying",
                     result.rowsCommitted ? "saved" : "not saved", result.seenCommitted ? "saved" : "not saved");
        }
        setSaveFailing(true);
    }
    // What moved during the queued saves is owed once the last of them is done, unless a later save in
    // the queue already wrote it.
    if (m_savesInFlight == 0 && m_saveDirty) {
        m_saveDirty = false;
        if (m_ackedGeneration != m_generation) {
            scheduleSave();
        }
    }
}

void
CallHistoryModel::acknowledgeSave(quint64 generation) {
    m_ackedGeneration = qMax(m_ackedGeneration, generation);
    // The spill lines the last fold read are on the disk in the stores now, so they can go. A tap that has
    // appended since keeps the file whole (truncateIfSize()); the next fold reads it all again, and a line
    // folded twice changes nothing.
    if (m_spillFoldedBytes >= 0 && m_ackedGeneration >= m_spillFoldGeneration) {
        (void)m_spill->truncateIfSize(m_spillFoldedBytes);
        m_spillFoldedBytes = -1;
    }
}

void
CallHistoryModel::setSaveFailing(bool failing) {
    if (failing == m_saveFailing) {
        return;
    }
    m_saveFailing = failing;
    Q_EMIT saveFailingChanged();
}

void
CallHistoryModel::setHistoryTapSource(CallHistoryTapSource source) {
    if (m_tap) {
        (void)m_tap->stop();
        m_tap.reset();
    }
    m_handoffOwed = false;
    if (source != nullptr) {
        m_tap = std::make_unique<CallHistoryTap>(source, m_spill.get());
    }
}

void
CallHistoryModel::onApplicationStateChanged(Qt::ApplicationState state) {
    m_appAway = state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden;
    if (m_appAway) {
        saveNow();
        startBackgroundCapture();
        return;
    }
    // Back on screen, or at least running: the next ingest takes over from the tap.
    if (m_tap && m_tap->running()) {
        m_handoffOwed = true;
    }
}

void
CallHistoryModel::startBackgroundCapture() {
    m_handoffOwed = false;
    // Held for a start (holdCaptureForStart()): the session it begins, or the release, starts the tap.
    if (!m_tap || m_tap->running() || m_captureHeld) {
        return;
    }
    CallHistoryTap::Context context;
    // The tap's own copy of the seen map, detached here as the save worker's is (queueSave()).
    context.seen = m_seen;
    context.seen.detach();
    context.session = m_session;
    context.sessionLabel = m_sessionLabel;
    context.sessionUid = m_sessionUid;
    context.clear = m_clear;
    // Issue #903: the tap's own copy of the owners, detached as the seen map is; it binds the rings it reads first.
    context.heardOwners = m_heardOwners;
    context.heardOwners.sessions.detach();
    context.heardOwners.rings.detach();
    // A block of ordinals for the rings the tap meets first, set aside and saved before it runs: a process killed while
    // it is away leaves rows whose ordinals no later run's can come before, however the spill is read back.
    context.ordinals = m_sessionOrdinals;
    context.ordinals.limit = m_sessionOrdinals.next <= std::numeric_limits<qint64>::max() - kTapOrdinalBlock
                                 ? m_sessionOrdinals.next + kTapOrdinalBlock
                                 : std::numeric_limits<qint64>::max();
    m_sessionOrdinals.next = context.ordinals.limit;
    saveSessionOrdinals();
    m_tapClearRev = m_clearRev;
    if (!m_tap->start(std::move(context))) {
        qWarning("call history: the background capture could not start");
    }
}

void
CallHistoryModel::takeTapReadPosition(bool read, const qulonglong pushSeq[2], quint64 ring) {
    if (!read) {
        return;
    }
    // The tap read the ring after this model last did (Qt was paused), unless the loop kept running: on the
    // same ring the later position of the two, else the tap's.
    if (!m_ringRead || ring != m_ringPushSeqRing) {
        m_ringPushSeq[0] = pushSeq[0];
        m_ringPushSeq[1] = pushSeq[1];
        m_ringPushSeqRing = ring;
        m_ringRead = true;
    } else {
        m_ringPushSeq[0] = qMax(m_ringPushSeq[0], pushSeq[0]);
        m_ringPushSeq[1] = qMax(m_ringPushSeq[1], pushSeq[1]);
    }
}

void
CallHistoryModel::handOffBackgroundCapture() {
    m_handoffOwed = false;
    if (!m_tap || !m_tap->running()) {
        return;
    }
    // What the tap's last append could not write (the spill still unwritable) is folded with the spill: only the model
    // has it now.
    if (foldSpill(stopBackgroundCapture())) {
        Q_EMIT countChanged();
    }
}

bool
CallHistoryModel::handOffBackgroundCaptureForSession() {
    // A start that completes while the app is away (a deferred restart, a USB start's continuation) changes the
    // session, label or system the tap attributes its rows to; it holds a copy of the old ones. What it found so far
    // folds in under them now, and a tap the app's return already owes a handoff stays stopped, as in clearAll().
    if (!m_tap || !m_tap->running()) {
        return false;
    }
    const bool resume = !m_handoffOwed;
    handOffBackgroundCapture();
    return resume;
}

void
CallHistoryModel::resumeBackgroundCapture(bool resume) {
    if (resume) {
        startBackgroundCapture();
    }
}

QByteArray
CallHistoryModel::stopBackgroundCapture() {
    const CallHistoryTap::Result tapped = m_tap->stop();
    // A clear before the next ingest covers what the tap read.
    takeTapReadPosition(tapped.read, tapped.pushSeq, tapped.ring);
    // Before any spill line folds: the ordinals the tap gave are the rings' for good, and so are the sessions it bound
    // them to (issue #903).
    adoptSessionOrdinals(tapped.ordinals);
    if (heard_owners_adopt_rings(&m_heardOwners, tapped.heardOwners)) {
        saveHeardOwners();
    }
    // A clear this model made before it had read any ring binds to what the first read finds (settleClear()). The
    // tap made that first read, so its binding is the one: binding later, here, would hide what the ring took in
    // between. A clear made after the tap started supersedes whatever the tap settled.
    if (m_clearRev == m_tapClearRev && m_clear.active && m_clear.pending && tapped.clear.active
        && !tapped.clear.pending) {
        m_clear = tapped.clear;
        saveClear();
    }
    return tapped.unwritten;
}

namespace {

/**
 * @brief Give a spilled reading @p row the GPS provenance the walk gave it (issue #800, 2.7), which a row stores only
 * with GPS text or a position (row_payload_to_json()): its run's session ordinal (@p ordinal, the line's `ord`) and the
 * ring's commit_rev when it was read (its fragment's version). So a reading whose GPS the core cleared while the app
 * was away is still its fragment's (call_history_same_fragment()), and withdraws that fragment's fix when folded, as
 * the same reading read live does.
 */
void
spilled_reading_provenance(CallHistoryModel::Row& row, qint64 ordinal) {
    if (row.fragments.size() != 1) {
        return;
    }
    if (row.gpsOrd <= 0 && ordinal > 0) {
        row.gpsOrd = ordinal;
    }
    if (row.gpsVer == 0U) {
        row.gpsVer = row.fragmentVers.value(row.fragments.constFirst(), 0U);
    }
}

} // namespace

bool
CallHistoryModel::foldSpill(const QByteArray& unwritten) {
    qint64 size = 0;
    QFile file;
    (void)m_spill->open(&file, &size);
    if (size <= 0 && unwritten.isEmpty()) {
        return false;
    }
    bool folded = false;
    bool rowsChanged = false;
    bool readWhole = false;
    // The spill's lines first: the tap wrote them before the ones it could not. Read a line at a time: a long
    // background stretch spills far more than the log keeps, and the fold holds no more of it than one line.
    if (size > 0) {
        CallHistorySpillReader reader(&file, size);
#ifdef DSD_NEO_TEST_HOOKS
        const int failAfter = g_spill_read_fail_after.exchange(-1);
        if (failAfter >= 0) {
            reader.failAfterLinesForTest(failAfter);
        }
#endif
        rowsChanged = foldSpillLines(reader, &folded);
        readWhole = !reader.failed();
        if (!readWhole) {
            qWarning("call history: reading the background spill failed; it is kept, and read again at the next fold");
        }
    }
    file.close(); // before a save's acknowledgement below truncates it
    rowsChanged = foldUnwritten(unwritten, &folded) || rowsChanged;
    rowsChanged = trimToCapacity() || rowsChanged;
    pruneSeen();
    if (folded) {
        markChanged();
    }
    // The spill's lines go once a save of this generation is acknowledged; at once when they added nothing the disk
    // lacks. The unwritten ones were never in it: the log's own saves keep them. A read that failed takes none of the
    // spill as read: the lines after the failure are in no store, and the fold that next reads it whole adds them, the
    // lines read before it again adding nothing.
    if (readWhole) {
        m_spillFoldedBytes = size;
        m_spillFoldGeneration = m_generation;
        if (m_ackedGeneration >= m_spillFoldGeneration) {
            acknowledgeSave(m_ackedGeneration);
        }
    }
    return rowsChanged;
}

bool
CallHistoryModel::foldUnwritten(const QByteArray& unwritten, bool* folded) {
    if (unwritten.isEmpty()) {
        return false;
    }
    QBuffer buffer;
    buffer.setData(unwritten);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return false;
    }
    CallHistorySpillReader reader(&buffer, unwritten.size());
    return foldSpillLines(reader, folded);
}

bool
CallHistoryModel::foldSpillLines(CallHistorySpillReader& reader, bool* folded) {
    bool rowsChanged = false;
    const bool cleared = clearApplies();
    // The twins taken are the ring's that took them (noteCommitRevs()). A line another ring read, a replay the tap
    // found begun in the background, folds against that ring's, which the walk after the fold would restore too late:
    // the fold has marked the line's notice seen by then.
    quint64 twinsRing = m_ringInstance[0];
    while (reader.next()) {
        rowsChanged = foldSpillLine(reader.line(), cleared, &twinsRing, folded) || rowsChanged;
    }
#ifdef DSD_NEO_TEST_HOOKS
    m_peakSpillReadBytesForTest = qMax(m_peakSpillReadBytesForTest, reader.peakBytes());
#endif
    if (twinsRing != m_ringInstance[0]) {
        restoreNoticeTwinsTaken(m_ringInstance[0], m_ringInstance[1]); // the rings read, as before the fold
    }
    return rowsChanged;
}

bool
CallHistoryModel::spilledRowCleared(bool cleared, const Row& row, const SeenState& seen) const {
    return cleared && seen.ring == m_clear.ring && row.slot >= 0 && row.slot < 2
           && row.seq <= m_clear.pushSeq[row.slot];
}

bool
CallHistoryModel::foldSpillLine(const QByteArray& line, bool cleared, quint64* twinsRing, bool* folded) {
    // A line a kill tore, or one a newer build wrote, is skipped; the rest still fold.
    const QJsonObject obj = QJsonDocument::fromJson(line).object();
    if (obj.value(QLatin1String("v")).toInt() != 1) {
        return false;
    }
    const QString key = obj.value(QLatin1String("key")).toString();
    const QJsonObject rowObj = obj.value(QLatin1String("row")).toObject();
    if (key.isEmpty() || rowObj.isEmpty()) {
        // No row: at most a seen entry the tap settled (settledSeenLine()).
        foldSettledSeen(key, obj, folded);
        return false;
    }
    Row row = row_from_json(rowObj);
    const SeenState seen = seenEntryFromJson(obj.value(QLatin1String("seen")).toObject());
    // The ordinal the tap gave the row's ring is that ring's (issue #800): kept as it was, never given again.
    const qint64 ordinal = json_i64_or(obj, "ord", 0);
    adoptSessionOrdinal(seen.ring, ordinal);
    spilled_reading_provenance(row, ordinal);
    if (spilledRowCleared(cleared, row, seen)) {
        // Cleared after the tap found it.
        return false;
    }
    if (seen.ring != 0U && seen.ring != *twinsRing) {
        restoreNoticeTwinsTaken(seen.ring, seen.ring);
        *twinsRing = seen.ring;
    }
    // The seen entry as the tap left it, folded in as a fresh read would be, so the next ring walk finds the
    // row seen and still notices what the core adds to it later.
    (void)noteSeen(m_seen, seen.session, key, seen.ring, seen, row.kind == KindVoice);
    if (!seen.twin.isEmpty()) {
        m_seen[key].twin = seen.twin;
    }
#ifdef DSD_NEO_TEST_HOOKS
    m_peakFoldSeenForTest = qMax(m_peakFoldSeenForTest, m_seen.size());
#endif
    // Idempotent by fragment: a row whose fragment the log already holds was folded before (a fold the
    // last save has not acknowledged, read again at load) or ingested by a GUI that kept running, and can
    // only refine that row. One it does not hold is a new row, whatever the tap judged it: an update whose
    // first sighting a bounded backlog dropped (the spill unwritable) carries the whole row, and the ring
    // walk after it finds the call seen.
    bool known = false;
    for (const QString& fragment : row.fragments) {
        known = known || m_fragmentIndex.contains(fragment);
    }
    const bool isUpdate = known;
    const bool again = obj.value(QLatin1String("again")).toBool() && !known;
    // The owner the tap resolved where it read the ring (issue #903): the line carries it.
    bool rowsChanged = ingestReading(row, isUpdate, again, obj.value(QLatin1String("ho")).toString());
    *folded = true;
    if (m_rows.size() > kMaxRows + kSpillFoldTrimSlack) {
        rowsChanged = trimToCapacity() || rowsChanged;
    }
#ifdef DSD_NEO_TEST_HOOKS
    m_peakFoldTwinsForTest = qMax(m_peakFoldTwinsForTest, m_noticeTwinsTaken.size());
#endif
    // Each line can add a seen entry, and a notice heard again a twin taken: pruned as the rows are trimmed,
    // as they go, so neither holds the whole backlog either.
    pruneSeenWhileFolding();
    return rowsChanged;
}

void
CallHistoryModel::pruneSeenWhileFolding() {
    const qsizetype before = m_seen.size();
    pruneSeen();
    if (m_seen.size() == before) {
        return;
    }
    // The twins taken that no entry left records go with the entries: the set stays bounded by the map, and a
    // relaunch rebuilding it from the seen store would not know them either (restoreNoticeTwinsTaken()).
    QSet<QString> recorded;
    for (const SeenState& state : m_seen) {
        if (!state.twin.isEmpty()) {
            recorded.insert(state.twin);
        }
    }
    m_noticeTwinsTaken.intersect(recorded);
}

void
CallHistoryModel::foldSettledSeen(const QString& key, const QJsonObject& line, bool* folded) {
    if (!key.isEmpty() && line.value(QLatin1String("cleared")).toBool()) {
        foldClearedProof(key, line, folded);
        return;
    }
    if (key.isEmpty() || !line.value(QLatin1String("settled")).toBool()) {
        return;
    }
    // Only an entry still without a digest takes it: one this model read since knows the text as it is now, and an
    // entry it no longer has holds nothing to settle. No row is read again.
    const SeenState seen = seenEntryFromJson(line.value(QLatin1String("seen")).toObject());
    const auto entry = m_seen.find(key);
    if (!seen.siteTextKnown || entry == m_seen.end() || entry->siteTextKnown) {
        return;
    }
    entry->siteText = seen.siteText;
    entry->siteTextKnown = true;
    *folded = true; // the seen store is owed a save
}

void
CallHistoryModel::foldClearedProof(const QString& key, const QJsonObject& line, bool* folded) {
    const SeenState seen = seenEntryFromJson(line.value(QLatin1String("seen")).toObject());
    const QString owner = line.value(QLatin1String("ho")).toString();
    const auto entry = m_seen.find(key);
    if (seen.heardVariant == 0U || owner.isEmpty() || entry == m_seen.end() || entry->heardVariant != 0U) {
        return;
    }
    entry->heardVariant = seen.heardVariant;
    entry->heardId = seen.heardId;
    entry->liveInput = seen.liveInput;
    noteHeard(owner, seen.heardVariant, seen.heardId, json_i64_or(line, "when", 0));
    *folded = true; // the seen store is owed a save
}

QByteArray
CallHistoryModel::clearedProofLine(const FreshRow& fresh) {
    QJsonObject obj;
    obj.insert(QLatin1String("v"), 1);
    obj.insert(QLatin1String("key"), fresh.key);
    obj.insert(QLatin1String("seen"), seenEntryToJson(fresh.seen));
    obj.insert(QLatin1String("cleared"), true);
    obj.insert(QLatin1String("ho"), fresh.heardOwner);
    obj.insert(QLatin1String("when"), fresh.row.end > 0 ? fresh.row.end : fresh.row.when);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray
CallHistoryModel::settledSeenLine(const QString& key, const SeenState& seen) {
    QJsonObject obj;
    obj.insert(QLatin1String("v"), 1);
    obj.insert(QLatin1String("key"), key);
    obj.insert(QLatin1String("seen"), seenEntryToJson(seen));
    obj.insert(QLatin1String("settled"), true);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray
CallHistoryModel::spillLine(const FreshRow& fresh) {
    QJsonObject obj;
    obj.insert(QLatin1String("v"), 1);
    obj.insert(QLatin1String("key"), fresh.key);
    obj.insert(QLatin1String("seen"), seenEntryToJson(fresh.seen));
    obj.insert(QLatin1String("row"), row_to_json(fresh.row));
    // The session ordinal of the ring the tap read the row from (issue #800), whether or not the row has a position.
    if (fresh.row.gpsOrd > 0) {
        obj.insert(QLatin1String("ord"), fresh.row.gpsOrd);
    }
    if (fresh.isUpdate) {
        obj.insert(QLatin1String("upd"), true);
    }
    if (fresh.again) {
        obj.insert(QLatin1String("again"), true);
    }
    if (!fresh.heardOwner.isEmpty()) {
        obj.insert(QLatin1String("ho"), fresh.heardOwner);
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

#ifdef DSD_NEO_TEST_HOOKS
void
CallHistoryModel::setBackgroundCaptureTimingForTest(int ms, int batchPolls) {
    if (m_tap) {
        m_tap->setTimingForTest(ms, batchPolls);
    }
}

bool
CallHistoryModel::backgroundCaptureRunningForTest() const {
    return m_tap && m_tap->running();
}

void
CallHistoryModel::waitForBackgroundPollForTest() {
    if (m_tap) {
        m_tap->waitForPollForTest();
    }
}

int
CallHistoryModel::spillAppendsForTest() const {
    return m_spill->appendsForTest();
}

int
CallHistoryModel::spillAppendAttemptsForTest() const {
    return m_spill->appendAttemptsForTest();
}
#endif

} // namespace dsd_qt
