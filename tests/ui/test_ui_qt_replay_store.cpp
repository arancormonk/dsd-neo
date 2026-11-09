// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the replay store (#832). A replay picked in the wizard copies on the import worker into replays/ (a
 * local file is answered with its path), within the free space less the reserve, with progress and cancel; the copy
 * is recorded pending, saved systems adopt and release it, and a later start deletes only pending copies no saved
 * system names, and nothing when a store cannot be read, the decoder runs or a restore is unfinished. The startup
 * migration of replays kept in the OS cache copies on the worker and repoints only rows still naming the cached file. */

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <memory>
#include <mutex>
#include <qflags.h>
#include <qsystemdetection.h>
#include <utility>

#include "../test_support/qt_test_paths.h"
#include "../test_support/test_support.h"
#include "backup_apply.h"
#include "decoder_host.h"
#include "document_import_transport.h"
#include "import_service.h"
#include "json_store.h"
#include "qt_ui.h"
#include "replay_store.h"
#include "saved_systems_model.h"

using dsd_qt::DecoderHost;
using dsd_qt::DocumentImportTransport;
using dsd_qt::ImportCopyProgress;
using dsd_qt::ImportService;
using dsd_qt::ReplayStore;
using dsd_qt::SavedSystemsModel;

namespace {

int failures = 0;

void
check_at(bool ok, int line, const char* why) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "test_ui_qt_replay_store.cpp:%d: %s\n", line, why);
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

/* Run the event loop until @p done holds; false after about @p ms milliseconds. */
bool
wait_until(const std::function<bool()>& done, int ms = 5000) {
    for (int i = 0; i < ms / 2; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        if (done()) {
            return true;
        }
        QThread::msleep(2);
    }
    return done();
}

/* What @p body printed to stderr (the library's log lines), kept off the test's output. */
QByteArray
captured_stderr(const std::function<void()>& body) {
    dsd_test_capture_stderr capture;
    if (dsd_test_capture_stderr_begin(&capture, "replay_store_log") != 0) {
        body();
        return {};
    }
    body();
    (void)dsd_test_capture_stderr_end(&capture);
    char text[4096];
    return dsd_test_capture_stderr_read(&capture, text, sizeof(text)) == 0 ? QByteArray(text) : QByteArray();
}

bool
staging_empty() {
    return QDir(ImportService::stagingRoot())
        .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
        .isEmpty();
}

QStringList
replay_files() {
    return QDir(ReplayStore::replaysRoot()).entryList(QDir::Files | QDir::Hidden | QDir::System);
}

/* The manifest as path → state; "unreadable" under the empty key when it is not a clean array of entries. */
QMap<QString, QString>
manifest() {
    QMap<QString, QString> states;
    QFile file(ReplayStore::manifestPath());
    if (!file.exists()) {
        return states;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(read_file(file.fileName()));
    if (!doc.isArray()) {
        states.insert(QString(), QStringLiteral("unreadable"));
        return states;
    }
    for (const auto& value : doc.array()) {
        const QJsonObject entry = value.toObject();
        states.insert(entry.value(QStringLiteral("path")).toString(), entry.value(QStringLiteral("state")).toString());
    }
    return states;
}

/* Every store and folder the tests write, gone. */
void
reset_data() {
    QDir(ReplayStore::replaysRoot()).removeRecursively();
    QDir(ImportService::stagingRoot()).removeRecursively();
    QFile::remove(ReplayStore::manifestPath());
    QFile::remove(ReplayStore::manifestPath() + QStringLiteral(".bad"));
    QFile::remove(dsd_qt::json_store_path(QStringLiteral("saved_systems.json")));
}

QString
sources_dir() {
    return dsd_qt::json_store_root() + QStringLiteral("/test-sources");
}

/* A host whose decoder is busy on request. */
class TestHost : public DecoderHost {
  public:
    bool busy = false;

    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return {};
    }

    bool
    start(const QStringList&) override {
        return false;
    }

    void
    stop() override {}

    bool
    engineBusy() const override {
        return busy;
    }
};

/*
 * The provider stand-in. content://test/<name> copies sources_dir()/<name> as a provider would (name and size
 * first); content://full/<name> writes part of a copy and fails for want of space; content://hold/<name> reports
 * progress and waits for its cancel; content://stuck/<name> ignores its cancel, as a provider blocked in a read
 * would: it waits for releaseStuck(), then copies sources_dir()/<name> whole. A local path copies as the base
 * transport does, held first while holdLocal is set.
 */
class FakeTransport : public DocumentImportTransport {
  public:
    int
    copy(qint64 request, const QString& reference, const QString& destPath, qint64 maxBytes) override {
        noteCopy(reference);
        if (reference.startsWith(QStringLiteral("content://test/"))) {
            return DocumentImportTransport::copy(request, sources_dir() + QLatin1Char('/') + reference.mid(15),
                                                 destPath, maxBytes);
        }
        if (reference.startsWith(QStringLiteral("content://full/"))) {
            write_file(destPath, QByteArray(100, 'x'));
            setFake(request, {reference.mid(15), 100, 4096, true});
            return DecoderHost::DocumentLocalFailed;
        }
        if (reference.startsWith(QStringLiteral("content://hold/"))) {
            setFake(request, {reference.mid(15), 1234, 5678, false});
            std::unique_lock<std::mutex> guard(m_lock);
            m_wake.wait(guard, [this, request]() { return m_cancelled.contains(request); });
            return DecoderHost::DocumentCancelled;
        }
        if (reference.startsWith(QStringLiteral("content://stuck/"))) {
            setFake(request, {reference.mid(16), 10, 100, false});
            std::unique_lock<std::mutex> guard(m_lock);
            m_stuck = true;
            m_wake.wait(guard, [this]() { return m_stuckReleased; });
            m_stuck = false;
            guard.unlock();
            return QFile::copy(sources_dir() + QLatin1Char('/') + reference.mid(16), destPath)
                       ? DecoderHost::DocumentCopied
                       : DecoderHost::DocumentFailed;
        }
        {
            std::unique_lock<std::mutex> guard(m_lock);
            m_wake.wait(guard, [this, request]() { return !m_holdLocal || m_cancelled.contains(request); });
        }
        return DocumentImportTransport::copy(request, reference, destPath, maxBytes);
    }

