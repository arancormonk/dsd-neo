// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QChar>
#include <QDebug>
#include <dsd-neo/core/receive_types.h>
#include "call_history_model.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QLocale>
#include <QPair>
#include <QStringView>
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
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/received_identity_site.h>
#include <dsd-neo/core/state.h>
#include <iterator>
#include <limits>
#include <stdint.h>
#include <utility>

#ifdef DSD_NEO_TEST_HOOKS
#include <QThread>
#include <atomic>
#endif

#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/analog_tones.h>
#include "call_history_merge.h"
#include "call_history_tap.h"
#include "call_recording_provider.h"
#include "dsd-neo/core/state_fwd.h"
#include "json_store.h"
#include "realtime_clock.h"
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
/* How far down the log a live fragment looks for the call it continues: fragments arrive while
 * their call is among the newest rows. */
constexpr int kMergeScanRows = 32;

#ifdef DSD_NEO_TEST_HOOKS
std::atomic<void (*)(const QThread*)> g_store_build_observer{nullptr};
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

/** @brief End of a logged row on the shared timeline; unknown durations count as 0. */
qint64
row_end_secs(const CallHistoryModel::Row& row) {
    return row.when + qMax(row.durationSecs, 0);
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
 * @brief "TODAY" / "YESTERDAY" / "MON 3 AUG" for the list's day sections.
 *
 * A deliberate real-time exception to the rule that decoded stamps are compared on the decode
 * clock. The log spans sessions, and its sections name the viewer's calendar day, retired by the
 * real midnight timer (scheduleDayRollover()). On the decode clock a week-old replay's calls would
 * read "TODAY" and the labels would never roll over on time.
 */
QString
day_label(qint64 when) {
    const QDate day = QDateTime::fromSecsSinceEpoch(when).date();
    const QDate today = realtimeCurrentDate();
    if (day == today) {
        return QStringLiteral("TODAY");
    }
    if (day == today.addDays(-1)) {
        return QStringLiteral("YESTERDAY");
    }
    return QLocale().toString(day, QStringLiteral("ddd d MMM")).toUpper();
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

void
CallHistoryModel::scheduleDayRollover() {
    const QDateTime now = realtimeCurrentDateTime();
    const QDateTime nextMidnight = realtimeCurrentDate().addDays(1).startOfDay();
    /* A second past the boundary, so a coarse timer that fires marginally early
     * cannot re-derive the very labels it was meant to retire. */
    m_dayTimer.start(static_cast<int>(qMin<qint64>(now.msecsTo(nextMidnight) + 1000, 86400000)));
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

/** @brief The frequency, access-code and site roles. Their texts are app-control's, spelled at read time. */
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
        case CallHistoryModel::DayLabelRole: return day_label(row.when);
        case CallHistoryModel::TimeTextRole:
            return QDateTime::fromSecsSinceEpoch(row.when).toString(QStringLiteral("HH:mm"));
        case CallHistoryModel::KindRole: return row.kind;
        case CallHistoryModel::DetailRole: return row.detail;
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
        case RecordingsRole:
            return m_recordings != nullptr && !row.fragments.isEmpty()
                       ? m_recordings->recordingsForFragments(row.fragments)
                       : QVariantList();
        case HasRecordingRole:
            return m_recordings != nullptr && !row.fragments.isEmpty()
                   && m_recordings->hasRecordingForFragments(row.fragments);
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

QList<uint32_t>
CallHistoryModel::heardTalkgroups(qint64 forSession) const {
    QList<uint32_t> heard;
    QSet<uint32_t> seen;
    for (const Row& row : m_rows) {
        if (row.kind != KindVoice || (forSession > 0 && row.session != forSession) || row.tg == 0U
            || row.tg > UINT32_MAX) {
            continue;
        }
        const auto tg = static_cast<uint32_t>(row.tg);
        if (!seen.contains(tg)) {
            seen.insert(tg);
            heard.append(tg);
        }
    }
    return heard;
}

QHash<int, QByteArray>
CallHistoryModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles.insert(NameRole, QByteArrayLiteral("name"));
    roles.insert(TgRole, QByteArrayLiteral("tg"));
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
CallHistoryModel::setSessionSystem(const QString& label, const QString& uid) {
    changeSession(label, uid, false);
}

void
CallHistoryModel::changeSession(const QString& label, const QString& uid, bool begin) {
    const bool labelMoved = label != m_sessionLabel;
    const bool uidMoved = uid != m_sessionUid;
    if (!labelMoved && !uidMoved && !begin) {
        return;
    }
    // One handoff for every field: a tap started again between them would attribute what it finds to half the change.
    const bool resumeTap = handOffBackgroundCaptureForSession();
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
     * state its last run used (the Android service reuses it when a start races the previous run's
     * stopSelfLatest()), and that ring keeps its identity, its rows and its push count. So a clear
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

/** @brief One committed ring item as a display row. */
void
row_take_name(CallHistoryModel::Row& row, const Event_History* item) {
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
    } else if (item->service_label[0] != '\0') {
        row.name = QString::fromUtf8(item->service_label);
    } else if (item->t_name[0] != '\0') {
        row.name = QString::fromUtf8(item->t_name);
    } else if (item->tgt_str[0] != '\0') {
        row.name = QString::fromUtf8(item->tgt_str);
    } else if (item->channel_label[0] != '\0') {
        /* No identity of its own: the channel it was heard on is what the
         * operator recognises it by, and "Talkgroup 0" is not. */
        row.name = QString::fromUtf8(item->channel_label);
    } else {
        row.name = item->systype == DSD_SYNC_ANALOG ? analog_fallback_name(item->analog_mode)
                                                    : QStringLiteral("Talkgroup %1").arg(row.tg);
    }
}

CallHistoryModel::Row
row_from_item(const Event_History* item, const QString& sessionLabel, const QString& systemUid, int slot,
              qulonglong seq) {
    CallHistoryModel::Row row;
    row.slot = slot;
    row.seq = seq;
    row.tg = static_cast<qulonglong>(item->target_id);
    row.src = static_cast<qulonglong>(item->source_id);
    row.sourceName = QString::fromUtf8(item->s_name[0] ? item->s_name : item->src_str);
    row.enc = item->enc != 0U;
    row.emergency = item->emergency != 0U;
    row.kind = item->category == DSD_EVENT_CATEGORY_VOICE ? CallHistoryModel::KindVoice : CallHistoryModel::KindNotice;
    row_take_name(row, item);
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
    row_take_carrier(row, item);
    row_take_site(row, item);
    if (row.kind == CallHistoryModel::KindVoice) {
        row.durationSecs = call_history_duration_secs(start, end);
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

// Record the site parts and call channel @p read gained in @p seen; returns whether it gained any (issue #799).
bool
CallHistoryModel::absorbSeenSite(SeenState& seen, const SeenState& read) {
    return call_history_seen_absorb_site(&seen.siteMask, &seen.callChannel, &seen.callChannelKind, read.siteMask,
                                         read.callChannel, read.callChannelKind);
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
    int64_t storedEnd = seen->end;
    uint64_t storedSrc = seen->src;
    bool storedEnc = seen->enc;
    const bool advanced = call_history_seen_absorb(&storedEnd, &storedSrc, &storedEnc, read.end, read.src, read.enc,
                                                   &seen->emergency, read.emergency);
    const bool labelAdvanced =
        !read.sourceName.isEmpty() && read.sourceName != seen->sourceName && read.src == storedSrc;
    int64_t storedFreqHz = seen->freqHz;
    int storedAcKind = seen->acKind;
    int storedAc = seen->ac;
    const bool filled =
        call_history_seen_absorb_fill(&storedFreqHz, &storedAcKind, &storedAc, read.freqHz, read.acKind, read.ac);
    const bool received = absorbSeenReceive(*seen, read);
    const bool sited = absorbSeenSite(*seen, read);
    if (!advanced && !labelAdvanced && !filled && !received && !sited) {
        return SeenUnchanged;
    }
    if (labelAdvanced) {
        seen->sourceName = read.sourceName;
    }
    seen->end = storedEnd;
    seen->src = storedSrc;
    seen->enc = storedEnc;
    seen->freqHz = storedFreqHz;
    seen->acKind = storedAcKind;
    seen->ac = storedAc;
    return SeenAdvanced;
}

void
CallHistoryModel::walkRing(const Event_History_I& ring, int slot, const RingWalk& walk, QList<FreshRow>& fresh) {
    const qulonglong pushSeq = static_cast<qulonglong>(ring.push_seq);
    const quint64 instance = static_cast<quint64>(ring.instance);
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
            continue;
        }
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
        // A mask read only: the walk reads every committed row, and nothing is formatted here.
        read.siteMask = item->site_id.valid;
        const ItemChannel channel = item_call_channel(item);
        read.callChannel = channel.channel;
        read.callChannelKind = channel.kind;
        // Must match keyFor() on the equivalent Row, or a relaunched UI would
        // re-ingest every row its predecessor already logged.
        const QString key = seen_key(slot, seq, read.when, item->target_id, kind);
        const int verdict = noteSeen(*walk.seen, walk.session, key, instance, read, kind == KindVoice);
        if (verdict == SeenUnchanged) {
            continue;
        }
        FreshRow row;
        row.row = row_from_item(item, walk.sessionLabel, walk.systemUid, slot, seq);
        row.row.session = walk.session;
        row.row.fragments.append(call_history_fragment_key(instance, slot, seq));
        row.isUpdate = verdict == SeenAdvanced;
        row.again = verdict == SeenAgain;
        row.key = key;
        row.seen = walk.seen->value(key);
        fresh.append(row);
    }
}

QList<CallHistoryModel::FreshRow>
CallHistoryModel::collectFresh(const dsd_state* snapshot, const bool scan[2], const QString& systemUid) {
    QList<FreshRow> fresh;
    RingWalk walk;
    walk.seen = &m_seen;
    walk.session = m_session;
    walk.sessionLabel = m_sessionLabel;
    walk.systemUid = systemUid;
    walk.cleared = clearApplies();
    for (int slot = 0; slot < 2; slot++) {
        if (!scan[slot]) {
            continue;
        }
        walk.clearedThrough = m_clear.pushSeq[slot];
        walkRing(snapshot->event_history_s[slot], slot, walk, fresh);
    }
    return fresh;
}

namespace {

/* Two fragments are one service when their confirmed identities and labels agree. A fragment committed before its
   access code or tone was confirmed carries no identity yet, and its label is at most a weaker (slot) name: it joins
   the confirmed fragment, which tryMerge() lets name the row. Two unconfirmed fragments must agree on the label. The
   over-the-air slot is proven on its own (receiveSlot, 0 while pending on the single-slot DMR decoder): two proven
   slots that differ are two services whatever the codes, and a pending slot's fragment may carry no slot name. */
bool
receive_services_compatible(const CallHistoryModel::Row& a, const CallHistoryModel::Row& b) {
    if (a.receiveSlot != 0 && b.receiveSlot != 0 && a.receiveSlot != b.receiveSlot) {
        return false;
    }
    /* One fragment proved its slot and the other had it pending (FDMA rows carry no slot at all, on either side). */
    const bool slotJustProven = (a.receiveSlot != 0) != (b.receiveSlot != 0);
    if (a.receiveKnown && b.receiveKnown) {
        /* The same confirmed service may gain the name it lacked (a name added, then the call reacquired). */
        return a.receiveKind == b.receiveKind && a.receiveValue == b.receiveValue
               && (a.service == b.service || slotJustProven || a.service.isEmpty() || b.service.isEmpty());
    }
    return a.receiveKnown || b.receiveKnown || a.service == b.service || slotJustProven;
}

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

/** @brief Whether two rows with no talkgroup (tg == 0) name the same destination; any rows with one do. */
bool
target_less_rows_same_destination(const CallHistoryModel::Row& existing, const CallHistoryModel::Row& row) {
    if (existing.tg != 0) {
        return true;
    }
    // Textual targets (M17/D-STAR/YSF callsigns, dPMR dial strings) all share
    // tg == 0, so the numeric check cannot tell two destinations apart;
    // the target text is their identity. A name that is only a label (a service
    // or channel name) is no destination of its own. Analog rows never get here
    // (rows_mergeable()).
    const QString existingTarget = row_textual_destination(existing);
    const QString rowTarget = row_textual_destination(row);
    if (!existingTarget.isEmpty() && !rowTarget.isEmpty() && existingTarget != rowTarget) {
        return false;
    }
    // A digital row with no IDs at all (issue #637) is named only by where it was heard:
    // two such transmissions on different frequencies are two calls, whatever the gap between them. Trunked fragments
    // keep a talkgroup, so their retunes still fold.
    return existing.src != 0 || row.src != 0 || existing.freqHz == 0 || row.freqHz == 0
           || existing.freqHz == row.freqHz;
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
    if (dsd_call_site_compatible(&existing.siteId, existing.callChannelKind, &row.siteId, row.callChannelKind) == 0) {
        return false;
    }
    if (!target_less_rows_same_destination(existing, row)) {
        return false;
    }
    // Matched sources get the full retune window; a src-unknown pairing gets
    // the tight one, or two distinct back-to-back calls on a busy talkgroup
    // collapse into one row (the wildcard above cannot tell units apart).
    const bool srcKnownMatch = existing.src != 0 && existing.src == row.src;
    return call_history_merge_within_window(existing.when, row_end_secs(existing), row.when, row_end_secs(row),
                                            srcKnownMatch);
}

/**
 * @brief Whether analog row @p row is the reception @p existing logged, read again (issue #667).
 *
 * Exactly the same: its start, its tone (or the same lack of one) and its frequency. A replay in a fresh ring pushes
 * that, and so does the ring an update of a row an older build stored (with no fragments) comes from.
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
    return existing.when == row.when && existing.freqHz == row.freqHz && sameTone;
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
    for (const QString& own : row.fragments) {
        const QStringView ring = fragment_ring(own);
        if (ring.isEmpty()) {
            continue;
        }
        for (const QString& held : existing.fragments) {
            if (held != own && fragment_ring(held) == ring) {
                return true;
            }
        }
    }
    return false;
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
        if (row_end_secs(existing) == row_end_secs(row)) {
            return i;
        }
        if (first < 0) {
            first = i;
        }
    }
    return first;
}

CallHistoryProvenance
provenance(qint64 when, qulonglong seq, int slot) {
    CallHistoryProvenance from;
    from.when = static_cast<int64_t>(when);
    from.seq = static_cast<uint64_t>(seq);
    from.slot = slot;
    return from;
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
        existing.callChannel = row.callChannel;
        existing.callChannelKind = row.callChannelKind;
        existing.cchWhen = row.cchWhen;
        existing.cchSeq = row.cchSeq;
        existing.cchSlot = row.cchSlot;
    }
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
    // A row keeps one radio and that radio's name. The merge never folds two known sources, but an update found by
    // its fragment (ingestRow()) is not merged again: a fragment of unknown source that joined one radio's row and
    // then learned another radio keeps the row's radio, so it must not bring that other radio's name.
    if (existing.src == 0 || row.src == 0 || existing.src == row.src) {
        merge_source_label(existing, row);
    }
    if (receive_adopt_identity(existing, row)) {
        existing.service = row.service;
        /* A service name outranks every other name a row has (row_take_name()). */
        if (!row.service.isEmpty()) {
            existing.name = row.service;
        }
    }
    fold_carrier(existing, row);
    fold_site(existing, row);
    // The same analog reception read again: a row an older build stored learns the mode it did not keep.
    if (existing.analog && dsd_analog_mode_known(existing.analogMode) == 0) {
        existing.analogMode = row.analogMode;
    }
    const qint64 start = qMin(existing.when, row.when);
    const qint64 span = qMax(row_end_secs(existing), row_end_secs(row)) - start;
    existing.when = start;
    if ((existing.durationSecs >= 0 || row.durationSecs >= 0) && span <= kCallHistoryMaxPlausibleDurationSecs) {
        existing.durationSecs = static_cast<int>(span);
    }
    existing.enc = existing.enc || row.enc;
    existing.emergency = existing.emergency || row.emergency;
    if (existing.src == 0) {
        existing.src = row.src;
    }
    // A call heard again in a later session is part of that session too: its views show
    // the merged row, and a full log keeps it as that session's.
    existing.session = qMax(existing.session, row.session);
    // The fragment's recordings are the merged row's now.
    unionFragments(existing, row.fragments);
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
        if (existing.kind == KindNotice && existing.when == row.when && existing.tg == row.tg && existing.src == row.src
            && existing.name == row.name && existing.detail == row.detail && existing.systemName == row.systemName
            && existing.systemUid == row.systemUid && existing.channel == row.channel
            && receive_services_compatible(existing, row) && !m_noticeTwinsTaken.contains(keyFor(existing))) {
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
    // Its fragment is the logged notice's now, so a spill folded twice finds it there (foldSpill()).
    unionFragments(logged, row.fragments);
    (void)receive_adopt_identity(logged, row);
    const QModelIndex idx = index(repeated);
    Q_EMIT dataChanged(idx, idx, {SessionRole, FreqHzRole, AccessCodeRole, AccessCodeTextRole, ReceivedToneRole});
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
    static const QVector<int> mergeRoles = {WhenRole,         SrcRole,          SourceNameRole, EncRole,
                                            DurationSecsRole, DayLabelRole,     TimeTextRole,   EmergencyRole,
                                            SessionRole,      FreqHzRole,       AccessCodeRole, AccessCodeTextRole,
                                            ServiceNameRole,  ReceivedToneRole, NameRole,       RecordingsRole,
                                            HasRecordingRole, SiteTextRole,     AnalogModeRole};
    (void)nameChannel(m_rows[merged]);
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
    const bool wholeLog = again || isUpdate;
    // An update refines the row that already holds its fragment, found by the fragment (issue #799, H1). Merged again
    // by its content it could miss that row once it brings what the row's other fragments disagree with (a site the
    // core filled in later), and be dropped. A row an older build stored keeps no fragments, and is found by merging.
    int merged = isUpdate ? rowHoldingFragment(row) : -1;
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
CallHistoryModel::refresh(const dsd_state* snapshot, const dsd_opts* opts_snapshot) {
    // The first ingest after the app returns takes over from the background tap: what the tap found is folded in
    // before this read, so the walk below finds those rows seen, and anything newer it finds itself.
    if (m_handoffOwed) {
        handOffBackgroundCapture();
    }
    if (snapshot == nullptr || snapshot->event_history_s == nullptr) {
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

    // Whether a row may carry the saved system's UID is its own stamp (row_from_item()), which covers scans started
    // from a saved system's extra arguments as well as the list UI. A snapshot read without its options cannot
    // establish identity.
    const QList<FreshRow> fresh = collectFresh(snapshot, scan, opts_snapshot != nullptr ? m_sessionUid : QString());

    if (fresh.isEmpty()) {
        return;
    }

    bool rowsChanged = false;
    for (const FreshRow& item : fresh) {
        rowsChanged = ingestRow(item.row, item.isUpdate, item.again) || rowsChanged;
    }
    rowsChanged = trimToCapacity() || rowsChanged;
    pruneSeen();
    if (rowsChanged) {
        Q_EMIT countChanged();
    }
    markChanged();
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
    // tap stops, its read position joins the model's, and it starts again behind the new mark. Everything spilled so
    // far is cleared rows, and so is what the tap could not write.
    // A tap the app's return already owes a handoff (no ingest since) is handed off instead: stopped, its clear
    // adopted and its spill folded, as the next ingest would have, and it stays stopped in the foreground.
    const bool tapWasRunning = m_tap && m_tap->running();
    const bool resumeTap = tapWasRunning && !m_handoffOwed;
    if (resumeTap) {
        const CallHistoryTap::Result tapped = m_tap->stop();
        takeTapReadPosition(tapped.read, tapped.pushSeq, tapped.ring);
    } else if (tapWasRunning) {
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

/** @brief A stored integer, or @p fallback when the key is missing (a store an older build wrote). */
qint64
json_i64_or(const QJsonObject& obj, const char* key, qint64 fallback) {
    return obj.contains(QLatin1String(key)) ? obj.value(QLatin1String(key)).toVariant().toLongLong() : fallback;
}

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

/**
 * @brief A stored integer that fits @p T, or false when the key is missing, holds no integer or does not fit.
 *
 * For values a ring row carries in fields of that width, so a hand-edited store cannot wrap one into another value.
 */
template <typename T>
bool
json_field_fits(const QJsonObject& obj, const char* key, T* out) {
    constexpr qint64 kNone = std::numeric_limits<qint64>::min();
    const QJsonValue value = obj.value(QLatin1String(key));
    const qint64 n = value.isDouble() ? value.toInteger(kNone) : kNone;
    if (n < static_cast<qint64>(std::numeric_limits<T>::min())
        || n > static_cast<qint64>(std::numeric_limits<T>::max())) {
        return false;
    }
    *out = static_cast<T>(n);
    return true;
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

/** @brief One stored row. */
CallHistoryModel::Row
row_from_json(const QJsonObject& obj) {
    CallHistoryModel::Row row;
    row.when = obj.value(QLatin1String("when")).toVariant().toLongLong();
    row.name = obj.value(QLatin1String("name")).toString();
    row.tg = obj.value(QLatin1String("tg")).toVariant().toULongLong();
    row.src = obj.value(QLatin1String("src")).toVariant().toULongLong();
    row.sourceName = obj.value(QLatin1String("srcName")).toString();
    row.enc = obj.value(QLatin1String("enc")).toBool();
    row.emergency = obj.value(QLatin1String("em")).toBool();
    row.durationSecs = obj.value(QLatin1String("durationSecs")).toInt(-1);
    row.systemName = obj.value(QLatin1String("systemName")).toString();
    row.systemUid = obj.value(QLatin1String("systemUid")).toString();
    row.kind = obj.value(QLatin1String("kind")).toInt(CallHistoryModel::KindVoice);
    row.detail = obj.value(QLatin1String("detail")).toString();
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
    // Issue #708; absent in stores older builds wrote (a uid is then assigned at load).
    row.uid = qMax<qint64>(json_i64_or(obj, "uid", 0), 0);
    for (const auto& fragment : obj.value(QLatin1String("fr")).toArray()) {
        const QString key = fragment.toString();
        if (!key.isEmpty() && !row.fragments.contains(key)) {
            row.fragments.append(key);
        }
    }
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

/** @brief One row as the rows store and the spill write it. */
QJsonObject
row_to_json(const CallHistoryModel::Row& row) {
    QJsonObject obj;
    obj.insert(QLatin1String("when"), row.when);
    obj.insert(QLatin1String("name"), row.name);
    obj.insert(QLatin1String("tg"), static_cast<qint64>(row.tg));
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
    obj.insert(QLatin1String("systemName"), row.systemName);
    obj.insert(QLatin1String("systemUid"), row.systemUid);
    obj.insert(QLatin1String("kind"), row.kind);
    if (!row.detail.isEmpty()) {
        obj.insert(QLatin1String("detail"), row.detail);
    }
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
    if (row.uid > 0) {
        obj.insert(QLatin1String("uid"), row.uid);
    }
    if (!row.fragments.isEmpty()) {
        obj.insert(QLatin1String("fr"), QJsonArray::fromStringList(row.fragments));
    }
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
        m_seen.insert(keyFor(*it), SeenState{it->when, it->when + qMax(it->durationSecs, 0), it->src, it->emergency,
                                             it->enc, it->sourceName, it->freqHz, it->codeKind, it->code,
                                             it->receiveKnown, it->receiveSlot, it->service, it->siteId.valid,
                                             it->callChannel, it->callChannelKind, it->session, 0U, QString()});
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
    if (state.callChannel != 0U || state.callChannelKind != DSD_CALL_CHANNEL_NONE) {
        obj.insert(QLatin1String("cch"), static_cast<qint64>(state.callChannel));
        obj.insert(QLatin1String("cchKind"), state.callChannelKind);
    }
    obj.insert(QLatin1String("session"), state.session);
    obj.insert(QLatin1String("ring"), QString::number(state.ring, 16));
    if (!state.twin.isEmpty()) {
        obj.insert(QLatin1String("twin"), state.twin);
    }
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
    if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden) {
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
    if (!m_tap || m_tap->running()) {
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
        return false;
    }
    const Row row = row_from_json(rowObj);
    const SeenState seen = seenEntryFromJson(obj.value(QLatin1String("seen")).toObject());
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
    bool rowsChanged = ingestRow(row, isUpdate, again);
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

QByteArray
CallHistoryModel::spillLine(const FreshRow& fresh) {
    QJsonObject obj;
    obj.insert(QLatin1String("v"), 1);
    obj.insert(QLatin1String("key"), fresh.key);
    obj.insert(QLatin1String("seen"), seenEntryToJson(fresh.seen));
    obj.insert(QLatin1String("row"), row_to_json(fresh.row));
    if (fresh.isUpdate) {
        obj.insert(QLatin1String("upd"), true);
    }
    if (fresh.again) {
        obj.insert(QLatin1String("again"), true);
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
