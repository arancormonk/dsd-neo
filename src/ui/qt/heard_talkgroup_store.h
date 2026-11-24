// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The talkgroups each saved system has been heard on, kept across sessions (issue #903).
 *
 * One json_store file (heard_talkgroups.json by default): a top-level array of `{v:1, owner, variant, id, first,
 * last}`, where owner is the saved system's uid, variant a dsd_heard_tg_variant (1..255), id the talkgroup (non-zero)
 * and first/last the earliest and latest retained observation in decode-domain wall seconds. Elements of another `v`
 * (or not objects at all) are kept verbatim and otherwise ignored; a v:1 element that breaks the schema is dropped. A
 * missing file is an empty store.
 *
 * Load status gates every write and purge: a file that could not be read whole is never replaced or moved, and
 * nothing is learnt for the rest of the process (json_store posts its notice). Each owner keeps at most
 * kMaxEntriesPerOwner entries; the one heard longest ago goes first, ties to the lower variant, then the lower id.
 *
 * Writes are serialized on one worker. At most one write waits behind the one running, and it writes the latest state
 * queued when it starts, so a burst of changes is one write and the file ends up holding the newest state. The store
 * stays owed (dirty) until a write of its latest state succeeds.
 * Background learning is written at most every kFlushIntervalMs of real monotonic time (flushIfDue(), from the UI
 * poll), and at once at Stop, app pause, quit and teardown (flushNow(), the destructor). Forget, clear and purge are
 * written straight away. GUI thread only.
 */

#ifndef DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_STORE_H_
#define DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_STORE_H_

#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QtGlobal>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "heard_talkgroup_sink.h"

namespace dsd_qt {

class HeardTalkgroupStore : public QObject, public HeardTalkgroupSink {
    Q_OBJECT

  public:
    /** @brief The store's file name in the app data directory. */
    static constexpr const char* kFileName = "heard_talkgroups.json";
    /** @brief The most entries one owner keeps. */
    static constexpr int kMaxEntriesPerOwner = 2000;
    /** @brief The shortest real monotonic time between two background writes. */
    static constexpr qint64 kFlushIntervalMs = 30000;

    struct Entry {
        QString owner;
        int variant = 0;
        quint32 id = 0;
        qint64 first = 0;
        qint64 last = 0;
    };

    /** @brief Load @p path, a json_store file name (relative to the app data directory). */
    explicit HeardTalkgroupStore(const QString& path, QObject* parent = nullptr);
    /** @brief Waits out the queued writes, then writes what is still owed (unless the load was unreadable). */
    ~HeardTalkgroupStore() override;

    /** @brief False after an unreadable load: nothing is learnt, written or purged. */
    bool loaded() const;

    /**
     * @brief Record @p id of @p variant heard on @p ownerUid at @p whenSecs: first and last widen to take it.
     *
     * Ignored when the load was unreadable, the policy does not allow the owner (no policy allows none), the owner is
     * empty, the variant is outside 1..255, the id is 0 or the time is negative. A new entry over the cap evicts the
     * owner's oldest (which may be this one, when it arrived late). ownerChanged() follows a change.
     */
    void observe(const QString& ownerUid, int variant, quint32 id, qint64 whenSecs) override;

    /** @brief Which owners observe() may learn for: a saved system that exists and has rememberTalkgroups on. */
    void setOwnerPolicy(std::function<bool(const QString&)> policy);

    /** @brief Whether observe() would learn for @p ownerUid now: loaded and allowed by the policy. */
    bool remembering(const QString& ownerUid) const;

    /** @brief @p ownerUid's entries, heard most recently first (ties: lower variant, then lower id). */
    QVector<Entry> entriesFor(const QString& ownerUid) const;

    /** @brief An entry's key (variant << 32 | id), as ownerChanged() names it, and its parts. */
    static quint64 entryKey(int variant, quint32 id);
    static int entryVariant(quint64 key);
    static quint32 entryId(quint64 key);

    /** @brief @p ownerUid's entry @p key's last heard into @p last; false (@p last untouched) when there is none.
     * One lookup. */
    bool entryLast(const QString& ownerUid, quint64 key, qint64* last) const;

    /** @brief @p visit(key, last) for each of @p ownerUid's entries, in no order (entriesFor() without its copy and
     * sort). */
    void visitEntries(const QString& ownerUid, const std::function<void(quint64, qint64)>& visit) const;

    /**
     * @brief The talkgroup number a policy row and the Talkgroups screen know entry @p id of @p variant by: an NXDN
     * Type-D id (`prefix << 11 | id`) by its 11-bit id, as the call state numbers it; every other id as it is.
     */
    static quint32 policyId(int variant, quint32 id);

    /**
     * @brief Remove every entry of @p ownerUid whose talkgroup number (policyId()) is @p id, every variant, and write at
     * once. Not gated by the policy.
     *
     * When it holds the id, the before-edit hook (setBeforeEdit()) runs first, so nothing learnt before the forget can
     * come back after it; when the hook fails, nothing is removed.
     *
     * @return false when the hook failed and the entries stay; true otherwise (removed, or nothing to remove).
     */
    bool forget(const QString& ownerUid, quint32 id);