    /* Let a content://stuck/ copy go on; one started later goes on at once. */
    void
    releaseStuck() {
        {
            const std::lock_guard<std::mutex> guard(m_lock);
            m_stuckReleased = true;
        }
        m_wake.notify_all();
    }

    bool
    stuck() const {
        const std::lock_guard<std::mutex> guard(m_lock);
        return m_stuck;
    }

    void
    cancel(qint64 request) override {
        {
            const std::lock_guard<std::mutex> guard(m_lock);
            m_cancelled.insert(request);
        }
        m_wake.notify_all();
        DocumentImportTransport::cancel(request);
    }

    ImportCopyProgress
    progress(qint64 request) const override {
        {
            const std::lock_guard<std::mutex> guard(m_lock);
            const auto found = m_fake.constFind(request);
            if (found != m_fake.constEnd()) {
                return *found;
            }
        }
        return DocumentImportTransport::progress(request);
    }

    void
    forget(qint64 request) override {
        {
            const std::lock_guard<std::mutex> guard(m_lock);
            m_fake.remove(request);
            m_cancelled.remove(request);
        }
        DocumentImportTransport::forget(request);
    }

    void
    setHoldLocal(bool hold) {
        {
            const std::lock_guard<std::mutex> guard(m_lock);
            m_holdLocal = hold;
        }
        m_wake.notify_all();
    }

    /* Each reference a copy was asked for, and whether every copy ran off the main thread. */
    QStringList
    copies() const {
        const std::lock_guard<std::mutex> guard(m_lock);
        return m_copies;
    }

    bool
    allOffMainThread() const {
        const std::lock_guard<std::mutex> guard(m_lock);
        return m_offMain;
    }

    int
    holding() const {
        const std::lock_guard<std::mutex> guard(m_lock);
        return m_holding;
    }

  private:
    void
    noteCopy(const QString& reference) {
        const std::lock_guard<std::mutex> guard(m_lock);
        m_copies.append(reference);
        m_offMain = m_offMain && QThread::currentThread() != QCoreApplication::instance()->thread();
        m_holding += m_holdLocal ? 1 : 0;
    }

    void
    setFake(qint64 request, const ImportCopyProgress& progress) {
        const std::lock_guard<std::mutex> guard(m_lock);
        m_fake.insert(request, progress);
    }

    mutable std::mutex m_lock;
    std::condition_variable m_wake;
    QHash<qint64, ImportCopyProgress> m_fake; // guarded by m_lock
    QSet<qint64> m_cancelled;                 // guarded by m_lock
    QStringList m_copies;                     // guarded by m_lock
    bool m_holdLocal = false;                 // guarded by m_lock
    bool m_offMain = true;                    // guarded by m_lock
    int m_holding = 0;                        // guarded by m_lock
    bool m_stuck = false;                     // guarded by m_lock
    bool m_stuckReleased = false;             // guarded by m_lock
};

/* One store over one service, with what it answered. */
struct Rig {
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    TestHost host;
    std::unique_ptr<ImportService> service;
    std::unique_ptr<ReplayStore> store;
    QMap<int, QVariantMap> answers;
    int progressSignals = 0;

    Rig() : service(new ImportService(transport)), store(new ReplayStore(&host, service.get())) {
        QObject::connect(store.get(), &ReplayStore::replayImportFinished,
                         [this](int id, const QVariantMap& result) { answers.insert(id, result); });
        QObject::connect(store.get(), &ReplayStore::progressChanged, [this]() { ++progressSignals; });
    }

    /* Begin an import and wait for its answer. */
    QVariantMap
    import(const QString& reference, const QString& name) {
        const int id = store->beginReplayImport(reference, name);
        CHECK(id > 0, "an import has a positive id");
        CHECK(!answers.contains(id), "an import is answered later, never inside the call");
        CHECK(wait_until([this, id]() { return answers.contains(id); }), "the import is answered");
        return answers.value(id);
    }
};

QVariantMap
replay_system(const QString& name, const QString& path) {
    return {{QStringLiteral("name"), name},
            {QStringLiteral("sourceType"), QStringLiteral("file")},
            {QStringLiteral("filePath"), path}};
}

/* I5: a local file is answered with its plain path, queued, and never copied or recorded. */
void
test_local_reference_answers_its_path() {
    reset_data();
    Rig rig;
    const QString local = sources_dir() + QStringLiteral("/local capture.wav");
    write_file(local, QByteArray("local"));
    QVariantMap result = rig.import(QUrl::fromLocalFile(local).toString(), QStringLiteral("local capture.wav"));
    CHECK(result.value(QStringLiteral("ok")).toBool(), "a local file URL is answered ok");
    CHECK(result.value(QStringLiteral("path")).toString() == local, "a file URL is answered with its plain path");
    result = rig.import(local, QStringLiteral("local capture.wav"));
    CHECK(result.value(QStringLiteral("path")).toString() == local, "an absolute path is answered as it is");
    CHECK(rig.transport->copies().isEmpty() && replay_files().isEmpty(), "a local file is never copied");
    CHECK(manifest().isEmpty(), "a local file is never recorded");
}

