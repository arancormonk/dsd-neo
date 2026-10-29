// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The call recording store (issue #708): the app's one index of call recordings, exposed to QML as
 * `recordings`.
 *
 * Recordings live in per-owner folders under the host's recording roots (DecoderHost::recordingRoots()), one folder per
 * saved system or scan list (`<name> (<uid8>)`, its folder.json naming the owner by full uid) and one `Explore` folder
 * for every other session. The decoder writes each folder's recordings.jsonl (the recording journal); the store tails
 * those journals on its own thread, which keeps running while Qt is paused in the background, and keeps the index
 * {id -> file, start, duration, history fragment, folder, flags}. Only journaled files are managed, plus the temp files
 * an earlier process left unfinished; nothing else in a folder is ever counted or deleted.
 *
 * The store owns deletion and its safety (indexed regular files inside a known root, no symlinks, re-checked right
 * before the unlink; never a file the rdio uploader, the player or an export holds; never this process's own temp
 * files), the storage limit (oldest first down to 95%), the free-space warning and journal compaction (only of a folder
 * no session is using). History rows ask it which recordings their fragments have (CallRecordingProvider), and it
 * tells them when that changes (recordingFragmentsChanged()).
 */

#ifndef DSD_NEO_SRC_UI_QT_RECORDING_STORE_H_
#define DSD_NEO_SRC_UI_QT_RECORDING_STORE_H_

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant> // IWYU pragma: keep -- Summary copies QVariantLists; IWYU asks for it in some units only.
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "call_recording_provider.h"
#include "recording_sessions.h"

class QByteArray;
class QDate;
class QFileInfo;

namespace dsd_qt {

class AppPrefs;
class DecoderHost;

struct RecordingJournalRecord;

/** @brief @p name made safe as a folder name on every filesystem the app writes to; may be empty. */
QString recording_sanitize_name(const QString& name);

/** @brief A session owner's folder name: `Explore` for kind "explore", else `<sanitized name> (<uid8>)`. */
QString recording_folder_base_name(const QString& kind, const QString& uid, const QString& name);

/**
 * @brief The start stamps (Unix seconds, [@p from, @p to)) an export date preset covers, on the local calendar of
 * @p today: "today", "yesterday", "last7" and "last30" (today and the days before it), "all". False for any other.
 */
bool recording_export_range(const QString& preset, const QDate& today, qint64* from, qint64* to);

/**
 * @brief Whether @p path lies strictly inside @p root, both resolved to their canonical forms and compared one path
 * component at a time; the last component of @p path is not resolved, so a symlink there stays a symlink.
 */
bool recording_path_inside_root(const QString& path, const QString& root);

class RecordingStore;

/**
 * @brief Tell @p store the session state @p host reports, now and at every change (RecordingStore::hostSessionReport()).
 *
 * A host may not know its state yet when the UI loads (Android learns it at its first poll of the decoder service), so
 * the store holds every journal until the host does.
 */
void recording_store_follow_host(RecordingStore* store, DecoderHost* host);

/**
 * @brief Let @p host play, export and share the recordings @p store manages: the file playing is held
 * (RecordingStore::setPlayingPath()), an export's or save's selection is released when it ends, files the host found
 * gone are looked at again (RecordingStore::recheckPaths()), only indexed recordings are shared, and a share is held
 * until its copy is made (DecoderHost::setRecordingHold()).
 */
void recording_store_serve_host(RecordingStore* store, DecoderHost* host);

/** @brief Give @p store the user's storage limit (AppPrefs::recordingLimitMb()) now and at every change: it applies
 * none until it has one, so this comes before RecordingStore::start(). */
void recording_store_follow_limit(RecordingStore* store, const AppPrefs* prefs);

class RecordingStore : public QObject, public CallRecordingProvider, public RecordingSessions {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool indexed READ indexed NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(qint64 usedBytes READ usedBytes NOTIFY changed)
    Q_PROPERTY(int recordingCount READ recordingCount NOTIFY changed)
    Q_PROPERTY(int unfinishedCount READ unfinishedCount NOTIFY changed)
    Q_PROPERTY(qint64 unfinishedBytes READ unfinishedBytes NOTIFY changed)
    Q_PROPERTY(QVariantList folders READ folders NOTIFY changed)
    Q_PROPERTY(qint64 freeBytes READ freeBytes NOTIFY changed)
    Q_PROPERTY(bool lowSpace READ lowSpace NOTIFY changed)
    Q_PROPERTY(int removedByLimit READ removedByLimit NOTIFY changed)
    Q_PROPERTY(int deletesWaiting READ deletesWaiting NOTIFY changed)
    Q_PROPERTY(int deleteFailures READ deleteFailures NOTIFY changed)
    Q_PROPERTY(QString lastDeleteError READ lastDeleteError NOTIFY changed)
    Q_PROPERTY(int limitMb READ limitMb WRITE setLimitMb NOTIFY limitMbChanged)

