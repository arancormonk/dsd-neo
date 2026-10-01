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
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

namespace dsd_qt {

class CallHistoryModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString sessionLabel READ sessionLabel WRITE setSessionLabel NOTIFY sessionLabelChanged)
    Q_PROPERTY(QString sessionUid READ sessionUid WRITE setSessionUid NOTIFY sessionUidChanged)
    Q_PROPERTY(qlonglong session READ session NOTIFY sessionChanged)
    Q_PROPERTY(QStringList systemLabels READ systemLabels NOTIFY countChanged)

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
        SessionRole     // decode session that logged the row, or last extended it (see session())
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
     * @brief Begin a new decode session, whose ring counts its pushes from zero.
     *
     * The UI calls this on every start. A replay start or an input change is a start,
     * and so is the first start after the process starts. Ingest the previous session's
     * tail first (UiController::flushHistory()), or it would be logged under the new session.
     * A clear made in the previous session stops applying: its push positions describe a
     * ring that is gone. The new ring's position is zero until a read moves it.
     */
    Q_INVOKABLE void beginSession();

    /** @brief Distinct system names present in the log, for the filter pill. */
    QStringList systemLabels() const;

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
     * Persisted, so a relaunched UI does not ingest the cleared rows again. A start counts as reading
     * the new ring at position zero, so a clear after it covers none of that ring. Only a freshly
     * constructed model (a relaunched UI clearing before its first tick) has not read the ring; its
     * clear covers what the first read finds.
     */
    Q_INVOKABLE void clearAll();

  Q_SIGNALS:
    void countChanged();
    void sessionLabelChanged();
    void sessionUidChanged();
    void sessionChanged();

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
    };

  private:
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
     * @brief What Clear wiped: session @c session's rows of ring @c ring up to @c pushSeq per slot.
     *
     * Named by push position rather than by stamps, because a ring's pushes are ordered and its
     * stamps are not (a replay's are the capture's). @c pending until the ring position is known.
     * The position means something only in its own ring (Event_History_I::instance; initState()
     * draws one for both slots), so @c ring names it. 0 names no ring: pending, a start's new ring
     * not yet read (at position zero, which covers nothing), or a mark an older build wrote.
     */
    struct ClearMark {
        bool active = false;
        qint64 session = 0;
        bool pending = false;
        qulonglong pushSeq[2] = {0U, 0U};
        quint64 ring = 0U;
    };

    /** @brief A ring row worth ingesting: brand new, heard again in a new ring, or a seen row that advanced. */
    struct FreshRow {
        Row row;
        bool isUpdate = false;
        bool again = false;
    };

    /** @brief noteSeen() verdicts. */
    enum SeenVerdict { SeenUnchanged = 0, SeenNew = 1, SeenAdvanced = 2, SeenAgain = 3 };

    static QString keyFor(const Row& row);

    /**
     * @brief Record what was just read from a ring row of ring @p ring and say what to do with it.
     * @return SeenNew for a first sighting, SeenAgain when the key was read from another ring (a
     *         call heard again: the same capture decoded in a new ring), SeenAdvanced when a voice
     *         row already ingested has since learned something, SeenUnchanged otherwise.
     */
    int noteSeen(const QString& key, quint64 ring, qint64 when, qint64 end, qulonglong src, bool emergency, bool enc,
                 bool voice, const QString& sourceName);

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
     * @brief Merge @p row into the log or insert it at its sorted position.
     *
     * An update (a seen ring row that advanced) may only refine an existing row;
     * if its row cannot be found it is dropped, never inserted as a duplicate.
     * A row heard @p again merges into the call it repeats wherever that call sits
     * in the log, and that call joins the running session.
     *
     * @return true when a new row was inserted (the count changed).
     */
    bool ingestRow(const Row& row, bool isUpdate, bool again);

    void load();
    void scheduleSave();
    /** @brief Serialize and write both stores on the calling thread. */
    void saveNow() const;
    /**
     * @brief Hand the current stores to the save worker.
     *
     * The arrays are built here on the GUI thread (cheap); the serialization and
     * the two file writes — the part that can stall a frame for tens of
     * milliseconds on phone flash — run on m_savePool. One save in flight at a
     * time; a request that lands mid-write re-arms the debounce timer instead.
     */
    void startAsyncSave();
    void onSaveFinished();
    QJsonArray rowsToJson() const;
    QJsonArray seenToJson() const;
    /** @brief Record each slot's commit_rev and ring; @p scan marks the slots a ring walk could find something new in.
        Returns whether either slot moved. */
    bool noteCommitRevs(const dsd_state* snapshot, bool scan[2]);
    /** @brief Rebuild m_noticeTwinsTaken for the rings now read, from the twins their seen entries record. */
    void restoreNoticeTwinsTaken();
    /** @brief Trim m_rows to kMaxRows, oldest session's oldest row first; returns whether any row went. */
    bool trimToCapacity();
    /** @brief Bound m_seen once it is well past what the ring could resurrect. */
    void pruneSeen();
    /** @brief Arm m_dayTimer for the next local midnight. */
    void scheduleDayRollover();
    /** @brief Whether the clear mark covers ring rows of the current session. */
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
    /* The ring that position is in (Event_History_I::instance); 0 for a start's new ring, not yet read. */
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
    bool m_saveInFlight = false;
    bool m_saveDirty = false;
    /* Fires at local midnight: "TODAY"/"YESTERDAY" section labels are derived
     * from the current date, and nothing else re-reads them when it rolls over. */
    QTimer m_dayTimer;
    QSettings m_settings;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_ */