/* A provider's document copies on the worker into replays/, under its provider's name, and is recorded pending. */
void
test_copy_is_recorded_pending() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/capture.wav"), QByteArray("replay bytes"));
    Rig rig;
    const int id = rig.store->beginReplayImport(QStringLiteral("content://test/capture.wav"), QStringLiteral("hint"));
    CHECK(rig.store->copying(), "copying from the start");
    CHECK(wait_until([&rig, id]() { return rig.answers.contains(id); }), "the copy is answered");
    const QVariantMap result = rig.answers.value(id);
    const QString path = result.value(QStringLiteral("path")).toString();
    CHECK(result.value(QStringLiteral("ok")).toBool(), "the copy is ok");
    CHECK(path == ReplayStore::replaysRoot() + QStringLiteral("/capture.wav"), "named by its provider in replays/");
    CHECK(result.value(QStringLiteral("name")).toString() == QStringLiteral("capture.wav"), "the name is answered");
    CHECK(read_file(path) == "replay bytes", "the copy holds the document");
    CHECK(manifest().value(path) == QStringLiteral("pending"), "the copy is recorded pending");
    CHECK(!rig.store->copying(), "not copying once answered");
    CHECK(rig.progressSignals > 0, "progress was announced");
    CHECK(wait_until(staging_empty), "the staging folder is removed");
    // The same name again takes a free form of it.
    const QVariantMap again = rig.import(QStringLiteral("content://test/capture.wav"), QStringLiteral("hint"));
    CHECK(again.value(QStringLiteral("path")).toString()
              == ReplayStore::replaysRoot() + QStringLiteral("/capture (2).wav"),
          "a second copy takes a free name");
}

/* B1: a re-picked copy nobody saved is deleted at the next start; the saved one is kept. */
void
test_repicked_copy_is_reclaimed() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/a.wav"), QByteArray("a"));
    write_file(sources_dir() + QStringLiteral("/b.wav"), QByteArray("b"));
    {
        Rig rig;
        SavedSystemsModel systems;
        systems.setReplayStore(rig.store.get());
        const QString a = rig.import(QStringLiteral("content://test/a.wav"), QString()).value("path").toString();
        const QString b = rig.import(QStringLiteral("content://test/b.wav"), QString()).value("path").toString();
        CHECK(systems.add(replay_system(QStringLiteral("Replay"), b)), "the system is saved");
        CHECK(manifest().value(b) == QStringLiteral("kept"), "the saved copy is kept");
        CHECK(manifest().value(a) == QStringLiteral("pending"), "the re-picked copy stays pending");
    }
    // The next start.
    Rig rig;
    SavedSystemsModel systems;
    rig.store->reclaim(systems, systems.storeLoaded());
    CHECK(replay_files() == QStringList{QStringLiteral("b.wav")}, "only the saved copy is left");
    CHECK(manifest().size() == 1 && manifest().value(ReplayStore::replaysRoot() + QStringLiteral("/b.wav")) == "kept",
          "the reclaimed copy leaves the manifest");
}

/* Adopt is written before the save: a save that fails then leaves the copy kept and unreferenced, never deleted. */
void
test_failed_save_after_adopt_keeps_the_copy() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/c.wav"), QByteArray("c"));
    QString c;
    {
        Rig rig;
        SavedSystemsModel systems;
        systems.setReplayStore(rig.store.get());
        c = rig.import(QStringLiteral("content://test/c.wav"), QString()).value("path").toString();
        const QString store = dsd_qt::json_store_path(QStringLiteral("saved_systems.json"));
        CHECK(QDir().mkpath(store), "the saved systems store is blocked");
        CHECK(!systems.add(replay_system(QStringLiteral("Blocked"), c)), "the save fails");
        CHECK(QDir(store).removeRecursively(), "the store is unblocked");
        CHECK(manifest().value(c) == QStringLiteral("kept"), "the copy was adopted before the save");
    }
    Rig rig;
    SavedSystemsModel systems;
    rig.store->reclaim(systems, systems.storeLoaded());
    CHECK(QFileInfo::exists(c), "a kept copy nobody names survives");
}

/* A manifest that cannot be read refuses the save that would name a copy, and is set aside at the next start. */
void
test_unreadable_manifest() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/d.wav"), QByteArray("d"));
    Rig rig;
    SavedSystemsModel systems;
    systems.setReplayStore(rig.store.get());
    const QString d = rig.import(QStringLiteral("content://test/d.wav"), QString()).value("path").toString();
    write_file(ReplayStore::manifestPath(), QByteArray("[{"));
    bool added = true;
    const QByteArray log =
        captured_stderr([&]() { added = systems.add(replay_system(QStringLiteral("Unrecorded"), d)); });
    CHECK(!added, "nothing is saved naming an unadopted copy");
    CHECK(log.contains("could not be marked as used"), "the refusal is logged");
    CHECK(systems.count() == 0, "no row was added");
    CHECK(systems.add(replay_system(QStringLiteral("Elsewhere"), QStringLiteral("/elsewhere/capture.wav"))),
          "a path outside replays/ saves without the manifest");
    rig.store->reclaim(systems, systems.storeLoaded());
    CHECK(QFileInfo::exists(d), "an unreadable manifest deletes nothing");
    CHECK(read_file(ReplayStore::manifestPath() + QStringLiteral(".bad")) == "[{", "it is set aside");
    CHECK(!QFileInfo::exists(ReplayStore::manifestPath()), "later copies start a fresh manifest");
    CHECK(systems.add(replay_system(QStringLiteral("Unlisted"), d)), "a copy the manifest no longer lists saves");
}