  public:
    /** Storage sizes are decimal, as the phone's own storage screens count them. */
    static constexpr qint64 kBytesPerMb = 1000000;
    /** D2: 1 GB unless the user picks another limit. */
    static constexpr int kDefaultLimitMb = 1000;
    /** Free space below this on the recording root shows a warning. */
    static constexpr qint64 kLowSpaceBytes = 200 * kBytesPerMb;

    explicit RecordingStore(QObject* parent = nullptr);
    ~RecordingStore() override;

    RecordingStore(const RecordingStore&) = delete;
    RecordingStore& operator=(const RecordingStore&) = delete;

    /**
     * @brief The folders recordings live in, preferred first (new folders go to the first available one). Set once,
     * before start().
     *
     * A root is available while it is a readable folder, or does not exist yet in a folder that does. One whose parent
     * is gone (storage not mounted) is unavailable: what it held stays in the index, untouched and undeleted, and its
     * folders are read again when it comes back. The last root is the fallback in the app's own storage: start()
     * creates its parent folder, which a first launch may not have made yet.
     */
    void setRoots(const QStringList& roots);

    /** @brief Build the index (on the worker thread) and keep it current. */
    void start();

    /**
     * @brief What the host knows of its session: whether it knows yet (@p known), whether one is active and whether
     * it is running (@p running, DecoderHost::Running).
     *
     * Until the host first reports a known state, every journal is held (a session may be running that no starter
     * began here: the decoder kept going while the UI was recreated); a first known state with none running ends that
     * hold, and keeps a folder a start already marked. A session that turns active without a beginSession() before it
     * holds every journal too. A session that ends after the host saw it running frees every folder, those of earlier
     * sessions that never ran included. One that ends without ever running keeps its folders uncompacted for the rest
     * of this process: its start may still reach the decoder late (Android's start grace can give up on an intent the
     * service has not handled yet). recording_store_follow_host() calls this.
     */
    void hostSessionReport(bool known, bool active, bool running);

    QString beginSession(const QString& kind, const QString& uid, const QString& name) override;
    void endSession() override;

    QVariantList recordingsForFragments(const QStringList& fragments) const override;
    bool hasRecordingForFragments(const QStringList& fragments) const override;

