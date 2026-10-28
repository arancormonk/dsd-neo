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

class RecordingStore : public QObject, public CallRecordingProvider, public RecordingSessions {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
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
     * folders are read again when it comes back.
     */
    void setRoots(const QStringList& roots);

    /** @brief Build the index (on the worker thread) and keep it current. */
    void start();

    /** @brief A session was already running when the store started: no journal is compacted until endSession(). */
    void assumeSessionRunning();

    QString beginSession(const QString& kind, const QString& uid, const QString& name) override;
    void endSession() override;

    QVariantList recordingsForFragments(const QStringList& fragments) const override;
    bool hasRecordingForFragments(const QStringList& fragments) const override;

    bool ready() const;
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
    /** @brief Recordings the storage limit removed since the app started. */
    int removedByLimit() const;
    /** @brief Deletions asked for and waiting for a file's hold to end (an upload, the player, an export). */
    int deletesWaiting() const;

    /** @brief The storage limit in MB (kBytesPerMb); 0 is no limit. */
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
    /** @brief The export or transfer of @p selection is over: its files may be deleted again. */
    Q_INVOKABLE void releaseSelection(int selection);
    /** @brief The file the player has open, held while it plays; empty when none. */
    Q_INVOKABLE void setPlayingPath(const QString& path);
    /** @brief Whether @p path is a finished recording the store manages (a share is refused otherwise). */
    Q_INVOKABLE bool isIndexedPath(const QString& path) const;

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
    };

    /* What one worker pass changed. */
    struct Pass {
        QSet<QString> fragments;
        bool published = false;
        bool rebuilt = false;
        bool dirty = false;
    };

    enum class DeleteResult { Removed, Held, Kept };

    /* What the store needs of one file, read without following a symlink in its place. */
    static FileStat statOf(const QFileInfo& info, bool withTime);
    static bool rootAvailableNow(const QString& root);
    static qint64 parseLines(const QByteArray& data, QList<RecordingJournalRecord>* records);
    static void statPublished(const QString& folder, const QList<RecordingJournalRecord>& records,
                              QHash<QString, FileStat>* files);

    void run();
    void pass(bool first);
    void refreshRoots(bool first, Pass* pass);
    void rebuildRoot(int root, Pass* pass);
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
    void processDeletes(Pass* pass);
    DeleteResult deleteEntry(const QString& id, Pass* pass);
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
    bool m_allActive = false;
    QString m_compacting;
    QHash<QString, int> m_holds;
    QHash<int, QStringList> m_selections;
    int m_nextSelection = 1;
    QString m_playing;
    QSet<QString> m_deleteQueue;
    qint64 m_limitBytes = kDefaultLimitMb * kBytesPerMb;
    bool m_limitChanged = false;
    /* A session start made or relabelled a folder: the next pass delivers the folder list again. */
    bool m_foldersChanged = false;
    /* A session ended: its folder may be compacted at the next pass. */
    bool m_compactDue = false;
    bool m_started = false;
    bool m_ready = false;
    bool m_stop = false;
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

    /* Worker thread only. */
    QString m_liveToken;
    std::function<bool(const QString&)> m_uploadHolds;
    std::function<qint64(const QString&)> m_freeSpaceProbe;
    qint64 m_freeBytes = -1;
    bool m_lowSpace = false;
    int m_removedByLimit = 0;
    quint64 m_lastFreeCheckMs = 0U;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RECORDING_STORE_H_ */