/* Release comes after the save that dropped the last system naming the copy, on a remove and on an edit. */
void
test_release_after_remove_and_edit() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/e.wav"), QByteArray("e"));
    write_file(sources_dir() + QStringLiteral("/f.wav"), QByteArray("f"));
    Rig rig;
    SavedSystemsModel systems;
    systems.setReplayStore(rig.store.get());
    const QString e = rig.import(QStringLiteral("content://test/e.wav"), QString()).value("path").toString();
    const QString f = rig.import(QStringLiteral("content://test/f.wav"), QString()).value("path").toString();
    CHECK(systems.add(replay_system(QStringLiteral("One"), e)) && systems.add(replay_system(QStringLiteral("Two"), e)),
          "two systems name one copy");
    CHECK(systems.remove(0) && manifest().value(e) == QStringLiteral("kept"), "still kept while one names it");
    const QString store = dsd_qt::json_store_path(QStringLiteral("saved_systems.json"));
    const QByteArray saved = read_file(store);
    CHECK(QFile::remove(store) && QDir().mkpath(store), "the saved systems store is blocked");
    CHECK(!systems.remove(0), "the remove fails");
    CHECK(manifest().value(e) == QStringLiteral("kept"), "a remove that did not save releases nothing");
    CHECK(!systems.update(0, {{QStringLiteral("filePath"), f}}), "the edit fails");
    CHECK(manifest().value(e) == QStringLiteral("kept"), "an edit that did not save releases nothing");
    CHECK(QDir(store).removeRecursively(), "the store is unblocked");
    write_file(store, saved);
    CHECK(systems.update(0, {{QStringLiteral("filePath"), f}}), "the edit saves");
    CHECK(manifest().value(f) == QStringLiteral("kept"), "the new copy is kept");
    CHECK(manifest().value(e) == QStringLiteral("pending"), "the copy no system names is released");
    CHECK(systems.remove(0) && manifest().value(f) == QStringLiteral("pending"), "a remove releases its copy");
    rig.store->reclaim(systems, systems.storeLoaded());
    CHECK(replay_files().isEmpty() && manifest().isEmpty(), "released copies are reclaimed at the next start");
}

/* A restore clears filePath but keeps the files: a kept copy stays kept. A restore waiting or stopped part-way skips
   the reclaim at that start. */
void
test_restore_keeps_kept_copies() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/k.wav"), QByteArray("k"));
    write_file(sources_dir() + QStringLiteral("/p.wav"), QByteArray("p"));
    QString kept;
    QString pending;
    {
        Rig rig;
        SavedSystemsModel systems;
        systems.setReplayStore(rig.store.get());
        kept = rig.import(QStringLiteral("content://test/k.wav"), QString()).value("path").toString();
        pending = rig.import(QStringLiteral("content://test/p.wav"), QString()).value("path").toString();
        CHECK(systems.add(replay_system(QStringLiteral("Kept"), kept)), "the system is saved");
    }
    // What the restore leaves: the same system, its filePath cleared.
    QJsonArray rows = dsd_qt::json_store_load_array(QStringLiteral("saved_systems.json"));
    QJsonObject row = rows.at(0).toObject();
    row.insert(QStringLiteral("filePath"), QString());
    rows.replace(0, row);
    CHECK(dsd_qt::json_store_save_array(QStringLiteral("saved_systems.json"), rows), "the restored store is written");
    Rig rig;
    QObject owner;
    for (const auto outcome : {dsd_qt::backup::ApplyOutcome::Deferred, dsd_qt::backup::ApplyOutcome::Stuck}) {
        SavedSystemsModel systems;
        CHECK(dsd_qt::load_replays(&rig.host, rig.service.get(), &systems, outcome, &owner) != nullptr,
              "the store loads");
        CHECK(QFileInfo::exists(pending), "an unfinished restore skips the reclaim");
    }
    SavedSystemsModel systems;
    (void)dsd_qt::load_replays(&rig.host, rig.service.get(), &systems, dsd_qt::backup::ApplyOutcome::Applied, &owner);
    CHECK(QFileInfo::exists(kept), "a kept copy outlives the restore");
    CHECK(!QFileInfo::exists(pending), "a pending copy goes after an applied restore");
}

/* Nothing is deleted when the saved systems store cannot be read, or while the decoder runs. */
void
test_unsafe_starts_delete_nothing() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/q.wav"), QByteArray("q"));
    Rig rig;
    QObject owner;
    const QString q = rig.import(QStringLiteral("content://test/q.wav"), QString()).value("path").toString();
    write_file(dsd_qt::json_store_path(QStringLiteral("saved_systems.json")), QByteArray("[1,"));
    {
        SavedSystemsModel corrupt;
        CHECK(!corrupt.storeLoaded(), "a malformed store does not load cleanly");
        (void)dsd_qt::load_replays(&rig.host, rig.service.get(), &corrupt, dsd_qt::backup::ApplyOutcome::None, &owner);
        CHECK(QFileInfo::exists(q), "a corrupt saved systems store deletes nothing");
    }
    QFile::remove(dsd_qt::json_store_path(QStringLiteral("saved_systems.json")));
    SavedSystemsModel systems;
    CHECK(systems.storeLoaded(), "an absent store is a clean one");
    rig.host.busy = true;
    (void)dsd_qt::load_replays(&rig.host, rig.service.get(), &systems, dsd_qt::backup::ApplyOutcome::None, &owner);
    CHECK(QFileInfo::exists(q), "nothing is deleted while the decoder runs");
    rig.host.busy = false;
    (void)dsd_qt::load_replays(&rig.host, rig.service.get(), &systems, dsd_qt::backup::ApplyOutcome::None, &owner);
    CHECK(!QFileInfo::exists(q), "an idle clean start reclaims it");
}