    /** @brief The roots were checked and the folder table read: sessions may start (beginSession() waits for it). */
    bool ready() const;
    /** @brief Every available folder's journal was read: the counts and History roles are complete. */
    bool indexed() const;
    bool available() const;
    qint64 usedBytes() const;
    int recordingCount() const;
    int unfinishedCount() const;
    qint64 unfinishedBytes() const;
    /** @brief One map per owner: uid, kind, name, count, bytes, first and last start, available. Newest first. */
    QVariantList folders() const;
    /** @brief Free bytes where new recordings go; -1 when unknown. */
    qint64 freeBytes() const;
    bool lowSpace() const;
    /** @brief Recordings the storage limit removed since the app started, or since acknowledgeNotices(). */
    int removedByLimit() const;
    /** @brief Deletions asked for and waiting for a file's hold to end (an upload, the player, an export). */
    int deletesWaiting() const;
    /** @brief Recordings the store failed to remove (asked for or by the limit) since the app started, or since
     * acknowledgeNotices(). */
    int deleteFailures() const;
    /** @brief Why the last of them failed (the system's text); empty when none did since the acknowledgement. */
    QString lastDeleteError() const;
    /**
     * @brief The user has seen what the storage limit removed and which deletes failed: removedByLimit() and
     * deleteFailures() count from zero again (the Call recordings screen calls this as it shows them). GUI thread;
     * limitReached() is unaffected.
     */
    Q_INVOKABLE void acknowledgeNotices();

    /**
     * @brief The storage limit in MB (kBytesPerMb); 0 is no limit.
     *
     * Applied only once set: the store enforces no limit until the app hands it the user's (a first pass never applies
     * the default to someone who chose a larger one).
     */
    int limitMb() const;
    void setLimitMb(int limitMb);

    /** @brief Delete recordings by id; each waits while something holds its file. History rows stay. */
    Q_INVOKABLE void deleteRecordings(const QStringList& ids);
    Q_INVOKABLE void deleteRecording(const QString& id);
    /** @brief Delete every recording of the owner @p uid, unfinished ones in its folders included. */
    Q_INVOKABLE void deleteFolder(const QString& uid);
    /** @brief Delete the unfinished recordings earlier runs left. */
    Q_INVOKABLE void deleteUnfinished();
    /** @brief Delete every managed recording. */
    Q_INVOKABLE void deleteAll();

    /**
     * @brief Resolve an export (D9) and hold its files until releaseSelection(): the finished recordings of owner
     * @p folderUid (empty: every owner) whose start falls in the date @p preset (recording_export_range(), today on
     * the real-time clock).
     *
     * @return {selection (0 when nothing matched), paths, names (`<folder>/<file>`, for the zip), count, bytes}.
     */
    Q_INVOKABLE QVariantMap selectForExport(const QString& folderUid, const QString& preset);
    /** @brief As selectForExport(), for the recordings @p ids (save or share one, a row's parts). */
    Q_INVOKABLE QVariantMap selectRecordings(const QStringList& ids);
    /** @brief As selectRecordings(), for the recording at @p path (a share the host queued); its selection, 0 when
     * there is none to hold. */
    int selectPath(const QString& path);
    /** @brief The export or transfer of @p selection is over: its files may be deleted again. */
    Q_INVOKABLE void releaseSelection(int selection);
    /** @brief The file the player has open, held while it plays; empty when none. */
    Q_INVOKABLE void setPlayingPath(const QString& path);
    /** @brief Whether @p path is a finished recording the store manages (a share is refused otherwise). */
    Q_INVOKABLE bool isIndexedPath(const QString& path) const;
    /**
     * @brief Look at the files of @p ids again (the player, an export or a share found one missing): one that is gone,
     * or no longer a regular file, leaves the index, its fragments announced; one still there has its size read again.
     */
    Q_INVOKABLE void recheckRecordings(const QStringList& ids);
    /** @brief recheckRecordings() for the recordings at @p paths (playback, an export or a share names files). */
    void recheckPaths(const QStringList& paths);

#ifdef DSD_NEO_TEST_HOOKS
    /** @brief Wait for a worker pass that starts after this call (at once when not started). Test hook. */
    void waitForPassForTest();
    /** @brief Pass every @p activeMs while a session runs, else every @p idleMs; before start(). Test hook. */
    void setTimingForTest(int activeMs, int idleMs);
    /** @brief Replace dsd_rdio_upload_holds() (called on the worker thread); before start(). Test hook. */
    void setUploadHoldsForTest(std::function<bool(const QString&)> holds);
    /** @brief Replace the free-space read (worker thread; -1 unknown); before start(). Test hook. */
    void setFreeSpaceProbeForTest(std::function<qint64(const QString&)> probe);
    /** @brief The folder paths the index knows. Test hook. */
    QStringList folderPathsForTest() const;
    /** @brief How many journals were compacted. Test hook. */
    int compactionsForTest() const;
    /** @brief How long the first worker pass (the index rebuild) took, in nanoseconds. Test hook. */
    qint64 rebuildNsForTest() const;
    /** @brief Replace the file removal (worker thread; false with the reason in @p error); before start(). */
    void setRemoveForTest(std::function<bool(const QString& path, QString* error)> remove);
    /** @brief Call @p hook on the worker after each folder.json read, before the folder's files and journal; before
     * start(). Test hook. */
    void setFolderLoadHookForTest(std::function<void(const QString& folder)> hook);
    /** @brief Call @p hook on the worker as a root is read whole (at the first pass, or back from away), before its
     * folders are listed; before start(). Test hook. */
    void setRootReadHookForTest(std::function<void(const QString& root)> hook);
    /** @brief Call @p hook on the worker after a folder's new journal lines are read, before their files are looked
     * at; before start(). Test hook. */
    void setTailHookForTest(std::function<void(const QString& folder)> hook);
#endif

