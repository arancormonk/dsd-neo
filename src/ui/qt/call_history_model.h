// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Structured, persistent call log behind the history screen and the
 *        monitor's recent-calls panel.
 *
 * Rows are ingested from the published snapshot's event-history ring — committed
 * voice rows plus data/control notices (SMS, LRRP positions, data calls), keyed
 * so a ring that shifts under us never double-counts — and persisted as JSON so
 * the log survives sessions and process death, which the in-memory event ring
 * deliberately does not. Refreshed from the single UI poll tick only.
 *
 * This model is the store alone: every view that shows it binds through its own
 * CallHistoryFilterModel, so one screen's search or pills never filter another.
 *
 * Identity, and order across sessions, never come from the rows' stamps. Those are
 * decode time, which in a replay is the capture's time, days or years before any live
 * row. A decode session (beginSession()) tags each row it logs. Clear wipes the
 * session's ring by its push order. A full log gives up the oldest session's rows
 * first, and only within one session the oldest start. A capture replayed in a
 * fresh state pushes the same rows at the same stamps into a new ring, told apart by
 * the ring's identity (Event_History_I::instance): those calls are heard again, and
 * each folds into a logged row it overlaps, which joins the running session. (A first
 * sighting merges only within the newest rows, so a call the first replay split in
 * two stays two rows.)
 *
 * Each row also carries the frequency and access code its call was heard with (issue
 * #575). A merged row takes each from the newest fragment that knew it, field by field
 * (call_history_merge.h), and the access code's text is app-control's, spelled when read.
 * Issue #799: and the system and site it was heard on, with the DMR call's own channel. Fragments
 * heard on two sites stay two rows, and the site line is app-control's, spelled when read.
 *
 * Issue #667: an analog row is one reception and never folds; heard again or updated, it is matched only by its exact
 * start, tone and frequency, and it keeps the mode it was heard in (NFM, AM, or unknown on audio input).
 *
 * Issue #708: each row has a stable uid, and the ring fragments it absorbed, which is how its
 * call recordings find it (call_recording_provider.h). While the app is in the background, where
 * Qt pauses this model's thread, CallHistoryTap keeps reading the ring and spills what it finds;
 * the model folds that in when it reads again. A save counts only when both stores committed.
 *
 * Issue #800: a row keeps its text message, its GPS text and the admitted position that text describes, voice rows as
 * well as notices, and a late enrichment of a committed call is read again. A merged row's GPS text and position come
 * together from one fragment, the newest by its run's session ordinal and the position's rev, never by its stamps. The
 * row also keeps its raw identity (sync type, source and target text, sender and reception system), and fragments
 * whose known identities differ never merge: two callsign callers, two textual destinations under one service name,
 * two conventional systems on two frequencies.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_

#include <QAbstractListModel> // IWYU pragma: keep -- public header for the base class.
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <functional>
#include <limits>
#include <memory>
#include <stdint.h>

#include "call_history_heard.h"
#include "call_history_merge.h"
#include "heard_talkgroup_source.h"

#ifdef DSD_NEO_TEST_HOOKS
class QThread;
#endif

namespace dsd_qt {

class CallHistorySpill;
class CallHistorySpillReader;
class CallHistoryTap;
class CallRecordingProvider;
class HeardTalkgroupSink;

/** @brief Where the background tap reads the published rings: dsd_app_history_tap_copy() in the app. */
using CallHistoryTapSource = int (*)(int slot, Event_History_I* out, dsd_app_history_tap_cursor* cursor,
                                     dsd_app_history_tap_meta* meta);

class CallHistoryModel : public QAbstractListModel, public HeardTalkgroupSource {
    Q_OBJECT
    Q_INTERFACES(dsd_qt::HeardTalkgroupSource)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString sessionLabel READ sessionLabel WRITE setSessionLabel NOTIFY sessionLabelChanged)
    Q_PROPERTY(QString sessionUid READ sessionUid WRITE setSessionUid NOTIFY sessionUidChanged)
    Q_PROPERTY(qlonglong session READ session NOTIFY sessionChanged)
    Q_PROPERTY(QStringList systemLabels READ systemLabels NOTIFY countChanged)
    /* True from the first save of the log that fails to reach the disk until one succeeds (issue #708, B6). */
    Q_PROPERTY(bool saveFailing READ saveFailing NOTIFY saveFailingChanged)

  public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        TgRole,
        SrcRole,
        EncRole,
        EmergencyRole,
        WhenRole,         // call start, seconds since epoch
        DurationSecsRole, // -1 when unknown
        SystemNameRole,
        SystemUidRole,
        DayLabelRole,   // "TODAY" / "YESTERDAY" / "MON 3 AUG" / "SAT 21 NOV 2015", "TIME UNKNOWN" — list sections
        TimeTextRole,   // "12:04", empty with no start
        KindRole,       // RowKind: voice call or data/control notice
        DetailRole,     // the row's text message and GPS text, joined (issue #800: voice rows too); else empty
        ChannelRole,    // scan channel the row was heard on (-Y row name or trunk-scan target id), else empty
        SourceNameRole, // resolved source label, falling back to the OTA source text
        SessionRole,    // decode session that logged the row, or last extended it (see session())
        FreqHzRole,     // frequency in Hz the call was heard on (qint64), 0 when unknown
        AccessCodeRole, // "CC 1", "NAC 293", "RAN 5", "CAN 0" (app_control/access_code_view.h), else empty
        ServiceNameRole,
        ReceivedToneRole,
        AccessCodeTextRole, // "Color code 1", "Network access code 293", ..., else empty
        UidRole,            // the row's stable identity (qint64), for rowForUid()
        RecordingsRole,     // the row's call recordings (CallRecordingProvider), oldest first
        HasRecordingRole,   // whether the row has any
        SiteTextRole,       // "DMR · Con+ · Net 3 · Site 5 · Connect Plus · LCN 3" (site_identity_view.h), else empty
        AnalogRole,         // a conventional analog (NFM/AM) reception (issue #667)
        AnalogModeRole,     // an analog row's mode, "NFM" or "AM"; empty when not known, and on every other row
        PositionRole,       // issue #800: the row's saved fix for the map's pin (position_record()); {} without one
        GiRole              // issue #800: who the call was to: 0 a talkgroup, 1 a radio (private), -1 not known
    };

    /** @brief What a row logs; pinned values because rows persist as JSON. */
    enum RowKind { KindVoice = 0, KindNotice = 1 };
    Q_ENUM(RowKind)

    explicit CallHistoryModel(QObject* parent = nullptr);
    ~CallHistoryModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return static_cast<int>(m_rows.size());
    }

    /**
     * @brief Which saved system the current session is on; stamped onto new rows.
     *
     * Persisted: the Android service outlives the Activity, and a relaunched UI
     * ingests the running session's backlog before any start button is pressed —
     * those rows must carry the system that produced them, not an empty string.
     */
    QString
    sessionLabel() const {
        return m_sessionLabel;
    }

    void setSessionLabel(const QString& label);

    /** Saved-system identity of the session; empty for exploring and scan lists. */
    QString
    sessionUid() const {
        return m_sessionUid;
    }

    void setSessionUid(const QString& uid);

    /**
     * @brief Identity of the decode session the history is logging.
     *
     * Every row carries the session that logged it, or that last extended it with a
     * later fragment. The views of "this session" (the monitor's recent calls, the heard
     * talkgroups) select on it rather than on the rows' stamps, which in a replay are the
     * capture's time. Persisted, so a relaunched UI reattaching to the running session keeps it.
     * 0 until the first beginSession(); only moves forward.
     */
    qlonglong
    session() const {
        return m_session;
    }

    /**
     * @brief Begin a new decode session.
     *
     * The UI calls this on every start. A replay start or an input change is a start,
     * and so is the first start after the process starts. Ingest the previous session's
     * tail first (UiController::flushHistory()), or it would be logged under the new session.
     * The start usually brings a fresh ring, but an embedding host can reuse the last run's
     * state, ring included. So a clear that names its ring is kept: it goes on covering that
     * ring, and the first read of any other ring drops it. The last read position stays too.
     */
    Q_INVOKABLE void beginSession();

    /**
     * @brief Begin a new decode session (beginSession()) on the system @p label, @p uid (sessionLabel, sessionUid).
     *
     * One change, not three setter calls: while the app is away the background tap is handed off once and started
     * again only after every field moved, so a call it finds is never attributed to half of the change.
     */
    Q_INVOKABLE void beginSessionOn(const QString& label, const QString& uid);

    /**
     * @brief beginSessionOn() with the new session's owners (setSessionOwners(): @p singleOwnerUid, and
     * @p scanEntryOwner mapping each scan target id to a saved system's uid), all under the one handoff of the
     * background tap, so it never runs on the session without them (issue #903). Main.startWithMap()'s call.
     */
    Q_INVOKABLE void beginSessionWithOwners(const QString& label, const QString& uid, const QString& singleOwnerUid,
                                            const QVariantMap& scanEntryOwner);

    /**
     * @brief Before Main asks the host for a start (issue #903): hand the background tap off now, its last poll binding
     * the old run's ring to the session ending (no starting run's ring exists yet), and keep it stopped, so no poll
     * between the host's accept and the new session can bind the starting run's ring to the old session.
     *
     * The next beginSession*() consumes the hold and starts the tap on the new session when the app is away. A start
     * that does not begin one releases it (releaseCaptureHold()), and so does the next event-loop turn if nothing did:
     * the start runs in one turn.
     */
    Q_INVOKABLE void holdCaptureForStart();

    /** @brief End a holdCaptureForStart() no session began from: the tap starts again when the app is away. */
    Q_INVOKABLE void releaseCaptureHold();

    /** @brief The session's system, label and uid together, the session itself kept (beginSessionOn()). */
    Q_INVOKABLE void setSessionSystem(const QString& label, const QString& uid);

    /**
     * @brief Names for the scan channels a call can be heard on: a trunk-scan target's id (the app writes each entry's
     * uid) to the name the operator gave it. A call with no name of its own is named by its channel, and an id is not
     * a name a person recognises (issue #671).
     *
     * The names add to the ones given before, a newer name for an id replacing the older: an id is one entry's for
     * good, and rows logged under an earlier session keep showing its name after another session starts. Each row also
     * keeps the name its channel had when it was logged, persisted with it, for an id no later call names. An id
     * named nowhere shows as it is.
     */
    Q_INVOKABLE void setChannelNames(const QVariantMap& names);

    /** @brief Distinct system names present in the log, for the filter pill. */
    QStringList systemLabels() const;

    /**
     * @brief The row that has @p uid now, or -1.
     *
     * A row's position moves as calls arrive, merge and age out; its uid does not. A view that follows one row (the
     * detail sheet) keeps the uid and looks its row up again on rowPositionsChanged().
     */
    Q_INVOKABLE int rowForUid(qint64 uid) const;

    /**
     * @brief The call recordings of the row that has @p uid now (RecordingsRole), or none.
     *
     * For the detail sheet, which follows one row by its uid: it reads them again on dataChanged() and
     * rowPositionsChanged(), so a fragment folded into the row, or a recording deleted, shows while it is open.
     */
    Q_INVOKABLE QVariantList recordingsForUid(qint64 uid) const;

    /**
     * @brief The uid of the row that holds the ring fragment @p fragment now, or 0 when none does (never logged,
     * trimmed, cleared) or the fragment is withdrawn from it (issue #800: another call's, FragmentPart::withdrawn).
     *
     * Issue #809: a Signal search recording names its call by fragment; the row it merged into is the one found. A
     * double because QML reads it as a JS number, which holds every uid this model hands out exactly.
     */
    Q_INVOKABLE double uidForFragment(const QString& fragment) const;

    /**
     * @brief The details of the row that has @p uid (its UidRole as text; QML passes the number) now, or an empty map.
     *
     * One key per role the detail sheet shows, by the role names (uid, name, when, systemName, systemUid, channel, tg,
     * src, emergency, enc, durationSecs, detail, freqHz, accessCode, accessCodeText, serviceName, receivedTone,
     * siteText, analog, analogMode), except the source label, which is sourceName. A sheet that follows one row by its
     * uid reads it again when the row changes, so a fragment folded in after it opened shows.
     */
    Q_INVOKABLE QVariantMap recordFor(const QString& uid) const;

    /**
     * @brief One row as a History export writes it: plain values, each read through the role History shows it by
     *        (data()), so the file says what the screen says.
     */
    struct ExportRow {
        qint64 when = 0;       // WhenRole: the start in seconds, 0 when not known
        int durationSecs = -1; // DurationSecsRole: -1 when not known
        int kind = KindVoice;  // KindRole
        QString name;          // NameRole: a channel id shown by its name
        qulonglong tg = 0;
        int gi = -1; // GiRole: who the call was to, 0 a talkgroup, 1 a radio, -1 not known
        qulonglong src = 0;
        QString sourceName;
        QString systemName;
        QString channel; // ChannelRole: the channel's display name
        qint64 freqHz = 0;
        QString accessCode;   // AccessCodeRole: "NAC 293"
        QString receivedTone; // ReceivedToneRole
        QString siteText;     // SiteTextRole
        bool analog = false;
        QString analogMode; // AnalogModeRole: "NFM", "AM", or empty when not known or not analog
        bool enc = false;
        bool emergency = false;
        QString detail;             // DetailRole
        QStringList recordingNames; // CallRecordingProvider::recordingEntryNames()
    };

    /**
     * @brief The rows at @p sourceRows (this model's rows, in the order given; a row out of range is skipped) as an
     *        export writes them.
     *
     * Every row's recording names come from one recordingEntryNames() call to the provider (none without one), so an
     * export takes the recording store's lock once, not once per row.
     */
    QList<ExportRow> exportRows(const QList<int>& sourceRows) const;

    /** @brief One row as a History playlist takes it (issue #909): who it was and its recordings, at the press. */
    struct PlaylistRow {
        qint64 uid = 0;          // UidRole
        QString name;            // NameRole: what the row shows
        qint64 when = 0;         // WhenRole: the start in seconds, 0 when not known
        QVariantList recordings; // CallRecordingProvider::recordingsForFragmentLists(): every one, oldest first
    };

    /**
     * @brief The rows at @p sourceRows (this model's rows, in the order given; a row out of range is skipped) as a
     *        playlist takes them.
     *
     * Every row's recordings (its withdrawn fragments left out, as RecordingsRole) come from one
     * recordingsForFragmentLists() call to the provider (none without one), so a playlist takes the recording store's
     * lock once, not once per row.
     */
    QList<PlaylistRow> playlistRows(const QList<int>& sourceRows) const;

    bool
    saveFailing() const {
        return m_saveFailing;
    }

    /**
     * @brief Read the recording roles from @p provider; nullptr reports no recordings. Not owned.
     *
     * The provider tells the model which fragments changed through recordingFragmentsChanged().
     */
    void setRecordingProvider(const CallRecordingProvider* provider);

    /**
     * @brief Let the background tap read the published rings through @p source (dsd_app_history_tap_copy()).
     *
     * Without a source (tests that drive the ring by hand) nothing reads the ring while the app is in the background.
     */
    void setHistoryTapSource(CallHistoryTapSource source);

    /**
     * @brief Hand each verified talkgroup History ingests to @p sink (issue #903); nullptr hands none. Not owned: the
     * sink outlives this model, whose teardown still ingests what the background tap could not spill.
     *
     * A fragment is handed over once, at the reading of it that first carries its proof (Event_History::heard_variant),
     * when that reading was taken on live input, the operator did not hold the tuner, and its session names its owner
     * (setSessionOwners()): at the foreground poll, the spill fold and the teardown fold, before the log is trimmed, at
     * the fragment's own time (its end, else its start), never the conversation's it merges into. A reload of the saved
     * log hands over nothing. What is ingested before a sink is set (the spill folded at construction) waits for it.
     */
    void setHeardSink(HeardTalkgroupSink* sink);

    /**
     * @brief Who owns the calls History session @p sessionId hears (issue #903): @p singleOwnerUid, the saved system a
     * saved-system start runs (empty for any other start), and for a scan-list start @p scanEntryOwner, each scan
     * target id (entry uid) to the saved system it runs, built from the start's entries and every target file it
     * launched before the engine drops duplicate ids, leaving out any id another saved system, an imported target file
     * or a frequency entry also gives.
     *
     * Bound to the decoder ring: a ring belongs to the session it was first read in (the background tap's reads
     * included), so the last calls of a session's ring that are read after the next session began keep its owners.
     * Persisted with the session, for a relaunched UI that rejoins the running session.
     */
    void setSessionOwners(qint64 sessionId, const QString& singleOwnerUid,
                          const QHash<QString, QString>& scanEntryOwner);

    /**
     * @brief Make the background spill this model folded unable to come back (issue #903): fold what a running tap
     * holds (it starts again), and a spill no fold has read whole (a read that failed, or lines a failed truncate
     * left) again; then, when a folded spill waits for a save's acknowledgement, wait out the saves in flight and write
     * both stores now, which acknowledges it and empties the spill. For HeardTalkgroupStore's forget and clear
     * (HeardTalkgroupStore::setBeforeEdit()), so a relaunch after the process dies refolds nothing they removed.
     * @return true only when no spill is left to replay. False when that write failed (the spill is kept, as every
     * failed save keeps it, and the log is reported not saving, saveFailing(), until a save lands), or when the spill
     * could not be read whole or emptied.
     */
    bool settleSpill();

    /** @brief settleSpill() of this model as HeardTalkgroupStore::setBeforeEdit() takes it; true once the model is gone
     * (nothing of it can fold again). */
    std::function<bool()> spillSettler();

    /**
     * @brief Whether the app came back from the background with the tap still running.
     *
     * The tap stops at the first ingest after that (refresh()), which also folds in what it spilled. UiController
     * calls flushHistory() for it when no redraw would.
     */
    bool
    backgroundCaptureHandoffOwed() const {
        return m_handoffOwed;
    }

    /** @brief HeardTalkgroupSource: read straight from the rows and their parts, no QVariant per row (issue #718),
     * each fragment by its own session and owner (Row::viewOwner), on its row's scan target (Row::channel) and its own
     * -Y scan-list row (Row::scanListRow, none off it). */
    QList<HeardTalkgroupRow> heardTalkgroups(qint64 forSession, const QString& owner,
                                             const HeardTarget& target) const override;

    /** @brief HeardTalkgroupSource: the serial ingestReading() gave the newest fragment it took in. */
    quint64
    heardSerial() const override {
        return m_heardSerial;
    }

    /** @brief HeardTalkgroupSource: from now on ingestReading() hands on no proof of a fragment it took in by now. */
    void heardForgotten(const QString& owner, quint32 number) override;

    /** @brief HeardTalkgroupSource: heard_owner_for() in the session @p ring is bound to, else this one. */
    QString heardOwnerOnAir(quint64 ring, bool operatorTuned, bool scanMember, const QString& channel) const override;

    /**
     * @brief Ingest newly committed voice rows from the snapshot ring.
     *
     * Call from the UI poll tick only. Duplicate protection keys on the row's
     * stable ring identity — slot plus the push stamp the row was committed at —
     * not its index, which shifts on every push. Updates are granular
     * (insert/change/remove), never a model reset — a reset would destroy every
     * delegate and the reader's scroll position per ingest.
     *
     * Options must accompany the snapshot to attribute hold-eligible rows. Which
     * rows are eligible is each row's own stamp (issue #671): a call heard under a
     * running scan, including one enabled through extra arguments, never is, and a
     * call heard while the operator held the tuner is filed as Exploring.
     *
     * Issue #903: without @p bindNewRings (the flush after the host accepted a start, before the session begins:
     * UiController::flushHistoryBeforeStart()), a snapshot of a ring no session owns is not read at all, its commits
     * left for the next read: the flush before the host was asked bound the old run's (holdCaptureForStart()), so such
     * a ring can only be the starting run's, whose calls the coming session owns. On a ring a session owns, what it
     * pushed since the last read is the old run's last or the new run's first on the ring it reused: it is read, and
     * no one's (readBeforeSession()).
     */
    void refresh(const dsd_state* snapshot, const dsd_opts* opts_snapshot = nullptr, bool bindNewRings = true);

    /**
     * @brief Empty the log, and keep the rows the session's ring still holds out of it.
     *
     * Those rows are named by the ring position (push_seq per slot) this model last read, in
     * the current session, and by that ring's identity (Event_History_I::instance). Everything the
     * ring takes in after that is shown, whatever its stamps, and so is every row of another ring.
     * Persisted, so a relaunched UI does not ingest the cleared rows again. It holds across a start
     * that reuses the ring, in any session, and goes at the first read of another ring. A clear
     * after a start, before the next read, covers what the last read found, in the ring it was read
     * in: a reused ring's rows stay cleared, and a fresh ring's first read drops the clear. Only a
     * freshly constructed model (a relaunched UI clearing before its first tick) has not read the
     * ring; its clear covers what the first read finds, or nothing if a start comes first.
     */
    Q_INVOKABLE void clearAll();

    /**
     * @brief Write what the stores owe now, without waiting out the save delay.
     *
     * For the moments the log must reach the disk: a session that just ended, the app leaving the
     * screen (applicationStateChanged to Suspended or Hidden, connected here), and aboutToQuit. Runs
     * on the save worker like every other save. A save in flight is not raced: the stores are
     * copied now and their save queued behind it, so it runs the moment that one finishes, with no
     * GUI event loop needed in between. Nothing owed, nothing written.
     */
    void saveNow();

  public Q_SLOTS:
    /** @brief The recording store's news: the recordings of @p fragments changed; their rows re-read their roles. */
    void recordingFragmentsChanged(const QStringList& fragments);

  public:
#ifdef DSD_NEO_TEST_HOOKS
    /** @brief Call @p observer with the thread each store's JSON is built on (nullptr stops). */
    static void setStoreBuildObserverForTest(void (*observer)(const QThread* thread));

    /** @brief Make the next fold's read of the spill fail after @p lines lines, as a device error there would. */
    static void failNextSpillReadAfterLinesForTest(int lines);

    /** @brief While @p fail, every truncate of the spill fails, as a spill that can be read but not written would. */
    static void failSpillTruncatesForTest(bool fail);

    /** @brief The fragment parts the folds' part lookups read or indexed since the last call, which resets it. */
    static qint64 takePartLookupStepsForTest();

    /** @brief Start the debounced save now, as its timer would. */
    void
    startSaveForTest() {
        m_saveTimer.stop();
        startAsyncSave();
    }

    /** @brief Wait for the save worker to finish its write. */
    void
    waitForSaveForTest() {
        m_savePool.waitForDone();
    }

    /** @brief The delay the next debounced save waits. */
    int
    saveDelayMsForTest() const {
        return m_saveTimer.interval();
    }

    /** @brief Whether a debounced save is waiting on its timer. */
    bool
    savePendingForTest() const {
        return m_saveTimer.isActive();
    }

    /** @brief Poll the published rings every @p ms while in the background, and spill after @p batchPolls polls. */
    void setBackgroundCaptureTimingForTest(int ms, int batchPolls);

    /** @brief Whether the background tap is running. */
    bool backgroundCaptureRunningForTest() const;

    /** @brief Wait until the tap has finished a poll that started after this call (at once when it is not running). */
    void waitForBackgroundPollForTest();

    /** @brief How many times the tap has appended to the spill since this model was built. */
    int spillAppendsForTest() const;

    /** @brief How many times the tap has tried to, written or not. */
    int spillAppendAttemptsForTest() const;

    /** @brief The most rows the log has held since this model was built. */
    int
    peakRowsForTest() const {
        return m_peakRowsForTest;
    }

    /** @brief How many rows the trims have looked at since this model was built. */
    qint64
    trimRowsScannedForTest() const {
        return m_trimRowsScannedForTest;
    }

    /** @brief The most entries the seen map and the notice twins taken have held while a spill folded. */
    qsizetype
    peakFoldSeenForTest() const {
        return m_peakFoldSeenForTest;
    }

    qsizetype
    peakFoldTwinsForTest() const {
        return m_peakFoldTwinsForTest;
    }

    /** @brief The most bytes a spill fold's reader has held at once. */
    qsizetype
    peakSpillReadBytesForTest() const {
        return m_peakSpillReadBytesForTest;
    }