/* The copy must leave kReserveBytes free: a document larger than the rest is too_large, with what it needs. */
void
test_not_enough_space() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/big.wav"), QByteArray(100, 'b'));
    Rig rig;
    rig.store->setFreeSpace([](const QString&) { return ReplayStore::kReserveBytes + 10; });
    QVariantMap result = rig.import(QStringLiteral("content://test/big.wav"), QString());
    CHECK(result.value(QStringLiteral("error")).toString() == QStringLiteral("too_large"), "too large for the space");
    CHECK(result.value(QStringLiteral("neededBytes")).toLongLong() == 100 + ReplayStore::kReserveBytes,
          "it needs its size and the reserve");
    CHECK(result.value(QStringLiteral("freeBytes")).toLongLong() == ReplayStore::kReserveBytes + 10,
          "the free space is answered");
    CHECK(result.value(QStringLiteral("reserveBytes")).toLongLong() == ReplayStore::kReserveBytes,
          "the reserve is answered, so the size can be told apart from it");
    CHECK(result.value(QStringLiteral("name")).toString() == QStringLiteral("big.wav"), "the name is answered");
    // Not even the reserve free: refused before the document is opened, even an empty one.
    write_file(sources_dir() + QStringLiteral("/empty.wav"), QByteArray());
    const qsizetype opened = rig.transport->copies().size();
    rig.store->setFreeSpace([](const QString&) { return ReplayStore::kReserveBytes / 2; });
    result = rig.import(QStringLiteral("content://test/empty.wav"), QStringLiteral("empty.wav"));
    CHECK(result.value(QStringLiteral("error")).toString() == QStringLiteral("too_large"), "less than the reserve");
    CHECK(result.value(QStringLiteral("neededBytes")).toLongLong() == -1
              && result.value(QStringLiteral("freeBytes")).toLongLong() == ReplayStore::kReserveBytes / 2
              && result.value(QStringLiteral("reserveBytes")).toLongLong() == ReplayStore::kReserveBytes,
          "the free space and the reserve are answered, the size unknown");
    CHECK(result.value(QStringLiteral("name")).toString() == QStringLiteral("empty.wav"),
          "the pick's name is answered");
    CHECK(rig.transport->copies().size() == opened, "the document is never opened");
    CHECK(replay_files().isEmpty() && manifest().isEmpty(), "nothing is kept of a refused copy");
    rig.store->setFreeSpace([](const QString&) { return ReplayStore::kReserveBytes + 100; });
    CHECK(rig.import(QStringLiteral("content://test/big.wav"), QString()).value("ok").toBool(), "an exact fit copies");
}

/* A request whose staging folder cannot be made never runs its stage; its answer still names the pick. */
void
test_every_answer_has_a_name() {
    reset_data();
    write_file(ImportService::stagingRoot(), QByteArray("not a folder"));
    Rig rig;
    const QVariantMap result = rig.import(QStringLiteral("content://test/x.wav"), QStringLiteral("picked.wav"));
    CHECK(result.value(QStringLiteral("error")).toString() == QStringLiteral("local"), "no staging folder is local");
    CHECK(result.value(QStringLiteral("name")).toString() == QStringLiteral("picked.wav"), "named from the pick");
    QFile::remove(ImportService::stagingRoot());
}

/* Finding r1-2: a pending copy a saved system names survives the reclaim (a row saved without the store, as by an
   older version); a release that cannot be written leaves the copy kept; a link or a folder in replays/ is never
   deleted. */
void
test_reclaim_guards() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/n.wav"), QByteArray("n"));
    write_file(sources_dir() + QStringLiteral("/k.wav"), QByteArray("k"));
    Rig rig;
    const QString named = rig.import(QStringLiteral("content://test/n.wav"), QString()).value("path").toString();
    const QString kept = rig.import(QStringLiteral("content://test/k.wav"), QString()).value("path").toString();
    {
        SavedSystemsModel unadopted; // no store: the copy stays pending while named
        CHECK(unadopted.add(replay_system(QStringLiteral("Named"), named)), "a system names the pending copy");
    }
    SavedSystemsModel systems;
    systems.setReplayStore(rig.store.get());
    CHECK(systems.add(replay_system(QStringLiteral("Kept"), kept)), "a system keeps the other copy");
    CHECK(manifest().value(named) == QStringLiteral("pending"), "the named copy is pending");
    rig.store->reclaim(systems, systems.storeLoaded());
    CHECK(QFileInfo::exists(named), "a pending copy a saved system names is never reclaimed");
    // The release cannot be written: the copy stays kept.
    const QByteArray list = read_file(ReplayStore::manifestPath());
    CHECK(QFile::remove(ReplayStore::manifestPath()) && QDir().mkpath(ReplayStore::manifestPath()), "list blocked");
    bool removed = false;
    const QByteArray log = captured_stderr([&systems, &removed]() { removed = systems.remove(1); });
    CHECK(removed && log.contains("could not be marked unused"), "the remove saves; the failed release is logged");
    CHECK(QDir(ReplayStore::manifestPath()).removeRecursively(), "the list is unblocked");
    write_file(ReplayStore::manifestPath(), list);
    CHECK(manifest().value(kept) == QStringLiteral("kept"), "a release that failed leaves the copy kept");
    // A link and a folder the list calls pending are not what the store made.
    const QString folder = ReplayStore::replaysRoot() + QStringLiteral("/folder.wav");
    CHECK(QDir().mkpath(folder), "a folder in replays/");
    QJsonArray entries = dsd_qt::json_store_load_array(QStringLiteral("replay_copies.json"));
    entries.append(QJsonObject{{QStringLiteral("path"), folder}, {QStringLiteral("state"), QStringLiteral("pending")}});
#ifdef Q_OS_UNIX
    const QString target = sources_dir() + QStringLiteral("/target.wav");
    const QString link = ReplayStore::replaysRoot() + QStringLiteral("/link.wav");
    write_file(target, QByteArray("outside"));
    CHECK(QFile::link(target, link), "a link in replays/");
    entries.append(QJsonObject{{QStringLiteral("path"), link}, {QStringLiteral("state"), QStringLiteral("pending")}});
#endif
    CHECK(dsd_qt::json_store_save_array(QStringLiteral("replay_copies.json"), entries), "the list names them");
    rig.store->reclaim(systems, systems.storeLoaded());
    CHECK(QFileInfo(folder).isDir() && manifest().contains(folder), "a folder is never deleted");
#ifdef Q_OS_UNIX
    CHECK(QFileInfo(link).isSymLink() && read_file(target) == "outside" && manifest().contains(link),
          "a link is never deleted or followed");
#endif
}

#ifdef Q_OS_UNIX
/* Finding r1-1: a reclaim that deleted a copy but could not save the list leaves a stale entry; a new copy under the
   same name replaces it, so adopting the new copy keeps it through a restore. */
