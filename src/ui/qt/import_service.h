// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The import worker (#832): picked documents copy and check off the Qt thread, then commit on it.
 *
 * Each request stages on one worker thread, in a private folder of its own under stagingRoot(), and commits on the Qt
 * thread. The library, the replay store and the backup's restore share one service. The commit journal records the directories a commit
 * creates until the index that names them is saved, so the next start can remove the ones a crash left unnamed.
 *
 * A document read (#831, the backup's restore) is a request of another kind: the worker copies a document into a file
 * its caller names, with no staging folder, and the commit hands back the copy's result.
 */

#ifndef DSD_NEO_SRC_UI_QT_IMPORT_SERVICE_H_
#define DSD_NEO_SRC_UI_QT_IMPORT_SERVICE_H_

#include <QHash>
#include <QMap> // IWYU pragma: keep -- QVariantMap is a QMap; the stage and commit signatures carry it by value.
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>
#include <functional>
#include <memory>

#include "document_import_transport.h"

class QDir;
class QThreadPool;

namespace dsd_qt {

/** @brief What a request's stage function gets, on the import worker. */
struct ImportJob {
    /** The request: the id submit() returned and the transport's request id. */
    qint64 id = 0;
    /**
     * The request's own folder, empty and readable by the owner only; removed once the commit returns. Empty for a
     * request of a kind that stages nothing here (a document read).
     */
    QString stagingDir;
    /** The service's transport: copy(id, …) for each document the request reads. */
    std::shared_ptr<DocumentImportTransport> transport;
    /** Whether the request was cancelled; a stage may stop early, as its commit is told it was cancelled. */
    std::function<bool()> cancelled;
};

class ImportService : public QObject {
    Q_OBJECT

  public:
    /**
     * Runs on the import worker and touches no QObject; its captures are destroyed there too. Its map goes to the
     * request's commit.
     */
    using Stage = std::function<QVariantMap(const ImportJob&)>;
    /** Runs on the Qt thread, once per request; @p cancelled when the request was cancelled before it ran. */
    using Commit = std::function<void(qint64 id, const QVariantMap& staged, bool cancelled)>;
    /** A document read's commit, on the Qt thread: the copy's DecoderHost::DocumentCopy value. */
    using ReadCommit = std::function<void(qint64 id, int copied)>;

    /** How long the destructor waits for the request in progress before it leaves it behind. */
    static constexpr int kStopWaitMs = 2000;
    /** How many names a publish tries before it gives up: a free name can still be taken (by another process)
     * before the rename claims it. */
    static constexpr int kPublishAttempts = 16;

    explicit ImportService(std::shared_ptr<DocumentImportTransport> transport, QObject* parent = nullptr);
    /**
     * Cancels every request and waits up to kStopWaitMs for the worker. A copy blocked past that (a provider that
     * ignores the cancel) is left to finish on a worker that is never deleted; it keeps its transport, never calls
     * this service again, and its staging folder goes when it ends (or at the next start). No commit runs.
     */
    ~ImportService() override;

    ImportService(const ImportService&) = delete;
    ImportService& operator=(const ImportService&) = delete;

    /** @brief `<AppData>/import-staging`: every request's staging folder, and nothing else. */
    static QString stagingRoot();
    /**
     * @brief Remove stagingRoot() wholly: what an earlier run left there is stale.
     *
     * Call it once at startup, before any service exists. A link is removed, never followed.
     */
    static void sweepStaging();
    /** @brief `<AppData>/import_journal.json`. */
    static QString journalPath();

    /**
     * @brief Move the staged file @p staged into @p dir under a free form of @p name (unique_import_destination(),
     * skipping @p reserved), never over an existing file, on the Qt thread at a commit.
     *
     * dsd_rename_noreplace() claims the name, so a name taken in between moves on to the next one (a bounded number of
     * tries). @p dir must exist on the staging folder's volume.
     *
     * @return true with @p published set, else false (no free name, or the move failed); @p staged is then left.
     */
    static bool publish(const QString& staged, const QDir& dir, const QString& name, const QSet<QString>& reserved,
                        QString* published);

    /**
     * @brief Queue a request: @p stage runs on the worker (requests run one at a time, in order), then @p commit on
     * the Qt thread, queued.
     *
     * @p commit runs exactly once for the id, cancelled or not; a request cancelled before its stage started skips the
     * stage and commits an empty map. When the request's staging folder cannot be made the stage does not run either,
     * and the commit gets {ok: false, error: "local"}.
     *
     * @return The request id: positive and unique in this process.
     */
    qint64 submit(Stage stage, Commit commit);

    /**
     * @brief Queue a document read: the worker copies the document @p reference names into @p destPath, at most
     * @p maxBytes of it, through the transport, then @p commit runs on the Qt thread with the copy's result.
     *
     * The request takes its turn among submit()'s, with the same ids, cancel(), progress() and teardown, but stages
     * nothing in stagingRoot(): @p destPath is the caller's (a fresh name in a directory that exists), and the service
     * never removes a copy it hands over. @p commit gets DocumentCopied for a whole copy, which is then the caller's,
     * and DocumentCancelled for a cancelled request. For any value but DocumentCopied the caller removes @p destPath:
     * a cancel that lands just after the copy ended leaves the copy there. With no commit to come (teardown), the
     * worker removes a copy that ends after the cancel.
     *
     * @return The request id, as submit()'s.
     */
    qint64 readDocument(const QString& reference, const QString& destPath, qint64 maxBytes, ReadCommit commit);

    /**
     * @brief Run a request at once on the calling thread: @p stage in a staging folder of its own, then @p commit
     * (never cancelled); the folder is removed after the commit.
     *
     * For the synchronous library calls, which share the stage and commit of a submitted request. The request has its
     * own id, as submit()'s; a folder that cannot be made skips the stage and commits {ok: false, error: "local"}.
     */
    void runNow(const Stage& stage, const Commit& commit);

    /**
     * @brief A request id no request uses: for a caller that answers a request at once, without the worker, under an
     * id unique among submit()'s.
     */
    static qint64 newRequestId();

    /**
     * @brief Cancel request @p id: a copy it runs stops, and its commit is told it was cancelled. Once the commit ran,
     * or for an unknown id, it does nothing.
     */
    void cancel(qint64 id);

    /** @brief The latest copy of request @p id; empty once its stage ended or for an unknown id. */
    ImportCopyProgress progress(qint64 id) const;

    /** @brief Whether a request is waiting for its commit. */
    bool busy() const;

    /*
     * The commit journal: directories a commit creates, recorded before it creates them and dropped once the index that
     * names them is saved. Paths are stored clean and absolute. The Qt thread writes it; the file is replaced whole, so
     * journalEntries() may also read it on the import worker (the revision cleanup does).
     */

    /** @brief Record @p directories; false when the journal cannot be read or saved (it is then left as it was). */
    static bool journalAdd(const QStringList& directories);
    /**
     * @brief Drop @p directories; false when the journal cannot be read or saved (it is then left as it was). A journal
     * left empty is removed.
     */
    static bool journalRemove(const QStringList& directories);
    /**
     * @brief The recorded directories. @p ok is false when the journal cannot be read or holds anything but absolute
     * paths: a caller then deletes nothing. A missing journal is empty and ok.
     */
    static QStringList journalEntries(bool* ok);
    /**
     * @brief Move a journal that cannot be read aside to import_journal.json.bad (replacing an older one), so the next
     * commit starts an empty one. What it named stays where it is: nothing proves the app made it. False when the
     * journal could not be moved.
     */
    static bool journalSetAside();

  Q_SIGNALS:
    void busyChanged();

  private:
    struct Shared;
    class Request;
    struct Work;

    struct Pending {
        std::shared_ptr<Request> request;
        Commit commit;
        QString stagingDir;
    };

    /** Queue @p stage and @p commit, with a staging folder of its own when @p staged. */
    qint64 enqueue(Stage stage, Commit commit, bool staged);
    static void runNext(const std::shared_ptr<Shared>& shared);
    static void runJob(const std::shared_ptr<Shared>& shared, const Work& work);
    void finish(qint64 id, const QVariantMap& staged);
    /** Queue the removal of @p dir on the worker, ahead of the requests waiting there. */
    void removeOnWorker(const QString& dir);
    void updateBusy();

    std::shared_ptr<Shared> m_shared;
    std::unique_ptr<QThreadPool> m_pool;
    QHash<qint64, Pending> m_pending;
    bool m_busy = false;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_IMPORT_SERVICE_H_