#endif

  Q_SIGNALS:
    void countChanged();
    void sessionLabelChanged();
    void sessionUidChanged();
    void sessionChanged();
    void saveFailingChanged();
    /** @brief Rows were inserted, removed or moved: a view following a uid looks its row up again. */
    void rowPositionsChanged();

  public:
    /**
     * @brief The admitted position a row carries (issue #800): Event_History::pos while it is valid, read through
     *        call_history_position_values_valid(). Everything but valid is meaningful only while valid is set.
     */
    struct Position {
        bool valid = false;
        qint32 latE7 = 0;  // degrees x 1e7, north positive
        qint32 lonE7 = 0;  // degrees x 1e7, east positive
        quint32 errM = 0;  // metres, 0 when not known
        quint16 flags = 0; // dsd_position_flag
        quint8 kind = 0;   // dsd_position_kind
        quint64 rev = 0;   // orders positions within one run
        quint64 procInstance = 0;
        qint64 rxTimeS = 0;     // when it was received; display only, never orders
        bool anonymous = false; // names no source a row could check (Event_History::pos.anonymous)
    };

    /** @brief One fragment's GPS text and the position it describes, with the fragment and reading they came from
     * (issue #800, 2.7): its start, push stamp and slot, its run's session ordinal and the ring's commit_rev when it
     * was read (Row::gpsWhen..gpsVer, gpsSlot). */
    struct GpsUnit {
        QString text;
        Position pos;
        qint64 when = 0;
        qulonglong seq = 0;
        int slot = 0;
        qint64 ord = 0;
        quint64 ver = 0;
    };

    /**
     * @brief What one fragment's readings gave its voice row besides its GPS unit (issue #800): its span, crypto
     * and emergency flags, frequency and access code, site and call channel, reception scope, receive evidence,
     * service and name, decode session, its sender (source number and text, sender system), sync type and target text, its source
     * label and its text message, each with the provenance the row's own fields carry. A row keeps one per fragment
     * (Row::parts), keyed by the fragment's ring identity, and is always the fold of its parts not withdrawn
     * (rebuild_from_parts()), so when a fragment turns out to be another call's (fragment_call_compatible()) the row
     * rebuilds every field it gave from its other fragments' by the rules a merge folds them with. A fragment's part is
     * its newest reading, whole: nothing of an earlier reading outlives it.
     */
    struct FragmentPart {
        /* The fragment's key (call_history_fragment_key()); for the part standing for what a row an older build stored
         * without fragments held, a key of that row's own (`legacy|` and its seen key, legacy_part_key()), so two such
         * rows folded together stay two parts. Empty only in a part an earlier build of this one stored. */
        QString fragment;
        /* The fragment's reading named another call than the row's: kept so the row knows the fragment, never
         * folded. */
        bool withdrawn = false;
        bool enc = false;
        bool emergency = false;
        bool receiveKnown = false;
        /* Whether rxGen is known (Row::rxGenKnown). */
        bool rxGenKnown = false;
        uint8_t callChannelKind = 0;
        /* Who the call was to (Event_History::gi): 0 a talkgroup, 1 a radio (a private call), -1 not known. */
        qint8 gi = -1;
        qint64 when = 0;
        qint64 end = 0;
        int durationSecs = -1;
        int receiveKind = 6;
        int receiveValue = 0;
        int receiveSlot = 0;
        int freqSlot = 0;
        qint64 freqHz = 0;
        qint64 freqWhen = 0;
        qulonglong freqSeq = 0;
        int codeKind = 0;
        int code = 0;
        qint64 acWhen = 0;
        qulonglong acSeq = 0;
        int acSlot = 0;
        int cchSlot = 0;
        uint32_t callChannel = 0;
        /* The reception generation the reception was read under, and the ring it counts in (Row::rxGen). */
        quint64 rxGen = 0;
        /* The tuned frequency the reception was read with (Row::rxHz). */
        qint64 rxHz = 0;
        qint64 cchWhen = 0;
        qulonglong cchSeq = 0;
        dsd_received_identity siteId = {};
        CallHistoryScope rxScope;
        quint64 rxRing = 0;
        QString name;
        QString service;
        /* The decode session the reading came from (Row::session): the row is the newest session's of its parts. */
        qint64 session = 0;
        /* The sender and the call it names (Row's fields of the same names): the numeric target is the call's, as
         * the target text is, and a fragment whose later reading learns it gives it to the row. */
        int protocol = -1;
        qulonglong tg = 0;
        qulonglong src = 0;
        QString sourceText;
        QString targetText;
        CallHistoryScope sourceScope;
        /* The source label and the text message, each with the fragment it came from. */
        QString sourceName;
        qint64 sourceNameWhen = 0;
        qulonglong sourceNameSeq = 0;
        int sourceNameSlot = 0;
        int msgSlot = 0;
        QString messageText;
        qint64 msgWhen = 0;
        qulonglong msgSeq = 0;
        /* Issue #903: the verified talkgroup proof of this fragment's reading (Row::heardVariant). */
        quint32 heardId = 0;
        uint8_t heardVariant = 0;
        bool liveInput = false;
        /* Issue #903: the saved system that owns the fragment, and its ingest serial (Row::viewOwner, heardSerial). */
        QString viewOwner;
        quint64 heardSerial = 0;
        /* Issue #903: the -Y scan-list row the fragment was heard on (Row::scanListRow, scanListRev). */
        quint32 scanListRow = 0;
        quint64 scanListRev = 0;
    };

    /** @brief One logged call or notice. Public only so file-local helpers can build one. */
    struct Row {
        qint64 when = 0;
        QString name;
        qulonglong tg = 0;
        qulonglong src = 0;
        QString sourceName;
        /* Keep the selected label's fragment identity separate from the merged
         * call span. Same-second fragments use their push stamp/slot as a stable
         * tie-break; the same fragment may still enrich its label in place. */
        qint64 sourceNameWhen = 0;
        qulonglong sourceNameSeq = 0;
        int sourceNameSlot = 0;
        bool emergency = false;
        bool enc = false;
        /* Who the call was to (Event_History::gi, issue #800): 0 a talkgroup, 1 a radio (a private call), -1 not
           known. Part of the call's identity: a group call and a private call to the same number are two calls
           (call_kinds_differ()). A fold adopts a known value. Persisted (`gi`) when known; older rows read -1. */
        qint8 gi = -1;
        int durationSecs = -1;
        /* When the call ended, in seconds; 0 when not known. Kept only with a start stamp it is measured from, and
           persisted (`end`), so a span past the plausible cap still reads unknown after a fold, an update or a reload.
           A row chains from this stored end only while its duration is known; a row past the cap chains from its
           start (row_merge_end_secs()). Notices keep none. */
        qint64 end = 0;
        QString systemName;
        QString systemUid;
        int kind = KindVoice;
        /* Issue #800: the sync type the row was decoded from (Event_History::systype; -1 when not known), part of the
         * raw identity described below. Kept beside kind so the two pack together. */
        int protocol = -1;
        /* The scan channel the row was heard on: a -Y row name or a trunk-scan
         * target id. Empty when the receiver was not scanning. It is not the
         * system: the system is the saved entry the session runs, and stays so
         * for every row of that session. */
        QString channel;
        /* The channel's display name when the row was logged or first named (setChannelNames()), for a channel no
           names given since cover. Persisted; older rows read empty. */
        QString channelName;
        QString service;
        /* A conventional analog (NFM/AM) transmission (issue #637): its name is only a label (the channel, the mode or
           a tone's service name), never its destination. Persisted; older rows read false. Each analog row is one
           reception: it never folds with another row, and is matched only by the same reception read again (issue
           #667). */
        bool analog = false;
        /* An analog row's mode (dsd_analog_mode, issue #667): NFM or AM as the demodulator ran, unknown (0) on audio
           input. Persisted (`analogMode`) when known; rows older builds stored read unknown, whatever their name
           says. */
        uint8_t analogMode = 0;
        int receiveKind = 6;
        int receiveValue = 0;
        bool receiveKnown = false;
        int receiveSlot = 0;
        /* Ring identity, persisted with the row so keyFor() can reproduce the key
         * the ring scan used. slot is the TDMA slot; seq is the push-sequence stamp
         * the row was committed at (push_seq - index + 1), which is stable for the
         * row's whole life in the ring — unlike its index, which shifts per push,
         * and unlike its stamps, which merges may still refine. */
        int slot = 0;
        qulonglong seq = 0;
        /* The decode session that logged the row, or last extended it (session()). Rows from
         * stores written before sessions existed read 0. */
        qint64 session = 0;
        /* The frequency in Hz the call was heard on (Event_History::freq_hz), 0 when unknown.
         * A trunked call lands on several voice channels, so like the source label it carries
         * the fragment it came from, and a merge keeps the newest fragment's known value
         * (call_history_fold_adopts()). */
        qint64 freqHz = 0;
        qint64 freqWhen = 0;
        qulonglong freqSeq = 0;
        int freqSlot = 0;
        /* The access code the call was heard with: a dsd_access_code_kind (0 = none known) and
         * its value, folded as one pair with its own provenance. The text is derived at read
         * time (app_control/access_code_view.h) and never stored. */
        int codeKind = 0;
        int code = 0;
        qint64 acWhen = 0;
        qulonglong acSeq = 0;
        int acSlot = 0;
        /* Issue #799. The system and site the call was heard on (Event_History::site_id, site parts only), and the
         * DMR call's own channel with the numbering it was named in (dsd_call_channel_kind; 0 and NONE when unknown,
         * and for every other protocol). A merge joins fragments only when their sites and channel kinds agree
         * (dsd_call_site_compatible()) and keeps the union of their parts; the channel folds by the fragment it came
         * from, as the frequency does. The DMR site text and brand are kept as they were received, for display; no
         * compare reads them. Persisted (`site`, `cch*`); older rows read empty. */
        dsd_received_identity siteId = {};
        uint32_t callChannel = 0;
        uint8_t callChannelKind = 0;
        qint64 cchWhen = 0;
        qulonglong cchSeq = 0;
        int cchSlot = 0;
        /* Issue #708. The row's identity for views that follow it, persisted (`uid`); assigned when the row is first
         * logged, never reused, and kept by the row a merge folds another into. 0 until assigned. */
        qint64 uid = 0;
        /* Every ring fragment the row absorbed, as call_history_fragment_key()s: its own, and each one a merge folded
         * in (load folding too). Persisted (`fr`); rows older builds wrote have none, and no recordings either. */
        QStringList fragments;
        /* Issue #800. The version of the newest reading of each fragment the row folded in: the ring's commit_rev when
         * it was read, which every change of a committed ring row moves and nothing resets. A reading of a fragment
         * older than one the row already holds (a spill line replayed over a newer save) is stale and folds in
         * nothing (foldRow()), so no older label, message, frequency or code comes back. Persisted (`frv`) for the
         * fragments whose version is known. */
        QHash<QString, quint64> fragmentVers;
        /* Issue #800. What each fragment gave a voice row (FragmentPart), one per fragment, so a fragment found to be
         * another call's is taken back and the row rebuilt from the rest. Persisted (`fp`) for a row of two or more
         * parts; a one-fragment row is its own part. A row an older build stored without them is given one per
         * fragment before it next folds (ensure_parts()): each holds the row's values, and the values that carry a
         * fragment's provenance only in the part of that fragment. */
        QList<FragmentPart> parts;
        /* Issue #800. The row's raw identity, beside the display labels above: the sync type it was decoded from
         * (protocol, above), the source and target text as received (src_str, tgt_str:
         * callsigns, dial strings), the sender's system (pos.scope with its discriminator) and where it was heard
         * (rx_scope, once resolved). A merge never joins two that differ (rows_mergeable(); systems only when known at
         * one strength, call_history_scopes_compatible(), and the row keeps the stronger), and a
         * fragment gives the row its text and position only when its identity agrees (fragment_identity_compatible()).
         * Persisted (`proto`, `srcText`, `tgtText`, `sscope`, `rx`) when known; rows older builds stored know none. */
        QString sourceText;
        QString targetText;
        CallHistoryScope sourceScope;
        CallHistoryScope rxScope;
        /* Issue #800. The reception generation rxScope was read under (Event_History::rx_scope.rx_gen, while
         * rxGenKnown: dsd_state_reception_generation()) and the ring that generation counts in
         * (Event_History_I::instance; 0 when not known): with rxScope, the reception the core's reception rule reads
         * (call_history_same_reception()). A fragment gives the row its position, and the row learns a stronger scope
         * from it, only when it was heard on the row's reception, by one protocol class (fragment_same_reception()).
         * Persisted (`rxGen`, `rxRing`) while known. */
        quint64 rxRing = 0;
        quint64 rxGen = 0;
        bool rxGenKnown = false;
        /* Issue #903: the -Y scan-list row the fragment was heard on (Event_History::scan_list_row, scan_list_rev): 1 +
         * the row and its list's identity (scanListRev), 0 and 0 off any -Y row. Each part keeps its own; the row shows
         * its first part's. Persisted (`slr`, `sll`, also per part); rows older builds stored read none. */
        quint32 scanListRow = 0;
        /* Issue #800. The tuned frequency rxScope was read with (Event_History::rx_scope.tuned_hz; 0 when unknown):
         * two carriers are two receptions, also of one system whose scope names no carrier. Unlike freqHz, which a
         * merge takes from the newest fragment, it goes with rxScope. Persisted (`rxHz`) while known. */
        qint64 rxHz = 0;
        /* Issue #800, B2. The text message the row carries (Event_History::text_message), from the newest fragment
         * that had one, with that fragment's provenance. Persisted (`msg`, `msg*`). */
        QString messageText;
        qint64 msgWhen = 0;
        qulonglong msgSeq = 0;
        int msgSlot = 0;
        /* The GPS unit's fragment slot (with gpsWhen and gpsSeq below), beside msgSlot so the two pack together. */
        int gpsSlot = 0;
        /* Issue #800, 2.7 and B2. The row's GPS text (Event_History::gps_s) and the admitted position it describes, as
         * one unit from one fragment: the one with the greatest (gpsOrd, pos.rev) (call_history_gps_adopts()). gpsOrd
         * is the session ordinal of that fragment's run (sessionOrdinalFor()); a fresh fragment carries its own.
         * gpsVer is the ring's commit_rev when that fragment was read (0: not known), which orders two readings of the
         * same fragment. Persisted (`gps`, `gps*`, `pos`) while known. */
        QString gpsText;
        Position pos;
        qint64 gpsWhen = 0;
        qulonglong gpsSeq = 0;
        qint64 gpsOrd = 0;
        quint64 gpsVer = 0;
        /* The GPS units of the row's other fragments, one per fragment, beside the one shown above, which is the best
         * of them all (call_history_gps_adopts()). So a fragment whose newer reading loses its fix gives way to the
         * best remaining fragment's, whatever order the readings arrive in. Persisted (`gpsAlt`) when there are any. */
        QList<GpsUnit> gpsOthers;
        /* Detail text a store an older build wrote held (the notice's message or GPS string), shown while the row
         * has neither of the two above. */
        QString legacyDetail;
        /* Issue #903: the verified talkgroup proof (Event_History::heard_variant, heard_id; 0 and 0 for none) and the
         * live-input latch (Event_History::live_input). Each fragment keeps its own in its part (FragmentPart), which is
         * what History hands its sink; the row shows its first part's proof, and is live only while every part is. A
         * merged conversation never vouches for a fragment that had none. Persisted (`hv`, `hid`, `live`, also per part);
         * rows older builds stored read none and not live. */
        quint32 heardId = 0;
        uint8_t heardVariant = 0;
        bool liveInput = false;
        /* Issue #903: the saved system that owns the fragment for the Talkgroups screen (heard_owner_of(), resolved
         * where its ring was read, from the ring's own session, whatever its proof; empty for none or without published
         * options). Each part keeps its own; the row shows its first part's. Persisted (`vo`, also per part). */
        QString viewOwner;
        /* Issue #903: when the fragment was last taken in as new, by this process (heardSerial()); 0 for a reload. Each
         * part keeps its own, and an update of a fragment keeps the serial it had; the row's is its parts' newest. Not
         * persisted. */
        quint64 heardSerial = 0;
        /* Issue #903: the identity of the list of the -Y scan-list row the fragment was heard on (scanListRow). */
        quint64 scanListRev = 0;
    };

    /**
     * @brief The session ordinals the model has given decoder runs (issue #800, 2.7): ring identity
     * (Event_History_I::instance) to ordinal, the next ordinal to give, and the first the holder may not give.
     *
     * An ordinal is given the first time a ring is read and never again: persisted with the model's settings and with
     * each fragment's position, kept across reloads and retried spill reads. Runs are sequential in one app, so the
     * ordinals follow the runs' real order whatever order their rows are read in. The background tap gives ordinals
     * from a block the model set aside for it (startBackgroundCapture()), so a ring it meets first takes one that no
     * later run's can come before, even if the process dies before the spill is folded.
     */
    struct SessionOrdinals {
        QHash<quint64, qint64> rings;
        qint64 next = 1;
        qint64 limit = std::numeric_limits<qint64>::max();
    };

    /** @brief The ordinal of ring @p ring in @p ordinals, given now when it has none; 0 for ring 0 (no identity), or
     * when the holder's block is used up. */
    static qint64 sessionOrdinalFor(SessionOrdinals& ordinals, quint64 ring);

  private:
    friend class CallHistoryTap;

    /**
     * @brief What was last read from a ring row, keyed by its content key.
     *
     * The core merges reacquired segments into a committed row in place — the
     * end stamp extends, src fills 0 -> real — without changing the row's key.
     * Comparing against these values is what lets those merges re-ingest as
     * updates instead of being skipped forever. Persisted alongside the rows:
     * after an Activity restart the service's ring still holds every fragment
     * this process's predecessor absorbed, and forgetting their keys would
     * re-ingest each one as a duplicate conversation.
     */
    struct SeenState {
        qint64 when = 0;
        qint64 end = 0;
        qulonglong src = 0;
        bool emergency = false;
        bool enc = false;
        QString sourceName;
        /* The frequency and access code last read (0 = unknown). The core only fills them, so
         * unknown -> known is an advance and any other change is ignored
         * (call_history_seen_absorb_fill()). */
        qint64 freqHz = 0;
        int acKind = 0;
        int ac = 0;
        /* The receive evidence and service name last read (issue #637). The core only adds to them in place (a
         * reacquired segment that confirmed the code, proved the slot or named the service), so a gain is an advance. */
        bool receiveKnown = false;
        int receiveSlot = 0;
        QString service;
        /* The site parts (dsd_received_identity::valid) and the call channel last read (issue #799). The core adds
         * site parts in place and fills the channel, so a part gained, or any change of the channel or its kind, is
         * an advance (call_history_seen_absorb_site()). */
        uint32_t siteMask = 0;
        uint32_t callChannel = 0;
        uint8_t callChannelKind = 0;
        /* The digest of the site text last read (call_history_site_text_digest(), 0 for none). The core can change the
         * text alone in place (a reacquisition merge's union preferring the text that names Capacity Max), so any
         * change is an advance. Not known in a seen entry an older build stored: the first read then takes the digest
         * without counting as an advance, and the seen store is saved with it (SeenSettled). */
        uint32_t siteText = 0;
        bool siteTextKnown = false;
        /* Issue #800, B2: digests of the GPS text, the text message (call_history_text_digest()) and the position
         * (call_history_position_digest()) last read, each 0 for none. The core writes them into a committed voice row
         * in place (a late enrichment), so any change is an advance. An entry an older build stored keeps none and
         * reads 0, so a row whose GPS or message that build left out is read again once. */
        uint32_t gpsText = 0;
        uint32_t messageText = 0;
        uint32_t position = 0;
        /* The session the ring row was last read in. The map keeps the newest sessions'
         * entries, not the newest stamps: a replay's rows are older than live ones. */
        qint64 session = 0;
        /* The ring it was read from (Event_History_I::instance); 0 in stores written before
         * rings had one. The key alone cannot tell a row read again from a new ring's row that
         * repeats its push stamp and content, as a capture replayed in a fresh state does. */
        quint64 ring = 0;
        /* For a notice heard again: the logged notice (by keyFor()) it took as its twin, else empty.
         * Persisted, so a relaunched model knows the twins its ring has taken (m_noticeTwinsTaken). */
        QString twin;
        /* Issue #800: the digest of the raw identity last read (call_history_identity_digest()). The core can resolve
         * a committed voice row's reception or sender system in place, and the fragment must then be read again (its
         * texts and position are checked against the row again, foldRow()), so any change is an advance. Not
         * known in an entry an older build stored or one load() seeded: the first read then takes the digest without
         * counting as an advance, as the site text digest's (SeenSettled). Last, so load()'s seed leaves it unknown. */
        uint32_t identity = 0;
        bool identityKnown = false;
        /* Issue #903: the verified talkgroup proof and live-input latch last read. The core only adds a proof (a late
         * checked message) and only closes the latch (a switch to file input), so either is an advance
         * (absorbSeenProof()). An entry an older build stored, or one load() seeded, reads none and not live. */
        quint32 heardId = 0;
        uint8_t heardVariant = 0;
        bool liveInput = false;
    };

    /**
     * @brief What Clear wiped: ring @c ring's rows up to @c pushSeq per slot.
     *
     * Named by push position rather than by stamps, because a ring's pushes are ordered and its
     * stamps are not (a replay's are the capture's). @c pending until the ring position is known.
     * The position means something only in its own ring (Event_History_I::instance; initState()
     * draws one for both slots), so @c ring names it, and the ring is the binding: a start that
     * reuses the ring keeps the mark. 0 names no ring: pending, a model that had read nothing when
     * a start came (at position zero, which covers nothing), or a mark an older build wrote.
     * @c session is the session the clear was made in, kept for reference only.
     */
    struct ClearMark {
        bool active = false;
        qint64 session = 0;
        bool pending = false;
        qulonglong pushSeq[2] = {0U, 0U};
        quint64 ring = 0U;
    };

    /**
     * @brief A ring row worth ingesting: brand new, heard again in a new ring, or a seen row that advanced.
     *
     * The seen key and entry as the walk left them travel with it, so the background tap can spill both.
     */
    struct FreshRow {
        Row row;
        bool isUpdate = false;
        bool again = false;
        QString key;
        SeenState seen;
        /* Issue #903: the saved system the reading's verified talkgroup is handed to (setHeardSink()), resolved where
         * the ring was read; empty when the reading is not the first to carry the fragment's proof or may not be
         * learnt. A spill line carries it (`ho`). */
        QString heardOwner;
        /* Issue #903: not a row to ingest: a proof that arrived after a clear on a row the clear covers, which the
         * clear keeps out of the log but which is still a talkgroup heard (noteClearedProof()). Its row holds only the
         * proof and the fragment's span. A spill line of its own (clearedProofLine()). */
        bool clearedProof = false;
    };

    using SeenMap = QHash<QString, SeenState>;

    /** @brief What a ring walk reads besides the ring: the model's, or the background tap's copy of it. */
    struct RingWalk {
        SeenMap* seen = nullptr;
        qint64 session = 0;
        QString sessionLabel;
        QString systemUid;                     // the attribution uid, empty unless the session is single-system
        qint64 sessionOrd = 0;                 // the walked ring's session ordinal (SessionOrdinals), 0 when none
        bool cleared = false;                  // the clear mark covers this ring...
        qulonglong clearedThrough = 0U;        // ...up to this push stamp of the walked slot
        bool* seenSettled = nullptr;           // set when a seen entry changed with nothing to ingest (SeenSettled)
        QList<QString>* settledKeys = nullptr; // the keys of those entries, for the background tap to spill
        HeardOwners* heardOwners = nullptr;    // issue #903: the owners, and the rings bound to their sessions
        bool optsPublished = false;            // options were published: a row's owner can be established
    };

    /** @brief noteSeen() verdicts. */
    enum SeenVerdict { SeenUnchanged = 0, SeenNew = 1, SeenAdvanced = 2, SeenAgain = 3, SeenSettled = 4 };

    static QString keyFor(const Row& row);

    /**
     * @brief Record what was just read from a ring row of ring @p ring in @p seenMap and say what to do with it.
     * @param session The session reading it.
     * @param read The row's content as just read (when, end, src, emergency, enc, sourceName, freqHz,
     *             acKind, ac); its session, ring and twin are ignored.
     * @return SeenNew for a first sighting, SeenAgain when the key was read from another ring (a
     *         call heard again: the same capture decoded in a new ring), SeenAdvanced when a voice
     *         row already ingested has since learned something, SeenSettled when nothing advanced but an
     *         entry an older build stored took the site text digest it lacked (the seen store is owed a
     *         save, or every launch would take it again), SeenUnchanged otherwise.
     */
    static int noteSeen(SeenMap& seenMap, qint64 session, const QString& key, quint64 ring, const SeenState& read,
                        bool voice);
    static bool absorbSeenReceive(SeenState& seen, const SeenState& read);
    static bool absorbSeenSite(SeenState& seen, const SeenState& read);
    static bool absorbSeenTexts(SeenState& seen, const SeenState& read);
    static bool absorbSeenCall(SeenState& seen, const SeenState& read);
    static bool absorbSeenProof(SeenState& seen, const SeenState& read);
    /** @brief What a ring walk reads of @p item for its seen entry (noteSeen()). */
    static SeenState seenReadOf(const Event_History* item);
    /** @brief Whether the seen entry @p key of ring @p ring already holds a proof (an earlier reading carried one). */
    static bool seenProven(const SeenMap& seen, const QString& key, quint64 ring);
    /**
     * @brief For a row of ring @p instance (@p slot, push stamp @p seq) a clear covers: when it now carries a learnable
     * proof its seen entry, of this ring, lacked (the checked message came after the clear), record the proof in that
     * entry and, when its owner resolves, append a FreshRow::clearedProof to @p fresh. A row the seen map does not know
     * (its entry lost, or rebuilt without the cleared rows) gives nothing: fail closed.
     */
    static void noteClearedProof(const Event_History* item, int slot, qulonglong seq, quint64 instance,
                                 const RingWalk& walk, qint64 session, QList<FreshRow>& fresh);
    /** @brief The owner a fresh reading @p item of ring session @p session is handed to, or empty (FreshRow). */
    static QString freshHeardOwner(const RingWalk& walk, qint64 session, const Event_History* item, bool provenBefore);

    /**
     * @brief Walk one slot's ring for rows not seen before, or seen but advanced, appending them to @p fresh.
     *
     * Reads nothing but its arguments, so the background tap runs the same rules on its own copies.
     */
    static void walkRing(const Event_History_I& ring, int slot, const RingWalk& walk, QList<FreshRow>& fresh);

    /** @brief Scan the flagged slots' rings for rows not seen before, or seen but advanced, binding the rings they
     * come from to their sessions (issue #903; saved when that changed). @p optsPublished: the snapshot's options were
     * published, so a row's identity and owner can be established. */
    QList<FreshRow> collectFresh(const dsd_state* snapshot, const bool scan[2], bool optsPublished, bool* seenSettled);

    /**
     * @brief Absorb @p row into a same-target row among the newest @p scanRows when the two
     *        overlap within the merge window; the merged row spans both fragments.
     *
     * An analog row never merges. With @p sameReception (a row heard again, or an update), an analog row is still
     * absorbed by the row that logged the same reception: an analog row with its start, tone and frequency.
     * @return Index of the row it merged into, or -1 when it is a new call.
     */
    int tryMerge(const Row& row, int scanRows, bool sameReception);

    /** @brief Fold fragment @p row into @p existing, the row it belongs to: the span, labels, carrier, site and
     * fragments. A site that conflicts with the row's is not taken, nor is its channel (call_history_site_fold()). */
    void foldRow(Row& existing, const Row& row);

    /** @brief The voice row holding one of @p row's fragments (m_fragmentIndex), or -1. */
    int rowHoldingFragment(const Row& row) const;

    /** @brief After a fold into row @p merged: name its channel, announce the change, keep the newest-first order. */
    void settleMerged(int merged);

    /** @brief The logged notice @p row repeats field for field, not yet taken by another notice of the ring, or -1. */
    int findRepeatedNotice(const Row& row) const;

    /**
     * @brief Take a notice heard again as the logged delivery it repeats, if there is one.
     *
     * The logged notice joins the running session and fills the frequency and access code it
     * lacked from @p row; a known value is kept. @return true when @p row repeated a logged notice.
     */
    bool absorbRepeatedNotice(const Row& row);

    /**
     * @brief Merge @p row into the log or insert it at its sorted position.
     *
     * An update (a seen ring row that advanced) may only refine an existing row: the one
     * holding its fragment (m_fragmentIndex), else one it merges with anywhere in the log; if
     * its row cannot be found it is dropped, never inserted as a duplicate. A row heard
     * @p again merges into the call it repeats wherever that call sits in the log, and that
     * call joins the running session. A first sighting merges only within the newest rows.
     *
     * @return true when a new row was inserted (the count changed).
     */
    bool ingestRow(const Row& row, bool isUpdate, bool again);

    /**
     * @brief ingestRow() for a ring reading, handing its verified talkgroup to the sink first when @p heardOwner names
     * an owner and the log does not already hold the fragment's proof (setHeardSink()).
     */
    bool ingestReading(const Row& row, bool isUpdate, bool again, const QString& heardOwner);
    /** @brief Whether the log's row holding @p row's one fragment already has that fragment's proof. */
    bool fragmentProven(const Row& row) const;
    /** @brief Whether a Forget or Clear of @p owner's number came after the fragment @p row reads (heardForgotten()). */
    bool heardForgottenSince(const Row& row, bool isUpdate, const QString& owner) const;
    /** @brief The ingest serial the log's row holding @p row's fragment keeps for it (Row::heardSerial), else 0. */
    quint64 heldSerial(const Row& row) const;
    /** @brief Hand a sighting to the sink, or hold it until one is set. */
    void noteHeard(const QString& owner, int variant, quint32 id, qint64 whenSecs);
    void saveHeardOwners();

    void load();
    /** @brief The log changed: a new generation is owed to the disk. */
    void markChanged();
    void scheduleSave();
    /** @brief Serialize and write both stores on the calling thread; returns whether both committed. */
    bool saveSync();
    /**
     * @brief Hand the current stores to the save worker.
     *
     * The GUI thread copies the rows and the seen map and detaches both copies eagerly, so the
     * worker owns its containers outright and never shares a block with later GUI edits. Building
     * the JSON — tens of milliseconds at a full log (issue #718) — and the two file writes run on
     * m_savePool, which reads nothing but those copies. One save in flight at a time; a request
     * that lands mid-write marks the stores dirty, and the save re-arms the debounce timer when
     * it finishes. saveNow() is the one caller that queues behind a save in flight.
     */
    void startAsyncSave();
    /** @brief Snapshot the stores and queue their save on m_savePool, behind any save in flight. */
    void queueSave();

    /** @brief What one save did: the bytes both stores took, the generation it held, and whether each committed. */
    struct SaveResult {
        qint64 bytes = 0;
        quint64 generation = 0;
        bool rowsCommitted = false;
        bool seenCommitted = false;
    };

    /** @brief A save finished: acknowledge or retry it, set the next delay, then start or arm what is owed. */
    void onSaveFinished(const SaveResult& result);
    /** @brief Generation @p generation reached the disk whole: drop the spill lines it covers. */
    void acknowledgeSave(quint64 generation);
    void setSaveFailing(bool failing);
    /** @brief The rows store; reads only its argument, so the save worker may call it. */
    static QJsonArray rowsToJson(const QList<Row>& rows);
    /** @brief The seen store, newest first and capped; reads only its argument, so the save worker may call it. */
    static QJsonArray seenToJson(const QHash<QString, SeenState>& seen);
    static QJsonObject seenEntryToJson(const SeenState& state);
    static SeenState seenEntryFromJson(const QJsonObject& obj);
    /** @brief One spill line (a JSON object and its newline) for a row the background tap found. */
    static QByteArray spillLine(const FreshRow& fresh);
    /**
     * @brief One spill line for a seen entry the background tap settled (SeenSettled): its key and entry, no row.
     *
     * The tap's seen map is a copy that its stop discards, so this is how the site text digest an older build's entry
     * took reaches the seen store. A build that predates it skips the line (no row).
     */
    static QByteArray settledSeenLine(const QString& key, const SeenState& seen);
    /** @brief One spill line for a FreshRow::clearedProof: its key, seen entry (with the proof), owner (`ho`) and time
     * (`when`), and `cleared`; no row. */
    static QByteArray clearedProofLine(const FreshRow& fresh);
    /** @brief Bound @p seen once it is well past what the ring could resurrect. */
    static void pruneSeenMap(SeenMap& seen);
    /** @brief Issue #903: whether @p snapshot may be read between a start's accept and its session (refresh() without
     * bindNewRings): not a ring no session owns. On a ring a session owns, what it pushed since the last read is noted
     * as no one's (heard_owners_note_unowned()). */
    bool readBeforeSession(const dsd_state* snapshot);

    /** @brief Give a row about to be logged its uid, and index its fragments. */
    void adoptNewRow(Row& row);
    /** @brief Give @p row its channel's display name, if it has none yet and one is known. Returns true when named. */
    bool nameChannel(Row& row) const;
    /** @brief The display name of @p row's channel: the newest given, else the one it was logged with, else the id. */
    QString channelDisplayName(const Row& row) const;
    /** @brief Add @p fragments to @p existing, and index them to it. */
    void unionFragments(Row& existing, const QStringList& fragments);
    void rebuildFragmentIndex();
    /** @brief Emit the recording roles' change for the rows that hold @p fragments. */
    void notifyRecordingRows(const QStringList& fragments);

    void onApplicationStateChanged(Qt::ApplicationState state);
    /** @brief Start the background tap on what the model knows now (the app is leaving the screen). */
    void startBackgroundCapture();
    /** @brief Stop the background tap, take the clear it settled and the ring position it read, and fold in its spill. */
    void handOffBackgroundCapture();
    /** @brief Hand off the running tap before the session it attributes rows to changes; @return whether it should
     * start again on the new one (the app still away), for resumeBackgroundCapture(). */
    bool handOffBackgroundCaptureForSession();
    /** @brief Start the tap again on the changed session when handOffBackgroundCaptureForSession() said so. */
    void resumeBackgroundCapture(bool resume);
    /** @brief Stop the running background tap and take the clear it settled and the ring position it read; @return the
     * lines it could not write to the spill. */
    QByteArray stopBackgroundCapture();
    /** @brief Take the ring position the tap read last as this model's last read, if the tap read any. */
    void takeTapReadPosition(bool read, const qulonglong pushSeq[2], quint64 ring);
    /** @brief Take the session ordinals @p ordinals gave rings this model has none for (the tap's, a spill line's). */
    void adoptSessionOrdinals(const QHash<quint64, qint64>& ordinals);
    /** @brief adoptSessionOrdinals() for one ring (a spill line's): ring 0 or an ordinal below 1 is none. */
    void adoptSessionOrdinal(quint64 ring, qint64 ordinal);
    /** @brief Record ordinal @p ordinal for ring @p ring if it has none; @return whether it was recorded. */
    bool noteSessionOrdinal(quint64 ring, qint64 ordinal);
    /** @brief The session ordinal of ring @p ring, given and saved now when it has none (SessionOrdinals). */
    qint64 sessionOrdinal(quint64 ring);
    void saveSessionOrdinals();
    void loadSessionOrdinals();
    /** @brief Fold the spill into the log, then @p unwritten (the lines the tap could not write to it), idempotently by
     * fragment; @return whether the row count changed. */
    bool foldSpill(const QByteArray& unwritten = QByteArray());
    /** @brief Fold @p unwritten, the lines the tap could not write to the spill, into the log; @p folded is set when any
     * line folded. @return whether rows changed. */
    bool foldUnwritten(const QByteArray& unwritten, bool* folded);
    /** @brief Fold the spill lines @p reader reads into the log; @p folded is set when any line folded. @return whether
     * rows changed. */
    bool foldSpillLines(CallHistorySpillReader& reader, bool* folded);
    /** @brief Fold one spill @p line, unless the clear mark covers it (@p cleared: the mark applies); @p twinsRing is
     * the ring whose twins taken are restored, and @p folded is set when the line folded. @return whether rows
     * changed. */
    bool foldSpillLine(const QByteArray& line, bool cleared, quint64* twinsRing, bool* folded);
    /** @brief For a settled spill @p line (settledSeenLine()), give the seen entry @p key the site text digest it
     * carries, if the entry has none; any other line is passed over. */
    void foldSettledSeen(const QString& key, const QJsonObject& line, bool* folded);
    /** @brief For a clearedProofLine() @p line: when the seen entry @p key still has no proof, give it the line's and
     * hand the talkgroup to the sink (issue #903); once, so a refold adds nothing. */
    void foldClearedProof(const QString& key, const QJsonObject& line, bool* folded);
    /** @brief Whether the clear mark (@p cleared: it applies) covers the spilled @p row its tap found as @p seen. */
    bool spilledRowCleared(bool cleared, const Row& row, const SeenState& seen) const;
    /** @brief pruneSeen() within a fold, and forget the twins taken that only the entries it dropped recorded. */
    void pruneSeenWhileFolding();
    /** @brief Record each slot's commit_rev and ring; @p scan marks the slots a ring walk could find something new in.
        Returns whether either slot moved. */
    bool noteCommitRevs(const dsd_state* snapshot, bool scan[2]);
    /** @brief Rebuild m_noticeTwinsTaken for rings @p a and @p b, from the twins their seen entries record. */
    void restoreNoticeTwinsTaken(quint64 a, quint64 b);
    /** @brief Trim m_rows to kMaxRows in one pass, oldest session's oldest row first; returns whether any row went. */
    bool trimToCapacity();

    /** @brief Bound m_seen once it is well past what the ring could resurrect. */
    void
    pruneSeen() {
        pruneSeenMap(m_seen);
    }

    /** @brief Move the session's label and uid to @p label and @p uid, and begin a new session when @p begin, with
     * @p owners as its owners when given (issue #903), under one handoff of the background tap. */
    void changeSession(const QString& label, const QString& uid, bool begin,
                       const HeardSessionOwners* owners = nullptr);
    /** @brief beginSession()'s step: the next session number, and the clear and read position a start keeps. */
    void advanceSession();
    /** @brief Whether the clear mark covers rows of the ring being read (settleClear() has bound or dropped it). */
    bool clearApplies() const;
    /** @brief Settle the clear mark against ring @p ring read at @p pushSeq: bind a pending one, drop one made on
        another ring. */
    void settleClear(const qulonglong pushSeq[2], quint64 ring);
    void saveClear();
    /** @brief Restore the session and the clear mark; retire the timestamp watermark older builds wrote. */
    void loadSessionState();

    QList<Row> m_rows; // newest first
    QHash<QString, SeenState> m_seen;
    QString m_sessionLabel;
    QString m_sessionUid;
    QHash<QString, QString> m_channelNames;
    qint64 m_session = 0;
    /* Persisted so the still-populated ring cannot resurrect cleared rows after an
     * Activity restart. */
    ClearMark m_clear;
    /* The ring position (push_seq per slot) the last refresh read; valid once m_ringRead. */
    qulonglong m_ringPushSeq[2] = {0U, 0U};
    /* The ring that position is in (Event_History_I::instance); 0 when a start came before any read. */
    quint64 m_ringPushSeqRing = 0U;
    bool m_ringRead = false;
    /* Committed-rows change counter per slot (Event_History_I::commit_rev). The
     * staged row re-renders at the poll rate while a call is up, bumping only
     * `revision`; gating the 2x254-row rescan on this instead keeps the idle and
     * live-call ticks free of ring walks that cannot find anything. */
    quint64 m_commitRev[2] = {0U, 0U};
    /* The ring (Event_History_I::instance) each slot was last read from. A new ring walks even at the
     * commit_rev the last one stopped at: a capture replayed in a fresh state reaches the same count. */
    quint64 m_ringInstance[2] = {0U, 0U};
    /* The logged notices (by keyFor()) that a notice heard again from the ring being read has taken as its
     * twin. Two identical notices, the same delivery logged twice, then promote two rows, even read in two
     * ticks or across a relaunch. A notice whose twin no notice of this ring has taken dedups onto it,
     * wherever its session: a new ring within the session repeats the session's own notices. Rebuilt when
     * the ring changes, a relaunched model's first read included, from the twins the seen entries of that
     * ring record (SeenState::twin). */
    QSet<QString> m_noticeTwinsTaken;
    bool m_seeded = false;
    QTimer m_saveTimer;
    QThreadPool m_savePool;
    /* Saves queued on m_savePool and not yet finished (onSaveFinished()); saveNow() can queue a
     * second behind the one in flight, and the pool's one thread runs them in order. */
    int m_savesInFlight = 0;
    bool m_saveDirty = false;
    /* Issue #708, B6. Every change to the log is a generation (markChanged()); a save holds the one it copied, and a
     * generation counts as on the disk (m_ackedGeneration) only when both stores committed. A failed save keeps the
     * log owed and retries on a growing delay without waiting for another change; the failure is reported once per
     * run of failures (saveFailing). */
    quint64 m_generation = 0;
    quint64 m_ackedGeneration = 0;
    int m_saveFailures = 0;
    bool m_saveFailing = false;
    /* The next row uid; persisted (callHistory/nextUid) so a uid is never handed out twice. */
    qint64 m_nextUid = 1;
    /* fragment key -> the uid of the row holding it. Rebuilt at load, kept by every ingest, fold, trim and clear. */
    QHash<QString, qint64> m_fragmentIndex;
    const CallRecordingProvider* m_recordings = nullptr;
    /* Issue #708, B1: the background capture. The spill is the tap's output and the model's input; the model folds it
     * at load and on the first ingest after the app returns, and drops the lines a save covers only once that save is
     * acknowledged: m_spillFoldedBytes is how much of it the last fold read (-1: nothing waits), m_spillFoldGeneration
     * the generation that fold made. */
    std::unique_ptr<CallHistorySpill> m_spill;
    std::unique_ptr<CallHistoryTap> m_tap;
    qint64 m_spillFoldedBytes = -1;
    quint64 m_spillFoldGeneration = 0;
    bool m_handoffOwed = false;
    /* Issue #800, 2.7: the session ordinal of every ring read (SessionOrdinals). Persisted
       (callHistory/sessionOrd/next, /rings). */
    SessionOrdinals m_sessionOrdinals;
    /* Issue #903: who owns each recent session's calls, the rings bound to them (persisted, callHistory/heardOwners),
       the sink the verified talkgroups go to, and the sightings waiting for it. */
    HeardOwners m_heardOwners;
    /* Issue #903: the newest ingest serial given (heardSerial()). */
    quint64 m_heardSerial = 0;
    /* Issue #903: per owner, the ingest serial a Forget of each number (HeardTalkgroupStore::policyId()), and a Clear,
       came at (heardForgotten()). */
    QHash<QString, QHash<quint32, quint64>> m_heardForgotten;
    QHash<QString, quint64> m_heardClearedThrough;
    /* holdCaptureForStart(): the hold, a count that retires its safety release, and whether the app is away (the last
       application state was Suspended or Hidden), which says whether the tap starts again when the hold ends. */
    bool m_captureHeld = false;
    quint64 m_captureHoldRev = 0;
    bool m_appAway = false;
    HeardTalkgroupSink* m_heardSink = nullptr;
    HeardPending m_heardPending;
    /* Bumped by every clearAll(): a tap started before a clear settled no mark the model may adopt. */
    quint64 m_clearRev = 0;
    quint64 m_tapClearRev = 0;
    QSettings m_settings;
#ifdef DSD_NEO_TEST_HOOKS
    int m_peakRowsForTest = 0;
    qint64 m_trimRowsScannedForTest = 0;
    qsizetype m_peakFoldSeenForTest = 0;
    qsizetype m_peakFoldTwinsForTest = 0;
    qsizetype m_peakSpillReadBytesForTest = 0;
#endif
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_ */
