// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "import_service.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QThreadPool>
#include <QUuid>
#include <QVariant>
#include <Qt>
#include <atomic>
#include <cerrno>
#include <deque>
#include <mutex>
#include <qflags.h>
#include <utility>

#include <dsd-neo/platform/file_compat.h>

#include "decoder_host.h"
#include "import_names.h"
#include "json_store.h"
#include "teardown_trace.h"

namespace dsd_qt {

namespace {

QString
journal_name() {
    return QStringLiteral("import_journal.json");
}

std::atomic<qint64> g_next_request{0};

/* Workers a teardown stopped waiting for. Deleting one would wait for its job, so each stays, unreachable from any
   service, until its job ends and the process exits. */
QList<QThreadPool*>&
abandoned_workers() {
    static auto* workers = new QList<QThreadPool*>();
    return *workers;
}

/* Make @p dir, which must not exist yet, readable by the owner only. */
bool
make_private_dir(const QString& dir) {
    const QFileInfo info(dir);
    return QDir().mkpath(info.absolutePath())
           && QDir(info.absolutePath())
                  .mkdir(info.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

/* The journal's entries, or false when it cannot be read or holds anything but absolute paths. */
bool
load_journal(QStringList& entries) {
    // Read without marking: the journal is the worker's own record, not a store a person keeps.
    const JsonStoreRead read = json_store_read(journal_name());
    if (read.status == JsonStoreLoad::Unreadable) {
        return false;
    }
    const QJsonArray& array = read.array;
    for (const auto& value : array) {
        const QString path = value.toString();
        if (!value.isString() || !QDir::isAbsolutePath(path)) {
            return false;
        }
        entries.append(QDir::cleanPath(path));
    }
    return true;
}

} // namespace

/* A request on its way to the worker. */
struct ImportService::Work {
    qint64 id = 0;
    QString stagingDir;
    Stage stage;
    std::shared_ptr<Request> request;
};

/* The work queue the worker runs, and the link it reaches the service through: both outlive the service. */
struct ImportService::Shared {
    std::shared_ptr<DocumentImportTransport> transport;
    std::mutex lock;
    ImportService* service = nullptr; // guarded by lock; cleared by the destructor
    std::deque<QString> removals;     // guarded by lock; run before any waiting request
    std::deque<Work> work;            // guarded by lock
};

/* A request's cancel, shared by the Qt thread and the worker. */
class ImportService::Request {
  public:
    bool
    cancelled() const {
        return m_cancelled.load();
    }

    void
    cancel(DocumentImportTransport& transport, qint64 id) {
        m_cancelled.store(true);
        const std::lock_guard<std::mutex> guard(m_lock);
        // Once forgotten, a cancel would only leave a record behind in the transport.
        if (!m_ended) {
            transport.cancel(id);
        }
    }

    /** The request's stage ended (or was skipped): the transport forgets it. */
    void
    end(DocumentImportTransport& transport, qint64 id) {
        const std::lock_guard<std::mutex> guard(m_lock);
        m_ended = true;
        transport.forget(id);
    }

  private:
    std::atomic<bool> m_cancelled{false};
    std::mutex m_lock;
    bool m_ended = false; // guarded by m_lock
};

ImportService::ImportService(std::shared_ptr<DocumentImportTransport> transport, QObject* parent)
    : QObject(parent), m_shared(std::make_shared<Shared>()), m_pool(std::make_unique<QThreadPool>()) {
    m_shared->transport = std::move(transport);
    m_shared->service = this;
    // One worker: requests stage one at a time, in the order they came.
    m_pool->setMaxThreadCount(1);
}

ImportService::~ImportService() {
    const TeardownStage stage("~ImportService");
    {
        const std::lock_guard<std::mutex> lock(m_shared->lock);
        m_shared->service = nullptr;
    }
    // No commit runs from here on, so every staging folder goes; a request still waiting skips its stage.
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        it->request->cancel(*m_shared->transport, it.key());
        if (!it->stagingDir.isEmpty()) {
            removeOnWorker(it->stagingDir);
        }
    }
    if (!m_pool->waitForDone(kStopWaitMs)) {
        // A copy blocked in a provider's read or a full disk's write ignores the cancel. Deleting the pool would wait
        // for it, so the pool stays; the job holds the transport and the queue, and the cleared link keeps it from
        // this service.
        qWarning("import service: a copy did not stop within %d ms; it is left behind as the app closes", kStopWaitMs);
        abandoned_workers().append(m_pool.release());
    }
}

QString
ImportService::stagingRoot() {
    return json_store_path(QStringLiteral("import-staging"));
}

void
ImportService::sweepStaging() {
    const QString root = stagingRoot();
    const QFileInfo info(root);
    if (info.isDir() && !info.isSymLink() && !info.isJunction()) {
        // removeRecursively() removes the links it meets inside and follows none.
        QDir(root).removeRecursively();
    } else {
        QFile::remove(root); // a file, or a link (never what it names); nothing when there is none
    }
}

QString
ImportService::journalPath() {
    return json_store_path(journal_name());
}

bool
ImportService::publish(const QString& staged, const QDir& dir, const QString& name, const QSet<QString>& reserved,
                       QString* published) {
    QSet<QString> skipped = reserved;
    for (int attempt = 0; attempt < kPublishAttempts; attempt++) {
        const QString destination = unique_import_destination(dir, name, skipped);
        if (destination.isEmpty()) {
            break;
        }
        int sourceLeft = 0;
        if (dsd_rename_noreplace(staged.toUtf8().constData(), destination.toUtf8().constData(), &sourceLeft) == 0) {
            *published = destination;
            return true;
        }
        if (errno != EEXIST) {
            break;
        }
        skipped.insert(dir.absoluteFilePath(QFileInfo(destination).fileName()));
    }
    return false;
}

qint64
ImportService::newRequestId() {
    return ++g_next_request;
}

void
ImportService::runNow(const Stage& stage, const Commit& commit) {
    const qint64 id = newRequestId();
    const QString dir = stagingRoot() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap staged;
    if (make_private_dir(dir)) {
        staged = stage(ImportJob{id, dir, m_shared->transport, []() { return false; }});
    } else {
        staged.insert(QStringLiteral("ok"), false);
        staged.insert(QStringLiteral("error"), QStringLiteral("local"));
    }
    m_shared->transport->forget(id);
    commit(id, staged, false);
    QDir(dir).removeRecursively();
}

qint64
ImportService::submit(Stage stage, Commit commit) {
    return enqueue(std::move(stage), std::move(commit), true);
}

qint64
ImportService::readDocument(const QString& reference, const QString& destPath, qint64 maxBytes, ReadCommit commit) {
    Stage stage = [reference, destPath, maxBytes](const ImportJob& job) {
        const int copied = job.transport->copy(job.id, reference, destPath, maxBytes);
        // A cancel that came while the copy ran (teardown's included) means no one takes the copy, maybe ever.
        if (copied == DecoderHost::DocumentCopied && job.cancelled()) {
            QFile::remove(destPath);
        }
        return QVariantMap{{QStringLiteral("copied"), copied}};
    };
    Commit read = [commit = std::move(commit)](qint64 id, const QVariantMap& staged, bool cancelled) {
        commit(id, cancelled
                       ? static_cast<int>(DecoderHost::DocumentCancelled)
                       : staged.value(QStringLiteral("copied"), static_cast<int>(DecoderHost::DocumentFailed)).toInt());
    };
    return enqueue(std::move(stage), std::move(read), false);
}

qint64
ImportService::enqueue(Stage stage, Commit commit, bool staged) {
    const qint64 id = newRequestId();
    auto request = std::make_shared<Request>();
    const QString dir =
        staged ? stagingRoot() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::WithoutBraces) : QString();
    m_pending.insert(id, Pending{request, std::move(commit), dir});
    {
        const std::lock_guard<std::mutex> lock(m_shared->lock);
        m_shared->work.push_back(Work{id, dir, std::move(stage), std::move(request)});
    }
    m_pool->start([shared = m_shared]() { runNext(shared); });
    updateBusy();
    return id;
}

void
ImportService::cancel(qint64 id) {
    const auto it = m_pending.constFind(id);
    if (it != m_pending.cend()) {
        it->request->cancel(*m_shared->transport, id);
    }
}

ImportCopyProgress
ImportService::progress(qint64 id) const {
    return m_pending.contains(id) ? m_shared->transport->progress(id) : ImportCopyProgress();
}

bool
ImportService::busy() const {
    return !m_pending.isEmpty();
}

void
ImportService::runNext(const std::shared_ptr<Shared>& shared) {
    // The worker: one start() per queued item, so each run takes one. A removal goes first: it frees space.
    QString removal;
    Work work;
    {
        const std::lock_guard<std::mutex> lock(shared->lock);
        if (!shared->removals.empty()) {
            removal = std::move(shared->removals.front());
            shared->removals.pop_front();
        } else if (!shared->work.empty()) {
            work = std::move(shared->work.front());
            shared->work.pop_front();
        } else {
            return;
        }
    }
    if (!removal.isEmpty()) {
        QDir(removal).removeRecursively();
    } else {
        runJob(shared, work);
    }
}

void
ImportService::runJob(const std::shared_ptr<Shared>& shared, const Work& work) {
    // No QObject is touched here; the result goes back through the link, queued.
    QVariantMap staged;
    if (!work.request->cancelled()) {
        if (work.stagingDir.isEmpty() || make_private_dir(work.stagingDir)) {
            const std::shared_ptr<Request> request = work.request;
            const ImportJob job{work.id, work.stagingDir, shared->transport,
                                [request]() { return request->cancelled(); }};
            staged = work.stage(job);
        } else {
            staged.insert(QStringLiteral("ok"), false);
            staged.insert(QStringLiteral("error"), QStringLiteral("local"));
        }
    }
    work.request->end(*shared->transport, work.id);
    const std::lock_guard<std::mutex> lock(shared->lock);
    ImportService* service = shared->service;
    if (service != nullptr) {
        const qint64 id = work.id;
        QMetaObject::invokeMethod(
            service, [service, id, staged]() { service->finish(id, staged); }, Qt::QueuedConnection);
    }
}

void
ImportService::finish(qint64 id, const QVariantMap& staged) {
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        return;
    }
    // Out of the table first: a cancel from here on is too late, and the commit may submit or cancel others.
    const Pending pending = std::move(*it);
    m_pending.erase(it);
    pending.commit(id, staged, pending.request->cancelled());
    // The staged files may be large; deleting them is the worker's job, not the Qt thread's.
    if (!pending.stagingDir.isEmpty()) {
        removeOnWorker(pending.stagingDir);
    }
    updateBusy();
}

