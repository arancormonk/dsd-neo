// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "document_export_writer.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1Char>
#include <QList>
#include <QLockFile>
#include <QMetaObject>
#include <QMetaType>
#include <QSaveFile>
#include <QSet>
#include <QThreadPool>
#include <QUuid>
#include <Qt>
#include <QtGlobal>
#include <mutex>
#include <qflags.h>
#include <utility>

#include "document_import_transport.h"
#include "document_save_transport.h"
#include "teardown_trace.h"

namespace dsd_qt {

namespace {

const QString&
lock_suffix() {
    static const QString suffix = QStringLiteral(".lock");
    return suffix;
}

/* A teardown that stopped waiting for its job: the worker and the lock stay, unreachable from any writer, until the
   job ends and the process exits. Deleting the worker would wait for the job; deleting the lock would free a folder
   the job still writes in. */
struct Abandoned {
    QThreadPool* worker;
    QLockFile* lock;
};

QList<Abandoned>&
abandoned_writers() {
    static auto* writers = new QList<Abandoned>();
    return *writers;
}

/* Remove @p path: a folder with what it holds, or a link (never what it names). */
void
remove_entry(const QString& path) {
    const QFileInfo info(path);
    if (info.isDir() && !info.isSymLink() && !info.isJunction()) {
        QDir(path).removeRecursively(); // removes the links it meets inside and follows none
    } else if (info.isSymLink() || info.isJunction()) {
        QFile::remove(path);
    }
}

/* Whether @p name is one a writer gives its folder: a UUID, brace-less and lowercase, as createUuid() writes it. */
bool
is_staging_name(const QString& name) {
    const QUuid id = QUuid::fromString(name);
    return !id.isNull() && id.toString(QUuid::WithoutBraces) == name;
}

/* Every staging folder under @p root a lock file or a folder names, but @p own. Only writers' names count: nothing
   else under the root is ever locked or removed. */
QSet<QString>
staged_names(const QString& root, const QString& own) {
    QSet<QString> names;
    const QDir dir(root);
    const QFileInfoList found =
        dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : found) {
        QString name = entry.fileName();
        if (!entry.isDir()) {
            if (!name.endsWith(lock_suffix())) {
                continue;
            }
            name.chop(lock_suffix().size());
        }
        if (is_staging_name(name)) {
            names.insert(name);
        }
    }
    names.remove(own);
    return names;
}

/* Remove each folder under @p root (but @p own) that no live writer holds: one whose lock this can take. */
void
sweep(const QString& root, const QString& own) {
    const QSet<QString> names = staged_names(root, own);
    for (const QString& name : names) {
        const QString folder = root + QLatin1Char('/') + name;
        QLockFile lock(folder + lock_suffix());
        // Age never makes a lock stale: only a holder that is gone (QLockFile checks its process) frees it.
        lock.setStaleLockTime(0);
        if (lock.tryLock(0)) {
            remove_entry(folder);
            lock.unlock(); // removes the lock file
        }
    }
}

/* Make @p dir, readable by the owner only; true when it is a folder now. */
bool
make_private_dir(const QString& dir) {
    const QFileInfo info(dir);
    if (info.isDir() && !info.isSymLink()) {
        return true;
    }
    return QDir().mkpath(info.absolutePath())
           && QDir(info.absolutePath())
                  .mkdir(info.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

/* Write @p bytes to @p path, owner-only; false when any step fails (nothing is left at @p path then). */
bool
stage_file(const QString& path, const QByteArray& bytes) {
    QSaveFile out(path);
    if (!open_private(out)) {
        return false;
    }
    if (out.write(bytes) != bytes.size()) {
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

} // namespace

/* What a job reaches the writer through; it outlives the writer. */
struct DocumentExportWriter::Link {
    std::mutex mutex;
    QObject* owner = nullptr;               // guarded by mutex; cleared by the writer's destructor
    DocumentExportWriter* writer = nullptr; // guarded by mutex; cleared with owner
};

namespace {

/* A job's data: all of it fixed before the job starts. */
struct ExportJob {
    int token = 0;
    QByteArray bytes;
    QString folder; // empty when the writer holds no lock: the job cannot stage
    QString reference;
    std::shared_ptr<DocumentSaveTransport> transport;
};

/* The worker: stage, save, remove the staged file; whether the document holds the bytes. Touches no QObject. */
bool
run_job(const ExportJob& job) {
    const QString path = job.folder + QLatin1Char('/') + QString::number(job.token) + QStringLiteral(".csv");
    if (job.folder.isEmpty() || !make_private_dir(job.folder) || !stage_file(path, job.bytes)) {
        // save() never took the reference, so its Download-folder row (#829) is let go of here.
        job.transport->discardPendingDownload(job.reference);
        if (!job.folder.isEmpty()) {
            QFile::remove(path);
        }
        return false;
    }
    const bool ok = job.transport->save(path, job.reference);
    QFile::remove(path);
    return ok;
}

} // namespace

DocumentExportWriter::DocumentExportWriter(std::shared_ptr<DocumentSaveTransport> transport, const QString& cacheRoot,
                                           QObject* owner, Reply reply)
    : m_transport(std::move(transport)), m_root(QDir::cleanPath(cacheRoot)), m_pool(std::make_unique<QThreadPool>()),
      m_link(std::make_shared<Link>()), m_reply(std::move(reply)) {
    m_link->owner = owner;
    m_link->writer = this;
    // One worker: one job at a time.
    m_pool->setMaxThreadCount(1);
    if (m_root.isEmpty() || QDir::isRelativePath(m_root)) {
        // No folder, lock or sweep: such a root would resolve against wherever the app happens to run.
        qWarning("export writer: the cache root is not an absolute path; exports cannot be staged");
        return;
    }
    const QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_folder = m_root + QLatin1Char('/') + name;
    m_lock = std::make_unique<QLockFile>(m_folder + lock_suffix());
    // Age never makes this lock stale: a writer may live as long as the app.
    m_lock->setStaleLockTime(0);
    if (holdLock()) {
        make_private_dir(m_folder);
    }
    sweep(m_root, name);
}

DocumentExportWriter::~DocumentExportWriter() {
    const TeardownStage stage("~DocumentExportWriter");
    // A job still running is stopped; one whose reply is only queued needs nothing.
    if (m_busy && !m_pool->waitForDone(0) && m_transport != nullptr) {
        m_transport->abort();
    }
    const bool stopped = m_pool->waitForDone(kStopWaitMs);
    {
        const std::lock_guard<std::mutex> lock(m_link->mutex);
        m_link->owner = nullptr;
        m_link->writer = nullptr;
    }
    if (!stopped) {
        // A save blocked in a provider ignores the abort. The job keeps the transport and the link, which no longer
        // reaches this writer; its folder and lock stay until the process ends, for the next start's sweep.
        qWarning("export writer: a save did not stop within %d ms; it is left behind as the app closes", kStopWaitMs);
        abandoned_writers().append(Abandoned{m_pool.release(), m_lock.release()});
        return;
    }
    if (m_lock != nullptr && m_lock->isLocked()) {
        remove_entry(m_folder);
        m_lock->unlock(); // removes the lock file
    }
}

bool
DocumentExportWriter::submit(int token, const QByteArray& bytes, const QString& reference) {
    if (m_transport == nullptr) {
        return false;
    }
    if (m_busy || m_transport->aborted() || m_lock == nullptr) {
        m_transport->discardPendingDownload(reference);
        return false;
    }
    ExportJob job{token, bytes, holdLock() ? m_folder : QString(), reference, m_transport};
    m_busy = true;
    m_pool->start([link = m_link, job = std::move(job)]() {
        const bool ok = run_job(job);
        const int done = job.token;
        const std::lock_guard<std::mutex> lock(link->mutex);
        QObject* owner = link->owner;
        if (owner != nullptr) {
            QMetaObject::invokeMethod(owner, [link, done, ok]() { deliver(link, done, ok); }, Qt::QueuedConnection);
        }
    });
    return true;
}

bool
DocumentExportWriter::busy() const {
    return m_busy;
}

bool
DocumentExportWriter::holdLock() {
    if (m_lock->isLocked()) {
        return true;
    }
    return QDir().mkpath(m_root) && m_lock->tryLock(0);
}

void
DocumentExportWriter::finish(int token, bool ok) {
    m_busy = false;
    // A copy: the reply may destroy the writer, and with it m_reply.
    const Reply reply = m_reply;
    if (reply) {
        reply(token, ok);
    }
}

void
DocumentExportWriter::deliver(const std::shared_ptr<Link>& link, int token, bool ok) {
    DocumentExportWriter* writer = nullptr;
    {
        const std::lock_guard<std::mutex> lock(link->mutex);
        writer = link->writer;
    }
    // The owner's thread, the only one the writer is destroyed on: a writer seen here lives until this returns, unless
    // the reply itself destroys it, as its last use.
    if (writer != nullptr) {
        writer->finish(token, ok);
    }
}

} // namespace dsd_qt