void
test_stale_entry_never_shadows_a_new_copy() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/capture.wav"), QByteArray("first"));
    Rig rig;
    const QString path = rig.import(QStringLiteral("content://test/capture.wav"), QString()).value("path").toString();
    const QString root = dsd_qt::json_store_root();
    const QFileDevice::Permissions writable = QFile::permissions(root);
    {
        SavedSystemsModel empty;
        CHECK(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::ExeOwner), "the list is read-only");
        const QByteArray log = captured_stderr([&rig, &empty]() { rig.store->reclaim(empty, empty.storeLoaded()); });
        CHECK(QFile::setPermissions(root, writable), "the data folder is writable again");
        CHECK(!QFileInfo::exists(path) && log.contains("could not be saved"), "deleted, but the list kept its entry");
        CHECK(manifest().value(path) == QStringLiteral("pending"), "the stale entry stays");
    }
    write_file(sources_dir() + QStringLiteral("/capture.wav"), QByteArray("second"));
    const QString again = rig.import(QStringLiteral("content://test/capture.wav"), QString()).value("path").toString();
    CHECK(again == path, "the re-pick takes the same name");
    CHECK(manifest().size() == 1, "the stale entry is replaced");
    {
        SavedSystemsModel systems;
        systems.setReplayStore(rig.store.get());
        CHECK(systems.add(replay_system(QStringLiteral("Replay"), again)), "a system names the new copy");
    }
    QJsonArray rows = dsd_qt::json_store_load_array(QStringLiteral("saved_systems.json"));
    QJsonObject row = rows.at(0).toObject();
    row.insert(QStringLiteral("filePath"), QString());
    rows.replace(0, row);
    CHECK(dsd_qt::json_store_save_array(QStringLiteral("saved_systems.json"), rows), "the restore clears filePath");
    SavedSystemsModel restored;
    rig.store->reclaim(restored, restored.storeLoaded());
    CHECK(read_file(again) == "second", "the kept copy outlives the restore");
}
#endif

/* A disk that fills during the copy: local with noSpace, and its staging folder goes. */
void
test_no_space_during_copy() {
    reset_data();
    Rig rig;
    const QVariantMap result = rig.import(QStringLiteral("content://full/huge.wav"), QString());
    CHECK(result.value(QStringLiteral("error")).toString() == QStringLiteral("local"), "a full disk is local");
    CHECK(result.value(QStringLiteral("noSpace")).toBool(), "for want of space");
    CHECK(result.value(QStringLiteral("name")).toString() == QStringLiteral("huge.wav"), "the name is answered");
    CHECK(wait_until(staging_empty), "the part written in staging is removed");
    CHECK(replay_files().isEmpty() && manifest().isEmpty(), "nothing is published");
}

/* Cancel answers cancelled; progress is read while the copy runs; a new import cancels the running one. */
void
test_cancel_and_progress() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/g.wav"), QByteArray("g"));
    Rig rig;
    const int id = rig.store->beginReplayImport(QStringLiteral("content://hold/held.wav"), QString());
    CHECK(wait_until([&rig]() { return rig.store->copyBytes() == 1234 && rig.store->copyTotal() == 5678; }),
          "the running copy's progress is read");
    CHECK(rig.store->copyName() == QStringLiteral("held.wav") && rig.store->copying(), "copying held.wav");
    rig.store->cancelReplayImport();
    CHECK(wait_until([&rig, id]() { return rig.answers.contains(id); }), "a cancelled copy is answered");
    CHECK(rig.answers.value(id).value(QStringLiteral("error")).toString() == QStringLiteral("cancelled"), "cancelled");
    CHECK(!rig.store->copying(), "no copy runs");
    const int held = rig.store->beginReplayImport(QStringLiteral("content://hold/held.wav"), QString());
    const QVariantMap next = rig.import(QStringLiteral("content://test/g.wav"), QString());
    CHECK(rig.answers.value(held).value(QStringLiteral("error")).toString() == QStringLiteral("cancelled"),
          "a new import cancels the running one");
    CHECK(next.value(QStringLiteral("ok")).toBool() && replay_files() == QStringList{QStringLiteral("g.wav")},
          "and copies");
}

/* A copy that ignores its cancel: Cancel is answered at once, exactly once, and copying() turns false; the copy
   publishes nothing when it returns, and the next copy waits behind it, then runs. */
void
test_cancel_does_not_wait_for_the_copy() {
    reset_data();
    write_file(sources_dir() + QStringLiteral("/stuck.wav"), QByteArray("stuck"));
    write_file(sources_dir() + QStringLiteral("/next.wav"), QByteArray("next"));
    Rig rig;
    int answered = 0;
    QObject::connect(rig.store.get(), &ReplayStore::replayImportFinished, [&answered]() { ++answered; });
    const int id = rig.store->beginReplayImport(QStringLiteral("content://stuck/stuck.wav"), QString());
    CHECK(wait_until([&rig]() { return rig.transport->stuck(); }), "the copy is stuck");
    rig.store->cancelReplayImport();
    CHECK(!rig.answers.contains(id), "a cancel is answered later, never inside the call");
    CHECK(!rig.store->copying(), "no copy shows from the cancel on");
    CHECK(wait_until([&rig, id]() { return rig.answers.contains(id); }), "the cancel is answered");
    CHECK(rig.answers.value(id).value(QStringLiteral("error")).toString() == QStringLiteral("cancelled")
              && rig.answers.value(id).value(QStringLiteral("name")).toString() == QStringLiteral("stuck.wav"),
          "cancelled, with its name");
    CHECK(rig.transport->stuck(), "while the copy is still stuck");
    const int next = rig.store->beginReplayImport(QStringLiteral("content://test/next.wav"), QString());
    CHECK(!wait_until([&rig, next]() { return rig.answers.contains(next); }, 200),
          "the next copy waits behind the stuck one");
    rig.transport->releaseStuck();
    CHECK(wait_until([&rig, next]() { return rig.answers.contains(next); }), "the next copy runs once it returns");
    CHECK(rig.answers.value(next).value(QStringLiteral("ok")).toBool(), "and is copied");
    CHECK(wait_until(staging_empty), "the stuck copy's staging goes");
    CHECK(answered == 2, "the cancelled copy is answered exactly once");
    CHECK(replay_files() == QStringList{QStringLiteral("next.wav")} && manifest().size() == 1,
          "the stuck copy publishes nothing when it returns");
}

