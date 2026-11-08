// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the import worker (#832): staging folders, the startup sweep, commits, cancel, teardown, journal. */

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <qflags.h>

#include "../test_support/qt_test_paths.h"
#include "decoder_host.h"
#include "document_import_transport.h"
#include "import_service.h"
#include "json_store.h"

using dsd_qt::DecoderHost;
using dsd_qt::DocumentImportTransport;
using dsd_qt::ImportJob;
using dsd_qt::ImportService;

namespace {

int failures = 0;

void
check_at(bool ok, int line, const char* why) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "test_ui_qt_import_service.cpp:%d: %s\n", line, why);
    }
}

#define CHECK(cond, why) check_at((cond), __LINE__, (why))

void
write_file(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "a fixture file is written");
}

QByteArray
read_file(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/* Run the event loop until @p done holds; false after about five seconds. */
bool
wait_until(const std::function<bool()>& done) {
    for (int i = 0; i < 2500; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        if (done()) {
            return true;
        }
        QThread::msleep(2);
    }
    return done();
}

/* Run the event loop a little longer, for anything that should not arrive. */
void
settle() {
    for (int i = 0; i < 25; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QThread::msleep(2);
    }
}

bool
staging_empty() {
    return QDir(ImportService::stagingRoot())
        .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
        .isEmpty();
}

/* One commit as the service delivered it. */
struct Committed {
    qint64 id = 0;
    QVariantMap staged;
    bool cancelled = false;
};

/* What the stages (worker) and commits (Qt thread) of one test saw. */
struct Log {
    std::mutex lock;
    QList<qint64> staged; // guarded by lock
    QList<Committed> commits;

    void
    noteStage(qint64 id) {
        const std::lock_guard<std::mutex> guard(lock);
        staged.append(id);
    }

    QList<qint64>
    stages() {
        const std::lock_guard<std::mutex> guard(lock);
        return staged;
    }

    ImportService::Commit
    commit() {
        return [this](qint64 id, const QVariantMap& map, bool cancelled) { commits.append({id, map, cancelled}); };
    }

    int
    commitsFor(qint64 id) const {
        int count = 0;
        for (const Committed& c : commits) {
            count += c.id == id ? 1 : 0;
        }
        return count;
    }
};

/*
 * A transport whose copies the test steers: Copy writes the destination at once, WaitForCancel runs until its request
 * is cancelled, and Block ignores every cancel until the test releases it (a provider stuck in a write).
 */
class FakeTransport : public DocumentImportTransport {
  public:
    enum class Mode { Copy, WaitForCancel, Block };

    explicit FakeTransport(Mode mode) : m_mode(mode) {}

    int
    copy(qint64 request, const QString& reference, const QString& destPath, qint64 maxBytes) override {
        (void)reference;
        (void)maxBytes;
        std::unique_lock<std::mutex> guard(m_lock);
        m_entered.insert(request);
        m_changed.notify_all();
        if (m_mode == Mode::WaitForCancel) {
            m_changed.wait(guard, [this, request]() { return m_cancels.contains(request); });
            return DecoderHost::DocumentCancelled;
        }
        if (m_mode == Mode::Block) {
            m_changed.wait(guard, [this]() { return m_released; });
        }
        guard.unlock();
        write_file(destPath, "copied");
        return DecoderHost::DocumentCopied;
    }

    void
    cancel(qint64 request) override {
        const std::lock_guard<std::mutex> guard(m_lock);
        m_cancels.append(request);
        m_changed.notify_all();
    }

    void
    forget(qint64 request) override {
        const std::lock_guard<std::mutex> guard(m_lock);
        m_forgets.append(request);
    }

    void
    release() {
        const std::lock_guard<std::mutex> guard(m_lock);
        m_released = true;
        m_changed.notify_all();
    }

    bool
    entered(qint64 request) const {
        const std::lock_guard<std::mutex> guard(m_lock);
        return m_entered.contains(request);
    }

    QList<qint64>
    cancels() const {
        const std::lock_guard<std::mutex> guard(m_lock);
        return m_cancels;
    }

    QList<qint64>
    forgets() const {
        const std::lock_guard<std::mutex> guard(m_lock);
        return m_forgets;
    }

  private:
    const Mode m_mode;
    mutable std::mutex m_lock;
    std::condition_variable m_changed;
    QSet<qint64> m_entered;
    QList<qint64> m_cancels;
    QList<qint64> m_forgets;
    bool m_released = false;
};

void
test_sweep() {
    const QString root = ImportService::stagingRoot();
    CHECK(root == dsd_qt::json_store_path(QStringLiteral("import-staging")), "staging lives in the app data folder");
    const QTemporaryDir outside;
    CHECK(outside.isValid(), "a scratch directory exists");
    const QString kept = outside.filePath(QStringLiteral("kept.csv"));
    write_file(kept, "mine");
    const QString library = dsd_qt::json_store_path(QStringLiteral("imports/talkgroups.csv"));
    write_file(library, "1,a\n");
    write_file(root + QStringLiteral("/left-over/a.csv"), "stale");
    write_file(root + QStringLiteral("/left-over/lib/bundles/x/b.csv"), "stale");
    write_file(root + QStringLiteral("/.hidden"), "stale");
#ifndef _WIN32
    CHECK(QFile::link(outside.path(), root + QStringLiteral("/link")), "a link out of staging is made");
#endif
    ImportService::sweepStaging();
    CHECK(!QFileInfo::exists(root), "the sweep removes the whole staging folder");
    CHECK(read_file(kept) == "mine", "the sweep follows no link out of staging");
    CHECK(read_file(library) == "1,a\n", "the sweep leaves the library alone");
    ImportService::sweepStaging();
    CHECK(!QFileInfo::exists(root), "a sweep with nothing to sweep is harmless");

#ifndef _WIN32
    CHECK(QFile::link(outside.path(), root), "staging made a link to another folder");
    ImportService::sweepStaging();
    CHECK(!QFileInfo::exists(root) && !QFileInfo(root).isSymLink(), "a staging link is removed");
    CHECK(read_file(kept) == "mine", "and the folder it named is left alone");
#endif
    QFile::remove(library);
}

/* Requests stage one at a time in order and commit once each, in order, with their staging folder still there. */
void
test_order_and_staging() {
    const QTemporaryDir src;
    CHECK(src.isValid(), "a scratch directory exists");
    const QString source = src.filePath(QStringLiteral("group.csv"));
    write_file(source, "1,a\n");

    Log log;
    QStringList dirsAtCommit;
    ImportService service(std::make_shared<DocumentImportTransport>());
    CHECK(!service.busy(), "a new service is idle");
    QList<qint64> ids;
    for (int i = 0; i < 3; ++i) {
        const qint64 id = service.submit(
            [&log, source](const ImportJob& job) {
                log.noteStage(job.id);
                const QFileInfo dir(job.stagingDir);
                QVariantMap staged;
                staged.insert(QStringLiteral("dir"), job.stagingDir);
                staged.insert(QStringLiteral("fresh"), dir.isDir() && QDir(job.stagingDir).isEmpty()
                                                           && dir.absolutePath() == ImportService::stagingRoot());
#ifndef _WIN32
                staged.insert(QStringLiteral("private"),
                              (dir.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
#endif
                const QString dest = QDir(job.stagingDir).filePath(QStringLiteral("group.csv"));
                staged.insert(QStringLiteral("code"), job.transport->copy(job.id, source, dest, 64));
                staged.insert(QStringLiteral("path"), dest);
                return staged;
            },
            [&log, &dirsAtCommit](qint64 id, const QVariantMap& staged, bool cancelled) {
                dirsAtCommit.append(read_file(staged.value(QStringLiteral("path")).toString()));
                log.commit()(id, staged, cancelled);
            });
        CHECK(id > 0 && !ids.contains(id), "each request gets a fresh positive id");
        ids.append(id);
    }
    CHECK(service.busy(), "a queued request makes the service busy");
    CHECK(wait_until([&log]() { return log.commits.size() == 3; }), "every request commits");
    settle();
    CHECK(log.commits.size() == 3, "no request commits twice");
    CHECK(log.stages() == ids, "requests stage in the order they came");
    QStringList dirs;
    for (int i = 0; i < log.commits.size(); ++i) {
        const Committed& c = log.commits.at(i);
        CHECK(c.id == ids.value(i) && !c.cancelled, "requests commit in order, not cancelled");
        CHECK(c.staged.value(QStringLiteral("fresh")).toBool(), "each stage gets its own empty staging folder");
#ifndef _WIN32
        CHECK(c.staged.value(QStringLiteral("private")).toBool(), "a staging folder is the owner's alone");
#endif
        CHECK(c.staged.value(QStringLiteral("code")).toInt() == DecoderHost::DocumentCopied, "the stage copies");
        dirs.append(c.staged.value(QStringLiteral("dir")).toString());
    }
    CHECK(dirsAtCommit == QStringList(3, QStringLiteral("1,a\n")), "the staged copy is there when its commit runs");
    CHECK(QSet<QString>(dirs.cbegin(), dirs.cend()).size() == 3, "no two requests share a staging folder");
    CHECK(wait_until([]() { return staging_empty(); }), "each staging folder goes once its commit returned");
    CHECK(!service.busy(), "the service is idle once everything committed");
    CHECK(service.progress(ids.first()).bytes == -1, "a committed request has no progress");
    service.cancel(ids.first());
    service.cancel(9999999);
    settle();
    CHECK(log.commits.size() == 3, "cancelling a committed or unknown id does nothing");
}

/* A request cancelled while another runs never stages; one cancelled during its copy stops it. */
void
test_cancel() {
    auto transport = std::make_shared<FakeTransport>(FakeTransport::Mode::WaitForCancel);
    Log log;
    ImportService service(transport);
    std::atomic<bool> sawCancel{false};
    QString runningDir;
    const ImportService::Stage copying = [&log, &sawCancel](const ImportJob& job) {
        log.noteStage(job.id);
        const QString dest = QDir(job.stagingDir).filePath(QStringLiteral("x.csv"));
        QVariantMap staged;
        staged.insert(QStringLiteral("dir"), job.stagingDir);
        staged.insert(QStringLiteral("code"), job.transport->copy(job.id, QStringLiteral("/x.csv"), dest, 64));
        sawCancel.store(job.cancelled());
        return staged;
    };
    const qint64 running = service.submit(copying, log.commit());
    const qint64 queued = service.submit(copying, log.commit());
    CHECK(wait_until([&transport, running]() { return transport->entered(running); }), "the first copy starts");
    CHECK(!transport->entered(queued), "the second request waits for the first");

    service.cancel(queued);
    CHECK(transport->cancels() == QList<qint64>{queued}, "a cancel reaches the transport, for an unstarted copy too");
    service.cancel(running);
    CHECK(wait_until([&log]() { return log.commits.size() == 2; }), "both cancelled requests commit");
    settle();
    CHECK(log.commits.size() == 2 && log.commitsFor(running) == 1 && log.commitsFor(queued) == 1,
          "each cancelled request commits exactly once");
    CHECK(log.stages() == QList<qint64>{running}, "a request cancelled before it started never stages");
    for (const Committed& c : log.commits) {
        CHECK(c.cancelled, "a cancelled request's commit is told so");
        if (c.id == queued) {
            CHECK(c.staged.isEmpty(), "a skipped stage commits an empty map");
        } else {
            CHECK(c.staged.value(QStringLiteral("code")).toInt() == DecoderHost::DocumentCancelled,
                  "the copy in flight stops as cancelled");
            runningDir = c.staged.value(QStringLiteral("dir")).toString();
        }
    }
    CHECK(sawCancel.load(), "a stage sees its request cancelled");
    CHECK(!runningDir.isEmpty() && wait_until([&runningDir]() { return !QFileInfo::exists(runningDir); }),
          "a cancelled request's staging folder goes");
    const QList<qint64> forgets = transport->forgets();
    CHECK(forgets.size() == 2 && forgets.contains(running) && forgets.contains(queued),
          "the transport forgets each request once it ended, skipped or not");
    CHECK(wait_until([]() { return staging_empty(); }), "nothing is left in staging");
}

/* busyChanged fires on the way in and on the way out, once each. */
void
test_busy() {
    Log log;
    ImportService service(std::make_shared<DocumentImportTransport>());
    int changes = 0;
    QList<bool> seen;
    QObject::connect(&service, &ImportService::busyChanged, [&service, &changes, &seen]() {
        ++changes;
        seen.append(service.busy());
    });
    const ImportService::Stage nothing = [](const ImportJob&) { return QVariantMap(); };
    service.submit(nothing, log.commit());
    service.submit(nothing, log.commit());
    CHECK(changes == 1 && service.busy(), "the first request makes the service busy; the second changes nothing");
    CHECK(wait_until([&log]() { return log.commits.size() == 2; }), "both requests commit");
    CHECK(changes == 2 && seen == QList<bool>({true, false}) && !service.busy(),
          "the service goes idle once, after the last commit");
}

/* A request whose staging folder cannot be made never stages and commits a local failure. */
void
test_staging_failure() {
    const QString root = ImportService::stagingRoot();
    QDir(root).removeRecursively();
    write_file(root, "a file where the staging folder belongs");
    Log log;
    ImportService service(std::make_shared<DocumentImportTransport>());
    const qint64 id = service.submit(
        [&log](const ImportJob& job) {
            log.noteStage(job.id);
            return QVariantMap();
        },
        log.commit());
    CHECK(wait_until([&log]() { return log.commits.size() == 1; }), "the request commits");
    CHECK(log.stages().isEmpty(), "no stage runs without its folder");
    const Committed c = log.commits.value(0);
    CHECK(c.id == id && !c.cancelled && !c.staged.value(QStringLiteral("ok"), true).toBool()
              && c.staged.value(QStringLiteral("error")).toString() == QStringLiteral("local"),
          "it commits a local failure");
    QFile::remove(root);
}

/* Teardown with a copy that ignores its cancel: the service waits a bounded time, then leaves it, and never commits. */
void
test_teardown_blocked() {
    auto transport = std::make_shared<FakeTransport>(FakeTransport::Mode::Block);
    Log log;
    auto service = std::make_unique<ImportService>(transport);
    std::atomic<bool> stageEnded{false};
    QString dir;
    std::mutex dirLock;
    const qint64 id = service->submit(
        [&dir, &dirLock, &stageEnded](const ImportJob& job) {
            {
                const std::lock_guard<std::mutex> guard(dirLock);
                dir = job.stagingDir;
            }
            const QString dest = QDir(job.stagingDir).filePath(QStringLiteral("x.csv"));
            job.transport->copy(job.id, QStringLiteral("/x.csv"), dest, 64);
            stageEnded.store(true);
            return QVariantMap();
        },
        log.commit());
    const qint64 waiting = service->submit([](const ImportJob&) { return QVariantMap(); }, log.commit());
    CHECK(wait_until([&transport, id]() { return transport->entered(id); }), "the copy starts and blocks");

    QElapsedTimer timer;
    timer.start();
    service.reset();
    const qint64 took = timer.elapsed();
    CHECK(took >= ImportService::kStopWaitMs - 100 && took < ImportService::kStopWaitMs + 1500,
          "teardown waits about kStopWaitMs for a blocked copy, then returns");
    CHECK(transport->cancels().contains(id) && transport->cancels().contains(waiting),
          "teardown cancels every request");

    transport->release();
    CHECK(wait_until([&stageEnded]() { return stageEnded.load(); }), "the left-behind copy ends once released");
    CHECK(wait_until([&transport, id, waiting]() {
              const QList<qint64> forgets = transport->forgets();
              return forgets.contains(id) && forgets.contains(waiting);
          }),
          "the left-behind worker still forgets its requests on the transport it kept");
    const QString blockedDir = [&dir, &dirLock]() {
        const std::lock_guard<std::mutex> guard(dirLock);
        return dir;
    }();
    CHECK(!blockedDir.isEmpty() && wait_until([&blockedDir]() { return !QFileInfo::exists(blockedDir); }),
          "the left-behind request's staging folder goes when it ends");
    settle();
    CHECK(log.commits.isEmpty(), "nothing commits after teardown");
}

/* Teardown with nothing blocked waits for the worker and commits nothing. */
void
test_teardown_idle() {
    auto transport = std::make_shared<FakeTransport>(FakeTransport::Mode::WaitForCancel);
    Log log;
    QString dir;
    {
        ImportService service(transport);
        const qint64 id = service.submit(
            [&dir](const ImportJob& job) {
                dir = job.stagingDir;
                job.transport->copy(job.id, QStringLiteral("/x.csv"), QDir(job.stagingDir).filePath("x.csv"), 64);
                return QVariantMap();
            },
            log.commit());
        CHECK(wait_until([&transport, id]() { return transport->entered(id); }), "the copy starts");
    }
    settle();
    CHECK(log.commits.isEmpty(), "a request cancelled by teardown never commits");
    CHECK(!dir.isEmpty() && !QFileInfo::exists(dir), "its staging folder is gone once teardown returns");
}

QStringList
entries(bool* ok) {
    return ImportService::journalEntries(ok);
}

void
test_journal() {
    const QString path = ImportService::journalPath();
    CHECK(path == dsd_qt::json_store_path(QStringLiteral("import_journal.json")), "the journal is an app data file");
    QFile::remove(path);
    bool ok = false;
    CHECK(entries(&ok).isEmpty() && ok, "a missing journal is empty");

    const QString a = dsd_qt::json_store_path(QStringLiteral("imports/bundles/a"));
    const QString b = dsd_qt::json_store_path(QStringLiteral("imports/bundles/b"));
    const QString c = dsd_qt::json_store_path(QStringLiteral("imports/bundles/b/rev"));
    CHECK(ImportService::journalAdd({a, b + QStringLiteral("/")}), "directories are recorded");
    CHECK(entries(&ok) == QStringList({a, b}) && ok, "the journal lists them, clean");
    CHECK(ImportService::journalAdd({b, c}), "a second record adds to the first");
    CHECK(entries(&ok) == QStringList({a, b, c}) && ok, "without listing a directory twice");
    CHECK(ImportService::journalRemove({a, dsd_qt::json_store_path(QStringLiteral("imports/never"))}),
          "a directory is dropped, and one never recorded is ignored");
    CHECK(entries(&ok) == QStringList({b, c}) && ok, "the rest stay");
    CHECK(ImportService::journalAdd({}) && ImportService::journalRemove({}), "an empty change succeeds");
    CHECK(!ImportService::journalAdd({QStringLiteral("relative/dir")}) && entries(&ok) == QStringList({b, c}),
          "a relative directory is refused and nothing is recorded");
    CHECK(ImportService::journalRemove({b, c}) && entries(&ok).isEmpty() && ok, "the journal empties");

    for (const QByteArray& bad : {QByteArray("{not json"), QByteArray("{\"a\":1}"), QByteArray("[1]"),
                                  QByteArray("[\"\"]"), QByteArray("[\"relative\"]")}) {
        write_file(path, bad);
        ok = true;
        CHECK(entries(&ok).isEmpty() && !ok, "a journal that cannot be read, or holds no paths, is not ok");
        CHECK(!ImportService::journalAdd({a}) && !ImportService::journalRemove({a}),
              "an unreadable journal takes no change");
        CHECK(read_file(path) == bad, "and is left as it was");
    }
    QFile::remove(path);
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-import-service-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QDir().mkpath(dataDir);
    // The teardown case warns that it left a copy behind; that warning is expected and checked, not printed.
    static QtMessageHandler previous = nullptr;
    static int abandonedWarnings = 0;
    previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context, const QString& message) {
        if (type == QtWarningMsg && message.contains(QStringLiteral("left behind"))) {
            ++abandonedWarnings;
            return;
        }
        if (previous != nullptr) {
            previous(type, context, message);
        } else {
            std::fprintf(stderr, "%s\n", qPrintable(message));
        }
    });

    test_sweep();
    test_order_and_staging();
    test_cancel();
    test_busy();
    test_staging_failure();
    test_teardown_idle();
    CHECK(abandonedWarnings == 0, "a teardown that finished in time warns of nothing");
    test_teardown_blocked();
    CHECK(abandonedWarnings == 1, "a teardown that left a copy behind says so");
    test_journal();
    QDir(dataDir).removeRecursively();
    return failures != 0 ? 1 : 0;
}