void
ImportService::removeOnWorker(const QString& dir) {
    {
        const std::lock_guard<std::mutex> lock(m_shared->lock);
        m_shared->removals.push_back(dir);
    }
    m_pool->start([shared = m_shared]() { runNext(shared); });
}

void
ImportService::updateBusy() {
    const bool now = busy();
    if (now != m_busy) {
        m_busy = now;
        Q_EMIT busyChanged();
    }
}

bool
ImportService::journalAdd(const QStringList& directories) {
    if (directories.isEmpty()) {
        return true;
    }
    QStringList entries;
    if (!load_journal(entries)) {
        return false;
    }
    const qsizetype before = entries.size();
    for (const QString& dir : directories) {
        if (!QDir::isAbsolutePath(dir)) {
            return false;
        }
        const QString clean = QDir::cleanPath(dir);
        if (!entries.contains(clean)) {
            entries.append(clean);
        }
    }
    return entries.size() == before || json_store_save_array(journal_name(), QJsonArray::fromStringList(entries));
}

bool
ImportService::journalRemove(const QStringList& directories) {
    if (directories.isEmpty()) {
        return true;
    }
    QStringList entries;
    if (!load_journal(entries)) {
        return false;
    }
    const qsizetype before = entries.size();
    for (const QString& dir : directories) {
        entries.removeAll(QDir::cleanPath(dir));
    }
    if (entries.size() == before) {
        return true;
    }
    // Nothing journaled leaves no journal behind; a missing journal is an empty one.
    return (entries.isEmpty() && QFile::remove(journalPath()))
           || json_store_save_array(journal_name(), QJsonArray::fromStringList(entries));
}

bool
ImportService::journalSetAside() {
    const QString path = journalPath();
    const QString aside = path + QStringLiteral(".bad");
    QFile::remove(aside);
    // A missing journal is an empty one, so the next commit starts a fresh journal.
    return !QFileInfo::exists(path) || QFile::rename(path, aside);
}

QStringList
ImportService::journalEntries(bool* ok) {
    QStringList entries;
    const bool loaded = load_journal(entries);
    if (ok != nullptr) {
        *ok = loaded;
    }
    return loaded ? entries : QStringList();
}

} // namespace dsd_qt