/* The service goes first at teardown (it is the engine's first child): a store left with a copy running is fine. */
void
test_teardown_service_first() {
    reset_data();
    Rig rig;
    (void)rig.store->beginReplayImport(QStringLiteral("content://hold/held.wav"), QString());
    CHECK(wait_until([&rig]() { return rig.store->copyBytes() == 1234; }), "the copy runs");
    rig.service.reset();
    rig.store.reset();
    CHECK(rig.answers.isEmpty(), "no answer after teardown");
}

QString
cache_file(const QString& name, const QByteArray& bytes) {
    const QString path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QLatin1Char('/') + name;
    write_file(path, bytes);
    return path;
}

/* B10: the cached-replay migration copies on the worker (the startup path returns at once), through the transport
   even though the source is local, repoints only rows still naming the cached file, and leaves that file. */
void
test_migration_runs_off_the_qt_thread() {
    reset_data();
    const QString legacy = cache_file(QStringLiteral("legacy.wav"), QByteArray("test replay copy"));
    const QString other = cache_file(QStringLiteral("other.wav"), QByteArray("other replay"));
    Rig rig;
    SavedSystemsModel systems;
    systems.setReplayStore(rig.store.get());
    CHECK(systems.add(replay_system(QStringLiteral("A"), legacy))
              && systems.add(replay_system(QStringLiteral("B"), legacy))
              && systems.add(replay_system(QStringLiteral("C"), other)),
          "three systems keep cached replays");
    rig.transport->setHoldLocal(true);
    rig.store->migrateCachedReplays(&systems);
    CHECK(systems.get(0).value(QStringLiteral("filePath")).toString() == legacy, "the startup path does not wait");
    CHECK(wait_until([&rig]() { return rig.transport->holding() > 0; }), "the copy runs on the worker");
    // Edited while the copy runs: the row stays as the user made it.
    CHECK(systems.update(2, {{QStringLiteral("filePath"), QStringLiteral("/elsewhere/other.wav")}}), "C is edited");
    rig.transport->setHoldLocal(false);
    CHECK(wait_until([&rig]() { return !rig.service->busy(); }), "the migration finishes");
    const QString durable = ReplayStore::replaysRoot() + QStringLiteral("/legacy.wav");
    const QString unused = ReplayStore::replaysRoot() + QStringLiteral("/other.wav");
    CHECK(systems.get(0).value(QStringLiteral("filePath")).toString() == durable
              && systems.get(1).value(QStringLiteral("filePath")).toString() == durable,
          "the rows still naming the cached file take one copy");
    CHECK(read_file(durable) == "test replay copy" && manifest().value(durable) == QStringLiteral("kept"),
          "the copy is kept");
    CHECK(systems.get(2).value(QStringLiteral("filePath")).toString() == QStringLiteral("/elsewhere/other.wav"),
          "an edited row keeps its edit");
    CHECK(manifest().value(unused) == QStringLiteral("pending"), "the copy no row took is released");
    CHECK(QFileInfo::exists(legacy) && QFileInfo::exists(other), "the cached originals stay");
    CHECK(rig.transport->copies().contains(legacy) && rig.transport->copies().contains(other),
          "the cached files copy through the transport");
    CHECK(rig.transport->allOffMainThread(), "every copy ran off the Qt thread");
    {
        SavedSystemsModel reopened;
        CHECK(reopened.get(0).value(QStringLiteral("filePath")).toString() == durable, "the repoint is saved");
    }
    const int copies = static_cast<int>(rig.transport->copies().size());
    rig.store->migrateCachedReplays(&systems);
    CHECK(!rig.service->busy() && rig.transport->copies().size() == copies, "a migrated system is not copied again");
    rig.store->reclaim(systems, systems.storeLoaded());
    CHECK(QFileInfo::exists(durable) && !QFileInfo::exists(unused), "the next start reclaims only the unused copy");
}

/* A repoint that cannot be saved leaves the rows on the cached file and its copy pending, for a later reclaim. */
void
test_migration_save_failure_releases_its_copy() {
    reset_data();
    const QString legacy = cache_file(QStringLiteral("legacy.wav"), QByteArray("legacy"));
    Rig rig;
    SavedSystemsModel systems;
    systems.setReplayStore(rig.store.get());
    CHECK(systems.add(replay_system(QStringLiteral("A"), legacy)), "a system keeps a cached replay");
    const QString store = dsd_qt::json_store_path(QStringLiteral("saved_systems.json"));
    CHECK(QFile::remove(store) && QDir().mkpath(store), "the saved systems store is blocked");
    QByteArray log;
    log = captured_stderr([&rig, &systems]() {
        rig.store->migrateCachedReplays(&systems);
        (void)wait_until([&rig]() { return !rig.service->busy(); });
    });
    CHECK(QDir(store).removeRecursively(), "the store is unblocked");
    CHECK(log.contains("keep the cached files"), "the failed save is logged");
    CHECK(systems.get(0).value(QStringLiteral("filePath")).toString() == legacy, "the row keeps the cached file");
    CHECK(manifest().value(ReplayStore::replaysRoot() + QStringLiteral("/legacy.wav")) == QStringLiteral("pending"),
          "its copy is released");
}