  Q_SIGNALS:
    void changed();
    void limitMbChanged();
    /** @brief The recordings of these History fragments (call_history_fragment_key()) changed. */
    void recordingFragmentsChanged(const QStringList& fragments);
    /** @brief The storage limit removed @p count recordings, oldest first. */
    void limitReached(int count);

  private:
    struct Entry {
        QString id;
        QString path;
        QString folder;
        qint64 start = 0;
        qint64 durationMs = 0;
        int rate = 0;
        qint64 bytes = 0;
        /* The record as last read, for a compaction to write back. */
        qint64 t = 0;
        QString file;
        bool hasInstance = false;
        quint64 instance = 0U;
        int slot = -1;
        bool hasRow = false;
        qulonglong row = 0U;
        QString fragment;
        bool pending = false;
        bool rdio = false;
        bool failed = false;
        bool unfinished = false;
        QString system;
        QString target;
        QString source;
        quint64 order = 0U;
    };

    struct Folder {
        QString path;
        QString uid;
        QString kind;
        QString name;
        int root = -1;
        /* Its journal and files were read; until then only folder.json is known (the folder table). */
        bool loaded = false;
        /* Not loaded, and read again (its storage came back): what the index held for it stays until that read
         * completes and replaces it. */
        bool rereading = false;
        qint64 offset = 0;
        qint64 lines = 0;
        qint64 compactFailedAt = -1;
    };

    struct FileStat {
        bool regular = false;
        qint64 size = 0;
        qint64 mtime = 0;
    };

    struct Summary {
        bool ready = false;
        bool indexed = false;
        bool available = false;
        qint64 usedBytes = 0;
        int count = 0;
        int unfinishedCount = 0;
        qint64 unfinishedBytes = 0;
        QVariantList folders;
        qint64 freeBytes = -1;
        bool lowSpace = false;
        int removedByLimit = 0;
        int deletesWaiting = 0;
        int deleteFailures = 0;
        QString lastDeleteError;
    };

    /* What one worker pass changed. */
    struct Pass {
        QSet<QString> fragments;
        bool published = false;
        bool rebuilt = false;
        bool dirty = false;
    };

    /* Removed: unlinked. Gone: already gone, now out of the index. Held: waits for a hold. Kept: not the store's to
     * remove (left on disk, out of the index) or this process's temp file. Failed: the unlink failed; still indexed. */
    enum class DeleteResult { Removed, Gone, Held, Kept, Failed };
    enum class DeleteCause { Asked, Limit };
    /* What is at a recording's path, without following a link there: nothing; a regular file inside an available root,
     * which the store may remove; or anything else, which it leaves alone. */
    enum class OnDisk { Missing, Removable, Foreign };

