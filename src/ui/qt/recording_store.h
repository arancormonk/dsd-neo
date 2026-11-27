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
 * tells them when that changes (recordingFragmentsChanged()). Signal search asks it which recordings a detection has
 * (recordingsForDetection(), issue #809), and re-reads them when `revision` moves.
 */

#ifndef DSD_NEO_SRC_UI_QT_RECORDING_STORE_H_
#define DSD_NEO_SRC_UI_QT_RECORDING_STORE_H_

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <QVariant> // IWYU pragma: keep -- Summary copies QVariantLists; IWYU asks for it in some units only.
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic>
#include <condition_variable>
#include <dsd-neo/platform/file_compat.h>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "call_recording_provider.h"
#include "recording_sessions.h"

class QByteArray;
class QDate;
class QFileInfo;
class QIODevice;

namespace dsd_qt {

class AppPrefs;
class DecoderHost;
class RecordingSelectionToken;

struct RecordingJournalRecord;

/** @brief @p name made safe as a folder name on every filesystem the app writes to; may be empty. */
QString recording_sanitize_name(const QString& name);

/** @brief A session owner's folder name: `Explore` for kind "explore", else `<sanitized name> (<uid8>)`. */
QString recording_folder_base_name(const QString& kind, const QString& uid, const QString& name);

/**
 * @brief The local calendar days [@p first, @p last] an export date preset covers, counted from @p today: "today",
 * "yesterday", "last7" and "last30" (today and the days before it, ending with today); "all" leaves both null (no
 * bound). False for any other, or with no @p today.
 *
 * A preset is a range of dates: recording_export_window() turns it into instants once, at the press, from the day
 * boundaries in the press's zone (a date the zone skipped, with no midnight of its own, starts with the next day that
 * has one), and recording_export_covers() compares each recording's start with them.
 */
bool recording_export_days(const QString& preset, const QDate& today, QDate* first, QDate* last);

/**
 * @brief An export date preset resolved once, at the press: the instants [@p from, @p to) its days span in the press's
 * zone, from the first day's start to the start of the day after the last (a date the zone skipped starts with the
 * next day that has a start, so a range of skipped days spans nothing). The scan then compares instants only: a zone
 * change during it moves no recording, and none is converted.
 */
struct RecordingExportWindow {
    bool all = true;
    qint64 from = 0;
    qint64 to = 0;
};

/**
 * @brief The zone a press reads today and every recording's date in: local time, as the phone's clock shows it. Not
 * QTimeZone::systemTimeZone(): for a POSIX TZ string (TZ=UTC+09, local UTC-09) Qt 6.11.3 names a zone the other side
 * of UTC.
 */
QTimeZone recording_export_press_zone();

/** @brief @p window for the preset @p preset from @p today in @p zone; false for an unknown preset, no @p today or an
 * invalid @p zone. */
bool recording_export_window(const QString& preset, const QDate& today, const QTimeZone& zone,
                             RecordingExportWindow* window);

/**
 * @brief Whether a recording that started at @p start (Unix seconds) falls in @p window; with "all", every start does.
 * One after the last day (a clock that ran ahead) is outside.
 */
bool recording_export_covers(const RecordingExportWindow& window, qint64 start);

/**
 * @brief The name a recording's zip entry takes, and a History export lists: the base name of its @p folder, a slash,
 * and its @p file as the journal named it (a nested file keeps its sub-folders).
 */
QString recording_entry_name(const QString& folder, const QString& file);

/**
 * @brief Whether the journal open in @p file holds nothing past @p offset that the store has not read: at most a line a
 * crash tore. False when the bytes past @p offset cannot all be read, so a compaction never drops records it never saw.
 */
bool recording_journal_tail_settled(QIODevice& file, qint64 offset);

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
 * @brief Let @p host play, export and share the recordings @p store manages: a single recording's file is held while
 * it plays (DecoderHost::playbackHoldPath(), RecordingStore::setPlayingPath()), an export's or save's selection is
 * released when it ends, files the host found gone are looked at again (RecordingStore::recheckPaths()), only indexed
 * recordings are shared, and a share is held until its copy is made (DecoderHost::setRecordingHold()).
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
    Q_PROPERTY(int deletesCancelled READ deletesCancelled NOTIFY changed)
    Q_PROPERTY(int deletesSkipped READ deletesSkipped NOTIFY changed)
    Q_PROPERTY(QString lastDeleteError READ lastDeleteError NOTIFY changed)
    Q_PROPERTY(int limitMb READ limitMb WRITE setLimitMb NOTIFY limitMbChanged)
    /* A double so QML reads it exactly well past any count a run reaches (a JS number). */
    Q_PROPERTY(double revision READ revision NOTIFY revisionChanged)

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
    /** @brief Every row's names under one hold of the store's lock (a History export), each named by
     * recording_entry_name() as selectForExport() names it. */
    QList<QStringList> recordingEntryNames(const QList<QStringList>& fragmentsPerRow) const override;
    /** @brief Every row's recordings under one hold of the store's lock (a History playlist, issue #909): each row's
     * maps as recordingsForFragments() gives them, plus `deleteQueued` (a delete asked for it waits). */
    /* Also probes the storage roots it would admit from (one stat each, under the lock): a root gone is marked seen
     * away and one back asks for its read again (R74). For Play all and Play from here, not per-refresh reads. */
    QList<QVariantList> recordingsForFragmentLists(const QList<QStringList>& fragmentsPerRow) const override;

    /** @brief How many recordings were heard on the Signal search detection @p key (`<session>:<hit>`, the journal's
     * `search`), those on storage that is away included. */
    Q_INVOKABLE int recordingCountForDetection(const QString& key) const;
    /**
     * @brief The recordings of the detection @p key, newest first (by start, then by the order they were published):
     * the maps recordingsForFragments() gives, plus `system`, `target` and `source` as received and the History
     * `fragment` (empty while it has none). One on storage that is away stays, with `available` false.
     */
    Q_INVOKABLE QVariantList recordingsForDetection(const QString& key) const;
    /**
     * @brief Moves (GUI thread, with revisionChanged()) whenever what recordingsForDetection() or
     * recordingsForFragments() returns may have changed: a recording added, removed or linked, its size, and storage
     * going or coming back. changed() follows the totals only, which such a change can leave equal.
     */
    double revision() const;

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
    /** @brief Deletes accepted and then dropped because their storage was read again while they waited (the files
     * stay; asking again deletes them), since the app started or acknowledgeNotices(). */
    int deletesCancelled() const;
    /** @brief Deletes (asked for or by the limit) not done because the file at the recording's name was not the one
     * recorded (another size or time, a name that cannot be read, a link): left as it is, nothing to ask again. */
    int deletesSkipped() const;
    /**
     * @brief The user has seen what the storage limit removed and which deletes failed, were cancelled or skipped:
     * removedByLimit(), deleteFailures(), deletesCancelled() and deletesSkipped() count from zero again (the Call
     * recordings screen calls this as it shows them). GUI thread; limitReached() is unaffected.
     */
    Q_INVOKABLE void acknowledgeNotices();

    /**
     * @brief The storage limit in MB (kBytesPerMb); 0 is no limit.
     *
     * Applied only once set: the store enforces no limit until the app hands it the user's (a first pass never applies
     * the default to someone who chose a larger one).
     */
    int limitMb() const;
    void setLimitMb(int mb);

    /** @brief Delete recordings by id; each waits while something holds its file. History rows stay.
     *
     * Every delete request (these and the ones below) is refused, and asks nothing, unless the index is settled
     * (storageSettledLocked(): no pass running or asked for, every available folder read): the index may then hold
     * part of a folder, or a folder under the owner it had before the storage came back. It is refused whole too when
     * any recording it names is on storage that is not there (a folder, Delete all or unfinished recordings that span
     * storage there and away delete none of them). False when refused, and deleteRefusal() says why. A delete asked
     * but not done yet (its file held by an export) is dropped when the worker reads storage again. */
    Q_INVOKABLE bool deleteRecordings(const QStringList& ids);
    Q_INVOKABLE bool deleteRecording(const QString& id);
    /**
     * @brief deleteRecording() of the file the user pressed Delete on (issue #909): @p id with the path and the
     * identity its map gave then (`path`, `identitySize`, `mtimeSec`, `mtimeNanos`).
     *
     * Checked under the store's lock as the request is taken: refused, queuing nothing, with deleteRefusal() "changed"
     * when the recording under @p id is another file now (a read again put another file under the id, at another path
     * or with another identity: R68), and "gone" when no recording has @p id now; otherwise as deleteRecording(). The
     * path goes with the queued request and is checked again right before the unlink.
     */
    Q_INVOKABLE bool deleteRecordingChecked(const QString& id, const QString& path, qint64 size, qint64 mtimeSec,
                                            int mtimeNanos);
    /** @brief Delete every recording of the owner @p uid, unfinished ones in its folders included. */
    Q_INVOKABLE bool deleteFolder(const QString& uid);
    /** @brief Delete the unfinished recordings earlier runs left. */
    Q_INVOKABLE bool deleteUnfinished();
    /** @brief Delete every managed recording. */
    Q_INVOKABLE bool deleteAll();
    /** @brief Why the last delete request refused (GUI thread): "loading" (the index is not settled), "storageAway"
     * (it named a recording on storage that is not there), "changed" or "gone" (deleteRecordingChecked() only), or
     * empty after one that was asked. */
    Q_INVOKABLE QString deleteRefusal() const;

    /**
     * @brief Resolve an export (D9) and hold its files until releaseSelection(): the finished recordings of owner
     * @p folderUid (empty: every owner) whose start falls in the window of the date @p preset
     * (recording_export_window()), from one reading of the local zone and of today in it on the real-time clock, taken
     * at the call (selectForExportAt()). Refused, selecting and holding nothing, while a folder is being read
     * (deleteRecordings()).
     *
     * @return {selection (0 when nothing matched or refused), paths, names (`<folder>/<file>`, for the zip), count,
     * bytes, sizes, mtimeSecs and mtimeNanos (each file's size and modification time as the index knows it, which the
     * transfer checks before it reads the file), day (that reading of today, `yyyy-MM-dd`, which names the zip: a press
     * across midnight names the day it selected), loading (true when refused while a folder is read)}.
     */
    Q_INVOKABLE QVariantMap selectForExport(const QString& folderUid, const QString& preset);
    /** @brief selectForExport() pressed at @p pressMs (milliseconds since the Unix epoch, given rather than read):
     * today is that instant's date in the press zone. */
    QVariantMap selectForExportAt(const QString& folderUid, const QString& preset, qint64 pressMs);
    /** @brief selectForExport() on @p today in @p zone, given rather than read. */
    QVariantMap selectForExportOn(const QString& folderUid, const QString& preset, const QDate& today,
                                  const QTimeZone& zone);
    /** @brief As selectForExport(), for the recordings @p ids (save one, a row's parts): refused, with loading true,
     * while the store is not settled. */
    Q_INVOKABLE QVariantMap selectRecordings(const QStringList& ids);
    /** @brief As selectRecordings(), for the recording at @p path (a share the host queued); its selection, 0 when
     * there is none to hold. */
    int selectPath(const QString& path);
    /** @brief Whether @p selection is held and no read of storage again has started since it was made: a read again
     * may give a path it names to another recording (two owners' folders swapped while the storage was away), so the
     * host refuses its transfer otherwise (recording_store_serve_host(), DecoderHost::TransferChanged). */
    bool selectionCurrent(int selection) const;
    /** @brief selectionCurrent(), and when it is, @p token (may be null) set to what the transfer checks again on its
     * own thread: stale once a read of storage again starts, or once the store is gone. A selection made while the
     * store was not settled (a share's hold) is never current. */
    bool selectionToken(int selection, RecordingSelectionToken* token) const;
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
    /** @brief Wait until @p count passes have ended, asking for none (so a pass running is not told another is asked
     * for, which a limit walk takes as its count gone stale). Test hook. */
    void waitForPassCountForTest(quint64 count);
    /** @brief The passes ended so far. Test hook. */
    quint64 passCountForTest() const;
    /** @brief Pass every @p activeMs while a session runs, else every @p idleMs; before start(). Test hook. */
    void setTimingForTest(int activeMs, int idleMs);
    /** @brief How long a folder whose folder.json or journal could not be read waits before its next read (30 s by
     * default). Test hook. */
    void setReadRetryForTest(int ms);
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
    /** @brief Call @p hook on the worker as a root is read whole (at the first pass, or back from away), once its
     * folders are listed and their folder.json read, before the table is installed; before start(). Test hook. */
    void setRootReadHookForTest(std::function<void(const QString& root)> hook);
    /** @brief Call @p hook on the worker after a folder's new journal lines are read, before their files are looked
     * at; before start(). Test hook. */
    void setTailHookForTest(std::function<void(const QString& folder)> hook);
    /** @brief Call @p hook on the worker after each slice of a folder's journal is applied, with the lock released;
     * before start() or between passes. Test hook. */
    void setSliceHookForTest(std::function<void(const QString& folder)> hook);
    /** @brief Call @p hook in a playlist's admission (recordingsForFragmentLists()) after its look at the storage
     * roots, with the lock released, before the store is looked at again; on the reading thread. Test hook. */
    void setAdmissionProbeHookForTest(std::function<void()> hook);
    /** @brief Call @p hook on the worker, under the lock, each time a sum takes its copy of the folder table, with
     * whether that copy still shares the table's storage (it never should: foldersCopyLocked()). Test hook. */
    void setFolderCopyHookForTest(std::function<void(bool shared)> hook);
    /** @brief Call @p hook on the worker before each recording a recheck looks at (recheckRecordings(),
     * recheckPaths(), the limit's look at what it counts), with the lock released; between passes. Test hook. */
    void setEntryCheckHookForTest(std::function<void(const QString& id)> hook);
    /** @brief Call @p hook on the worker as each pass ends, the lock released, before the next pass can begin; between
     * passes. Test hook. */
    void setPassEndHookForTest(std::function<void()> hook);
    /** @brief The ids waiting to be deleted, sorted, and none waiting after. Test hook. */
    QStringList takeDeleteQueueForTest();
    /** @brief Put the entry under @p id at @p path, as a read again that indexed the id elsewhere would. Test hook. */
    void moveEntryPathForTest(const QString& id, const QString& path);
    /** @brief How many times a History read has taken the store's lock (ReaderLock). Test hook. */
    int readerLocksForTest() const;
#endif

  Q_SIGNALS:
    void changed();
    void limitMbChanged();
    void revisionChanged();
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
        /* The Signal search detection it was heard on; empty for none. */
        QString search;
        quint64 order = 0U;
        /* Its file as the read that indexed it saw it (dsd_path_file_identity()): a delete removes only that file. */
        dsd_file_identity_t identity{};
        /* Its name could not be read (no access, an I/O error) when it was indexed: kept with its journal line, never
         * deleted, counted, exported, played or shared, until a read of its folder sees the file. */
        bool unreadable = false;
    };

    /* A folder as the index held it before a read of it again (loadFolder() with replace), which drops its entries and
     * rebuilds them a slice at a time: until the read is complete, the GUI's reads take the folder from here, never from
     * the half-rebuilt index (an open detection list would close itself, a History row lose its recordings). */
    struct CommittedFolder {
        QHash<QString, Entry> entries;
        QHash<QString, QStringList> byFragment;
        QHash<QString, QStringList> bySearch;
        QHash<QString, QString> byPath;
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
        /* Session starts that adopted, made or relabelled the folder at this path: a read that saw another count
         * commits nothing (loadFolder()). */
        quint64 starts = 0U;
        /* When a read that failed (folder.json or journal) is tried again (dsd_realtime_mono_ms()); 0 at once. */
        qint64 retryAtMs = 0;
    };

    /* regular: a regular file, read; missing: nothing at the name; unreadable: the read failed for a reason that says
     * nothing about the file (no access to the name, an I/O error); none of them: no file (a link, a folder, a FIFO). */
    struct FileStat {
        bool regular = false;
        bool missing = false;
        bool unreadable = false;
        qint64 size = 0;
        qint64 mtime = 0;
        dsd_file_identity_t identity{};
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
        int deletesCancelled = 0;
        int deletesSkipped = 0;
        QString lastDeleteError;

        /* Whether anything the GUI shows differs from @p other. */
        bool differsFrom(const Summary& other) const;
    };

    /* What one worker pass changed. */
    struct Pass {
        QSet<QString> fragments;
        bool published = false;
        bool rebuilt = false;
        bool dirty = false;
        /* Storage read again: a root listed (rebuildRoot()) or a folder read (loadFolder()). Drops the deletes asked. */
        bool reread = false;
    };

    /* Removed: unlinked. Gone: already gone, now out of the index. Held: waits for a hold. Kept: not the store's to
     * remove (left on disk, out of the index) or this process's temp file. Failed: the unlink failed; still indexed.
     * Changed: the file at its path is not the one the request (or, for the limit, the index) knew; left alone. */
    /* Stale: a removal for the limit found the index it counted no longer whole (storageReadLocked()). */
    /* Unreadable: the recording's name could not be read (now, or when it was indexed or rechecked): kept, skipped. */
    enum class DeleteResult { Removed, Gone, Held, Kept, Failed, Changed, Stale, Unreadable };
    enum class DeleteCause { Asked, Limit };
    /* What is at a recording's path, without following a link there: nothing; a regular file inside an available root,
     * which the store may remove; anything else, which it leaves alone; or (onDiskAsKnownLocked()) a removable file that
     * is not the one known, which it leaves alone too, or a name that cannot be read, which it never removes. */
    enum class OnDisk { Missing, Removable, Foreign, Replaced, Unreadable };
    /* What a folder name an owner's new folder would take, already there, comes to: taken for the owner, or the next
     * name, or the next root (the owner's, taking no new files). */
    enum class CandidateFolder { Taken, NextName, NextRoot };

    /* A recording the limit may remove, and what it goes in the order of. */
    struct LimitCandidate {
        QString id;
        qint64 bytes = 0;
        qint64 start = 0;
        quint64 order = 0U;
    };

    /* What the limit counts: its usage of the roots it counts, and the recordings in it. */
    struct LimitUsage {
        qint64 limit = 0;
        qint64 total = 0;
        /* The roots the total counts. */
        QList<bool> rootAvailable;
        /* Before the files were looked at, the ids to look at; after, the recordings the limit may remove. */
        QStringList counted;
        QList<LimitCandidate> candidates;
    };

    /* What the store needs of one file, read without following a symlink in its place. */
    /* What is at @p path, never following a link: a regular file's size, modification time and identity. */
    static FileStat statOf(const QString& path);
    /* @p journalPath read whole into @p data; true for one read to its end, or a name confirmed missing (nothing
     * published yet), false for anything else (a read that failed, a link, a name that cannot be read). */
    static bool readJournal(const QString& journalPath, QByteArray* data);
    /* loadFolder()'s read of @p path's folder.json into @p folder: true to go on with the owner read; false when the
     * load ends (not readable now: read again later; no folder of the app's: dropped, unless a start adopted it since
     * @p starts). */
    bool readFolderOwner(const QString& path, int root, quint64 starts, Folder* folder, Pass* pass);
    static bool rootAvailableNow(const QString& root);
    /* Whether @p root is an existing directory that can be read now (R77): a playlist's admission's look. Not
     * rootAvailableNow(), whose root that can be made counts for a session start, not for recordings already in it. */
    static bool rootPlayableNow(const QString& root);
    /* Whether root @p root (at @p path) is there for the index (R83): an existing readable directory, or a missing one
     * a session could make (rootAvailableNow()) while the index holds no row on it (m2). So a root never made, or one
     * whose folders hold no recording, reads as there and empty, and one whose directory went (renamed, removed) with
     * rows indexed holds them away, never dropped. Takes the lock only when the directory is missing; the Locked form
     * is for a caller holding it. */
    bool rootThereForIndex(int root, const QString& path) const;
    /* rootThereForIndex() for a root whose directory is not a readable one: one a session could make, with no row
     * indexed on it. Takes the lock for the row look. */
    bool rootMissingTakenAsThere(int root, const QString& path) const;
    /* refreshRoots()'s look at @p root (at @p path): @p there whether it is there for the index, and true when this
     * pass lists it in full (the first pass, back from away, seen away, or its missing directory there now, I1). The
     * missing flag is written under the lock from a look no session start overtook, and a seen-away note made since
     * the pass's top is taken into this listing (m1). */
    bool listRootNow(int root, const QString& path, bool first, bool was, QSet<int>* seenAway, bool* there);
    bool rootThereForIndexLocked(int root) const;
    /* Whether the index holds a row on @p root: an entry in a folder on it (m_rowsInFolder), or one a folder's read
     * again holds from before it (m_committed). Lock held. */
    bool rootHoldsRowsLocked(int root) const;
    /* A session start made @p root's directory (it was missing): with rows indexed on it (I2), it is listed again in
     * full at once, its old rows unavailable until then; with none, nothing is left to list. Lock held. */
    void noteRootMadeLocked(int root);
    /* Whether the regular file at @p path opens to read, as a transfer opens it (dsd_fopen_existing_regular_file()). */
    static bool opensToRead(const QString& path);
    /* False for @p path, read as @p stat, while it is a recording a transfer could not open (m_unopenable) and it still
     * does not open; true otherwise, and then it is forgotten. Lock held. */
    bool opensAgainLocked(const QString& path, const FileStat& stat);
    static qint64 parseLines(const QByteArray& data, QList<RecordingJournalRecord>* records);
    /* Each file @p records publishes into @p files (one the folder's listing read already is kept). */
    static void statPublished(const QString& folder, const QList<RecordingJournalRecord>& records,
                              QHash<QString, FileStat>* files);

    void run();
    void runPass(bool first);
    /* The limit, when it is due, then the free space, when it is due. */
    void checkLimitAndSpace(bool first, Pass* pass);
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
    /* False, installing nothing, when the root left again as it was listed. */
    bool rebuildRoot(int root, Pass* pass);
    /* @p root's folder table, as rebuildRoot() listed it, in place of what the index held for the root. Under the
     * store's lock. */
    void installRootTableLocked(int root, QList<Folder>& table, Pass* pass);
    /* A session start put @p path into the folder table or relabelled it: told to a listing of its root going on now.
     * Under the store's lock. */
    void noteFolderAddedLocked(const QString& path, int root);
    /* For installRootTableLocked(): the folders a session start touched during @p root's listing keep the start's owner
     * and label in @p table; the ones the listing never saw (made new) leave @p stale, and are returned: their earlier
     * recordings at that path go. Under the store's lock. */
    QSet<QString> keepSessionFoldersLocked(int root, QList<Folder>* table, QSet<QString>* stale);
    /* A pass's first read of storage again: Pass::reread, and every selection made before it goes stale at once. */
    void noteRereadStart(Pass* pass);
    /* The journals and files of the folders the table lists but nobody read yet. */
    void loadPendingFolders(Pass* pass);
    /* Read a folder whole: its folder.json, its files and its journal. @p replace drops what the index held for it,
     * which the GUI's reads keep seeing until the read is complete. */
    void loadFolder(const QString& path, int root, bool replace, Pass* pass);
    /* The files of the folder at @p path: plain ones into @p files, and an earlier process's temp files as unfinished
     * recordings. */
    void listFolder(const QString& path, QHash<QString, FileStat>* files, QList<Entry>* unfinished) const;
    /* applyRecordLocked() for each of @p records, taking the lock a slice at a time. */
    void applyRecordsInSlices(const Folder& folder, const QList<RecordingJournalRecord>& records,
                              const QHash<QString, FileStat>& files, Pass* pass);
    void tailFolders(Pass* pass);
    /* @p path's new journal lines from @p offset, applied only while the folder is still read with the session starts
     * count @p starts it had when its cursor was taken. */
    void tailFolder(const QString& path, qint64 offset, quint64 starts, Pass* pass);
    /* Whether @p path is still read, at @p offset, with @p starts: the tail's lines may be applied. Lock held. */
    bool tailCursorCurrentLocked(const QString& path, qint64 offset, quint64 starts) const;
    void applyRecordLocked(const Folder& folder, const RecordingJournalRecord& record,
                           const QHash<QString, FileStat>& files, Pass* pass);
    void applyPublishLocked(const Folder& folder, const RecordingJournalRecord& record,
                            const QHash<QString, FileStat>& files, Pass* pass);
    void applyLinkLocked(const RecordingJournalRecord& record, Pass* pass);
    void insertEntryLocked(Entry entry, Pass* pass);
    void unindexFragmentLocked(const Entry& entry, Pass* pass);
    /* What a query reads changed: the pass delivers, and the revision moves. Under the store's lock. */
    void noteQueryChangeLocked(Pass* pass);
    void removeEntryLocked(const QString& id, Pass* pass);
    void removeFolderEntriesLocked(const QString& folder, Pass* pass);
    /* Keep what the index holds of @p folder for the GUI's reads until commitFolderLocked(). */
    void holdCommittedFolderLocked(const QString& folder);
    /* The read of @p folder again is complete: the GUI reads it from the index again. */
    void commitFolderLocked(const QString& folder, Pass* pass);
    /* The GUI's reads, under the lock: the entry with @p id, the id indexed at @p path, and the entries indexed under
     * @p key (a fragment, or with @p bySearch a detection), each a folder being read again giving what it held. */
    const Entry* readerEntryLocked(const QString& id) const;
    QString readerIdForPathLocked(const QString& path) const;
    QList<const Entry*> readerEntriesLocked(const QString& key, bool bySearch) const;
    /* The recordings of a History row's @p fragments, as the GUI reads them: each once (by a set), oldest first (start,
     * then publish order). */
    QList<const Entry*> fragmentEntriesLocked(const QStringList& fragments) const;
    bool folderAvailableLocked(const QString& folder) const;
    /* A root seen away (R74): found missing outside a pass, or back and not read again yet, from that first look
     * through the pass's read of it. */
    bool rootSeenAwayLocked(int root) const;

    /* One root a playlist's admission admits from, as the store held it when the admission took its roots (R77). */
    struct AdmissionRoot {
        QString path;
        bool known = false;     // m_rootAvailable
        bool pending = false;   // in m_rootsSeenAway: a listing in full asked for, not begun
        bool rereading = false; // in m_rootsRereading
        bool listing = false;   // in m_listing: rebuildRoot() is listing it now
        bool missing = false;   // m_rootDirMissing: there and empty only because its directory is missing (item 5)

        bool
        operator==(const AdmissionRoot& other) const {
            return path == other.path && known == other.known && pending == other.pending
                   && rereading == other.rereading && listing == other.listing && missing == other.missing;
        }
    };

    /* The roots the recordings of @p found are on, and every root with its missing flag set (one whose folder may be
     * back, holding recordings the index has not seen), each as the store holds it. Under the store's lock. */
    QHash<int, AdmissionRoot> admissionRootsLocked(const QList<QList<const Entry*>>& found) const;
    /* A playlist's admission records what its look found, @p playable (rootPlayableNow()) for each root of @p roots:
     * true when a pass was asked for (a root gone was noted seen away, or one back asks for its read again). Under the
     * store's lock, which the look itself never held. */
    bool admissionLookRecordedLocked(const QHash<int, AdmissionRoot>& roots, const QHash<int, bool>& playable) const;
    /* Whether @p folder has been read (Folder::loaded): an unread one offers no recording path to any reader. */
    bool folderReadLocked(const QString& folder) const;
    /* The store is ready, the worker has no pass running or asked for (a pass reads storage, rechecks files and
     * applies deletes from start to end), and every folder on available storage has had its read (none adopted by a
     * session start or back with its storage still waiting for it). Exports and delete requests wait for it. */
    bool storageSettledLocked() const;
    /* storageSettledLocked() within a pass: no pass asked for, and every folder on storage that is there read. */
    bool storageReadLocked() const;
    /* Whether a root the index holds as away is back now (checkRootsLocked()): a pass is asked for to read it, and the
     * caller wakes the worker and refuses its export or save as loading. Under the store's lock. */
    bool awayRootsBackLocked();
    /* Queue @p ids that are indexed for deletion; false, queuing nothing, when any is on storage not there now
     * (m_deleteRefusal says so). @p wake: a pass was asked for. The caller checked storageSettledLocked(). */
    bool queueDeletesLocked(const QStringList& ids, bool* wake);
    /* @p roots as they are now, for a delete request: @p away when one is missing, @p unread when one is back but not
     * read again since (marked away, or found missing by a request); true when a pass must read one (m_rootsSeenAway). */
    bool checkRootsLocked(const QList<int>& roots, bool* away, bool* unread);
    /* One delete request: refused unless storageSettledLocked() (and, for a @p bulk one, no root held as away is back:
     * awayRootsBackLocked()), else the ids @p collect gives (under the lock) are queued by queueDeletesLocked(). */
    bool requestDeletes(bool bulk, const std::function<QStringList()>& collect);
    QString folderUidLocked(const QString& folder) const;
    bool liveTemp(const QString& path) const;
    bool stopping() const;
    /* Let History reads waiting for the lock take it before the worker takes it again (the mutex is not fair). */
    void yieldToReaders() const;
    void processRechecks(Pass* pass);
    /* Look at the files of @p ids again: one confirmed gone, or no recording file, leaves the index (kept when its root
     * went: the storage-loss rule; kept when its name cannot be read), one still there has its size read again, and
     * with @p readChanged (a recheck the host asked for) another file than the index knew reads its folder again. */
    void recheckEntries(const QStringList& ids, Pass* pass, bool readChanged);
    /* recheckEntries() for @p entry, a regular file now as @p stat reads it. Under the store's lock. */
    void recheckRegularLocked(Entry& entry, const FileStat& stat, bool readChanged, Pass* pass);
    /* Read @p path whole again at the next pass, with what the index held for it kept until then; the store is not
     * settled meanwhile. Under the store's lock. */
    void scheduleFolderReadLocked(const QString& path, Pass* pass);
    /* @p path read whole again before any request is taken (loaded false, what the index holds kept). Lock held. */
    void markFolderUnreadLocked(const QString& path);
    /* scheduleFolderReadLocked() after a read that failed: tried again once m_readRetryMs has passed. Lock held. */
    void deferFolderReadLocked(const QString& path, Pass* pass);
    /* A session start found @p root missing: listed in full when it is back, deletes held, selections stale. Lock
     * held. */
    void noteRootSeenAwayLocked(int root) const;
    /* A recheck could not read @p id's name: kept, its journal line with it, and out of every index, count and usage
     * (Entry::unreadable). Under the store's lock. */
    void markEntryUnreadableLocked(const QString& id, Pass* pass);
    /* @p entry out of the detection index. Under the store's lock. */
    void unindexSearchLocked(const Entry& entry);
    /* tailFolder() could not read @p path's new journal lines: read whole at the next pass (its root still there). */
    void noteTailReadFailed(const QString& path, Pass* pass);
    void processDeletes(Pass* pass);

    /* @p counted: the roots a limit's total counts, each checked again before the unlink (Held when one is gone). */
    /* A delete asked: the file it names, by its identity and its path when the request was accepted (R68). */
    struct DeleteAsk {
        dsd_file_identity_t identity{};
        QString path;
    };

    /* @p expected: the file a user's delete was asked of (its identity and path when the request was accepted); null
     * for the limit's, which removes the file the index knows. One whose entry has another path now is not it. */
    DeleteResult deleteEntry(const QString& id, DeleteCause cause, Pass* pass, const QList<bool>* counted = nullptr,
                             const DeleteAsk* expected = nullptr, qint64* foundBytes = nullptr);
    /* A recording not removable as found (@p found: missing, or not the store's to remove) leaves the index, unless
     * what is there changed meanwhile. Under the store's lock. */
    DeleteResult dropUnremovableLocked(const QString& id, OnDisk found, Pass* pass);
    OnDisk onDiskLocked(const QString& path) const;
    /* onDiskLocked(), and Replaced when a removable file there is not the one @p known names (its size or time differs),
     * or when the name cannot be read at all. */
    OnDisk onDiskAsKnownLocked(const QString& path, const dsd_file_identity_t& known, qint64* bytes = nullptr) const;

    /* What deleteEntry() settles after the lock when the unlink did not decide: what is at the name, its root, and the
     * failure's reason. */
    struct UnlinkAttempt {
        OnDisk found = OnDisk::Removable;
        int root = -1;
        QString error;
        /* The size of the regular file the check found at the name; -1 when it found none. */
        qint64 bytes = -1;
    };

    /* deleteEntry()'s step under the lock: the holds, the check and the unlink. Failed when deleteEntry() still has to
     * settle it from @p attempt (a missing or foreign name, a refused or failed removal). */
    DeleteResult unlinkEntryLocked(const QString& id, const QString& path, bool uploading, DeleteCause cause,
                                   const DeleteAsk* expected, Pass* pass, UnlinkAttempt* attempt);
    /* unlinkEntryLocked()'s refusals before it looks at the name: what decides for @p entry (at @p path when it was
     * taken) instead of the unlink, or Failed when none does. */
    DeleteResult unlinkRefusedLocked(const Entry& entry, const QString& path, bool uploading, DeleteCause cause,
                                     const DeleteAsk* expected) const;
    /* The player, an export or a save holds @p path. */
    bool heldLocked(const QString& path) const;
    void noteDeleteFailure(DeleteCause cause, const QString& error);
    void enforceLimit(Pass* pass);
    /* The folder table, copied whole under the store's lock, for the worker to read once the lock is let go. A shared
     * copy would not do: a session start on the GUI thread writes the table, and Qt decides a QHash is no longer shared
     * with a relaxed load, which orders none of the worker's reads of the shared table before that write in place. */
    QHash<QString, Folder> foldersCopyLocked() const;
    /* The limit's usage now; false when nothing is to go (no limit the user set, or under it). Once @p looked, the
     * recordings it may remove; before, the ids of the files to look at first. */
    bool countLimitUsage(bool looked, LimitUsage* usage);
    /* Remove @p usage's candidates in order until its total is down to @p target; true when a root marked away
     * meanwhile means it must be counted again. */
    bool removeForLimit(LimitUsage* usage, qint64 target, Pass* pass, int* removed, bool* held);
    /* Whether a session start could make a folder in @p root now. */
    static bool rootWritableNow(const QString& root);
    /* Where recordings go: the active session's root, or when idle the root a session start would take now. */
    QString spaceRoot() const;
    bool checkFreeSpace();
    void compactFolders();
    bool compactFolder(const QString& path);
    static RecordingJournalRecord journalRecordOf(const Entry& entry);
    /* Rewrite the journal at @p journalPath with one publish per @p live recording, when nothing but a torn line lies
     * past @p offset; @p written is the bytes it holds then. */
    /* @p stillDue is asked right before the rewrite is committed; false cancels it. */
    static bool rewriteJournal(const QString& journalPath, qint64 offset, const QList<Entry>& live, qint64* written,
                               const std::function<bool()>& stillDue);
    /* Whether @p path's journal may be compacted: read, on available storage not seen away since. Lock held. */
    bool compactableLocked(const QString& path) const;
    /* summarize() for an entry no listed recording: an unfinished one counted as such, one whose name cannot be read
     * not at all. True when @p entry is one of them. */
    static bool tallyUnlisted(const Entry& entry, Summary* summary);
    static Summary summarize(const QHash<QString, Entry>& entries, const QHash<QString, Folder>& folders,
                             const QList<bool>& rootAvailable);
    void publish(const Pass& pass);
    void deliver();
    QString folderLocked(const QString& kind, const QString& uid, const QString& name);
    /* folderLocked()'s steps, under the store's lock: the roots usable now; the owner's folder (its path, @p gone
     * listing the unread folders no longer its); that folder, relabelled; a folder name already there, taken or not;
     * and a new folder. */
    QList<bool> usableRootsLocked();
    QString ownerFolderPathLocked(const QString& uid, QList<bool>* usable, QStringList* gone, QStringList* unread);
    /* Whether @p folder's folder.json still names @p uid; one gone with its root marks the root unusable (a pass asked
     * for), one no longer the owner's and never read goes to @p gone, one that cannot be read to @p unread (its
     * folder is read again before any request). Under the store's lock. */
    bool ownerFolderOnDiskLocked(const Folder& folder, const QString& uid, QList<bool>* usable, QStringList* gone,
                                 QStringList* unread);
    QString existingFolderLocked(const QString& kind, const QString& uid, const QString& name, QList<bool>* usable);
    CandidateFolder takeCandidateFolderLocked(const QString& path, int root, const QString& uid, const QFileInfo& info);
    bool makeFolderLocked(const QString& path, int root, const QString& kind, const QString& uid, const QString& name);
    QString newFolderLocked(const QString& kind, const QString& uid, const QString& name, const QList<bool>& usable);
    QVariantMap entryMap(const Entry& entry) const;
    /* entryMap() and what a detection's list shows of a recording. */
    QVariantMap detectionEntryMap(const Entry& entry) const;
    QVariantMap selectLocked(QList<const Entry*> entries);
    /* selectLocked() of the finished recordings @p ids names, whenever (selectRecordings() checks first). */
    QVariantMap selectIdsLocked(const QStringList& ids);

    /* GUI thread: the last summary the worker delivered, and the generation it came with. */
    Summary m_summary;
    quint64 m_revision = 0U;
    int m_limitMb = kDefaultLimitMb;

    class ReaderLock;

    std::thread m_thread;
    mutable std::mutex m_mutex;
    /* History reads waiting for m_mutex (ReaderLock). */
    mutable std::atomic<int> m_readersWaiting{0};
#ifdef DSD_NEO_TEST_HOOKS
    /* History reads that took m_mutex (readerLocksForTest()). */
    mutable std::atomic<int> m_readerLocks{0};
#endif
    /* Mutable: a const read that records what it observed (the playlist read's live root look) wakes the worker. */
    mutable std::condition_variable m_wake;
    std::condition_variable m_passed;
    std::condition_variable m_settled;

    /* Under m_mutex. */
    QStringList m_roots;
    QList<bool> m_rootAvailable;
    /* Roots indexed as there and empty only because their directory is missing (I1: missing at a cold start, nothing
     * indexed on them): one found a readable directory since is listed in full at once, as a root back from away is.
     * Cleared when it is listed, or when a session start makes the directory itself. */
    QList<bool> m_rootDirMissing;
    /* How many entries each folder holds (insertEntryLocked(), removeEntryLocked()): whether a root holds rows without
     * a walk of every entry. */
    QHash<QString, int> m_rowsInFolder;
    /* How many times a session start made each root's directory (noteRootMadeLocked()): a pass's look at a root, taken
     * without the lock, writes its missing flag only if no start made the directory meanwhile (m1). */
    QList<quint64> m_rootMade;
    QHash<QString, Folder> m_folders;
    QHash<QString, Entry> m_entries;
    QHash<QString, QStringList> m_byFragment;
    /* Detection key -> ids. Changed only where an entry is inserted or removed, never by a link: storage going away
     * keeps an entry, and so its detection. */
    QHash<QString, QStringList> m_bySearch;
    QHash<QString, QString> m_byPath;
    /* Folder path -> what it held, for each folder being read again (CommittedFolder). */
    QHash<QString, CommittedFolder> m_committed;
    QSet<QString> m_active;
    /* Why the last delete request refused (deleteRefusal()). */
    enum class DeleteRefusal { None, Loading, StorageAway, Changed, Gone };
    DeleteRefusal m_deleteRefusal = DeleteRefusal::None;
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

    /* An export's or a save's files, m_storageReads when they were selected, and whether the store was settled then
     * (only such a selection is transferred). */
    struct Selection {
        QStringList paths;
        quint64 storageReads = 0U;
        bool settled = false;
    };

    QHash<int, Selection> m_selections;
    /* Reads of storage again started (noteRereadStart()), and one more as the store goes: a selection made before one
     * is not transferred. Shared with the tokens transfers carry (selectionToken()), which outlive the store. Changed
     * under the store's lock. */
    const std::shared_ptr<std::atomic<quint64>> m_storageReads = std::make_shared<std::atomic<quint64>>(0U);
    int m_nextSelection = 1;
    QString m_playing;
    /* Each delete asked and not done yet, with its file's identity and path as the index knew them when the request was
     * accepted. */
    QHash<QString, DeleteAsk> m_deleteQueue;
    /* Roots found missing outside a pass (a delete request's or export's look, rootThereForIndexLocked(); a playlist's
     * admission, rootPlayableNow(); a session start's, rootAvailableNow()), back before a pass read them, or whose
     * directory a session start made over indexed folders (I2): each is listed again in full by the next pass
     * (refreshRoots()), whatever m_rootAvailable says. */
    /* Mutable, under m_mutex: a const read that records what it observed (the playlist read's live root look). */
    mutable QSet<int> m_rootsSeenAway;
    /* The seen-away roots the pass under way took into refreshRoots() and has not finished reading yet (R74): their
     * recordings stay unavailable to every reader until that read is done, as while they wait in m_rootsSeenAway. */
    QSet<int> m_rootsRereading;
    /* Paths of recordings a transfer could not open (recheckRegularLocked()): unreadable until an open succeeds. */
    QSet<QString> m_unopenable;
    /* The roots rebuildRoot() is listing now, each with the folders a session start made, took or relabelled on it since
     * the listing began (keepSessionFoldersLocked()). */
    QHash<int, QSet<QString>> m_listing;
    /* Accepted deletes a read again of their storage dropped (deletesCancelled()), since the app started. */
    int m_deletesCancelled = 0;
    /* Deletes not done because the file was not the one recorded (deletesSkipped()), since the app started. */
    int m_deletesSkipped = 0;
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
    /* Mutable, under m_mutex: a const read that records what it observed asks for the pass that reads it again. */
    mutable bool m_passRequested = false;
    bool m_inPass = false;
    /* A pass's end hook running (setPassEndHookForTest()): the pass is not counted done until it returns. */
    bool m_passEnding = false;
    quint64 m_passes = 0U;
    int m_activeMs = 1000;
    int m_idleMs = 60000;
    /* deferFolderReadLocked()'s wait before a failed folder read is tried again. */
    qint64 m_readRetryMs = 30000;
    quint64 m_nextOrder = 1U;
    int m_compactions = 0;
    qint64 m_rebuildNs = 0;
    /* Bumped at every change a query can see (noteQueryChangeLocked()). */
    quint64 m_generation = 0U;
    /* The latest summary and the fragments changed since the GUI last took them; one delivery queued at a time, so a
     * GUI thread paused in the background never collects more than one. The generation goes with the latest summary:
     * a later publication never carries an older one, so coalescing never loses a change. */
    Summary m_outbox;
    quint64 m_outGeneration = 0U;
    QSet<QString> m_outFragments;
    bool m_deliveryQueued = false;

    /* GUI thread: the totals acknowledgeNotices() took, which the notice readings count from. */
    int m_removedByLimitSeen = 0;
    int m_deleteFailuresSeen = 0;
    int m_deletesCancelledSeen = 0;
    int m_deletesSkippedSeen = 0;

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
    std::function<void(const QString&)> m_sliceHook;
    std::function<void()> m_admissionProbeHook; // set and copied under m_mutex: called on the reading thread
    std::function<void(bool)> m_folderCopyHook;
    std::function<void(const QString&)> m_entryCheckHook;
    std::function<void()> m_passEndHook;
    int m_removedByLimit = 0;
    int m_deleteFailures = 0;
    QString m_lastDeleteError;
    /* One warning per run for each cause; later failures are counted. */
    bool m_deleteFailureLogged[2] = {false, false};
    quint64 m_lastFreeCheckMs = 0U;
    /* What spaceRoot() followed at the last free-space reading: a session in use, and each root there. */
    bool m_lastFreeActive = false;
    QList<bool> m_lastFreeRoots;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RECORDING_STORE_H_ */
