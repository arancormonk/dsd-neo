// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The export writer (#826): an exported file staged in the cache, then saved into the document a picker chose.
 *
 * One job at a time runs on a worker thread of the writer's own: it writes the bytes to `<token>.csv` in the writer's
 * staging folder, hands that file to the save transport, and removes it whatever happened. Each writer stages in a
 * folder of its own, `<cacheRoot>/<random>/`, and holds the lock file `<cacheRoot>/<random>.lock` for its whole life,
 * so no two writers (in this process or another) share a path. A writer removes, when made, every other folder under
 * the cache root whose lock it can take: one no live writer holds.
 *
 * Owner-thread only: make, use and destroy a writer on the thread its owner lives on; the reply runs there too.
 */

#ifndef DSD_NEO_SRC_UI_QT_DOCUMENT_EXPORT_WRITER_H_
#define DSD_NEO_SRC_UI_QT_DOCUMENT_EXPORT_WRITER_H_

#include <QString>
#include <functional>
#include <memory>

class QByteArray;
class QLockFile;
class QObject;
class QThreadPool;

namespace dsd_qt {

class DocumentSaveTransport;

class DocumentExportWriter {
  public:
    /** Runs on the owner's thread, once per accepted submit(), unless the writer is destroyed first. */
    using Reply = std::function<void(int token, bool ok)>;

    /** How long the destructor waits for the job in progress before it leaves it behind. */
    static constexpr int kStopWaitMs = 2000;

    /**
     * @param transport Saves the staged file into the picked document; the jobs keep it, so it may outlive the host
     *        that handed it out.
     * @param cacheRoot The folder every writer stages under (the model passes `<CacheLocation>/signal-search-export`).
     * @param owner Where @p reply is delivered: queued to its thread. It must outlive the writer.
     * @param reply Told each accepted job's outcome.
     */
    DocumentExportWriter(std::shared_ptr<DocumentSaveTransport> transport, const QString& cacheRoot, QObject* owner,
                         Reply reply);
    /**
     * No reply runs from here on. A job in progress is stopped (the transport is aborted: teardown only) and waited
     * for up to kStopWaitMs. A job blocked past that (a provider that ignores the abort) is left to finish on a worker
     * that is never deleted; the writer's folder and lock stay until the process ends, and the next writer's sweep
     * removes them. Otherwise the folder and the lock go now.
     */
    ~DocumentExportWriter();

    DocumentExportWriter(const DocumentExportWriter&) = delete;
    DocumentExportWriter& operator=(const DocumentExportWriter&) = delete;

    /**
     * @brief Stage @p bytes and save them into the document @p reference names, on the worker.
     *
     * @p token names the job in its reply and its staging file, so it must be unique for this writer. A job that
     * cannot stage the file (no folder, no lock, an open, write or commit failure) lets go of the reference's
     * Download-folder row through the transport and replies false.
     *
     * @return false, with the reference's Download-folder row let go of at once and no reply, while a job runs or once
     *         the transport was aborted (the app is closing).
     */
    bool submit(int token, const QByteArray& bytes, const QString& reference);

    /** @brief Whether a job was accepted and its reply has not run yet. */
    bool busy() const;

  private:
    struct Link;

    /** Take the writer's lock, once more when an earlier try failed (a cache root that could not be made). */
    bool holdLock();
    void finish(int token, bool ok);
    static void deliver(const std::shared_ptr<Link>& link, int token, bool ok);

    std::shared_ptr<DocumentSaveTransport> m_transport;
    QString m_root;
    QString m_folder;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<QThreadPool> m_pool;
    std::shared_ptr<Link> m_link;
    Reply m_reply;
    bool m_busy = false;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_DOCUMENT_EXPORT_WRITER_H_