    /* What the store needs of one file, read without following a symlink in its place. */
    static FileStat statOf(const QFileInfo& info, bool withTime);
    static bool rootAvailableNow(const QString& root);
    static qint64 parseLines(const QByteArray& data, QList<RecordingJournalRecord>* records);
    static void statPublished(const QString& folder, const QList<RecordingJournalRecord>& records,
                              QHash<QString, FileStat>* files);

    void run();
    void pass(bool first);
    void refreshRoots(bool first, Pass* pass);
    /* @p root turned unavailable: what it held stays indexed, untouched, and the rows its recordings belong to are told
     * (their recordings now read as away). Under the store's lock. */
    void markRootAwayLocked(int root, Pass* pass);
    /* The storage-loss rule (docs/code_map.md, RecordingStore): asked before any outcome that drops index state or a
     * request, or counts usage, on the strength of a read in @p root (a file or journal missing, a short listing, a
     * failed unlink). Whether @p root is there now: one that went away since the pass began is marked away, and one
     * marked away earlier in the pass stays so; for either, nothing in it is treated as deleted, and the caller keeps
     * what the index and the queues hold. */
    bool rootStillAvailable(int root, Pass* pass);
    /* rootStillAvailable() for each root @p roots marks true; false when any of them is gone. */
    bool rootsStillAvailable(const QList<bool>& roots, Pass* pass);
    void rebuildRoot(int root, Pass* pass);
    /* The journals and files of the folders the table lists but nobody read yet. */
    void loadPendingFolders(Pass* pass);
    /* Read a folder whole: its folder.json, its files and its journal. @p replace drops what the index held for it. */
    void loadFolder(const QString& path, int root, bool replace, Pass* pass);
    void tailFolders(Pass* pass);
    void tailFolder(const QString& path, qint64 offset, Pass* pass);
    void applyRecordLocked(const Folder& folder, const RecordingJournalRecord& record,
                           const QHash<QString, FileStat>& files, Pass* pass);
    void insertEntryLocked(Entry entry, Pass* pass);
    void unindexFragmentLocked(const Entry& entry, Pass* pass);
    void removeEntryLocked(const QString& id, Pass* pass);
    void removeFolderEntriesLocked(const QString& folder, Pass* pass);
    bool folderAvailableLocked(const QString& folder) const;
    QString folderUidLocked(const QString& folder) const;
    bool liveTemp(const QString& path) const;
    bool stopping() const;
    /* Let History reads waiting for the lock take it before the worker takes it again (the mutex is not fair). */
    void yieldToReaders() const;
    void processRechecks(Pass* pass);
    /* Look at the files of @p ids again: a gone one leaves the index (kept when its root went: the storage-loss rule),
     * one still there has its size read again. */
    void recheckEntries(const QStringList& ids, Pass* pass);
    void processDeletes(Pass* pass);
    /* @p counted: the roots a limit's total counts, each checked again before the unlink (Held when one is gone). */
    DeleteResult deleteEntry(const QString& id, DeleteCause cause, Pass* pass, const QList<bool>* counted = nullptr);
    OnDisk onDiskLocked(const QString& path) const;
    void noteDeleteFailure(DeleteCause cause, const QString& error);
    void enforceLimit(Pass* pass);
    bool checkFreeSpace();
    void compactFolders();
    bool compactFolder(const QString& path);
    static Summary summarize(const QHash<QString, Entry>& entries, const QHash<QString, Folder>& folders,
                             const QList<bool>& rootAvailable);
    void publish(const Pass& pass);
    void deliver();
    QString folderLocked(const QString& kind, const QString& uid, const QString& name);
    QVariantMap entryMap(const Entry& entry) const;
    QVariantMap selectLocked(QList<const Entry*> entries);

