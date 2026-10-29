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
 *
 * Issue #708: each row has a stable uid, and the ring fragments it absorbed, which is how its
 * call recordings find it (call_recording_provider.h). While the app is in the background, where
 * Qt pauses this model's thread, CallHistoryTap keeps reading the ring and spills what it finds;
 * the model folds that in when it reads again. A save counts only when both stores committed.
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
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <memory>
#include <stdint.h>

#include "heard_talkgroup_source.h"

#ifdef DSD_NEO_TEST_HOOKS
class QThread;
#endif

namespace dsd_qt {

class CallHistorySpill;
class CallHistorySpillReader;
class CallHistoryTap;
class CallRecordingProvider;

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
        DayLabelRole,   // "TODAY" / "YESTERDAY" / "MON 3 AUG" — drives list sections
        TimeTextRole,   // "12:04"
        KindRole,       // RowKind: voice call or data/control notice
        DetailRole,     // notice payload: decoded text message or GPS string
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
        HasRecordingRole    // whether the row has any
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

    /** @brief The session's system, label and uid together, the session itself kept (beginSessionOn()). */
    Q_INVOKABLE void setSessionSystem(const QString& label, const QString& uid);

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
     * @brief Whether the app came back from the background with the tap still running.
     *
     * The tap stops at the first ingest after that (refresh()), which also folds in what it spilled. UiController
     * calls flushHistory() for it when no redraw would.
     */
    bool
    backgroundCaptureHandoffOwed() const {
        return m_handoffOwed;
    }

    /** @brief HeardTalkgroupSource: read straight from the rows, no QVariant per row (issue #718). */
    QList<uint32_t> heardTalkgroups(qint64 forSession) const override;

    /**
     * @brief Ingest newly committed voice rows from the snapshot ring.
     *
     * Call from the UI poll tick only. Duplicate protection keys on the row's
     * stable ring identity — slot plus the push stamp the row was committed at —
     * not its index, which shifts on every push. Updates are granular
     * (insert/change/remove), never a model reset — a reset would destroy every
     * delegate and the reader's scroll position per ingest.
     *
     * Options must accompany the snapshot to attribute hold-eligible rows.
     * Scanning enabled through extra arguments also excludes that attribution.
     */
    void refresh(const dsd_state* snapshot, const dsd_opts* opts_snapshot = nullptr);

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
        int durationSecs = -1;
        QString systemName;
        QString systemUid;
        int kind = KindVoice;
        QString detail;
        /* The scan channel the row was heard on: a -Y row name or a trunk-scan
         * target id. Empty when the receiver was not scanning. It is not the
         * system: the system is the saved entry the session runs, and stays so
         * for every row of that session. */
        QString channel;
        QString service;
        /* A conventional analog (NFM) transmission (issue #637): its name is only a label (the channel, "NFM" or a
           tone's service name), never its destination. Persisted; older rows read false. */
        bool analog = false;
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
        /* Issue #708. The row's identity for views that follow it, persisted (`uid`); assigned when the row is first
         * logged, never reused, and kept by the row a merge folds another into. 0 until assigned. */
        qint64 uid = 0;
        /* Every ring fragment the row absorbed, as call_history_fragment_key()s: its own, and each one a merge folded
         * in (load folding too). Persisted (`fr`); rows older builds wrote have none, and no recordings either. */
        QStringList fragments;
    };

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
    };

    using SeenMap = QHash<QString, SeenState>;

    /** @brief What a ring walk reads besides the ring: the model's, or the background tap's copy of it. */
    struct RingWalk {
        SeenMap* seen = nullptr;
        qint64 session = 0;
        QString sessionLabel;
        QString systemUid;              // the attribution uid, empty unless the session is single-system
        bool cleared = false;           // the clear mark covers this ring...
        qulonglong clearedThrough = 0U; // ...up to this push stamp of the walked slot
    };

    /** @brief noteSeen() verdicts. */
    enum SeenVerdict { SeenUnchanged = 0, SeenNew = 1, SeenAdvanced = 2, SeenAgain = 3 };

    static QString keyFor(const Row& row);

    /**
     * @brief Record what was just read from a ring row of ring @p ring in @p seenMap and say what to do with it.
     * @param session The session reading it.
     * @param read The row's content as just read (when, end, src, emergency, enc, sourceName, freqHz,
     *             acKind, ac); its session, ring and twin are ignored.
     * @return SeenNew for a first sighting, SeenAgain when the key was read from another ring (a
     *         call heard again: the same capture decoded in a new ring), SeenAdvanced when a voice
     *         row already ingested has since learned something, SeenUnchanged otherwise.
     */
    static int noteSeen(SeenMap& seenMap, qint64 session, const QString& key, quint64 ring, const SeenState& read,
                        bool voice);
    static bool absorbSeenReceive(SeenState& seen, const SeenState& read);

    /**
     * @brief Walk one slot's ring for rows not seen before, or seen but advanced, appending them to @p fresh.
     *
     * Reads nothing but its arguments, so the background tap runs the same rules on its own copies.
     */
    static void walkRing(const Event_History_I& ring, int slot, const RingWalk& walk, QList<FreshRow>& fresh);

    /** @brief Scan the flagged slots' rings for rows not seen before, or seen but advanced. */
    QList<FreshRow> collectFresh(const dsd_state* snapshot, const bool scan[2], const QString& systemUid);

    /**
     * @brief Absorb @p row into a same-target row among the newest @p scanRows when the two
     *        overlap within the merge window; the merged row spans both fragments.
     * @return Index of the row it merged into, or -1 when it is a new call.
     */
    int tryMerge(const Row& row, int scanRows);

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
     * An update (a seen ring row that advanced) may only refine an existing row, which it
     * looks for through the whole log; if its row cannot be found it is dropped, never
     * inserted as a duplicate. A row heard @p again merges into the call it repeats wherever
     * that call sits in the log, and that call joins the running session. A first sighting
     * merges only within the newest rows.
     *
     * @return true when a new row was inserted (the count changed).
     */
    bool ingestRow(const Row& row, bool isUpdate, bool again);

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
    /** @brief Bound @p seen once it is well past what the ring could resurrect. */
    static void pruneSeenMap(SeenMap& seen);

    /** @brief Give a row about to be logged its uid, and index its fragments. */
    void adoptNewRow(Row& row);
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

    /** @brief Arm m_dayTimer for the next local midnight. */
    void scheduleDayRollover();
    /** @brief Move the session's label and uid to @p label and @p uid, and begin a new session when @p begin, under one
     * handoff of the background tap. */
    void changeSession(const QString& label, const QString& uid, bool begin);
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
    /* Bumped by every clearAll(): a tap started before a clear settled no mark the model may adopt. */
    quint64 m_clearRev = 0;
    quint64 m_tapClearRev = 0;
    /* Fires at local midnight: "TODAY"/"YESTERDAY" section labels are derived
     * from the current date, and nothing else re-reads them when it rolls over. */
    QTimer m_dayTimer;
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