    /** @brief Remove every entry of @p ownerUid, and write at once. Not gated by the policy. Runs the before-edit hook
     * first when it holds the owner, as forget() does, and removes nothing when it fails. @return as forget(). */
    bool clear(const QString& ownerUid);

    /**
     * @brief What forget() and clear() run before they remove anything (empty: nothing).
     *
     * The app passes History's CallHistoryModel::settleSpill(): it folds what the background tap holds (which may
     * observe here) and writes History, so the background spill it folded is acknowledged and gone, and no later fold of
     * it (a relaunch after the process dies) observes again what the edit removes. It returns false when that write
     * failed, or the spill could not be read whole or emptied, and the edit is then refused: the spill it could not
     * settle would bring the entries back.
     */
    void setBeforeEdit(std::function<bool()> hook);

    /**
     * @brief Remove every entry of @p ownerUid, a saved system just deleted, and write at once. No before-edit hook:
     * an owner that no longer exists learns nothing, so no fold can bring its entries back.
     */
    void dropOwner(const QString& ownerUid);

    /**
     * @brief Remove every owner not in @p liveUids, and write at once.
     *
     * Call it only after the saved systems loaded successfully: @p liveUids is then every saved system there is.
     */
    void purgeMissingOwners(const QSet<QString>& liveUids);

    /** @brief Queue a write when one is owed and kFlushIntervalMs has passed since the last one this started. */
    void flushIfDue(qint64 realtimeMonoMs);

    /** @brief Queue a write now when one is owed and no queued write already carries the latest state. */
    void flushNow();

    /** @brief Whether the latest state is not yet known to be on the disk. */
    bool dirty() const;

    /** @brief Block until every queued write has finished. */
    void waitForWrites();

#ifdef DSD_NEO_TEST_HOOKS
    /** @brief Run @p hook on the worker before each write (empty clears it). */
    static void setBeforeWriteForTest(std::function<void()> hook);
#endif

  Q_SIGNALS:
    /**
     * @brief @p ownerUid's entries changed. @p entries names the ones that did (entryKey()) when an observe changed
     * them: the entry it added or moved and any the cap evicted for it. Empty: any may have (a forget, clear or purge).
     */
    void ownerChanged(const QString& ownerUid, const QVector<quint64>& entries);

  private:
    struct Times {
        qint64 first = 0;
        qint64 last = 0;
    };

    /* Keyed by variant << 32 | id. */
    using OwnerEntries = QHash<quint64, Times>;

    void load();
    /** Runs the before-edit hook for @p edit ("forget", "clear"); false, with a diagnostic, when it failed. */
    bool settleBeforeEdit(const char* edit);
    /** Takes one v:1 element; false when it breaks the schema. */
    bool takeStored(const QJsonValue& value);
    /** Evicts @p entries down to the cap, oldest first; @return the keys it evicted. */
    static QVector<quint64> evictOverCap(OwnerEntries& entries);
    /** Notes a change to @p ownerUid's entries (@p entries as ownerChanged() names them): the state is owed, and
     * ownerChanged() is sent. */
    void changed(const QString& ownerUid, const QVector<quint64>& entries = {});
    /** Queues a write of the whole state as it is now. */
    void queueWrite();
    /** Every entry, as one flat list a write can own. */
    QVector<Entry> flatEntries() const;

    const QString m_fileName;
    bool m_loaded = false;
    QHash<QString, OwnerEntries> m_owners;
    QJsonArray m_kept; // elements of another v, written back as they were read
    std::function<bool(const QString&)> m_policy;
    std::function<bool()> m_beforeEdit;

    quint64 m_generation = 0;       // bumped by every change
    quint64 m_queuedGeneration = 0; // the state the last queued write carries
    qint64 m_lastFlushMs = -1;      // when flushIfDue() last queued, real monotonic ms

    /* What the GUI thread and the worker share: the newest generation written, how many writes are queued or
       running, and, under the lock, the latest state queued and whether a write that has not started will take it. */
    struct Shared {
        std::atomic<quint64> written{0};
        std::atomic<int> pending{0};
        std::mutex lock;
        bool jobWaiting = false;
        bool hasState = false;
        QVector<Entry> entries;
        QJsonArray kept;
        quint64 generation = 0;
    };

    /** Takes the latest queued state, if any, and lets the next queueWrite() start a write. */
    static bool takeQueued(Shared& shared, QVector<Entry>* entries, QJsonArray* kept, quint64* generation);
    /** The worker's part of a write: take the latest queued state, write it, report. */
    static void runWrite(const std::shared_ptr<Shared>& shared, const QString& fileName);

    std::shared_ptr<Shared> m_shared;
    QThreadPool m_pool;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_STORE_H_ */