    /* GUI thread: the last summary the worker delivered. */
    Summary m_summary;
    int m_limitMb = kDefaultLimitMb;

    class ReaderLock;

    std::thread m_thread;
    mutable std::mutex m_mutex;
    /* History reads waiting for m_mutex (ReaderLock). */
    mutable std::atomic<int> m_readersWaiting{0};
    std::condition_variable m_wake;
    std::condition_variable m_passed;
    std::condition_variable m_settled;

    /* Under m_mutex. */
    QStringList m_roots;
    QList<bool> m_rootAvailable;
    QHash<QString, Folder> m_folders;
    QHash<QString, Entry> m_entries;
    QHash<QString, QStringList> m_byFragment;
    QHash<QString, QString> m_byPath;
    QSet<QString> m_active;
    /* Every journal held: until the host's state is known, and while a session no starter began here is active. */
    bool m_allActive = true;
    bool m_hostKnown = false;
    bool m_hostActive = false;
    /* A starter's beginSession() came before the session the host reports. */
    bool m_starterBegan = false;
    /* The host has seen the active session running. */
    bool m_sawRunning = false;
    /* Folders of sessions that ended without ever running: never compacted until a later session ran and ended. */
    QSet<QString> m_unproven;
    QString m_compacting;
    QHash<QString, int> m_holds;
    QHash<int, QStringList> m_selections;
    int m_nextSelection = 1;
    QString m_playing;
    QSet<QString> m_deleteQueue;
    QSet<QString> m_recheckQueue;
    qint64 m_limitBytes = kDefaultLimitMb * kBytesPerMb;
    bool m_limitChanged = false;
    bool m_limitSet = false;
    /* The last enforceLimit() stopped over its target because what was left was held (played, selected, uploading):
     * a released hold wakes a pass and every timed pass tries again (an rdio upload has no release to wake one). */
    bool m_limitPending = false;
    /* A session start made or relabelled a folder: the next pass delivers the folder list again. */
    bool m_foldersChanged = false;
    /* A session ended: its folder may be compacted at the next pass. */
    bool m_compactDue = false;
    bool m_started = false;
    bool m_ready = false;
    bool m_stop = false;
    /* m_stop, readable without the lock between the folders and slices of a long read. */
    std::atomic<bool> m_stopping{false};
    bool m_passRequested = false;
    bool m_inPass = false;
    quint64 m_passes = 0U;
    int m_activeMs = 1000;
    int m_idleMs = 60000;
    quint64 m_nextOrder = 1U;
    int m_compactions = 0;
    qint64 m_rebuildNs = 0;
    /* The latest summary and the fragments changed since the GUI last took them; one delivery queued at a time, so a
     * GUI thread paused in the background never collects more than one. */
    Summary m_outbox;
    QSet<QString> m_outFragments;
    bool m_deliveryQueued = false;

    /* GUI thread: the totals acknowledgeNotices() took, which the notice readings count from. */
    int m_removedByLimitSeen = 0;
    int m_deleteFailuresSeen = 0;

    /* Worker thread only. */
    QString m_liveToken;
    std::function<bool(const QString&)> m_uploadHolds;
    std::function<qint64(const QString&)> m_freeSpaceProbe;
    qint64 m_freeBytes = -1;
    bool m_lowSpace = false;
    std::function<bool(const QString&, QString*)> m_remove;
    std::function<void(const QString&)> m_folderLoadHook;
    std::function<void(const QString&)> m_rootReadHook;
    std::function<void(const QString&)> m_tailHook;
    int m_removedByLimit = 0;
    int m_deleteFailures = 0;
    QString m_lastDeleteError;
    /* One warning per run for each cause; later failures are counted. */
    bool m_deleteFailureLogged[2] = {false, false};
    quint64 m_lastFreeCheckMs = 0U;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RECORDING_STORE_H_ */