/* Finding r1-7: the migration publishes its copy pending and adopts it just before the repoint is saved. A crash
   between the two leaves a pending copy nothing names, beside rows still on the cached file: the next start reclaims
   that copy and copies again. (The crash is the state it leaves on disk.) */
void
test_migration_crash_before_adopt() {
    reset_data();
    const QString legacy = cache_file(QStringLiteral("legacy.wav"), QByteArray("legacy"));
    const QString orphan = ReplayStore::replaysRoot() + QStringLiteral("/legacy.wav");
    {
        SavedSystemsModel systems;
        CHECK(systems.add(replay_system(QStringLiteral("A"), legacy)), "a system keeps a cached replay");
    }
    write_file(orphan, QByteArray("legacy"));
    CHECK(dsd_qt::json_store_save_array(
              QStringLiteral("replay_copies.json"),
              QJsonArray{QJsonObject{{QStringLiteral("path"), orphan}, {QStringLiteral("state"), "pending"}}}),
          "the crash left the copy published pending");
    Rig rig;
    SavedSystemsModel systems;
    QObject owner;
    (void)dsd_qt::load_replays(&rig.host, rig.service.get(), &systems, dsd_qt::backup::ApplyOutcome::None, &owner);
    CHECK(wait_until([&rig]() { return !rig.service->busy(); }), "the migration runs again");
    CHECK(systems.get(0).value(QStringLiteral("filePath")).toString() == orphan, "the row takes the new copy");
    CHECK(manifest().size() == 1 && manifest().value(orphan) == QStringLiteral("kept"),
          "the orphan was reclaimed and the new copy kept");
}

/* Gate r1: an Edit draft opened while the migration copies still names the cached file. Saving it afterwards (even a
   name-only edit) keeps the copy: the row stays on it, the copy stays kept, and an evicted cache loses nothing. A new
   system naming that cached file takes the copy too, and an open draft hears when to read its file again. */
void
test_draft_opened_during_migration_keeps_the_copy() {
    reset_data();
    const QString legacy = cache_file(QStringLiteral("legacy.wav"), QByteArray("legacy"));
    const QString durable = ReplayStore::replaysRoot() + QStringLiteral("/legacy.wav");
    Rig rig;
    SavedSystemsModel systems;
    systems.setReplayStore(rig.store.get());
    CHECK(systems.add(replay_system(QStringLiteral("A"), legacy)), "a system keeps a cached replay");
    const QString uid = systems.get(0).value(QStringLiteral("uid")).toString();
    int migrated = 0;
    QObject::connect(rig.store.get(), &ReplayStore::replaysMigrated, [&migrated] { ++migrated; });
    rig.transport->setHoldLocal(true);
    rig.store->migrateCachedReplays(&systems);
    CHECK(wait_until([&rig]() { return rig.transport->holding() > 0; }), "the migration copies");
    // The wizard opens for Edit now: its draft holds the cached path.
    QVariantMap draft = systems.get(0);
    CHECK(rig.store->migratedPath(uid, legacy) == legacy, "nothing is moved before the migration commits");
    rig.transport->setHoldLocal(false);
    CHECK(wait_until([&rig]() { return !rig.service->busy(); }), "the migration finishes");
    CHECK(systems.get(0).value(QStringLiteral("filePath")).toString() == durable, "the row takes the copy");
    CHECK(migrated == 1, "an open draft is told the migration moved files");
    CHECK(rig.store->migratedPath(uid, legacy) == durable && rig.store->migratedPath(QString(), legacy) == durable,
          "the cached path reads as its copy, for its system and for any");
    CHECK(rig.store->migratedPath(uid, QStringLiteral("/elsewhere/x.wav")) == QStringLiteral("/elsewhere/x.wav"),
          "any other path reads as itself");

    draft.insert(QStringLiteral("name"), QStringLiteral("A renamed"));
    CHECK(systems.update(0, draft), "the stale draft saves");
    CHECK(systems.get(0).value(QStringLiteral("filePath")).toString() == durable,
          "saving the stale draft keeps the row on the copy");
    CHECK(manifest().value(durable) == QStringLiteral("kept"), "the copy stays kept");
    CHECK(systems.add(replay_system(QStringLiteral("B"), legacy))
              && systems.get(1).value(QStringLiteral("filePath")).toString() == durable,
          "a new system naming the cached file takes the copy");

    // Android evicts the cache; the next start keeps the only copy left.
    CHECK(QFile::remove(legacy), "the cache is evicted");
    rig.store->reclaim(systems, systems.storeLoaded());
    CHECK(read_file(durable) == "legacy", "the copy survives the next reclaim");
}

void
test_names() {
    const QString root = ReplayStore::replaysRoot();
    CHECK(ReplayStore::names({root + QStringLiteral("/./x.wav")}, root + QStringLiteral("/x.wav")),
          "a spelling of the same path names it");
    CHECK(!ReplayStore::names({root + QStringLiteral("/y.wav")}, root + QStringLiteral("/x.wav")), "another path");
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    dsd_test_qt_isolate_paths();
    test_names();
    test_local_reference_answers_its_path();
    test_copy_is_recorded_pending();
    test_repicked_copy_is_reclaimed();
    test_failed_save_after_adopt_keeps_the_copy();
    test_unreadable_manifest();
    test_release_after_remove_and_edit();
    test_restore_keeps_kept_copies();
    test_unsafe_starts_delete_nothing();
    test_not_enough_space();
    test_no_space_during_copy();
    test_cancel_and_progress();
    test_cancel_does_not_wait_for_the_copy();
    test_teardown_service_first();
    test_migration_runs_off_the_qt_thread();
    test_migration_save_failure_releases_its_copy();
    test_migration_crash_before_adopt();
    test_draft_opened_during_migration_keeps_the_copy();
    test_every_answer_has_a_name();
    test_reclaim_guards();
#ifdef Q_OS_UNIX
    test_stale_entry_never_shadows_a_new_copy();
#endif
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "OK\n");
    return 0;
}
