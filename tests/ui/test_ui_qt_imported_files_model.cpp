// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the Qt frontend's imported-files layer — the ImportedFilesModel library the wizard and imports screen
 * share: imports copied and checked on the import worker (unique names, atomic replace, bundles) and committed on the
 * Qt thread. Registered only when the Qt frontend is enabled (DSD_ENABLE_QT_UI), since these link Qt. */

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <thread>
#include <utility>
#include "../test_support/qt_test_paths.h"
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/crypto/backup_seal.h>
#include <dsd-neo/platform/env.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/scan_options.h>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <qflags.h>
#include <stdint.h>
extern "C" {
#include <dsd-neo/core/managed_file.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/timing.h>
#include "managed_file_test_support.h"
}
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include "backup_archive.h"
#include "csv_bundle_import.h"
#include "decoder_host.h"
#include "document_import_transport.h"
#include "import_service.h"
#include "imported_files_model.h"
#include "json_store.h"
#include "saved_systems_model.h"

namespace {

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* What a test host's import copies see, shared with the import worker that runs them (hence the lock). */
struct TestDocuments {
    std::mutex lock;
    /* A fake provider's documents: reference -> local file. */
    QMap<QString, QString> files;
    /* Every reference a copy was asked for, in order. */
    QStringList references;
    /* Android's URI contract: a scheme-less reference is refused. */
    bool urisOnly = false;
    /* Fault injection: a copy of this reference writes half the document and then fails, as a read error would. */
    QString failHalfway;
    /* Fault injection: before the copy of this reference, a folder is put where each target list's primary goes in
       the request's mirror, so its rewrite cannot write the destination. */
    QString blockPrimary;
    /* Fault injection: a copy of this reference ignores its cancel, as a provider blocked in a read would: it waits
       until stuckReleased, then copies the whole document. */
    QString stuck;
    bool stuckRunning = false;
    bool stuckReleased = false;
    std::condition_variable stuckChanged;
    /* Every request a cancel reached, in order. */
    QList<qint64> cancels;
};

/* A content provider's documents for the import copies: a reference the test names maps to a local file. */
class TestTransport : public dsd_qt::DocumentImportTransport {
  public:
    explicit TestTransport(std::shared_ptr<TestDocuments> documents) : m_documents(std::move(documents)) {}

    int
    copy(qint64 request, const QString& reference, const QString& destPath, qint64 maxBytes) override {
        QString mapped;
        bool refused = false;
        bool halfway = false;
        bool stuck = false;
        {
            const std::lock_guard<std::mutex> guard(m_documents->lock);
            m_documents->references.append(reference);
            refused = m_documents->urisOnly && QUrl(reference).scheme().isEmpty();
            mapped = m_documents->files.value(reference, reference);
            halfway = !m_documents->failHalfway.isEmpty() && reference == m_documents->failHalfway;
            stuck = !m_documents->stuck.isEmpty() && reference == m_documents->stuck;
            if (!m_documents->blockPrimary.isEmpty() && reference == m_documents->blockPrimary) {
                blockPrimaries(destPath);
            }
        }
        if (refused) {
            return dsd_qt::DecoderHost::DocumentFailed;
        }
        if (stuck) {
            waitUntilReleased();
            return QFile::copy(dsd_qt::local_document_path(mapped), destPath) ? dsd_qt::DecoderHost::DocumentCopied
                                                                              : dsd_qt::DecoderHost::DocumentFailed;
        }
        if (halfway) {
            // The valid first half lands where the copy goes, then the read fails.
            QFile source(dsd_qt::local_document_path(mapped));
            QFile out(destPath);
            if (source.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly)) {
                out.write(source.read(source.size() / 2));
            }
            return dsd_qt::DecoderHost::DocumentFailed;
        }
        return DocumentImportTransport::copy(request, mapped, destPath, maxBytes);
    }

    /* #831: a probe reads the mapped document through the base transport, and can be stuck as a copy can. */
    int
    probe(qint64 request, const QString& reference, dsd_qt::ImportProbe* out, qint64 maxCount) override {
        QString mapped;
        bool stuck = false;
        {
            const std::lock_guard<std::mutex> guard(m_documents->lock);
            m_documents->references.append(reference);
            mapped = m_documents->files.value(reference, reference);
            stuck = !m_documents->stuck.isEmpty() && reference == m_documents->stuck;
        }
        if (stuck) {
            waitUntilReleased();
        }
        return DocumentImportTransport::probe(request, mapped, out, maxCount);
    }

    void
    cancel(qint64 request) override {
        {
            const std::lock_guard<std::mutex> guard(m_documents->lock);
            m_documents->cancels.append(request);
        }
        DocumentImportTransport::cancel(request);
    }

  private:
    void
    waitUntilReleased() {
        std::unique_lock<std::mutex> guard(m_documents->lock);
        m_documents->stuckRunning = true;
        m_documents->stuckChanged.notify_all();
        m_documents->stuckChanged.wait(guard, [this]() { return m_documents->stuckReleased; });
        m_documents->stuckRunning = false;
    }

    /* destPath is <staging>/in/<n>; the mirror is <staging>/lib. */
    static void
    blockPrimaries(const QString& destPath) {
        const QDir bundles(QFileInfo(destPath).dir().absoluteFilePath(QStringLiteral("../lib/bundles")));
        for (const QString& root : bundles.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QDir(bundles.filePath(root)).mkpath(QStringLiteral("targets.csv/blocked"));
        }
    }

    std::shared_ptr<TestDocuments> m_documents;
};

/* The fake documents as the tests read and change them. */
class DocumentMap {
  public:
    explicit DocumentMap(std::shared_ptr<TestDocuments> documents) : m_documents(std::move(documents)) {}

    void
    insert(const QString& reference, const QString& file) {
        const std::lock_guard<std::mutex> guard(m_documents->lock);
        m_documents->files.insert(reference, file);
    }

  private:
    std::shared_ptr<TestDocuments> m_documents;
};

/* The copy that ignores its cancel, as the tests set it, wait for it and let it go. */
class StuckCopy {
  public:
    explicit StuckCopy(std::shared_ptr<TestDocuments> documents) : m_documents(std::move(documents)) {}

    StuckCopy(const StuckCopy&) = delete;
    StuckCopy& operator=(const StuckCopy&) = delete;

    /* A copy left stuck goes on, so the import worker can end. */
    ~StuckCopy() { release(); }

    void
    set(const QString& reference) {
        const std::lock_guard<std::mutex> guard(m_documents->lock);
        m_documents->stuck = reference;
        m_documents->stuckReleased = false;
    }

    /* Whether the copy is stuck now, once it has started (waiting up to 5 s for it). */
    bool
    waitRunning() {
        std::unique_lock<std::mutex> guard(m_documents->lock);
        return m_documents->stuckChanged.wait_for(guard, std::chrono::seconds(5),
                                                  [this]() { return m_documents->stuckRunning; });
    }

    bool
    running() const {
        const std::lock_guard<std::mutex> guard(m_documents->lock);
        return m_documents->stuckRunning;
    }

    void
    release() {
        {
            const std::lock_guard<std::mutex> guard(m_documents->lock);
            m_documents->stuckReleased = true;
        }
        m_documents->stuckChanged.notify_all();
    }

  private:
    std::shared_ptr<TestDocuments> m_documents;
};

/* The references the copies were asked for, as the tests read and clear them. */
class ReferenceLog {
  public:
    explicit ReferenceLog(std::shared_ptr<TestDocuments> documents) : m_documents(std::move(documents)) {}

    void
    clear() {
        const std::lock_guard<std::mutex> guard(m_documents->lock);
        m_documents->references.clear();
    }

    bool
    operator==(const QStringList& expected) const {
        const std::lock_guard<std::mutex> guard(m_documents->lock);
        return m_documents->references == expected;
    }

  private:
    std::shared_ptr<TestDocuments> m_documents;
};

class TestHost : public dsd_qt::DecoderHost {
  public:
    bool running = false;
    /* Whether the host has DSD-neo's file browser (#830), which confines the companions a CSV finds by path. */
    bool browser = false;
    // The live answer, apart from the cached phase: Android's reads Idle until its first refresh().
    bool engine = false;
    std::shared_ptr<TestDocuments> shared = std::make_shared<TestDocuments>();
    DocumentMap documents{shared};

    std::shared_ptr<dsd_qt::DocumentImportTransport>
    importTransport() const override {
        return keepImportTransport(std::make_shared<TestTransport>(shared));
    }

    bool
    isRunning() const override {
        return running;
    }

    bool
    engineBusy() const override {
        return engine;
    }

    bool
    fileBrowserSupported() const override {
        return browser;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {}
};

// Model Android's URI contract before resolving fake SAF documents. The desktop
// importer also accepts bare paths and would otherwise hide a missing file://.
class UriTestHost : public TestHost {
  public:
    ReferenceLog references{shared};

    UriTestHost() { shared->urisOnly = true; }
};

bool
write_file(const QString& path, const QByteArray& contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(contents);
    return true;
}

QByteArray
read_file(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

QVariantMap
waitForImport(QSignalSpy& spy, int id) {
    const auto answers = [&spy, id]() {
        QList<QVariantMap> found;
        for (const auto& arguments : spy) {
            if (arguments.at(0).toInt() == id) {
                found.append(arguments.at(1).toMap());
            }
        }
        return found;
    };
    for (int waits = 0; waits < 100 && answers().isEmpty(); waits++) {
        spy.wait(50);
    }
    QCoreApplication::processEvents();
    const auto found = answers();
    expect("an import is answered exactly once", found.size() == 1);
    return found.value(0);
}

QVariantMap
importAndWait(dsd_qt::ImportedFilesModel& model, const QString& reference, const QString& fileName, const QString& type,
              const QString& replacePath = QString(), const QVariantMap& companions = QVariantMap()) {
    QSignalSpy spy(&model, &dsd_qt::ImportedFilesModel::importFinished);
    const int id = model.beginImport(reference, fileName, type, companions, replacePath);
    expect("an import has an id", id > 0);
    return waitForImport(spy, id);
}

/* The stored path of library row @p row, or one naming no row (a replace of it is refused as "changed"). */
QString
rowPath(const dsd_qt::ImportedFilesModel& model, int row) {
    return row >= 0 && row < model.rowCount() ? model.get(row).value(QStringLiteral("path")).toString()
                                              : QStringLiteral("/no such row");
}

/* Replace library row @p row with a pick through the async import, and wait for its answer. */
QVariantMap
replaceAndWait(dsd_qt::ImportedFilesModel& model, int row, const QString& reference, const QString& fileName,
               const QVariantMap& companions = QVariantMap()) {
    const QString type = model.get(row).value(QStringLiteral("type")).toString();
    return importAndWait(model, reference, fileName, type, rowPath(model, row), companions);
}

void
test_imported_files_model(void) {
    /* Fresh app-data tree: the imports directory is shared state, and copies
     * left by earlier tests would unique-ify this test's names. */
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    TestHost host;
    QTemporaryDir sourceDir;
    expect("model source dir created", sourceDir.isValid());

    const QString groupSrc = sourceDir.filePath(QStringLiteral("county.csv"));
    expect("group source written", write_file(groupSrc, "TG,Mode,Name\n101,D,Dispatch\n102,D,Fire\nbogus,D,Bad\n"));
    const QString chanSrc = sourceDir.filePath(QStringLiteral("chan.csv"));
    expect("chan source written", write_file(chanSrc, "channel,freq\n1,851000000\n"));
    const QString emptySrc = sourceDir.filePath(QStringLiteral("header_only.csv"));
    expect("empty source written", write_file(emptySrc, "TG,Mode,Name\n"));

    QString storedGroupPath;
    {
        dsd_qt::ImportedFilesModel model(&host);
        expect("model starts empty", model.rowCount() == 0);

        const QVariantMap imported = importAndWait(model, QUrl::fromLocalFile(groupSrc).toString(),
                                                   QStringLiteral("county.csv"), QStringLiteral("group"));
        expect("group import ok", imported.value(QStringLiteral("ok")).toBool());
        expect("group import counts accepted", imported.value(QStringLiteral("accepted")).toInt() == 2);
        expect("group import counts skipped", imported.value(QStringLiteral("skipped")).toInt() == 1);
        expect("group import has no error", imported.value(QStringLiteral("error")).toString().isEmpty());
        storedGroupPath = imported.value(QStringLiteral("path")).toString();
        expect("group import stores a path", !storedGroupPath.isEmpty() && QFile::exists(storedGroupPath));
        expect("group import adds a row", model.rowCount() == 1);

        const QVariantMap chan = importAndWait(model, QUrl::fromLocalFile(chanSrc).toString(),
                                               QStringLiteral("chan.csv"), QStringLiteral("chan"));
        expect("chan import ok", chan.value(QStringLiteral("ok")).toBool());
        expect("chan import counts accepted", chan.value(QStringLiteral("accepted")).toInt() == 1);

        /* A parseable file with zero usable rows is kept — recoverable via
         * update — but flagged so the UI can warn instead of silently storing
         * a talkgroup list that names nothing. */
        const QVariantMap empty = importAndWait(model, QUrl::fromLocalFile(emptySrc).toString(),
                                                QStringLiteral("header_only.csv"), QStringLiteral("group"));
        expect("empty import kept", empty.value(QStringLiteral("ok")).toBool());
        expect("empty import flagged", empty.value(QStringLiteral("error")).toString() == QStringLiteral("empty"));
        expect("empty import accepted zero", empty.value(QStringLiteral("accepted")).toInt() == 0);

        const QVariantMap bad = importAndWait(model, QUrl::fromLocalFile(sourceDir.filePath("absent.csv")).toString(),
                                              QStringLiteral("absent.csv"), QStringLiteral("group"));
        expect("unreadable import rejected", !bad.value(QStringLiteral("ok")).toBool());
        expect("unreadable import adds no row", model.rowCount() == 3);

        const QVariantList groups = model.entriesForType(QStringLiteral("group"));
        expect("type filter finds group rows", groups.size() == 2);
        const QVariantList chans = model.entriesForType(QStringLiteral("chan"));
        expect("type filter finds chan row",
               chans.size() == 1
                   && chans.at(0).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("chan.csv"));

        expect("rowForPath finds the stored file", model.rowForPath(storedGroupPath) == 0);
        expect("rowForPath misses unknown path", model.rowForPath(QStringLiteral("/nope.csv")) == -1);
    }

    {
        /* Fresh instance: the library must reload from disk. */
        dsd_qt::ImportedFilesModel model(&host);
        expect("library persists across instances", model.rowCount() == 3);
        const QVariantMap row = model.get(0);
        expect("persisted row keeps name",
               row.value(QStringLiteral("name")).toString() == QStringLiteral("county.csv"));
        expect("persisted row keeps type", row.value(QStringLiteral("type")).toString() == QStringLiteral("group"));
        expect("persisted row keeps counts", row.value(QStringLiteral("accepted")).toInt() == 2);

        /* Update in place: the stored path must not change, the counts must. */
        const QString updatedSrc = sourceDir.filePath(QStringLiteral("updated.csv"));
        expect("updated source written",
               write_file(updatedSrc, "TG,Mode,Name\n201,D,PD\n202,D,FD\n203,D,EMS\n204,D,DPW\n"));
        const QVariantMap updated =
            replaceAndWait(model, 0, QUrl::fromLocalFile(updatedSrc).toString(), QStringLiteral("updated.csv"));
        expect("update ok", updated.value(QStringLiteral("ok")).toBool());
        expect("update keeps the stored path", updated.value(QStringLiteral("path")).toString() == storedGroupPath);
        expect("update refreshes counts", model.get(0).value(QStringLiteral("accepted")).toInt() == 4);

        /* Remove deletes the stored file with the row. */
        const QString chanPath = model.get(1).value(QStringLiteral("path")).toString();
        model.remove(1);
        expect("remove drops the row", model.rowCount() == 2);
        expect("remove deletes the file", !QFile::exists(chanPath));
    }

    {
        /* A stored file deleted behind the app's back must not survive as a
         * ghost row pointing nowhere. */
        expect("stored group file removable", QFile::remove(storedGroupPath));
        dsd_qt::ImportedFilesModel model(&host);
        expect("load drops rows for missing files", model.rowCount() == 1);
        expect("survivor is the header-only row",
               model.get(0).value(QStringLiteral("name")).toString() == QStringLiteral("header_only.csv"));

        /* Pruning the row is only half the repair: a saved system still holding
         * that path builds a `-G <missing>` argv and fails to start with a parse
         * error naming the input settings, not the file. The owner reconciles
         * that, so the path has to survive the prune long enough to be handed
         * over — and exactly once, or a later caller would re-clear a path some
         * system had legitimately re-selected. */
        const QStringList pruned = model.takePrunedPaths();
        expect("prune reports the vanished path", pruned == QStringList{storedGroupPath});
        expect("prune is reported only once", model.takePrunedPaths().isEmpty());
    }

    {
        /* Nothing missing: no reconciliation for the owner to do. */
        dsd_qt::ImportedFilesModel model(&host);
        expect("a clean load prunes nothing", model.takePrunedPaths().isEmpty());
    }
}

/*
 * A replace swaps the row's stored file. Validating only after the replace
 * would have no rollback: a re-pick of a file that is not parseable as this row's
 * type would clobber a working CSV, leave the row advertising stale counts, and
 * hand every saved system pointing at that path an unusable -G/-C.
 */
void
test_update_rejects_invalid_pick(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    TestHost host;
    QTemporaryDir sourceDir;
    expect("update-guard source dir created", sourceDir.isValid());

    const QByteArray good = "TG,Mode,Name\n101,A,Dispatch\n102,A,Fire\n";
    const QString groupSrc = sourceDir.filePath(QStringLiteral("group.csv"));
    expect("update-guard group written", write_file(groupSrc, good));

    dsd_qt::ImportedFilesModel model(&host);
    const QVariantMap imported = importAndWait(model, QUrl::fromLocalFile(groupSrc).toString(),
                                               QStringLiteral("group.csv"), QStringLiteral("group"));
    expect("update-guard import ok", imported.value(QStringLiteral("ok")).toBool());
    const QString storedPath = imported.value(QStringLiteral("path")).toString();
    expect("update-guard stored copy matches", read_file(storedPath) == good);

    /* A directory is not a readable regular file, so the row's type validator
     * cannot parse it. */
    const QString unreadable = sourceDir.filePath(QStringLiteral("a-directory"));
    expect("update-guard directory created", QDir().mkpath(unreadable));

    const int before = model.get(0).value(QStringLiteral("accepted")).toInt();
    const QVariantMap rejected =
        replaceAndWait(model, 0, QUrl::fromLocalFile(unreadable).toString(), QStringLiteral("group.csv"));
    expect("invalid pick rejected", !rejected.value(QStringLiteral("ok")).toBool());
    expect("invalid pick leaves the stored file byte-identical", read_file(storedPath) == good);
    expect("invalid pick leaves the row's counts alone",
           model.get(0).value(QStringLiteral("accepted")).toInt() == before);
    expect("invalid pick adds no row", model.rowCount() == 1);

    /* The staging copy must not survive as an orphan either. */
    const QDir importsDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                          + QStringLiteral("/imports"));
    expect("invalid pick strands no staging copy",
           importsDir.entryList(QDir::Files | QDir::NoDotAndDotDot).size() == 1);
}

/*
 * Issue #829: an update names its row by the stored path it had when the pick
 * was asked for. A row removed while the picker was up shifts the later rows'
 * indexes, so an index would replace the next file instead; the path either
 * finds the row that was asked for or refuses with "changed" (#832: the
 * replace runs through beginImport()).
 */
void
test_replace_by_stored_path(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    TestHost host;
    const QTemporaryDir sourceDir;
    expect("replace-by-path source dir created", sourceDir.isValid());
    dsd_qt::ImportedFilesModel model(&host);
    QStringList stored;
    QList<QByteArray> bytes;
    for (int i = 0; i < 3; i++) {
        const QByteArray body = QStringLiteral("TG,Mode,Name\n%1,A,Group %1\n").arg(101 + i).toUtf8();
        const QString source = sourceDir.filePath(QStringLiteral("group-%1.csv").arg(i));
        expect("replace-by-path source written", write_file(source, body));
        const QVariantMap imported = importAndWait(model, QUrl::fromLocalFile(source).toString(),
                                                   QStringLiteral("group-%1.csv").arg(i), QStringLiteral("group"));
        expect("replace-by-path import ok", imported.value(QStringLiteral("ok")).toBool());
        stored.append(imported.value(QStringLiteral("path")).toString());
        bytes.append(body);
    }
    const QByteArray replacement = "TG,Mode,Name\n201,A,Replacement\n202,A,Second\n";
    const QString pick = sourceDir.filePath(QStringLiteral("replacement.csv"));
    expect("replace-by-path pick written", write_file(pick, replacement));
    const QString reference = QUrl::fromLocalFile(pick).toString();

    /* The second file's update was asked for; the first goes while the picker is up. */
    expect("replace-by-path first row removed", model.remove(0));
    QVariantMap result =
        importAndWait(model, reference, QStringLiteral("replacement.csv"), QStringLiteral("group"), stored.at(1));
    expect("replace-by-path replaces the row asked for", result.value(QStringLiteral("ok")).toBool());
    expect("replace-by-path keeps the stored path", result.value(QStringLiteral("path")).toString() == stored.at(1));
    expect("replace-by-path wrote the pick", read_file(stored.at(1)) == replacement);
    expect("replace-by-path leaves the next file alone", read_file(stored.at(2)) == bytes.at(2));
    expect("replace-by-path adds no row", model.rowCount() == 2);

    /* The row asked for goes while the picker is up: nothing is replaced. */
    expect("replace-by-path asked-for row removed", model.remove(model.rowForPath(stored.at(1))));
    result = importAndWait(model, reference, QStringLiteral("replacement.csv"), QStringLiteral("group"), stored.at(1));
    expect("replace-by-path refuses a removed row", !result.value(QStringLiteral("ok")).toBool());
    expect("replace-by-path says it changed", result.value(QStringLiteral("error")).toString() == "changed");
    expect("replace-by-path explains",
           result.value(QStringLiteral("detail")).toString() == "That file changed while you were picking. Try again.");
    expect("replace-by-path refusal leaves the other file alone", read_file(stored.at(2)) == bytes.at(2));
    expect("replace-by-path refusal adds no row", model.rowCount() == 1);

    /* No path names no row: the model takes an empty path for a new import, so the flow refuses an update whose row
       had no path before it asks the model (tst_csv_import_flow covers it). */
    while (model.rowCount() > 0) {
        model.remove(0);
    }
}

/*
 * Pins the ORDER of the replace flows, which the case above cannot: an
 * unreadable source fails at the copy, so it would look the same whether the
 * validation ran before or after the stored file was replaced.
 *
 * The validator is stricter than the copier -- it refuses a type it does not
 * recognise, and it opens with O_NOFOLLOW and requires a regular file, none of
 * which QFile cares about -- so a source can copy cleanly and still be rejected.
 * A row carrying a type this build does not know is the portable way to build
 * that: a store written by a newer version, opened by an older one. Validate
 * after replacing and the working CSV is already gone, the row still advertises
 * its old counts, and every saved system pointing at that path now has an
 * unusable -G/-C.
 */
void
test_replace_validates_before_touching_the_stored_file(void) {
    TestHost host;
    QTemporaryDir sourceDir;
    expect("order-guard source dir created", sourceDir.isValid());

    const QByteArray kept = "TG,Mode,Name\n101,A,Keep me\n";
    const QByteArray replacement = "TG,Mode,Name\n201,A,Should not land\n202,A,Nor this\n";
    const QString replacementSrc = sourceDir.filePath(QStringLiteral("replacement.csv"));
    expect("order-guard replacement written", write_file(replacementSrc, replacement));

    for (int pass = 0; pass < 2; pass++) {
        const bool viaRefresh = (pass == 0);
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

        const QString importsDir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/imports");
        expect("order-guard imports dir created", QDir().mkpath(importsDir));
        const QString storedPath = importsDir + QStringLiteral("/kept.csv");
        expect("order-guard stored file written", write_file(storedPath, kept));

        QJsonObject row;
        row.insert(QStringLiteral("name"), QStringLiteral("kept.csv"));
        row.insert(QStringLiteral("path"), storedPath);
        row.insert(QStringLiteral("type"), QStringLiteral("futureType"));
        row.insert(QStringLiteral("importedAt"), 1700000000LL);
        row.insert(QStringLiteral("accepted"), 7);
        row.insert(QStringLiteral("skipped"), 0);
        QJsonArray array;
        array.append(row);
        dsd_qt::json_store_save_array(QStringLiteral("imported_files.json"), array);

        dsd_qt::ImportedFilesModel model(&host);
        expect("order-guard row loaded", model.rowCount() == 1);

        const QVariantMap result = viaRefresh ? model.refreshGeneratedFile(0, replacementSrc)
                                              : replaceAndWait(model, 0, QUrl::fromLocalFile(replacementSrc).toString(),
                                                               QStringLiteral("replacement.csv"));
        expect(viaRefresh ? "refresh rejects an unvalidatable replacement"
                          : "update rejects an unvalidatable replacement",
               !result.value(QStringLiteral("ok")).toBool());
        expect(viaRefresh ? "refresh leaves the stored file byte-identical"
                          : "update leaves the stored file byte-identical",
               read_file(storedPath) == kept);
        expect(viaRefresh ? "refresh leaves the row's counts alone" : "update leaves the row's counts alone",
               model.get(0).value(QStringLiteral("accepted")).toInt() == 7);
        expect(viaRefresh ? "refresh strands no staging copy" : "update strands no staging copy",
               QDir(importsDir).entryList(QDir::Files | QDir::NoDotAndDotDot).size() == 1);
    }
}

/*
 * Generated files: the RadioReference flow writes a staging file itself and hands
 * it over by path, so it never touches the picker. Provenance rides along so the
 * imports screen can offer "Refresh from RadioReference" on exactly those rows.
 */
void
test_generated_import_and_refresh(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    TestHost host;
    QTemporaryDir stagingDir;
    expect("staging dir created", stagingDir.isValid());

    const QByteArray first = "DEC,Mode,Name (generated from RadioReference)\n101,A,Dispatch\n102,DE,Encrypted\n";
    const QString stagedPath = stagingDir.filePath(QStringLiteral("group.csv"));
    expect("staged group written", write_file(stagedPath, first));

    QVariantMap origin;
    origin.insert(QStringLiteral("origin"), QStringLiteral("radioreference"));
    origin.insert(QStringLiteral("rrSid"), 6673);
    /* The whole selection, so a refresh can reproduce it: a conventional import
     * selects several repeaters, and only siteId identifies one — the RF site
     * number repeats within a system. */
    origin.insert(QStringLiteral("rrSiteIds"), QStringLiteral("4001,4002,4003"));
    origin.insert(QStringLiteral("rrKind"), QStringLiteral("group"));
    /* Deliberately the non-default answer, so a refresh that substituted the UI
     * default would be visible. */
    origin.insert(QStringLiteral("rrPartialEnc"), false);

    QString storedPath;
    {
        dsd_qt::ImportedFilesModel model(&host);
        const QVariantMap result =
            model.importGeneratedFile(stagedPath, QStringLiteral("SARA group.csv"), QStringLiteral("group"), origin);
        expect("generated import ok", result.value(QStringLiteral("ok")).toBool());
        expect("generated import counts rows", result.value(QStringLiteral("accepted")).toInt() == 2);
        expect("generated import has no error", result.value(QStringLiteral("error")).toString().isEmpty());
        storedPath = result.value(QStringLiteral("path")).toString();
        expect("generated import stores a copy", !storedPath.isEmpty() && read_file(storedPath) == first);
        expect("generated import adds a row", model.rowCount() == 1);

        const QVariantMap row = model.get(0);
        expect("provenance origin recorded",
               row.value(QStringLiteral("origin")).toString() == QStringLiteral("radioreference"));
        expect("provenance sid recorded", row.value(QStringLiteral("rrSid")).toInt() == 6673);
        expect("provenance kind recorded", row.value(QStringLiteral("rrKind")).toString() == QStringLiteral("group"));
        /* The database siteId, not the RF site number: two RR rows can share a
         * number, so only this identifies the site a refresh re-fetches. */
        expect("provenance site ids recorded",
               row.value(QStringLiteral("rrSiteIds")).toString() == QStringLiteral("4001,4002,4003"));
        expect("generated import keeps its type",
               row.value(QStringLiteral("type")).toString() == QStringLiteral("group"));

        /* A picked file carries no provenance, so the refresh action must not
         * offer itself on one. */
        QTemporaryDir pickDir;
        const QString pickSrc = pickDir.filePath(QStringLiteral("picked.csv"));
        expect("picked source written", write_file(pickSrc, "TG,Mode,Name\n1,A,One\n"));
        const QVariantMap picked = importAndWait(model, QUrl::fromLocalFile(pickSrc).toString(),
                                                 QStringLiteral("picked.csv"), QStringLiteral("group"));
        expect("picked import ok", picked.value(QStringLiteral("ok")).toBool());
        expect("picked row has no origin", model.get(1).value(QStringLiteral("origin")).toString().isEmpty());
        expect("picked row has no sid", model.get(1).value(QStringLiteral("rrSid")).toInt() == 0);
    }

    {
        /* Provenance has to survive the JSON round trip, or the refresh button
         * disappears the moment the app restarts. */
        dsd_qt::ImportedFilesModel model(&host);
        expect("provenance persists across instances",
               model.get(0).value(QStringLiteral("origin")).toString() == QStringLiteral("radioreference"));
        expect("persisted sid", model.get(0).value(QStringLiteral("rrSid")).toInt() == 6673);
        expect("persisted site list",
               model.get(0).value(QStringLiteral("rrSiteIds")).toString() == QStringLiteral("4001,4002,4003"));

        /* Refresh: same path, new content, re-validated counts. */
        const QByteArray second =
            "DEC,Mode,Name (generated from RadioReference)\n101,A,Dispatch\n102,DE,Encrypted\n103,A,Third\n";
        const QString refreshSrc = stagingDir.filePath(QStringLiteral("group2.csv"));
        expect("refresh source written", write_file(refreshSrc, second));

        const QVariantMap refreshed = model.refreshGeneratedFile(0, refreshSrc);
        expect("refresh ok", refreshed.value(QStringLiteral("ok")).toBool());
        expect("refresh preserves the stored path", refreshed.value(QStringLiteral("path")).toString() == storedPath);
        expect("refresh rewrites the stored file", read_file(storedPath) == second);
        expect("refresh re-validates", model.get(0).value(QStringLiteral("accepted")).toInt() == 3);
        expect("refresh keeps provenance",
               model.get(0).value(QStringLiteral("rrSid")).toInt() == 6673
                   && model.get(0).value(QStringLiteral("rrKind")).toString() == QStringLiteral("group"));

        /* A refresh whose staging file is unusable — a fault page, a truncated
         * body — must leave the working copy exactly as it was. */
        const QString badSrc = stagingDir.filePath(QStringLiteral("a-directory"));
        expect("refresh-guard directory created", QDir().mkpath(badSrc));
        const QVariantMap failed = model.refreshGeneratedFile(0, badSrc);
        expect("bad refresh rejected", !failed.value(QStringLiteral("ok")).toBool());
        expect("bad refresh leaves the stored file byte-identical", read_file(storedPath) == second);
        expect("bad refresh leaves the counts alone", model.get(0).value(QStringLiteral("accepted")).toInt() == 3);

        expect("refresh rejects an out-of-range row",
               !model.refreshGeneratedFile(99, refreshSrc).value(QStringLiteral("ok")).toBool());
        expect("refresh rejects a negative row",
               !model.refreshGeneratedFile(-1, refreshSrc).value(QStringLiteral("ok")).toBool());

        /* The partial-encryption answer the import was given is provenance too:
         * without it a refresh regenerates with the UI default and silently
         * re-marks every partly-encrypted talkgroup DE, which blocks tuning. */
        expect("partial-enc answer recorded", !model.get(0).value(QStringLiteral("rrPartialEnc")).toBool());
    }

    {
        /* A user re-picking their own file over a generated row makes the bytes
         * theirs. Keeping the provenance would leave "Refresh from
         * RadioReference" on offer for a file it would then overwrite. */
        dsd_qt::ImportedFilesModel model(&host);
        const QString ownSrc = stagingDir.filePath(QStringLiteral("mine.csv"));
        expect("own source written", write_file(ownSrc, "DEC,Mode,Name\n501,A,Mine\n"));

        const QVariantMap updated =
            replaceAndWait(model, 0, QUrl::fromLocalFile(ownSrc).toString(), QStringLiteral("mine.csv"));
        expect("re-pick ok", updated.value(QStringLiteral("ok")).toBool());
        expect("re-pick drops the origin", model.get(0).value(QStringLiteral("origin")).toString().isEmpty());
        expect("re-pick drops the sid", model.get(0).value(QStringLiteral("rrSid")).toInt() == 0);
        expect("re-pick drops the site ids", model.get(0).value(QStringLiteral("rrSiteIds")).toString().isEmpty());
        expect("re-pick drops the kind", model.get(0).value(QStringLiteral("rrKind")).toString().isEmpty());
    }
}

/*
 * The P25 band plan kind (#567): a band plan CSV validates and is stored under
 * "p25Bandplan", and a channel map picked as that kind parses to no usable
 * rows — it is kept and flagged "empty" like every other zero-row file, so the
 * screens warn instead of quietly storing a file the decoder will ignore.
 */
void
test_p25_bandplan_kind(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    TestHost host;
    QTemporaryDir sourceDir;
    expect("bandplan source dir created", sourceDir.isValid());

    const QString planSrc = sourceDir.filePath(QStringLiteral("bandplan.csv"));
    expect("bandplan source written",
           write_file(planSrc, "iden,base_hz,spacing_hz,type,tx_offset_hz,bandwidth_hz,wacn,sysid\n"
                               "0,851006250,6250,1,-45000000,12500,,\n"));
    const QString chanSrc = sourceDir.filePath(QStringLiteral("chan.csv"));
    expect("bandplan chan source written", write_file(chanSrc, "channel,freq\n1,851000000\n"));

    dsd_qt::ImportedFilesModel model(&host);
    const QVariantMap plan = importAndWait(model, QUrl::fromLocalFile(planSrc).toString(),
                                           QStringLiteral("bandplan.csv"), QStringLiteral("p25Bandplan"));
    expect("bandplan import ok", plan.value(QStringLiteral("ok")).toBool());
    expect("bandplan import has no error", plan.value(QStringLiteral("error")).toString().isEmpty());
    expect("bandplan import counts the iden row", plan.value(QStringLiteral("accepted")).toInt() == 1);
    expect("bandplan import stores its kind",
           plan.value(QStringLiteral("type")).toString() == QStringLiteral("p25Bandplan"));

    const QVariantList plans = model.entriesForType(QStringLiteral("p25Bandplan"));
    expect("type filter finds the bandplan row", plans.size() == 1);
    expect("bandplan is not offered as a channel map", model.entriesForType(QStringLiteral("chan")).isEmpty());

    /* A channel map is `number,number`; the band plan importer wants eight
     * columns and accepts none of its rows. */
    const QVariantMap wrong = importAndWait(model, QUrl::fromLocalFile(chanSrc).toString(), QStringLiteral("chan.csv"),
                                            QStringLiteral("p25Bandplan"));
    expect("chan map as bandplan is kept", wrong.value(QStringLiteral("ok")).toBool());
    expect("chan map as bandplan is flagged empty",
           wrong.value(QStringLiteral("error")).toString() == QStringLiteral("empty"));
    expect("chan map as bandplan accepts nothing", wrong.value(QStringLiteral("accepted")).toInt() == 0);

    while (model.rowCount() > 0) {
        model.remove(0);
    }
}

void
test_src_kind(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    TestHost host;
    QTemporaryDir sourceDir;
    expect("src source dir created", sourceDir.isValid());

    const QString srcPath = sourceDir.filePath(QStringLiteral("src.csv"));
    expect("src source written", write_file(srcPath, "id,name,tags\n"
                                                     "1201,Engine 21,Fire\n2000-2099,Dispatch,Ops\n"));
    const QString emptyPath = sourceDir.filePath(QStringLiteral("empty.csv"));
    expect("empty source written", write_file(emptyPath, "id,name,tags\n"));

    dsd_qt::ImportedFilesModel model(&host);
    const QVariantMap result =
        importAndWait(model, QUrl::fromLocalFile(srcPath).toString(), QStringLiteral("src.csv"), QStringLiteral("src"));
    expect("src import ok", result.value(QStringLiteral("ok")).toBool());
    expect("src import has no error", result.value(QStringLiteral("error")).toString().isEmpty());
    expect("src import counts exact and range rows", result.value(QStringLiteral("accepted")).toInt() == 2);
    expect("src import stores its kind", result.value(QStringLiteral("type")).toString() == QStringLiteral("src"));

    const QVariantList entries = model.entriesForType(QStringLiteral("src"));
    expect("type filter finds the src row", entries.size() == 1);
    expect("src is not offered as talkgroups", model.entriesForType(QStringLiteral("group")).isEmpty());

    // A header-only source list is retained with an empty warning.
    const QVariantMap wrong = importAndWait(model, QUrl::fromLocalFile(emptyPath).toString(),
                                            QStringLiteral("empty.csv"), QStringLiteral("src"));
    expect("header-only file as src is kept", wrong.value(QStringLiteral("ok")).toBool());
    expect("header-only file as src is flagged empty",
           wrong.value(QStringLiteral("error")).toString() == QStringLiteral("empty"));
    expect("header-only file as src accepts nothing", wrong.value(QStringLiteral("accepted")).toInt() == 0);

    while (model.rowCount() > 0) {
        model.remove(0);
    }
}

/*
 * Stores written before provenance existed must load unchanged. rowFromMap reads
 * through QVariantMap::value, which default-constructs a missing key, so this
 * holds by construction — but only a test keeps it that way.
 */
void
test_legacy_store_without_provenance(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    TestHost host;
    const QString importsDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/imports");
    expect("legacy imports dir created", QDir().mkpath(importsDir));
    const QString legacyPath = importsDir + QStringLiteral("/legacy.csv");
    expect("legacy file written", write_file(legacyPath, "TG,Mode,Name\n1,A,One\n"));

    QJsonObject legacy;
    legacy.insert(QStringLiteral("name"), QStringLiteral("legacy.csv"));
    legacy.insert(QStringLiteral("path"), legacyPath);
    legacy.insert(QStringLiteral("type"), QStringLiteral("group"));
    legacy.insert(QStringLiteral("importedAt"), 1700000000LL);
    legacy.insert(QStringLiteral("accepted"), 1);
    legacy.insert(QStringLiteral("skipped"), 0);
    QJsonArray array;
    array.append(legacy);
    dsd_qt::json_store_save_array(QStringLiteral("imported_files.json"), array);

    dsd_qt::ImportedFilesModel model(&host);
    expect("legacy store loads", model.rowCount() == 1);
    const QVariantMap row = model.get(0);
    expect("legacy row keeps its name", row.value(QStringLiteral("name")).toString() == QStringLiteral("legacy.csv"));
    expect("legacy row keeps its counts", row.value(QStringLiteral("accepted")).toInt() == 1);
    expect("legacy row defaults origin to empty", row.value(QStringLiteral("origin")).toString().isEmpty());
    expect("legacy row defaults sid to zero", row.value(QStringLiteral("rrSid")).toInt() == 0);
    expect("legacy row defaults kind to empty", row.value(QStringLiteral("rrKind")).toString().isEmpty());
    expect("legacy row defaults the site list to empty", row.value(QStringLiteral("rrSiteIds")).toString().isEmpty());
    expect("legacy row is not prunable", model.takePrunedPaths().isEmpty());
}

/* A restored store whose whole numbers are past int's range (a backup keeps a large JSON number as a 64-bit integer):
 * each reads as the field's default, as an absent key does, never as its low 32 bits. Main read 12 accepted, 3
 * skipped, RadioReference system 6673 and encryption policy 0. */
void
test_store_past_int_range(void) {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

    TestHost host;
    const QString importsDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/imports");
    expect("oversized imports dir created", QDir().mkpath(importsDir));
    const QString path = importsDir + QStringLiteral("/restored.csv");
    expect("oversized file written", write_file(path, "TG,Mode,Name\n1,A,One\n"));

    const qint64 past = qint64(1) << 32;
    QJsonObject restored;
    restored.insert(QStringLiteral("name"), QStringLiteral("restored.csv"));
    restored.insert(QStringLiteral("path"), path);
    restored.insert(QStringLiteral("type"), QStringLiteral("group"));
    restored.insert(QStringLiteral("importedAt"), 1700000000LL);
    restored.insert(QStringLiteral("accepted"), past + 12);
    restored.insert(QStringLiteral("skipped"), past + 3);
    restored.insert(QStringLiteral("origin"), QStringLiteral("radioreference"));
    restored.insert(QStringLiteral("rrSid"), past + 6673);
    restored.insert(QStringLiteral("rrEncryptionPolicy"), past);
    QJsonArray array;
    array.append(restored);
    dsd_qt::json_store_save_array(QStringLiteral("imported_files.json"), array);

    const dsd_qt::ImportedFilesModel model(&host);
    expect("oversized store loads", model.rowCount() == 1);
    const QVariantMap row = model.get(0);
    expect("oversized counts read as 0",
           row.value(QStringLiteral("accepted")).toInt() == 0 && row.value(QStringLiteral("skipped")).toInt() == 0);
    expect("an oversized sid reads as none", row.value(QStringLiteral("rrSid")).toInt() == 0);
    expect("an oversized policy reads as the default", row.value(QStringLiteral("rrEncryptionPolicy")).toInt() == 3);
}

/* Issue #828: a library index that could not be read loads empty, and the next import keeps it byte for byte instead
 * of writing over it. */
void
test_unreadable_index_is_kept() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    const QTemporaryDir sourceDir;
    const QString source = sourceDir.filePath(QStringLiteral("county.csv"));
    expect("unreadable index: source written", write_file(source, "TG,Mode,Name\n101,D,Dispatch\n"));
    const QString index = dsd_qt::json_store_path(QStringLiteral("imported_files.json"));
    const QByteArray junk("[{\"name\":\"county.csv\"");
    expect("unreadable index: fixture", QDir().mkpath(QFileInfo(index).absolutePath()) && write_file(index, junk));
    dsd_qt::ImportedFilesModel model(&host);
    expect("unreadable index: loads empty", model.rowCount() == 0 && model.takePrunedPaths().isEmpty());
    expect("unreadable index: nothing moves at load", read_file(index) == junk);
    const QVariantMap imported = importAndWait(model, QUrl::fromLocalFile(source).toString(),
                                               QStringLiteral("county.csv"), QStringLiteral("group"));
    expect("unreadable index: the import saves", imported.value(QStringLiteral("ok")).toBool());
    expect("unreadable index: kept byte for byte", read_file(index + QStringLiteral(".unreadable-1")) == junk);
    expect("unreadable index: the new index has the import",
           dsd_qt::json_store_load_array(QStringLiteral("imported_files.json")).size() == 1);
}

#ifndef _WIN32
/* Issue #828: a stored copy in a folder that cannot be searched is not known to be gone. Its row stays and is not
 * handed on for its references to be cleared; only a confirmed absence prunes it. */
void
test_unsearchable_copy_is_not_pruned() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    const QString folder =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/locked");
    const QString path = folder + QStringLiteral("/county.csv");
    expect("unsearchable: folder", QDir().mkpath(folder) && write_file(path, "TG,Mode,Name\n1,D,One\n"));
    QJsonObject row;
    row.insert(QStringLiteral("name"), QStringLiteral("county.csv"));
    row.insert(QStringLiteral("path"), path);
    row.insert(QStringLiteral("type"), QStringLiteral("group"));
    expect("unsearchable: index", dsd_qt::json_store_save_array(QStringLiteral("imported_files.json"), {row}));
    const QString index = dsd_qt::json_store_path(QStringLiteral("imported_files.json"));
    const QByteArray stored = read_file(index);
    if (geteuid() == 0) {
        DSD_FPRINTF(stderr, "note: running as root, the unsearchable-folder case is skipped\n");
    } else {
        expect("unsearchable: locked", chmod(QFile::encodeName(folder).constData(), 0000) == 0);
        {
            dsd_qt::ImportedFilesModel model(&host);
            expect("unsearchable: the row stays", model.rowCount() == 1 && model.takePrunedPaths().isEmpty());
        }
        expect("unsearchable: unlocked", chmod(QFile::encodeName(folder).constData(), 0700) == 0);
        expect("unsearchable: the index is not rewritten", read_file(index) == stored);
    }
    expect("unsearchable: the copy is removed", QFile::remove(path));
    dsd_qt::ImportedFilesModel model(&host);
    expect("unsearchable: a confirmed absence prunes",
           model.rowCount() == 0 && model.takePrunedPaths() == QStringList{path});
}
#endif

void
test_export_registration_in_place() {
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    const int before = model.count();
    const QString path = model.newTalkgroupListPath();
    expect("generated path is unique", path != model.newTalkgroupListPath());
    expect("destination is not registered before completion", model.count() == before && !QFile::exists(path));
    expect("missing export cannot be registered", !model.registerTalkgroupList(path));
    const QByteArray csv("id,mode,name,priority,preempt\n42,A,Dispatch,50,1\n");
    expect("write canonical export", write_file(path, csv));
    expect("register existing export", model.registerTalkgroupList(path));
    expect("same path and bytes retained", model.get(model.rowForPath(path)).value("path").toString() == path
                                               && read_file(path) == csv && model.count() == before + 1);
    expect("register completion only once", model.registerTalkgroupList(path) && model.count() == before + 1);
    QTemporaryDir outside;
    const QString external = outside.filePath("external.csv");
    expect("write outside fixture", write_file(external, csv));
    expect("outside path refused without deleting it",
           !model.registerTalkgroupList(external) && QFile::exists(external));
    model.remove(model.rowForPath(path));
}

/* Issue #748: groupListCounts() reads the file as it is now, where a library row's counts are from its import, and
 * reads a file outside the library the same way. */
void
test_group_list_counts_read_the_file_now() {
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    const QString path = model.newTalkgroupListPath();
    const auto counts = [&model](const QString& file) {
        const QVariantMap map = model.groupListCounts(file);
        return QStringLiteral("%1 %2 %3")
            .arg(map.value("ok").toBool() ? QStringLiteral("ok") : QStringLiteral("unread"))
            .arg(map.value("accepted").toInt())
            .arg(map.value("skipped").toInt());
    };
    expect("write the list", write_file(path, QByteArray("id,mode,name\n1001,A,Dispatch\n1002,A,Fire\n")));
    expect("register the list", model.registerTalkgroupList(path));
    expect("rows counted", counts(path) == QStringLiteral("ok 2 0"));
    expect("emptied in place", write_file(path, QByteArray("id,mode,name\n")));
    expect("an emptied list has none, whatever its import counted",
           counts(path) == QStringLiteral("ok 0 0")
               && model.get(model.rowForPath(path)).value("accepted").toInt() == 2);
    expect("a ';' list", write_file(path, QByteArray("id;mode;name\n1001;A;Dispatch\n")));
    expect("rows it cannot use are skipped", counts(path) == QStringLiteral("ok 0 1"));
    const QTemporaryDir outside;
    const QString external = outside.filePath("external.csv");
    expect("write outside fixture", write_file(external, QByteArray("id,mode,name\n1001,A,Dispatch\n")));
    expect("a file outside the library is read too", counts(external) == QStringLiteral("ok 1 0"));
    expect("an outside ';' list", write_file(external, QByteArray("id;mode;name\n1001;A;Dispatch\n")));
    expect("an outside list's unusable rows are skipped", counts(external) == QStringLiteral("ok 0 1"));
    expect("file removed", QFile::remove(path));
    expect("a missing file is not read", counts(path) == QStringLiteral("unread 0 0"));
    expect("no path is not read", counts(QString()) == QStringLiteral("unread 0 0"));
    model.remove(model.rowForPath(path));
}

/* The decoder's side of a session using a row's group file: its own list, loaded from that file. */
struct DecoderList {
    dsd_opts* opts = static_cast<dsd_opts*>(calloc(1, sizeof(dsd_opts)));
    dsd_state* state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));

    explicit DecoderList(const QString& path) {
        if (opts != nullptr && state != nullptr) {
            DSD_SNPRINTF(opts->group_in_file, sizeof opts->group_in_file, "%s", path.toUtf8().constData());
            loaded = dsd_tg_policy_reload_group_file(opts, state) == 0;
        }
    }

    ~DecoderList() {
        if (state != nullptr) {
            dsd_state_ext_free_all(state);
        }
        free(state);
        free(opts);
    }

    DecoderList(const DecoderList&) = delete;
    DecoderList& operator=(const DecoderList&) = delete;

    int
    save() {
        return dsd_tg_policy_write_group_file(opts, state);
    }

    int
    appendAlias(uint32_t id) {
        dsd_tg_policy_entry alias;
        if (dsd_tg_policy_make_exact_entry(id, "D", "Learned", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &alias) != 0
            || dsd_tg_policy_learn_exact(state, &alias) != DSD_TG_POLICY_LEARN_ADDED) {
            return -100;
        }
        return dsd_tg_policy_append_group_file_row(opts, state, &alias, "");
    }

    bool loaded = false;
};

/* Wait, two seconds at most, until another thread is blocked on the managed-file lock or @p done is set. */
void
wait_for_lock_waiter(const std::atomic<bool>& done) {
    for (int i = 0; i < 2000 && dsd_managed_file_test_waiters() < 1 && !done.load(); i++) {
        dsd_sleep_ms(1);
    }
}

struct CheckedRace {
    std::atomic<bool> atChecked{false};
    std::atomic<bool> qtDone{false};
};

// The races the managed-file hook drives (the hook takes no context pointer).
std::unique_ptr<CheckedRace> g_checked_race;

void
race_at_checked(dsd_managed_file_step step) {
    CheckedRace* race = g_checked_race.get();
    if (race != nullptr && step == DSD_MANAGED_FILE_STEP_CHECKED && !race->atChecked.exchange(true)) {
        wait_for_lock_waiter(race->qtDone);
    }
}

/* Discussion #748 follow-up R4: an "Update from file" (and a RadioReference refresh) of the session's group file
 * cannot land between the decoder's check of the file and its rewrite. The decoder saves on a worker and stops at
 * its checked point; the update starts then, and its move (#832: on the Qt thread, after the import worker staged
 * the copy) must wait for the save, and lands on top. The decoder's next save then refuses to write over the update
 * until the list is reloaded. */
void
test_update_waits_for_decoder_rewrite(bool viaRefresh) {
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir fixtures;
    const auto original = fixtures.filePath("original.csv");
    const auto replacement = fixtures.filePath("replacement.csv");
    const QByteArray update("id,mode,name\n123,A,Original\n456,A,Added\n");
    expect("race fixture", write_file(original, "id,mode,name\n123,A,Original\n") && write_file(replacement, update));
    const auto path = importAndWait(model, original, "race.csv", "group").value("path").toString();
    const int row = model.rowForPath(path);
    DecoderList decoder(path);
    expect("race decoder load", row >= 0 && decoder.loaded);
    if (row < 0 || !decoder.loaded) {
        return;
    }
    expect("race decoder lockout", dsd_tg_policy_set_mode(decoder.state, 123, 123, "B") == 0);

    g_checked_race = std::make_unique<CheckedRace>();
    CheckedRace& race = *g_checked_race;
    int saved = -100;
    dsd_managed_file_test_set_hook(race_at_checked);
    std::thread saver([&decoder, &saved] { saved = decoder.save(); });
    for (int i = 0; i < 2000 && !race.atChecked.load(); i++) {
        dsd_sleep_ms(1);
    }
    const QVariantMap result =
        viaRefresh ? model.refreshGeneratedFile(row, replacement)
                   : importAndWait(model, QUrl::fromLocalFile(replacement).toString(), "update.csv", "group", path);
    race.qtDone = true;
    saver.join();
    dsd_managed_file_test_set_hook(nullptr);
    expect("race decoder reached its checked point", race.atChecked.load());
    expect("race decoder saved", saved == 0);
    expect(viaRefresh ? "race refresh applied" : "race update applied", result.value("ok").toBool());
    expect(viaRefresh ? "race refresh is on disk" : "race update is on disk", read_file(path) == update);
    expect("race next save refuses the unreloaded update", decoder.save() == 2);
    expect("race update stays", read_file(path) == update);
    model.remove(model.rowForPath(path));
    g_checked_race.reset();
}

// Whether a replacement reached its installed point, for the hook below (the hook takes no context pointer).
std::atomic<bool> g_update_installed{false};

void
note_installed(dsd_managed_file_step step) {
    if (step == DSD_MANAGED_FILE_STEP_INSTALLED) {
        g_update_installed = true;
    }
}

/* The move of an update or RadioReference refresh is its last step (#832), so a failure before it, here the library
 * index that cannot be saved, leaves the file as it was: nothing was installed, so nothing is rolled back over a
 * newer file, and a decoder alias append after the failure lands and its next save goes through (R4). */
void
test_failed_update_leaves_the_file(bool viaRefresh) {
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir fixtures;
    const auto original = fixtures.filePath("original.csv");
    const auto replacement = fixtures.filePath("replacement.csv");
    const QByteArray before("id,mode,name\n123,A,Original\n");
    expect("failed update fixture",
           write_file(original, before) && write_file(replacement, "id,mode,name\n321,B,Replacement\n"));
    const auto path = importAndWait(model, original, "rollback.csv", "group").value("path").toString();
    const int row = model.rowForPath(path);
    auto decoderOwner = std::make_unique<DecoderList>(path);
    DecoderList& decoder = *decoderOwner;
    expect("failed update decoder load", row >= 0 && decoder.loaded);
    if (row < 0 || !decoder.loaded) {
        return;
    }
    const auto store = dsd_qt::json_store_path("imported_files.json");
    expect("failed update move metadata store", QFile::rename(store, store + ".held"));
    expect("failed update block metadata write", QDir().mkdir(store));
    g_update_installed = false;
    dsd_managed_file_test_set_hook(note_installed);
    const QVariantMap result = viaRefresh ? model.refreshGeneratedFile(row, replacement)
                                          : importAndWait(model, QUrl::fromLocalFile(replacement).toString(),
                                                          "replacement.csv", "group", path);
    dsd_managed_file_test_set_hook(nullptr);
    expect("failed update unblock metadata write", QDir().rmdir(store) && QFile::rename(store + ".held", store));
    expect("failed update reports failure", !result.value("ok").toBool());
    expect("failed update installed nothing", !g_update_installed.load());
    expect("failed update keeps the file", read_file(path) == before);
    expect("failed update alias appended", decoder.appendAlias(789) == 0);
    expect("failed update alias landed", read_file(path) == before + "789,D,Learned\n");
    expect("failed update decoder save after both", decoder.save() == 0);
    model.remove(model.rowForPath(path));
}

/* B11: a RadioReference library file updated with the user's own bytes no longer describes the site the saved systems
 * naming it came from. They let go of it first, as a removal clears references before the file goes: when that save
 * fails the update is refused (`save`) with the file and the rows as they were; when the install fails after it, the
 * rows stay cleared over the file's old bytes. A RadioReference refresh keeps them, and so does an update of a file that
 * never came from RadioReference (the site's own key list): it says nothing about the site. */
void
test_user_replace_lets_go_first() {
    TestHost host;
    dsd_qt::SavedSystemsModel systems;
    while (systems.count() > 0 && systems.remove(0)) {}
    QTemporaryDir fixtures;
    const auto original = fixtures.filePath("original.csv");
    const auto replacement = fixtures.filePath("replacement.csv");
    const auto keys = fixtures.filePath("keys.csv");
    const auto newKeys = fixtures.filePath("new-keys.csv");
    const QByteArray before("id,mode,name\n123,A,Original\n");
    const QByteArray after("id,mode,name\n321,B,Replacement\n");
    expect("lets go: fixtures", write_file(original, before) && write_file(replacement, after)
                                    && write_file(keys, "id,value\n1,12345\n")
                                    && write_file(newKeys, "id,value\n2,67890\n"));
    QString path;
    QString keyPath;
    {
        dsd_qt::ImportedFilesModel library(&host);
        const QVariantMap origin{{"origin", "radioreference"}, {"rrSid", 12}, {"rrSiteIds", "3"}, {"rrKind", "group"}};
        path = library.importGeneratedFile(original, "SARA group.csv", "group", origin).value("path").toString();
        keyPath = importAndWait(library, keys, "keys.csv", "keysDec").value("path").toString();
    }
    expect("lets go: library files", !path.isEmpty() && !keyPath.isEmpty());
    const QVariantMap place{{"states", QVariantList{QVariantMap{{"stid", 19}, {"name", "Iowa"}}}}};
    const QVariantMap site{{"name", "SARA — North"}, {"rrSid", 12},      {"rrSiteId", 3},      {"siteName", "North"},
                           {"siteLat", 41.6},        {"siteLon", -91.6}, {"hasSitePos", true}, {"groupCsvPath", path},
                           {"keyCsvPath", keyPath},  {"place", place}};
    expect("lets go: a grouped site names both files", systems.add(site) && systems.setAvoidSite(0, true));
    const auto grouped = [&systems]() {
        const QVariantMap row = systems.get(0);
        return row.value("rrSid").toInt() == 12 && row.value("rrSiteId").toInt() == 3 && row.value("avoidSite").toBool()
               && row.value("siteName") == "North" && row.value("hasSitePos").toBool()
               && !row.value("place").toMap().isEmpty();
    };
    const auto cleared = [&systems, &path]() {
        const QVariantMap row = systems.get(0);
        return row.value("rrSid").toInt() == 0 && row.value("rrSiteId").toInt() == 0
               && row.value("siteName").toString().isEmpty() && !row.value("hasSitePos").toBool()
               && row.value("place").toMap().isEmpty() && row.value("groupCsvPath") == path;
    };
    const auto letGo = [&systems](const QString& stored) { return systems.invalidatePathProvenance(stored); };
    const QString url = QUrl::fromLocalFile(replacement).toString();

    {
        // A RadioReference refresh is trusted: the rows keep their site.
        dsd_qt::ImportedFilesModel library(&host);
        library.setBeforeUserReplace(letGo);
        expect("lets go: a refresh lands",
               library.refreshGeneratedFile(library.rowForPath(path), original).value("ok").toBool());
        expect("lets go: a refresh keeps the rows' site", grouped());
    }
    {
        // A file the user picked says nothing about the site: updating the site's keys keeps it grouped.
        dsd_qt::ImportedFilesModel library(&host);
        library.setBeforeUserReplace(letGo);
        const QVariantMap updated = replaceAndWait(library, library.rowForPath(keyPath),
                                                   QUrl::fromLocalFile(newKeys).toString(), "new-keys.csv");
        expect("lets go: a key list update lands", updated.value("ok").toBool());
        expect("lets go: a key list update keeps the site", grouped());
    }
    {
        dsd_qt::ImportedFilesModel library(&host);
        library.setBeforeUserReplace(letGo);
        const QString store = dsd_qt::json_store_path("saved_systems.json");
        expect("lets go: systems store blocked", QFile::rename(store, store + ".held") && QDir().mkdir(store));
        const QVariantMap refused = replaceAndWait(library, library.rowForPath(path), url, "replacement.csv");
        expect("lets go: systems store unblocked", QDir().rmdir(store) && QFile::rename(store + ".held", store));
        expect("lets go: a failed let-go refuses the update", refused.value("error") == "save");
        expect("lets go: the file keeps its bytes", read_file(path) == before);
        expect("lets go: the library keeps its counts and provenance",
               library.get(library.rowForPath(path)).value("accepted").toInt() == 1
                   && library.get(library.rowForPath(path)).value("origin") == "radioreference");
        expect("lets go: the rows keep their site", grouped());
    }
    {
        dsd_qt::ImportedFilesModel library(&host);
        library.setBeforeUserReplace(letGo);
        library.setReplaceFile([](const QString&, const QString&) {
            errno = EIO;
            return -1;
        });
        const QVariantMap failed = replaceAndWait(library, library.rowForPath(path), url, "replacement.csv");
        expect("lets go: a failed install is reported", failed.value("error") == "local");
        expect("lets go: the file keeps its bytes after a failed install", read_file(path) == before);
        expect("lets go: the library keeps its provenance after a failed install",
               library.get(library.rowForPath(path)).value("origin") == "radioreference");
        expect("lets go: the rows stay cleared", cleared());
    }
    expect("lets go: the site again", systems.update(0, site) && systems.setAvoidSite(0, true) && grouped());
    {
        dsd_qt::ImportedFilesModel library(&host);
        library.setBeforeUserReplace(letGo);
        const QVariantMap updated = replaceAndWait(library, library.rowForPath(path), url, "replacement.csv");
        expect("lets go: an update lands", updated.value("ok").toBool() && read_file(path) == after);
        expect("lets go: and the rows let go of the site", cleared());
        expect("lets go: the file is the user's own now", library.get(library.rowForPath(path)).value("origin") == "");
        int calls = 0;
        library.setBeforeUserReplace([&calls](const QString&) { return ++calls > 0; });
        expect("lets go: a second update",
               replaceAndWait(library, library.rowForPath(path), QUrl::fromLocalFile(original).toString(), "again.csv")
                   .value("ok")
                   .toBool());
        expect("lets go: the user's own file asks nothing of the rows", calls == 0);
        library.remove(library.rowForPath(path));
        library.remove(library.rowForPath(keyPath));
    }
    while (systems.count() > 0 && systems.remove(0)) {}
}

void
test_metadata_failure_keeps_file() {
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir fixtures;
    const auto original = fixtures.filePath("original.csv");
    const auto replacement = fixtures.filePath("replacement.csv");
    const QByteArray before("id,mode,name\n123,A,Original\n");
    expect("metadata fixture", write_file(original, before));
    expect("metadata replacement", write_file(replacement, "id,mode,name\n321,B,Replacement\n"));
    const auto imported = importAndWait(model, original, "atomic.csv", "group");
    expect("metadata baseline import", imported.value("ok").toBool());
    const auto path = imported.value("path").toString();
    const int row = model.rowForPath(path);
    if (row < 0) {
        return;
    }
    const auto store = dsd_qt::json_store_path("imported_files.json");
    expect("move metadata store", QFile::rename(store, store + ".held"));
    expect("block metadata write", QDir().mkdir(store));
    expect("update reports metadata failure",
           !replaceAndWait(model, row, replacement, "replacement.csv").value("ok").toBool());
    expect("metadata failure preserves original bytes", read_file(path) == before);
    expect("metadata failure preserves counts", model.get(row).value("accepted").toInt() == 1);
    expect("unblock metadata write", QDir().rmdir(store) && QFile::rename(store + ".held", store));
    model.remove(row);
}

/* Issue #801: removal deletes the file before the row, and each step can run again. A file that will not delete keeps
 * its row, in memory and in the index; an index that will not save after the file went keeps the row too, and the
 * next call finishes the removal. */
void
test_remove_finishes_on_retry() {
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir fixtures;
    const auto source = fixtures.filePath("talkgroups.csv");
    expect("removal fixture", write_file(source, "id,mode,name\n123,A,Dispatch\n"));
    const auto path = importAndWait(model, source, "removal.csv", "group").value("path").toString();
    const auto store = dsd_qt::json_store_path("imported_files.json");
    const auto indexed = [&store, &path]() { return read_file(store).contains(QFileInfo(path).fileName().toUtf8()); };
    const int count = model.count();
    // A non-empty directory where the file was: nothing deletes it as a file.
    expect("hold the stored file", QFile::rename(path, path + ".held"));
    expect("block the file delete", QDir().mkpath(path + "/inside"));
    expect("an undeletable file is reported", !model.remove(model.rowForPath(path)));
    expect("an undeletable file keeps its row", model.count() == count && model.rowForPath(path) >= 0 && indexed());
    expect("unblock the file delete", QDir(path).removeRecursively() && QFile::rename(path + ".held", path));
    // The file goes, then the index save fails.
    expect("move the index", QFile::rename(store, store + ".held"));
    expect("block the index save", QDir().mkdir(store));
    expect("an unsaved index is reported", !model.remove(model.rowForPath(path)));
    expect("the file went first", !QFile::exists(path));
    expect("an unsaved index keeps the row", model.count() == count && model.rowForPath(path) >= 0);
    expect("unblock the index save", QDir().rmdir(store) && QFile::rename(store + ".held", store));
    expect("the retry finishes", model.remove(model.rowForPath(path)));
    expect("the row is gone", model.count() == count - 1 && model.rowForPath(path) < 0 && !indexed());
}

/*
 * Issue #832: imports copy and check on the import worker and answer through importFinished(), once per id, queued.
 */

/* The answer to import @p id that @p spy caught: waits for it, and expects exactly one. */
QString
importsDirPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/imports");
}

/* Whether every request folder is gone; the worker removes each once its commit returned, so this waits a little. */
bool
stagingEmpty() {
    const auto empty = []() {
        return QDir(dsd_qt::ImportService::stagingRoot()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
    };
    for (int waits = 0; waits < 100 && !empty(); waits++) {
        QThread::msleep(20);
    }
    return empty();
}

int
storedFileCount() {
    return static_cast<int>(QDir(importsDirPath()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size());
}

/* The accepted count the index on disk records for @p path; -1 when no row names it. */
int
indexedAccepted(const QString& path) {
    for (const auto& value : dsd_qt::json_store_load_array(QStringLiteral("imported_files.json"))) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("path")).toString() == path) {
            return row.value(QStringLiteral("accepted")).toInt();
        }
    }
    return -1;
}

/* Put a directory where the index is, so it cannot be saved; undo with unblockIndex(). */
bool
blockIndex() {
    const QString store = dsd_qt::json_store_path(QStringLiteral("imported_files.json"));
    if (!QFile::exists(store) && !write_file(store, "[]")) {
        return false;
    }
    return QFile::rename(store, store + QStringLiteral(".held")) && QDir().mkdir(store);
}

bool
unblockIndex() {
    const QString store = dsd_qt::json_store_path(QStringLiteral("imported_files.json"));
    return QDir().rmdir(store) && QFile::rename(store + QStringLiteral(".held"), store);
}

/* Make the library folder read-only; false when this system still lets the owner add a file there (root, Windows). */
bool
makeImportsReadOnly() {
    const QString dir = importsDirPath();
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QFile probe(dir + QStringLiteral("/probe"));
    if (probe.open(QIODevice::WriteOnly)) {
        probe.close();
        probe.remove();
        return false;
    }
    return true;
}

void
makeImportsWritable() {
    QFile::setPermissions(importsDirPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

void
test_async_import_outcomes() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const QString groups = source.filePath("county.csv");
    expect("async group fixture", write_file(groups, "TG,Mode,Name\n101,D,Dispatch\n102,D,Fire\nbogus,D,Bad\n"));

    QSignalSpy importing(&model, &dsd_qt::ImportedFilesModel::importingChanged);
    QSignalSpy entries(&model, &dsd_qt::ImportedFilesModel::entriesChanged);
    QSignalSpy finished(&model, &dsd_qt::ImportedFilesModel::importFinished);
    const int revision = model.revision();
    const int id = model.beginImport(QUrl::fromLocalFile(groups).toString(), "county.csv", "group", {}, QString());
    expect("an import in progress is reported", model.importing() && importing.size() == 1);
    expect("an import is answered later, never inside beginImport()", finished.isEmpty() && model.count() == 0);
    const QVariantMap ok = waitForImport(finished, id);
    expect("async import succeeds", ok.value("ok").toBool() && ok.value("error").toString().isEmpty());
    expect("async import counts its rows", ok.value("accepted").toInt() == 2 && ok.value("skipped").toInt() == 1);
    expect("async import keeps the provider's name", ok.value("name") == "county.csv" && ok.value("type") == "group"
                                                         && ok.value("path") == importsDirPath() + "/county.csv");
    expect("async import stores the bytes", read_file(ok.value("path").toString()) == read_file(groups));
    expect("async import adds and saves a row", model.count() == 1 && model.rowForPath(ok.value("path").toString()) == 0
                                                    && indexedAccepted(ok.value("path").toString()) == 2);
    expect("an ended import is reported", !model.importing() && importing.size() == 2);
    expect("an added row changes the library revision", entries.size() == 1 && model.revision() != revision);

    const QString header = source.filePath("header.csv");
    expect("async empty fixture", write_file(header, "TG,Mode,Name\n"));
    const QVariantMap empty = importAndWait(model, header, "header.csv", "group");
    expect("a file with no usable row is kept and flagged",
           empty.value("ok").toBool() && empty.value("error") == "empty" && model.count() == 2);

    const QString large = source.filePath("large.csv");
    QFile oversized(large);
    expect("async oversized fixture", oversized.open(QIODevice::WriteOnly) && oversized.resize(32LL * 1024 * 1024 + 1));
    oversized.close();
    const QVariantMap tooLarge = importAndWait(model, large, "large.csv", "group");
    expect("a file over 32 MiB is refused with the limit",
           !tooLarge.value("ok").toBool() && tooLarge.value("error") == "too_large"
               && tooLarge.value("limitBytes").toLongLong() == 32LL * 1024 * 1024
               && tooLarge.value("name") == "large.csv");

    const QVariantMap unreadable = importAndWait(model, source.filePath("absent.csv"), "absent.csv", "group");
    expect("a missing document is a read failure",
           !unreadable.value("ok").toBool() && unreadable.value("error") == "read");

    // A file where the staging root belongs: no request folder can be made, so nothing can be copied locally.
    const QString staging = dsd_qt::ImportService::stagingRoot();
    // Once the worker has removed the earlier requests' folders: a removal still running would race this one.
    expect("block the staging root",
           stagingEmpty() && QDir(staging).removeRecursively() && write_file(staging, "not a folder"));
    const QVariantMap local = importAndWait(model, groups, "county.csv", "group");
    expect("a copy this device cannot write is a local failure",
           !local.value("ok").toBool() && local.value("error") == "local");
    expect("unblock the staging root", QFile::remove(staging));

    const QString badMap = source.filePath("tg-keys.csv");
    expect("async format fixture", write_file(badMap, "not a talkgroup row\n"));
    const QVariantMap format = importAndWait(model, badMap, "tg-keys.csv", "dmrTgKeys");
    expect("a file its type cannot parse is a format failure with its detail",
           !format.value("ok").toBool() && format.value("error") == "format" && format.contains("detail"));
    // The parser's own reason, naming the file as the user knows it, never the staging copy.
    const QString why = format.value("detail").toString();
    expect("a format failure says why the parser refused the file",
           why.contains("tg-keys.csv") && why.contains("no mappings") && !why.contains(staging) && why.at(0).isUpper());
    const QString badMode = source.filePath("bad-mode.csv");
    expect("bad mode fixture", write_file(badMode, "channel,frequency,mode\n1,851012500,warp\n"));
    const QString mode = importAndWait(model, badMode, "bad-mode.csv", "chan").value("detail").toString();
    expect("a channel map's reason names its row and the bad mode",
           mode.startsWith("Channel map file 'bad-mode.csv' row 2: invalid mode") && !mode.contains(staging));
    const QVariantMap unknown = importAndWait(model, groups, "county.csv", "futureType");
    expect("an unknown type is refused through the same answer",
           !unknown.value("ok").toBool() && unknown.value("error") == "format");
    expect("no failure adds a row or leaves a file", model.count() == 2 && storedFileCount() == 2);

    QSignalSpy pair(&model, &dsd_qt::ImportedFilesModel::importFinished);
    const int cancelled = model.beginImport(groups, "county.csv", "group", {}, QString());
    model.cancelImport(cancelled);
    const int after = model.beginImport(groups, "county.csv", "group", {}, QString());
    const QVariantMap cancel = waitForImport(pair, cancelled);
    expect("a cancelled import answers cancelled",
           !cancel.value("ok").toBool() && cancel.value("error") == "cancelled" && cancel.value("type") == "group");
    const QVariantMap next = waitForImport(pair, after);
    expect("an import after a cancel succeeds", next.value("ok").toBool() && model.count() == 3);
    expect("the cancelled copy left nothing", storedFileCount() == 3);

    QSignalSpy twice(&model, &dsd_qt::ImportedFilesModel::importFinished);
    const int first = model.beginImport(header, "header.csv", "group", {}, QString());
    const int second = model.beginImport(header, "header.csv", "group", {}, QString());
    const QVariantMap one = waitForImport(twice, first);
    const QVariantMap two = waitForImport(twice, second);
    expect("the same document picked twice gets two ids", first != second);
    expect("the same document picked twice gets two names",
           one.value("ok").toBool() && two.value("ok").toBool() && one.value("path") != two.value("path")
               && QFileInfo(two.value("path").toString()).fileName() == "header (3).csv");
    expect("both copies are listed", model.count() == 5 && twice.size() == 2);

    // Two different documents with the same name (two agencies both ship a "dup.csv"): both are kept.
    QTemporaryDir agencies;
    const QString dupA = agencies.filePath("a/dup.csv");
    const QString dupB = agencies.filePath("b/dup.csv");
    expect("same-name fixtures", QDir(agencies.path()).mkpath("a") && QDir(agencies.path()).mkpath("b")
                                     && write_file(dupA, "TG,Mode,Name\n101,D,Dispatch\n")
                                     && write_file(dupB, "TG,Mode,Name\n201,D,PD\n202,D,FD\n"));
    const QVariantMap dupOne = importAndWait(model, dupA, "dup.csv", "group");
    const QVariantMap dupTwo = importAndWait(model, dupB, "dup.csv", "group");
    const QString dupOnePath = dupOne.value("path").toString();
    const QString dupTwoPath = dupTwo.value("path").toString();
    expect("two documents of one name are both kept, at distinct paths",
           dupOne.value("ok").toBool() && dupTwo.value("ok").toBool() && model.count() == 7 && dupOnePath != dupTwoPath
               && model.rowForPath(dupOnePath) >= 0 && model.rowForPath(dupTwoPath) >= 0);
    expect("the second leaves the first document's bytes as they were",
           read_file(dupOnePath) == "TG,Mode,Name\n101,D,Dispatch\n"
               && read_file(dupTwoPath) == "TG,Mode,Name\n201,D,PD\n202,D,FD\n");

    while (model.count() > 0) {
        model.remove(0);
    }
}

/* A replace commits only over the row it began against: a refresh, a remove and re-add, or a new type in between makes
 * it stale, and the file keeps what the change in between wrote. */
void
test_async_replace_stale() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const QString groups = source.filePath("groups.csv");
    const QByteArray refreshed("TG,Mode,Name\n7,D,Refreshed\n");
    const QString refresh = source.filePath("refresh.csv");
    const QString repick = source.filePath("repick.csv");
    expect("stale fixtures", write_file(groups, "TG,Mode,Name\n1,D,One\n") && write_file(refresh, refreshed)
                                 && write_file(repick, "TG,Mode,Name\n2,D,Two\n3,D,Three\n"));
    const QString stored = importAndWait(model, groups, "groups.csv", "group").value("path").toString();

    QSignalSpy spy(&model, &dsd_qt::ImportedFilesModel::importFinished);
    int id = model.beginImport(repick, "repick.csv", "group", {}, stored);
    expect("refresh between begin and commit",
           model.refreshGeneratedFile(model.rowForPath(stored), refresh).value("ok").toBool());
    QVariantMap result = waitForImport(spy, id);
    expect("a replace after a refresh is stale", !result.value("ok").toBool() && result.value("error") == "stale");
    expect("a stale replace keeps the refreshed bytes", read_file(stored) == refreshed);
    expect("a stale replace keeps the refreshed counts", model.get(model.rowForPath(stored)).value("accepted") == 1);

    id = model.beginImport(repick, "repick.csv", "group", {}, stored);
    expect("remove between begin and commit", model.remove(model.rowForPath(stored)));
    expect("re-add at the same path", importAndWait(model, groups, "groups.csv", "group").value("path") == stored);
    result = waitForImport(spy, id);
    expect("a replace after a remove and re-add is stale", result.value("error") == "stale");
    expect("the re-added file stays", read_file(stored) == read_file(groups));

    id = model.beginImport(repick, "repick.csv", "group", {}, stored);
    expect("remove before a new type", model.remove(model.rowForPath(stored)));
    expect("re-add as another type", importAndWait(model, groups, "groups.csv", "src").value("path") == stored);
    result = waitForImport(spy, id);
    expect("a replace after a type change is stale", result.value("error") == "stale");
    expect("the re-typed row stays", model.get(model.rowForPath(stored)).value("type") == "src");

    const QVariantMap missing = importAndWait(model, repick, "repick.csv", "group", stored + ".gone");
    expect("a replace of no row is refused as changed (#829)",
           missing.value("error") == "changed"
               && missing.value("detail") == "That file changed while you were picking. Try again.");
    const QVariantMap wrongType = importAndWait(model, repick, "repick.csv", "group", stored);
    expect("a replace as another type is refused", wrongType.value("error") == "path");
    expect("refusals change nothing", model.count() == 1 && read_file(stored) == read_file(groups));
    model.remove(model.rowForPath(stored));
}

/* Issue #832 (B11) and #748: the running engine reads a target list, so replacing one is refused while the decoder
 * runs: at the start and again at the commit. A talkgroup list is replaced while it runs: its move takes the
 * managed-file lock the engine's own writes take, and the Imports screen asks the session to reload it. */
void
test_group_replace_busy() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const QString groups = source.filePath("groups.csv");
    const QString repick = source.filePath("repick.csv");
    const QString targets = source.filePath("targets.csv");
    const QString retarget = source.filePath("retarget.csv");
    const QByteArray header("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n");
    const QByteArray keptTargets = header + "one,p25-conventional,851500000,,,,\n";
    expect("busy fixtures",
           write_file(groups, "TG,Mode,Name\n1,D,One\n") && write_file(repick, "TG,Mode,Name\n2,D,Two\n3,D,Three\n")
               && write_file(targets, keptTargets)
               && write_file(retarget,
                             header + "one,p25-conventional,851500000,,,,\n" + "two,p25-conventional,852500000,,,,\n"));
    const QString storedGroups = importAndWait(model, groups, "groups.csv", "group").value("path").toString();
    const QString storedTargets = importAndWait(model, targets, "targets.csv", "trunkTargets").value("path").toString();
    const int groupRow = model.rowForPath(storedGroups);
    const int targetRow = model.rowForPath(storedTargets);
    expect("busy rows", groupRow >= 0 && targetRow >= 0);

    host.running = true;
    const QVariantMap group = importAndWait(model, repick, "repick.csv", "group", storedGroups);
    expect("a group replace runs while decoding",
           group.value("ok").toBool() && group.value("path") == storedGroups
               && model.get(model.rowForPath(storedGroups)).value("accepted") == 2);

    const QVariantMap atStart = importAndWait(model, retarget, "retarget.csv", "trunkTargets", storedTargets);
    expect("a target replace is refused at the start while decoding",
           atStart.value("error") == "busy"
               && atStart.value("detail") == "Stop the decoder before replacing target files.");
    expect("a refused replace keeps the file", read_file(storedTargets) == keptTargets);

    host.running = false;
    QSignalSpy spy(&model, &dsd_qt::ImportedFilesModel::importFinished);
    const int id = model.beginImport(retarget, "retarget.csv", "trunkTargets", {}, storedTargets);
    host.running = true;
    const QVariantMap atCommit = waitForImport(spy, id);
    expect("a target replace is refused at the commit once decoding started", atCommit.value("error") == "busy");
    expect("a replace refused at the commit keeps the file", read_file(storedTargets) == keptTargets);

    host.running = false;
    const QVariantMap stopped = importAndWait(model, retarget, "retarget.csv", "trunkTargets", storedTargets);
    expect("a target replace runs once the decoder stopped",
           stopped.value("ok").toBool() && stopped.value("path") == storedTargets);
    model.remove(model.rowForPath(storedTargets));
    model.remove(model.rowForPath(storedGroups));
}

/* The replace order: the index is saved first, then the file moves over the old one. Either step failing leaves the
 * old index and the old bytes. */
void
test_replace_failures() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const QString groups = source.filePath("groups.csv");
    const QString repick = source.filePath("repick.csv");
    const QByteArray kept("TG,Mode,Name\n1,D,One\n");
    expect("replace failure fixtures",
           write_file(groups, kept) && write_file(repick, "TG,Mode,Name\n2,D,Two\n3,D,Three\n"));
    const QString stored = importAndWait(model, groups, "groups.csv", "group").value("path").toString();
    const int row = model.rowForPath(stored);

    expect("block the index", blockIndex());
    const QVariantMap unsaved = importAndWait(model, repick, "repick.csv", "group", stored);
    expect("unblock the index", unblockIndex());
    expect("an index that will not save fails the replace", unsaved.value("error") == "save");
    expect("an unsaved replace keeps the old bytes and counts",
           read_file(stored) == kept && model.get(row).value("accepted") == 1 && indexedAccepted(stored) == 1);

    QSignalSpy entries(&model, &dsd_qt::ImportedFilesModel::entriesChanged);
    if (makeImportsReadOnly()) {
        const QVariantMap unpublished = importAndWait(model, repick, "repick.csv", "group", stored);
        expect("a file that will not move fails the replace", unpublished.value("error") == "local");
        expect("an unpublished replace puts the old index back", indexedAccepted(stored) == 1);
        expect("an unpublished replace keeps the old bytes and counts",
               read_file(stored) == kept && model.get(row).value("accepted") == 1 && entries.isEmpty());

        // A RadioReference refresh shares the order and its rollback (B6).
        const QVariantMap refreshed = model.refreshGeneratedFile(row, repick);
        expect("a refresh whose file will not move fails", refreshed.value("error") == "local");
        expect("a failed refresh puts the old index back and keeps the bytes",
               indexedAccepted(stored) == 1 && read_file(stored) == kept && model.get(row).value("accepted") == 1);
        makeImportsWritable();
    } else {
        makeImportsWritable();
        DSD_FPRINTF(stderr, "SKIP: this system lets the owner write a read-only folder\n");
    }
    const QVariantMap replaced = importAndWait(model, repick, "repick.csv", "group", stored);
    expect("the replace succeeds once both can be written",
           replaced.value("ok").toBool() && indexedAccepted(stored) == 2 && read_file(stored) == read_file(repick));
    model.remove(row);
}

/* Issue #832 (B8): a list the export flow is told was added must still be listed after a restart. */
void
test_register_unsaved_export() {
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    const int before = model.count();
    const QString stored = model.newTalkgroupListPath();
    expect("unsaved export fixture", write_file(stored, "id,mode,name,priority,preempt\n42,A,Dispatch,50,1\n"));
    expect("block the index for registration", blockIndex());
    const bool registered = model.registerTalkgroupList(stored);
    expect("unblock the index after registration", unblockIndex());
    expect("an export the index cannot record is not registered", !registered);
    expect("an unrecorded export adds no row", model.count() == before && model.rowForPath(stored) < 0);
    expect("an unrecorded export keeps the engine's file", QFile::exists(stored));
    expect("a later registration records it", model.registerTalkgroupList(stored) && model.count() == before + 1);
    model.remove(model.rowForPath(stored));
}

/* Issue #832 (B7): the screens list entriesForType() again on entriesChanged, so a replace or a refresh that keeps the
 * count still announces itself. */
void
test_entries_changed_on_replace() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const QString groups = source.filePath("groups.csv");
    const QString repick = source.filePath("repick.csv");
    expect("entries fixtures",
           write_file(groups, "TG,Mode,Name\n1,D,One\n") && write_file(repick, "TG,Mode,Name\n2,D,Two\n3,D,Three\n"));
    const QString map = source.filePath("map.csv");
    const QString remap = source.filePath("remap.csv");
    expect("map fixtures", write_file(map, "channel,frequency\n1,851012500\n")
                               && write_file(remap, "channel,frequency\n1,851012500\n2,852012500\n"));
    const QString storedMap = importAndWait(model, map, "map.csv", "chan").value("path").toString();
    const QString stored = importAndWait(model, groups, "groups.csv", "group").value("path").toString();
    const int row = model.rowForPath(stored);
    QSignalSpy entries(&model, &dsd_qt::ImportedFilesModel::entriesChanged);
    QSignalSpy counts(&model, &dsd_qt::ImportedFilesModel::countChanged);

    int revision = model.revision();
    expect("plain replace", replaceAndWait(model, row, repick, "repick.csv").value("ok").toBool());
    expect("a plain replace announces the change", entries.size() == 1 && model.revision() != revision);
    revision = model.revision();
    expect("refresh", model.refreshGeneratedFile(row, groups).value("ok").toBool());
    expect("a refresh announces the change", entries.size() == 2 && model.revision() != revision);
    revision = model.revision();
    expect("async replace", importAndWait(model, repick, "repick.csv", "group", stored).value("ok").toBool());
    expect("an async replace announces the change", entries.size() == 3 && model.revision() != revision);
    revision = model.revision();
    const QVariantMap rebuilt = importAndWait(model, remap, "remap.csv", "chan", storedMap);
    expect("a channel map's bundle replace announces the change",
           rebuilt.value("ok").toBool() && rebuilt.value("accepted").toInt() == 2 && entries.size() == 4
               && model.revision() != revision);
    // The screens list entriesForType() by the revision now; a replace never moves the count (#832 B7).
    expect("a replace keeps the count, so it does not announce one", counts.isEmpty());
    expect("remove", model.remove(row));
    expect("a remove announces both", entries.size() == 5 && counts.size() == 1);
}

/* Nothing in the library is swept by name: a stored file shaped like the old Android staging names survives imports
 * that succeed and fail. And no import takes a name the talkgroup export handed out. */
void
test_stored_names_kept() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const QByteArray mine("TG,Mode,Name\n9,D,Mine\n");
    const QString stored = importsDirPath() + "/.import-x.tmp";
    expect("stored dot file fixture", QDir().mkpath(importsDirPath()) && write_file(stored, mine));
    const QString groups = source.filePath("groups.csv");
    expect("names fixture", write_file(groups, "TG,Mode,Name\n1,D,One\n"));
    const QString added = importAndWait(model, groups, "groups.csv", "group").value("path").toString();
    expect("a stored .import file survives a successful import", read_file(stored) == mine && !added.isEmpty());
    const QVariantMap failed = importAndWait(model, source.filePath("absent.csv"), "absent.csv", "group");
    expect("a stored .import file survives a failed import", !failed.value("ok").toBool() && read_file(stored) == mine);

    const QString reserved = model.newTalkgroupListPath();
    const QString sameName = source.filePath(QFileInfo(reserved).fileName());
    expect("reserved name fixture", write_file(sameName, "TG,Mode,Name\n1,D,One\n"));
    const QVariantMap result = importAndWait(model, sameName, QFileInfo(reserved).fileName(), "group");
    expect("an import skips a name the export handed out",
           result.value("ok").toBool() && result.value("path") != reserved && !QFile::exists(reserved)
               && QFileInfo(result.value("path").toString()).fileName()
                      == QFileInfo(reserved).completeBaseName() + " (2).csv");
    while (model.count() > 0) {
        model.remove(0);
    }
    QFile::remove(stored);
}

/*
 * Issue #832: channel maps and target lists build with their companions on the import worker, in a mirror of the
 * library inside the request's staging folder, within a 64 MiB budget, and move into the library at the commit.
 */

/* Wait until @p service has staged and committed every request queued so far (revision cleanups included). */
void
waitForWorker(dsd_qt::ImportService& service) {
    for (int waits = 0; waits < 200 && service.busy(); waits++) {
        QSignalSpy spy(&service, &dsd_qt::ImportService::busyChanged);
        spy.wait(50);
    }
    QCoreApplication::processEvents();
    expect("the import worker finishes its requests", !service.busy());
}

QStringList
revisionsOf(const QString& root) {
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
}

QStringList
journal() {
    bool ok = false;
    const QStringList entries = dsd_qt::ImportService::journalEntries(&ok);
    expect("the commit journal reads", ok);
    return entries;
}

/* Nothing but the stored files and the bundles folder in the library: no staging or check file is left there. */
bool
libraryHoldsOnly(const QStringList& names) {
    QStringList found = QDir(importsDirPath()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    found.removeAll(QStringLiteral("bundles"));
    found.sort();
    QStringList expected = names;
    expected.sort();
    return found == expected;
}

/* The companion a stored channel map's first row names in its column @p column, as a path. */
QString
firstCompanion(const QString& map, int column) {
    const QString relative = QString::fromUtf8(read_file(map)).split('\n').value(1).split(',').value(column);
    return QDir::cleanPath(QFileInfo(map).dir().filePath(relative));
}

void
test_async_channel_bundles() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportService service(host.importTransport());
    dsd_qt::ImportedFilesModel model(&host, &service);
    QTemporaryDir source;
    const QByteArray keys("keyid,value\n02,ABCDE\n");
    const QString plain = source.filePath("plain.csv");
    const QString bundled = source.filePath("bundled.csv");
    expect("channel bundle fixtures",
           write_file(source.filePath("keys.csv"), keys)
               && write_file(plain, "channel,frequency,mode\n1,851012500,p25\n")
               && write_file(bundled, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,keys.csv\n"));

    // A new bundle: the primary in a new root under its own name, the companion in a revision beside it.
    QVariantMap result = importAndWait(model, bundled, "bundled.csv", "chan");
    const QString stored = result.value("path").toString();
    const QString root = model.get(model.rowForPath(stored)).value("bundleRoot").toString();
    expect("a new channel bundle imports on the worker", result.value("ok").toBool() && result.value("accepted") == 1);
    expect("its primary lies in its own root under the document's name",
           QFileInfo(stored).absolutePath() == root && QFileInfo(stored).fileName() == "bundled.csv"
               && QFileInfo(root).absolutePath() == importsDirPath() + "/bundles");
    expect("its companion lies in the root's one revision", revisionsOf(root).size() == 1
                                                                && read_file(firstCompanion(stored, 3)) == keys
                                                                && firstCompanion(stored, 3).startsWith(root + '/'));
    waitForWorker(service);
    expect("the commit leaves no journal entry, staging or stray file",
           journal().isEmpty() && stagingEmpty() && libraryHoldsOnly({}));

    // A plain map gains a bundle: it keeps its place and names the companion in a new root.
    const QString loose = importAndWait(model, plain, "plain.csv", "chan").value("path").toString();
    expect("a map without companions is a plain file",
           QFileInfo(loose).absolutePath() == importsDirPath()
               && model.get(model.rowForPath(loose)).value("bundleRoot") == QString());
    result = importAndWait(model, bundled, "bundled.csv", "chan", loose);
    const QString gained = model.get(model.rowForPath(loose)).value("bundleRoot").toString();
    expect("a plain map gains a bundle and keeps its path",
           result.value("ok").toBool() && result.value("path") == loose && !gained.isEmpty() && gained != root);
    expect("the plain map names its companion in the new root",
           read_file(loose).contains("bundles/" + QFileInfo(gained).fileName().toUtf8() + '/')
               && read_file(firstCompanion(loose, 3)) == keys);
    expect("the gained bundle's map still loads", model.channelProfiles(model.rowForPath(loose)).value("ok").toBool());

    // An existing bundle is replaced: same root, a new revision, the old one reclaimed once the worker runs (B5).
    const QStringList before = revisionsOf(root);
    const QByteArray newKeys("keyid,value\n02,12345\n");
    expect("replacement companion", write_file(source.filePath("keys.csv"), newKeys));
    result = importAndWait(model, bundled, "bundled.csv", "chan", stored);
    expect("an existing channel bundle is replaced in place",
           result.value("ok").toBool() && result.value("path") == stored
               && model.get(model.rowForPath(stored)).value("bundleRoot") == root
               && read_file(firstCompanion(stored, 3)) == newKeys);
    waitForWorker(service);
    expect("the replaced channel revision is reclaimed (B5)",
           revisionsOf(root).size() == 1 && revisionsOf(root) != before);

    // A replace whose map names no companion keeps the root, and the revisions the map no longer reaches go.
    result = importAndWait(model, plain, "plain.csv", "chan", stored);
    expect("a replace without companions keeps the stored path and root",
           result.value("ok").toBool() && read_file(stored) == read_file(plain)
               && model.get(model.rowForPath(stored)).value("bundleRoot") == root);
    waitForWorker(service);
    expect("a map that names no companion keeps no revision", revisionsOf(root).isEmpty());
    expect("no replace leaves a check file or staging", libraryHoldsOnly({"plain.csv"}) && stagingEmpty());
    while (model.count() > 0) {
        model.remove(0);
    }
}

/* A replace whose move fails and whose rollback cannot be saved either: the index on disk holds the new state, so the
 * rows in memory follow it, the answer is "save" with a detail, the stored bytes stay the old ones, and a bundle's new
 * revision stays journaled for the next start to settle. */
void
test_replace_double_failure() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportService service(host.importTransport());
    dsd_qt::ImportedFilesModel model(&host, &service);
    QTemporaryDir source;
    const QString groups = source.filePath("groups.csv");
    const QString repick = source.filePath("repick.csv");
    const QString bundled = source.filePath("bundled.csv");
    const QByteArray kept("TG,Mode,Name\n1,D,One\n");
    const QByteArray map("channel,frequency,mode,keys_hex_csv\n1,851012500,p25,keys.csv\n");
    expect("double failure fixtures",
           write_file(groups, kept) && write_file(repick, "TG,Mode,Name\n2,D,Two\n3,D,Three\n")
               && write_file(bundled, map) && write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n"));
    const QString stored = importAndWait(model, groups, "groups.csv", "group").value("path").toString();
    const QString storedMap = importAndWait(model, bundled, "bundled.csv", "chan").value("path").toString();
    const QString root = model.get(model.rowForPath(storedMap)).value("bundleRoot").toString();
    waitForWorker(service);
    const QStringList revisions = revisionsOf(root);
    const QByteArray mapBytes = read_file(storedMap);

    // The move fails after the index saved, and the index can no longer be written for the rollback.
    bool blocked = false;
    model.setReplaceFile([&blocked](const QString&, const QString&) {
        blocked = blockIndex();
        errno = EACCES;
        return -1;
    });
    QSignalSpy entries(&model, &dsd_qt::ImportedFilesModel::entriesChanged);
    const QVariantMap single = importAndWait(model, repick, "repick.csv", "group", stored);
    expect("the index was blocked between the save and the rollback", blocked && unblockIndex());
    expect("a replace whose rollback cannot be saved answers save with a detail",
           single.value("error") == "save" && !single.value("detail").toString().isEmpty());
    expect("the rows in memory follow the index on disk", model.get(model.rowForPath(stored)).value("accepted") == 2
                                                              && indexedAccepted(stored) == 2 && entries.size() == 1);
    expect("the stored file keeps its old bytes", read_file(stored) == kept);

    blocked = false;
    expect("replacement companion", write_file(source.filePath("keys.csv"), "keyid,value\n02,12345\n"));
    const QVariantMap bundle = importAndWait(model, bundled, "bundled.csv", "chan", storedMap);
    expect("the index was blocked for the bundle's rollback", blocked && unblockIndex());
    expect("a bundle replace whose rollback cannot be saved answers save", bundle.value("error") == "save");
    const QStringList now = revisionsOf(root);
    QStringList added = now;
    for (const QString& revision : revisions) {
        added.removeAll(revision);
    }
    expect("the new revision is made and the old one kept", added.size() == 1 && now.size() == revisions.size() + 1);
    expect("the new revision stays journaled for the next start",
           added.size() == 1 && journal().contains(QDir::cleanPath(root + '/' + added.value(0))));
    expect("the bundle's primary keeps its old bytes", read_file(storedMap) == mapBytes);
    waitForWorker(service);
    while (model.count() > 0) {
        model.remove(0);
    }
}

/* A copy that ignores its cancel (a provider blocked in a read): Cancel is answered at once, exactly once; the copy
 * publishes nothing when it returns, and the next import waits behind it, then runs. */
void
test_cancel_does_not_wait_for_the_copy() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportService service(host.importTransport());
    dsd_qt::ImportedFilesModel model(&host, &service);
    QTemporaryDir source;
    const QString groups = source.filePath("county.csv");
    expect("stuck copy fixture", write_file(groups, "TG,Mode,Name\n101,D,Dispatch\n"));
    const QString reference = QStringLiteral("content://stuck/document/county.csv");
    host.documents.insert(reference, groups);
    StuckCopy stuck(host.shared);
    stuck.set(reference);

    QSignalSpy finished(&model, &dsd_qt::ImportedFilesModel::importFinished);
    const auto answers = [&finished](int id) {
        int count = 0;
        for (const auto& arguments : finished) {
            count += arguments.at(0).toInt() == id ? 1 : 0;
        }
        return count;
    };
    const int id = model.beginImport(reference, "county.csv", "group", {}, QString());
    expect("the copy is stuck", stuck.waitRunning());
    model.cancelImport(id);
    expect("a cancel is answered later, never inside the call", finished.isEmpty());
    expect("a cancelled import is no longer pending", !model.importing());
    const QVariantMap cancel = waitForImport(finished, id);
    expect("a stuck copy's cancel is answered at once", cancel.value("error") == "cancelled"
                                                            && cancel.value("type") == "group"
                                                            && cancel.value("name") == "county.csv");
    expect("while the copy is still stuck", stuck.running() && service.busy());
    model.cancelImport(id);

    const int next = model.beginImport(QUrl::fromLocalFile(groups).toString(), "county.csv", "group", {}, QString());
    finished.wait(200);
    expect("the next import waits behind the stuck copy", answers(next) == 0 && model.importing());
    stuck.release();
    const QVariantMap after = waitForImport(finished, next);
    expect("the next import runs once the copy returns", after.value("ok").toBool());
    waitForWorker(service);
    expect("the cancelled import is answered exactly once", answers(id) == 1);
    expect("the stuck copy publishes nothing when it returns", model.count() == 1 && storedFileCount() == 1);
    expect("its staging goes", stagingEmpty());
    model.remove(0);
}

/* Every answer documentInspected() gave probe @p id so far. */
QList<QVariantMap>
inspections(const QSignalSpy& spy, int id) {
    QList<QVariantMap> found;
    for (const auto& arguments : spy) {
        if (arguments.at(0).toInt() == id) {
            found.append(arguments.at(1).toMap());
        }
    }
    return found;
}

/* inspectDocument(@p reference)'s one answer, waited for. */
QVariantMap
inspectAndWait(dsd_qt::ImportedFilesModel& model, const QString& reference) {
    QSignalSpy spy(&model, &dsd_qt::ImportedFilesModel::documentInspected);
    const int id = model.inspectDocument(reference);
    expect("a probe has an id", id > 0);
    expect("a probe is answered later, never inside the call", spy.isEmpty());
    for (int waits = 0; waits < 200 && inspections(spy, id).isEmpty(); waits++) {
        spy.wait(50);
    }
    QCoreApplication::processEvents();
    const QList<QVariantMap> found = inspections(spy, id);
    expect("a probe is answered exactly once", found.size() == 1);
    return found.value(0);
}

/* A sealed backup of @p payload, with the cheapest parameters a backup may carry. */
QByteArray
sealedBackup(const QByteArray& payload) {
    dsd_backup_kdf_params kdf{};
    kdf.log2_n = 14;
    kdf.r = 8;
    kdf.p = 1;
    QByteArray sealed(static_cast<qsizetype>(dsd_backup_sealed_size(static_cast<size_t>(payload.size()))), '\0');
    size_t length = 0;
    const QByteArray pass("correct horse battery");
    const int status =
        dsd_backup_seal(reinterpret_cast<const uint8_t*>(pass.constData()), static_cast<size_t>(pass.size()), &kdf,
                        reinterpret_cast<const uint8_t*>(payload.constData()), static_cast<size_t>(payload.size()),
                        reinterpret_cast<uint8_t*>(sealed.data()), static_cast<size_t>(sealed.size()), &length);
    expect("a backup is sealed", status == DSD_BACKUP_OK && length == static_cast<size_t>(sealed.size()));
    return sealed;
}

/*
 * #831: a document opened from another app is told apart before anything is asked: a backup, a newer, damaged or
 * too large one, any other document, or one that cannot be read; with the provider's name. Probes are not imports.
 */
void
test_inspect_document() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportService service(host.importTransport());
    dsd_qt::ImportedFilesModel model(&host, &service);
    QSignalSpy importing(&model, &dsd_qt::ImportedFilesModel::importingChanged);
    QSignalSpy imported(&model, &dsd_qt::ImportedFilesModel::importFinished);
    QTemporaryDir dir;
    const QByteArray sealed = sealedBackup("DNBA payload");
    const auto document = [&dir, &host](const QString& name, const QByteArray& bytes) {
        const QString path = dir.filePath(name);
        expect("a probe fixture is written", write_file(path, bytes));
        const QString reference = QStringLiteral("content://provider/document/") + name;
        host.documents.insert(reference, path);
        return reference;
    };

    struct Case {
        QString name;
        QByteArray bytes;
        const char* kind;
    };

    QByteArray newer = sealed;
    newer[9] = 2; // the header's version, big-endian at 8
    const QList<Case> cases = {
        {QStringLiteral("app.dsdbak"), sealed, "backup"},
        {QStringLiteral("newer.dsdbak"), newer, "newerBackup"},
        {QStringLiteral("cut.dsdbak"), sealed.left(sealed.size() - 1), "damagedBackup"},
        {QStringLiteral("talkgroups.csv"), "TG,Mode,Name\n101,D,Dispatch\n", "other"},
        {QStringLiteral("empty.csv"), QByteArray(), "other"},
    };
    for (const Case& c : cases) {
        const QVariantMap answer = inspectAndWait(model, document(c.name, c.bytes));
        expect(c.kind, answer.value("kind").toString() == QLatin1String(c.kind) && answer.value("name") == c.name);
    }

    /* One byte over the cap a restore reads: the probe counts to the cap + 1 and calls it too large. */
    const QString bigPath = dir.filePath(QStringLiteral("big.dsdbak"));
    {
        QFile big(bigPath);
        expect("a large backup fixture", big.open(QIODevice::WriteOnly) && big.write(sealed.left(64)) == 64
                                             && big.resize(dsd_qt::backup::kMaxSealedBytes + 1));
    }
    host.documents.insert(QStringLiteral("content://provider/document/big"), bigPath);
    expect("a backup over the cap is too large",
           inspectAndWait(model, QStringLiteral("content://provider/document/big")).value("kind") == "tooLargeBackup");
    QFile::remove(bigPath);

    const QVariantMap missing = inspectAndWait(model, QStringLiteral("content://provider/document/gone"));
    expect("a document that cannot be read is unreadable", missing.value("kind") == "unreadable");

    expect("a probe is never an import", importing.isEmpty() && !model.importing() && imported.isEmpty());
    waitForWorker(service);
    expect("probes leave nothing behind", stagingEmpty() && storedFileCount() == 0 && model.count() == 0);

    /* With no import worker every probe is unreadable, answered later as any other. */
    dsd_qt::ImportedFilesModel orphan(&host, static_cast<dsd_qt::ImportService*>(nullptr));
    expect("a probe without a worker is unreadable",
           inspectAndWait(orphan, QStringLiteral("content://provider/document/app.dsdbak")).value("kind")
               == "unreadable");
}

/*
 * #831: Cancel is answered at once, exactly once, without waiting for a read a provider holds; the read's late result
 * is dropped. A cancel never reaches an import, nor an import's cancel a probe. A model destroyed with a probe in
 * flight tells the read to stop and answers nothing.
 */
void
test_cancel_inspect() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportService service(host.importTransport());
    auto model = std::make_unique<dsd_qt::ImportedFilesModel>(&host, &service);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("app.dsdbak"));
    expect("a stuck probe fixture", write_file(path, sealedBackup("DNBA payload")));
    const QString reference = QStringLiteral("content://stuck/document/app.dsdbak");
    host.documents.insert(reference, path);
    StuckCopy stuck(host.shared);
    stuck.set(reference);

    QSignalSpy inspected(model.get(), &dsd_qt::ImportedFilesModel::documentInspected);
    QSignalSpy imported(model.get(), &dsd_qt::ImportedFilesModel::importFinished);
    const int id = model->inspectDocument(reference);
    expect("the probe is stuck", stuck.waitRunning());
    model->cancelImport(id);
    QCoreApplication::processEvents();
    expect("an import's cancel never reaches a probe", inspected.isEmpty() && imported.isEmpty());
    model->cancelInspect(id);
    expect("a cancel is answered later, never inside the call", inspected.isEmpty());
    for (int waits = 0; waits < 100 && inspections(inspected, id).isEmpty(); waits++) {
        inspected.wait(50);
    }
    expect("a stuck probe's cancel is answered at once",
           inspections(inspected, id).size() == 1 && inspections(inspected, id).value(0).value("kind") == "cancelled"
               && stuck.running());
    expect("and reaches the read", host.shared->cancels.contains(id));
    model->cancelInspect(id);
    stuck.release();
    waitForWorker(service);
    expect("the probe is answered exactly once", inspections(inspected, id).size() == 1);

    /* Destroyed mid-probe: the read is told to stop, and its result reaches no one. */
    stuck.set(reference);
    QSignalSpy late(model.get(), &dsd_qt::ImportedFilesModel::documentInspected);
    const int orphaned = model->inspectDocument(reference);
    expect("the second probe is stuck", stuck.waitRunning());
    model.reset();
    expect("a model destroyed mid-probe cancels its read", host.shared->cancels.contains(orphaned));
    stuck.release();
    waitForWorker(service);
    expect("and answers nothing", late.isEmpty());
}

/* A companion the map names only after the first answer: the import asks for it, and a retry with it succeeds. */
void
test_async_companion_retry() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const QString channels = source.filePath("channels.csv");
    const QString keys = source.filePath("chosen.csv");
    expect("retry fixtures", write_file(channels, "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,keys.csv\n")
                                 && write_file(keys, "id,value\n1,12345\n"));
    host.documents.insert("content://channels", channels);
    const QVariantMap asked = importAndWait(model, "content://channels", "Map.csv", "chan");
    const QString slot = asked.value("required").toStringList().value(0);
    expect("a document with no sibling access asks for its companion",
           asked.value("error") == "companions" && !slot.isEmpty()
               && asked.value("requiredDetails").toMap().value(slot).toMap().contains("candidates"));
    expect("asking adds nothing", model.count() == 0 && revisionsOf(importsDirPath() + "/bundles").isEmpty());
    const QVariantMap retried =
        importAndWait(model, "content://channels", "Map.csv", "chan", QString(), {{slot, keys}});
    expect("the retry with the companion imports",
           retried.value("ok").toBool() && model.count() == 1
               && read_file(firstCompanion(retried.value("path").toString(), 3)) == read_file(keys));
    model.remove(0);
}

/* A target list whose channel map names its own companion: both land in the target list's one revision. */
void
test_async_nested_target_bundle() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportService service(host.importTransport());
    dsd_qt::ImportedFilesModel model(&host, &service);
    QTemporaryDir source;
    const QString targets = source.filePath("targets.csv");
    expect(
        "nested fixtures",
        write_file(source.filePath("key.csv"), "id,value\n01,ABCDEF0123\n")
            && write_file(source.filePath("map.csv"), "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,key.csv\n")
            && write_file(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                                   "one,p25-trunk,851012500,map.csv,250,,\n"));
    QVariantMap result = importAndWait(model, targets, "Mine.csv", "trunkTargets");
    const QString path = result.value("path").toString();
    const QString root = model.get(model.rowForPath(path)).value("bundleRoot").toString();
    expect("a nested target bundle imports on the worker",
           result.value("ok").toBool() && result.value("accepted") == 1 && QFileInfo(path).fileName() == "targets.csv");
    expect("the target row keeps the document's name",
           model.get(model.rowForPath(path)).value("name") == "targets.csv");
    const QStringList revisions = revisionsOf(root);
    expect("the map and its companion share the one revision",
           revisions.size() == 1 && QDir(root + '/' + revisions.value(0)).entryList(QDir::Files).size() == 2);
    expect("the stored bundle validates on its own",
           dsd_app_trunk_scan_validate_bundle(path.toUtf8().constData(), nullptr, nullptr, nullptr, 0) == 0);
    result = importAndWait(model, targets, "Mine.csv", "trunkTargets", path);
    waitForWorker(service);
    expect("a replaced target revision is reclaimed",
           result.value("ok").toBool() && revisionsOf(root).size() == 1 && revisionsOf(root) != revisions);
    model.remove(model.rowForPath(path));
}

/* Each copy of a bundle is at most 32 MiB and all of them together at most 64 MiB; a failed copy is a read failure,
   and never imports the part of the document it did read (B9). */
void
test_async_bundle_limits_and_failures() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const auto sparse = [&source](const QString& name, qint64 size) {
        QFile file(source.filePath(name));
        expect("sparse companion", file.open(QIODevice::WriteOnly) && file.resize(size));
    };
    sparse("a.csv", 30LL * 1024 * 1024);
    sparse("b.csv", 30LL * 1024 * 1024);
    sparse("c.csv", 30LL * 1024 * 1024);
    sparse("huge.csv", 32LL * 1024 * 1024 + 1);
    const QString three = source.filePath("three.csv");
    const QString huge = source.filePath("huge-map.csv");
    expect("limit fixtures",
           write_file(three, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,a.csv\n"
                             "2,851025000,p25,b.csv\n3,851037500,p25,c.csv\n")
               && write_file(huge, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,huge.csv\n"));
    QVariantMap result = importAndWait(model, three, "three.csv", "chan");
    expect("a bundle over 64 MiB in all is too large, with the bundle's limit",
           result.value("error") == "too_large" && result.value("limitBytes").toLongLong() == 64LL * 1024 * 1024);
    result = importAndWait(model, huge, "huge-map.csv", "chan");
    expect("a companion over 32 MiB is too large, with the file limit and its name",
           result.value("error") == "too_large" && result.value("limitBytes").toLongLong() == 32LL * 1024 * 1024
               && result.value("name") == "huge.csv");
    // Only 4 MiB of the bundle's budget is left when huge.csv comes, but it is over the file limit on its own.
    const QString late = source.filePath("late-huge.csv");
    expect("late huge fixture", write_file(late, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,a.csv\n"
                                                 "2,851025000,p25,b.csv\n3,851037500,p25,huge.csv\n"));
    result = importAndWait(model, late, "late-huge.csv", "chan");
    expect("a companion over 32 MiB is too large with the file limit, whatever the bundle has left",
           result.value("error") == "too_large" && result.value("limitBytes").toLongLong() == 32LL * 1024 * 1024
               && result.value("name") == "huge.csv");

    const QString keyed = source.filePath("keyed.csv");
    expect("read fixture", write_file(keyed, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,gone.csv\n"));
    result =
        importAndWait(model, keyed, "keyed.csv", "chan", QString(), {{"gone.csv", source.filePath("missing.csv")}});
    expect("a companion that cannot be read is a read failure", result.value("error") == "read");

    // The transport writes the valid first half of the map, then fails as a read error does.
    const QString longMap = source.filePath("long.csv");
    QByteArray rows("channel,frequency,mode\n");
    for (int i = 1; i <= 40; i++) {
        rows += QByteArray::number(i) + ",851012500,p25\n";
    }
    expect("partial read fixture", write_file(longMap, rows));
    {
        const std::lock_guard<std::mutex> guard(host.shared->lock);
        host.shared->failHalfway = QUrl::fromLocalFile(longMap).toString();
    }
    result = importAndWait(model, longMap, "long.csv", "chan");
    expect("a read that fails part way is a read failure, never its valid part (B9)",
           result.value("error") == "read" && !result.value("ok").toBool());
    {
        const std::lock_guard<std::mutex> guard(host.shared->lock);
        host.shared->failHalfway.clear();
    }
    expect("no failed bundle adds a row or a file",
           model.count() == 0 && libraryHoldsOnly({}) && revisionsOf(importsDirPath() + "/bundles").isEmpty());
}

/* A target list whose rewrite cannot write its destination is this device's failure, not a format error (B9). */
void
test_async_target_write_failure() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const QString targets = source.filePath("targets.csv");
    expect("write failure fixture",
           write_file(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                               "one,dmr-conventional,461000000,,,,\n"));
    {
        const std::lock_guard<std::mutex> guard(host.shared->lock);
        host.shared->blockPrimary = QUrl::fromLocalFile(targets).toString();
    }
    const QVariantMap result = importAndWait(model, targets, "targets.csv", "trunkTargets");
    expect("a rewrite that cannot write its destination is local, not format",
           result.value("error") == "local" && model.count() == 0);
}

/* Startup cleanup (B5): a channel map's revisions its primary no longer reaches go, unless a typed path field names a
   file in one or the decoder runs; one it reaches stays. */
void
test_startup_revision_cleanup() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    QTemporaryDir source;
    const QString bundled = source.filePath("bundled.csv");
    expect("startup fixtures",
           write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
               && write_file(bundled, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,keys.csv\n"));
    // One worker for every library this test opens.
    dsd_qt::ImportService service(host.importTransport());
    QString root;
    QString reached;
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        const QString stored = importAndWait(model, bundled, "bundled.csv", "chan").value("path").toString();
        root = model.get(model.rowForPath(stored)).value("bundleRoot").toString();
        reached = revisionsOf(root).value(0);
    }
    const QString orphan = root + "/33333333-3333-3333-3333-333333333333";
    const QString named = root + "/44444444-4444-4444-4444-444444444444";
    expect("orphan revisions", QDir().mkpath(orphan) && QDir().mkpath(named) && write_file(orphan + "/0.csv", "x\n")
                                   && write_file(named + "/0.csv", "id,value\n1,1\n"));
    // A saved system names a file in one of them.
    expect(
        "a typed path names a revision file",
        dsd_qt::json_store_save_array("saved_systems.json", QJsonArray{QJsonObject{{"keyCsvPath", named + "/0.csv"}}}));

    host.running = true;
    {
        const dsd_qt::ImportedFilesModel model(&host, &service);
        waitForWorker(service);
        expect("nothing is reclaimed while the decoder runs", QFileInfo::exists(orphan));
    }
    host.running = false;
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        waitForWorker(service);
        expect("an unreached channel revision is reclaimed at startup (B5)", !QFileInfo::exists(orphan));
        expect("the revision the map reaches stays", QFileInfo::exists(root + '/' + reached));
        expect("a revision holding a file a saved system names stays", QFileInfo::exists(named + "/0.csv"));
        model.remove(0);
    }
    QFile::remove(dsd_qt::json_store_path("saved_systems.json"));
}

/* Gate r3: with the terminal output in ASCII (DSD_FORCE_ASCII) and app data under a non-ASCII folder, a parser's
   reason still names the file by the name the user picked it by, never by its altered staging path. */
void
test_reason_under_ascii_logging() {
    const QString name = QCoreApplication::applicationName();
    QCoreApplication::setApplicationName(name + QString::fromUtf8("-caf\xC3\xA9"));
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    expect("force ASCII logging", dsd_env_set(DSD_ENV_LAYER_HOST, "DSD_FORCE_ASCII", "1") == 0);
    {
        TestHost host;
        dsd_qt::ImportedFilesModel model(&host);
        const QTemporaryDir source;
        const QString badMode = source.filePath("bad-mode.csv");
        expect("ASCII reason fixture", write_file(badMode, "channel,frequency,mode\n1,851012500,warp\n"));
        const QString why = importAndWait(model, badMode, "bad-mode.csv", "chan").value("detail").toString();
        expect("an ASCII log still maps the reason to the picked name",
               why.startsWith("Channel map file 'bad-mode.csv' row 2: invalid mode")
                   && !why.contains("import-staging"));
    }
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
    QDir(dataDir).removeRecursively();
    QCoreApplication::setApplicationName(name);
}

/* A model built while the cached phase still reads Idle but the host's live answer says a decoder starts or runs (an
   Android UI recreated beside its running service): neither the revision cleanup nor journal recovery deletes
   anything, and the journal keeps its entries until a start that finds the decoder stopped. */
void
test_startup_cleanup_asks_the_live_engine() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    const QTemporaryDir source;
    const QString bundled = source.filePath("bundled.csv");
    expect("live-engine fixtures",
           write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
               && write_file(bundled, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,keys.csv\n"));
    dsd_qt::ImportService service(host.importTransport());
    QString root;
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        const QString stored = importAndWait(model, bundled, "bundled.csv", "chan").value("path").toString();
        root = model.get(model.rowForPath(stored)).value("bundleRoot").toString();
    }
    const QString orphan = root + "/99999999-9999-9999-9999-999999999999";
    const QString unsaved = importsDirPath() + "/bundles/aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    expect("live-engine leftovers", QDir().mkpath(orphan) && write_file(orphan + "/0.csv", "x\n")
                                        && QDir().mkpath(unsaved) && write_file(unsaved + "/0.csv", "x\n")
                                        && dsd_qt::ImportService::journalAdd({unsaved}));

    host.running = false;
    host.engine = true;
    {
        const dsd_qt::ImportedFilesModel model(&host, &service);
        waitForWorker(service);
        expect("the startup cleanup asks the live engine, not the cached phase", QFileInfo::exists(orphan + "/0.csv"));
        expect("journal recovery asks the live engine too", QFileInfo::exists(unsaved + "/0.csv"));
        expect("the journal keeps its entries for the next start", journal() == QStringList{unsaved});
    }
    host.engine = false;
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        waitForWorker(service);
        expect("a start that finds the decoder stopped reclaims the revision", !QFileInfo::exists(orphan));
        expect("and settles the journal", !QFileInfo::exists(unsaved) && journal().isEmpty());
        model.remove(0);
    }
}

/* The commit journal after a crash: the next load removes the journaled directories the index does not reference and
   keeps the ones it does; a journal that cannot be read deletes nothing. */
void
test_journal_recovery() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    QTemporaryDir source;
    const QString bundled = source.filePath("bundled.csv");
    expect("journal fixtures",
           write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
               && write_file(bundled, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,keys.csv\n"));
    // One worker for every library this test opens.
    dsd_qt::ImportService service(host.importTransport());
    QString stored;
    QString root;
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        stored = importAndWait(model, bundled, "bundled.csv", "chan").value("path").toString();
        root = model.get(model.rowForPath(stored)).value("bundleRoot").toString();
    }
    const QString reached = root + '/' + revisionsOf(root).value(0);
    const QString bundles = importsDirPath() + "/bundles";
    // Stopped after the journal entry of a new bundle: its root is filled but no row names it.
    const QString unsaved = bundles + "/55555555-5555-5555-5555-555555555555";
    // Stopped after the index save of a replace: the root is the row's, the revision not yet what the map reaches.
    const QString unpublished = root + "/66666666-6666-6666-6666-666666666666";
    // Not provably the app's: outside bundles/, and a name a commit never makes.
    const QString outside = importsDirPath() + "/kept-folder";
    const QString oddName = bundles + "/not.an.owned.name";
    for (const QString& dir : {unsaved + "/77777777-7777-7777-7777-777777777777", unpublished, outside, oddName}) {
        expect("journaled directory", QDir().mkpath(dir) && write_file(dir + "/0.csv", "x\n"));
    }
    expect("journal the crash's directories",
           dsd_qt::ImportService::journalAdd({unsaved, unpublished, root, reached, outside, oddName}));
    {
        const dsd_qt::ImportedFilesModel model(&host, &service);
        expect("a journaled root no row names is removed", !QFileInfo::exists(unsaved));
        expect("a journaled revision the primary does not reach is removed", !QFileInfo::exists(unpublished));
        expect("a journaled root a row names is kept", QFileInfo::exists(stored) && QFileInfo::exists(root));
        expect("a journaled revision the primary reaches is kept", QFileInfo::exists(reached + "/0.csv"));
        expect("a journaled directory the app cannot prove its own is kept",
               QFileInfo::exists(outside + "/0.csv") && QFileInfo::exists(oddName + "/0.csv"));
        expect("the journal is settled", journal().isEmpty());
    }

    // A journal that cannot be read proves nothing: nothing goes, it is set aside, and a fresh one starts.
    const QString again = bundles + "/88888888-8888-8888-8888-888888888888";
    expect("unproven directory", QDir().mkpath(again) && write_file(again + "/0.csv", "x\n"));
    const QByteArray damaged = "[\"" + again.toUtf8() + "\",";
    expect("damage the journal", write_file(dsd_qt::ImportService::journalPath(), damaged));
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        expect("an unreadable journal deletes nothing", QFileInfo::exists(again + "/0.csv"));
        expect("an unreadable journal is set aside",
               read_file(dsd_qt::ImportService::journalPath() + ".bad") == damaged && journal().isEmpty());
        expect("later commits journal again", dsd_qt::ImportService::journalAdd({again})
                                                  && journal() == QStringList{again}
                                                  && dsd_qt::ImportService::journalRemove({again}));
        model.remove(model.rowForPath(stored));
    }
    QDir(outside).removeRecursively();
    QDir(oddName).removeRecursively();
    QDir(again).removeRecursively();
}

/* A journal entry that outlived its replace (its removal failed) names a revision a later replace left behind, and a
   saved system names a file in it: recovery keeps it, as the revision cleanup does. A store that cannot be read
   deletes nothing and keeps the journal for the next start. */
void
test_journal_recovery_keeps_named_revisions() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    const QTemporaryDir source;
    const QString bundled = source.filePath("bundled.csv");
    expect("named journal fixtures",
           write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
               && write_file(bundled, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,keys.csv\n"));
    dsd_qt::ImportService service(host.importTransport());
    QString stored;
    QString root;
    QString older;
    QString keys;
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        stored = importAndWait(model, bundled, "bundled.csv", "chan").value("path").toString();
        root = model.get(model.rowForPath(stored)).value("bundleRoot").toString();
        older = root + '/' + revisionsOf(root).value(0);
        keys = firstCompanion(stored, 3);
        waitForWorker(service);
        // The commit's journal entry could not be removed; a saved system then picks the stored key list.
        expect("the replaced revision's entry outlives its commit", dsd_qt::ImportService::journalAdd({older}));
        expect("a saved system names the older revision's key list",
               dsd_qt::json_store_save_array("saved_systems.json",
                                             QJsonArray{QJsonObject{{"uid", "s1"}, {"keyCsvPath", keys}}}));
        expect("later replace",
               write_file(source.filePath("keys.csv"), "keyid,value\n02,12345\n")
                   && importAndWait(model, bundled, "bundled.csv", "chan", stored).value("ok").toBool());
        waitForWorker(service);
        expect("the map now reaches a new revision", !firstCompanion(stored, 3).startsWith(older + '/'));
    }
    const QString unnamed = root + "/99999999-9999-9999-9999-999999999999";
    expect("an unnamed journaled revision", QDir().mkpath(unnamed) && write_file(unnamed + "/0.csv", "x\n")
                                                && dsd_qt::ImportService::journalAdd({unnamed}));
    {
        const dsd_qt::ImportedFilesModel model(&host, &service);
        expect("recovery keeps a journaled revision a saved system names a file in", QFileInfo::exists(keys));
        expect("recovery still removes a journaled revision nothing names", !QFileInfo::exists(unnamed));
        expect("the journal is settled", journal().isEmpty());
        // Its startup cleanup lists the root's revisions: it must finish before the next fixture puts one back.
        waitForWorker(service);
    }

    // An unreadable saved_systems.json may name anything: recovery deletes nothing and keeps the entries.
    expect("an unnamed journaled revision again", QDir().mkpath(unnamed) && write_file(unnamed + "/0.csv", "x\n")
                                                      && dsd_qt::ImportService::journalAdd({unnamed}));
    expect("damage saved_systems.json", write_file(dsd_qt::json_store_path("saved_systems.json"), "{not json"));
    {
        const dsd_qt::ImportedFilesModel model(&host, &service);
        expect("an unreadable store deletes nothing", QFileInfo::exists(unnamed + "/0.csv") && QFileInfo::exists(keys));
        expect("an unreadable store keeps the journal for the next start", journal() == QStringList{unnamed});
        waitForWorker(service);
    }
    QFile::remove(dsd_qt::json_store_path("saved_systems.json"));
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        expect("a readable store settles the kept entry", !QFileInfo::exists(unnamed) && journal().isEmpty());
        model.remove(model.rowForPath(stored));
        waitForWorker(service);
    }
}

/* Whether the channel map @p map, a plain stored row, still loads, and the hex key list its first row names by -K
   still exists and validates. */
bool
plainMapValidates(const QString& map, const QString& keys) {
    dsd_csv_validation stats{};
    return dsd_csv_validate_chan_file(map.toUtf8().constData(), &stats) == 0 && QFileInfo::exists(keys)
           && dsd_csv_validate_key_file_hex(keys.toUtf8().constData(), &stats) == 0;
}

/* Another library row's CSV may need a file in a bundle's older revision (a library an older app version wrote, or a
   restore): a plain map's option cell, and a target list's channel map, name bundle A's first key list. The cleanup
   after A's replace, the startup cleanup and journal recovery all keep that revision while a row needs it, and still
   remove what nothing needs; a stored CSV that cannot be parsed deletes nothing. */
void
test_cleanup_keeps_revisions_other_rows_reach() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    const QTemporaryDir source;
    const QString bundled = source.filePath("bundled.csv");
    const QString plain = source.filePath("plain.csv");
    const QString targets = source.filePath("targets.csv");
    expect(
        "cross-row fixtures",
        write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
            && write_file(bundled, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,keys.csv\n")
            && write_file(plain, "channel,frequency,mode\n1,851012500,p25\n")
            && write_file(source.filePath("key.csv"), "id,value\n01,ABCDEF0123\n")
            && write_file(source.filePath("map.csv"), "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,key.csv\n")
            && write_file(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                                   "one,p25-trunk,851012500,map.csv,250,,\n"));
    dsd_qt::ImportService service(host.importTransport());
    QString a;
    QString b;
    QString t;
    QString root;
    QString keys;
    QByteArray mapB;
    const auto targetValidates = [&t] {
        return dsd_app_trunk_scan_validate_bundle(t.toUtf8().constData(), nullptr, nullptr, nullptr, 0) == 0;
    };
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        a = importAndWait(model, bundled, "bundled.csv", "chan").value("path").toString();
        b = importAndWait(model, plain, "plain.csv", "chan").value("path").toString();
        t = importAndWait(model, targets, "targets.csv", "trunkTargets").value("path").toString();
        waitForWorker(service);
        root = model.get(model.rowForPath(a)).value("bundleRoot").toString();
        keys = firstCompanion(a, 3);
        const QString nested = firstCompanion(t, 3);
        // B's option cell and T's channel map name A's key list where it is stored now.
        mapB = "channel,frequency,mode,options\n1,851012500,p25,-K "
               + QFileInfo(b).dir().relativeFilePath(keys).toUtf8() + "\n";
        expect("other rows name A's key list",
               !root.isEmpty() && keys.startsWith(root + '/') && write_file(b, mapB)
                   && write_file(nested, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,"
                                             + QFileInfo(nested).dir().relativeFilePath(keys).toUtf8() + "\n"));
        expect("both rows validate", plainMapValidates(b, keys) && targetValidates());
        // While the decoder runs a replace reclaims nothing, so the startup cleanup below meets A's older revision.
        write_file(source.filePath("keys.csv"), "keyid,value\n02,12345\n");
        host.running = true;
        expect("A is replaced while the decoder runs",
               importAndWait(model, bundled, "bundled.csv", "chan", a).value("ok").toBool());
        host.running = false;
        waitForWorker(service);
        expect("A's map now reaches a new revision", !firstCompanion(a, 3).startsWith(QFileInfo(keys).absolutePath()));
    }
    const QString older = QFileInfo(keys).absolutePath();
    const QString orphan = root + "/33333333-3333-3333-3333-333333333333";
    expect("an orphan revision", QDir().mkpath(orphan) && write_file(orphan + "/0.csv", "x\n"));
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        waitForWorker(service);
        expect("the startup cleanup removes a revision nothing needs", !QFileInfo::exists(orphan));
        expect("the startup cleanup keeps a revision other rows need", plainMapValidates(b, keys) && targetValidates());
        const QString middle = QFileInfo(firstCompanion(a, 3)).absolutePath();
        expect("A is replaced again", importAndWait(model, bundled, "bundled.csv", "chan", a).value("ok").toBool());
        waitForWorker(service);
        expect("the cleanup after a replace removes the revision A left", !QFileInfo::exists(middle));
        expect("the cleanup after a replace keeps a revision other rows need",
               plainMapValidates(b, keys) && targetValidates());
    }

    // Journal recovery: the older revision's entry outlived its commit, beside one nothing needs.
    const QString unnamed = root + "/99999999-9999-9999-9999-999999999999";
    expect("journaled revisions", QDir().mkpath(unnamed) && write_file(unnamed + "/0.csv", "x\n")
                                      && dsd_qt::ImportService::journalAdd({older, unnamed}));
    {
        const dsd_qt::ImportedFilesModel model(&host, &service);
        expect("recovery keeps a journaled revision other rows need", plainMapValidates(b, keys) && targetValidates());
        expect("recovery removes a journaled revision nothing needs", !QFileInfo::exists(unnamed));
        expect("recovery settles the journal", journal().isEmpty());
        waitForWorker(service);
    }

    // A stored CSV that cannot be parsed (a repeated column) may need anything: neither recovery nor a cleanup deletes
    // a thing.
    expect("unparsed fixtures", QDir().mkpath(orphan) && write_file(orphan + "/0.csv", "x\n") && QDir().mkpath(unnamed)
                                    && write_file(unnamed + "/0.csv", "x\n")
                                    && dsd_qt::ImportService::journalAdd({unnamed})
                                    && write_file(b, "channel,frequency,mode,options,options\n"));
    {
        const dsd_qt::ImportedFilesModel model(&host, &service);
        expect("an unparsed row keeps the journal for the next start", journal() == QStringList{unnamed});
        waitForWorker(service);
        expect("an unparsed row deletes nothing",
               QFileInfo::exists(orphan + "/0.csv") && QFileInfo::exists(unnamed + "/0.csv") && targetValidates());
    }
    expect("B parses again", write_file(b, mapB));
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        waitForWorker(service);
        expect("once every row parses, what nothing needs goes",
               !QFileInfo::exists(orphan) && !QFileInfo::exists(unnamed) && journal().isEmpty());
        expect("and what other rows need stays", plainMapValidates(b, keys) && targetValidates());
        model.remove(model.rowForPath(t));
        model.remove(model.rowForPath(b));
        model.remove(model.rowForPath(a));
        waitForWorker(service);
    }
}

#ifndef _WIN32
/* A reference spelled through a link to the library (as /data/data is to /data/user/0 on Android) still keeps its file:
   a stored map's option cell and a saved system's key list name A's revisions through a symlinked alias of imports/.
   The cleanup after a replace, the startup cleanup and journal recovery compare canonical paths. */
void
test_cleanup_matches_linked_spellings() {
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(data).removeRecursively();
    TestHost host;
    const QTemporaryDir source;
    const QString bundled = source.filePath("bundled.csv");
    const QString plain = source.filePath("plain.csv");
    expect("linked fixtures",
           write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
               && write_file(bundled, "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,keys.csv\n")
               && write_file(plain, "channel,frequency,mode\n1,851012500,p25\n"));
    dsd_qt::ImportService service(host.importTransport());
    const QString alias = data + "/linked-imports";
    QString a;
    QString b;
    QString linkedKeys;
    QString orphan;
    QString named;
    QString older;
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        a = importAndWait(model, bundled, "bundled.csv", "chan").value("path").toString();
        b = importAndWait(model, plain, "plain.csv", "chan").value("path").toString();
        waitForWorker(service);
        const QString root = model.get(model.rowForPath(a)).value("bundleRoot").toString();
        const QString keys = firstCompanion(a, 3);
        older = QFileInfo(keys).absolutePath();
        orphan = root + "/33333333-3333-3333-3333-333333333333";
        named = root + "/44444444-4444-4444-4444-444444444444";
        const auto linked = [&alias](const QString& path) { return alias + path.mid(importsDirPath().size()); };
        linkedKeys = linked(keys);
        expect("a linked alias of the library", keys.startsWith(importsDirPath() + '/')
                                                    && QFile::link(importsDirPath(), alias)
                                                    && QFileInfo::exists(linkedKeys));
        expect("B and a saved system name A's revisions through the alias",
               write_file(b, "channel,frequency,mode,options\n1,851012500,p25,-K " + linkedKeys.toUtf8() + "\n")
                   && QDir().mkpath(orphan) && write_file(orphan + "/0.csv", "x\n") && QDir().mkpath(named)
                   && write_file(named + "/0.csv", "id,value\n1,1\n")
                   && dsd_qt::json_store_save_array("saved_systems.json",
                                                    QJsonArray{QJsonObject{{"keyCsvPath", linked(named + "/0.csv")}}}));
        write_file(source.filePath("keys.csv"), "keyid,value\n02,12345\n");
        expect("A is replaced", importAndWait(model, bundled, "bundled.csv", "chan", a).value("ok").toBool());
        waitForWorker(service);
        expect("the cleanup after a replace removes a revision nothing needs", !QFileInfo::exists(orphan));
        expect("the cleanup after a replace keeps what a linked spelling names",
               plainMapValidates(b, linkedKeys) && QFileInfo::exists(named + "/0.csv"));
    }
    {
        const dsd_qt::ImportedFilesModel model(&host, &service);
        waitForWorker(service);
        expect("the startup cleanup keeps what a linked spelling names",
               plainMapValidates(b, linkedKeys) && QFileInfo::exists(named + "/0.csv"));
    }
    expect("journaled revisions named through the alias", dsd_qt::ImportService::journalAdd({older, named}));
    {
        dsd_qt::ImportedFilesModel model(&host, &service);
        expect("recovery keeps what a linked spelling names",
               plainMapValidates(b, linkedKeys) && QFileInfo::exists(named + "/0.csv") && journal().isEmpty());
        waitForWorker(service);
        model.remove(model.rowForPath(b));
        model.remove(model.rowForPath(a));
        waitForWorker(service);
    }
    QFile::remove(dsd_qt::json_store_path("saved_systems.json"));
    QFile::remove(alias);
}
#endif

} // namespace

/* Issue #526: the channel-map review carries an analog row's own channel width in Hz -- an nfm row's
   --nfm-bandwidth-hz, an am row's --am-bandwidth-hz -- and nothing for a row that inherits the configured width or a
   digital row. */
static void
test_channel_review_bandwidth() {
    QTemporaryDir source;
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QFile file(source.filePath("nfm.csv"));
    const QByteArray bytes =
        "channel,frequency_hz,mode,options\n1,154430000,nfm,--nfm-bandwidth-hz 12500\n"
        "2,155100000,nfm,--squelch auto+8\n3,461000000,dmr,\n4,118300000,am,--am-bandwidth-hz 8333\n"
        "5,121500000,AM,\n6,154800000,nfm,--squelch noise+11\n";
    expect("write nfm map", file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
    file.close();
    const auto result = importAndWait(model, source.filePath("nfm.csv"), "Nfm.csv", "chan");
    expect("nfm map imports", result.value("ok").toBool());
    const int row = model.rowForPath(result.value("path").toString());
    if (row < 0) {
        return;
    }
    const auto review = model.channelProfiles(row);
    const auto rows = review.value("rows").toList();
    expect("nfm map review", review.value("ok").toBool() && rows.size() == 6);
    if (rows.size() == 6) {
        expect("reports the row's own width", rows[0].toMap().value("bandwidthHz").toInt() == 12500);
        expect("an inheriting nfm row carries none", !rows[1].toMap().value("bandwidthHz").isValid());
        expect("a digital row carries none", !rows[2].toMap().value("bandwidthHz").isValid());
        expect("the nfm row names its mode", rows[0].toMap().value("mode") == "nfm");
        expect("reports the am row's own width",
               rows[3].toMap().value("bandwidthHz").toInt() == 8333 && rows[3].toMap().value("mode") == "am");
        expect("an inheriting am row carries none, in the canonical spelling",
               !rows[4].toMap().value("bandwidthHz").isValid() && rows[4].toMap().value("mode") == "am");
        /* Issue #518 follow-up: a row's own auto squelch reaches the review as its margin; a level carries none. */
        expect("reports the row's own auto squelch",
               rows[1].toMap().value("squelchMarginDb").toInt() == 8 && rows[1].toMap().value("squelchDb").isValid());
        expect("a row without an auto squelch carries no margin", !rows[0].toMap().value("squelchMarginDb").isValid());
        expect("an auto squelch is not the noise squelch", !rows[1].toMap().value("squelchNoise").toBool());
        expect("reports the row's own noise squelch", rows[5].toMap().value("squelchMarginDb").toInt() == 11
                                                          && rows[5].toMap().value("squelchNoise").toBool());
    }
    model.remove(row);
}

/* Issue #527: the channel-map review carries an nfm row's own tone policy (its dsd_tone_filter_mode and its list as
   displayed), and nothing for a row that runs the configured policy -- an am row, which cannot set one, included. */
static void
test_channel_review_tone_filter() {
    QTemporaryDir source;
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QFile file(source.filePath("tone.csv"));
    const QByteArray bytes = "channel,frequency_hz,mode,options\n1,154430000,nfm,--tone-allow 100.0/D023N\n"
                             "2,155475000,nfm,--tone-block=67\n3,155520000,nfm,--no-tone-filter\n"
                             "4,155535000,nfm,\n5,118300000,am,\n6,461000000,dmr,\n";
    expect("write tone map", file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
    file.close();
    const auto result = importAndWait(model, source.filePath("tone.csv"), "Tone.csv", "chan");
    expect("tone map imports", result.value("ok").toBool());
    const int row = model.rowForPath(result.value("path").toString());
    if (row < 0) {
        return;
    }
    const auto review = model.channelProfiles(row);
    const auto rows = review.value("rows").toList();
    expect("tone map review", review.value("ok").toBool() && rows.size() == 6);
    if (rows.size() == 6) {
        expect("reports the row's allow list", rows[0].toMap().value("toneFilter").toInt() == DSD_TONE_FILTER_ALLOW
                                                   && rows[0].toMap().value("toneList") == "100.0 Hz/D023N");
        expect("reports the row's block list", rows[1].toMap().value("toneFilter").toInt() == DSD_TONE_FILTER_BLOCK
                                                   && rows[1].toMap().value("toneList") == "67.0 Hz");
        expect("reports the row's own off", rows[2].toMap().value("toneFilter").isValid()
                                                && rows[2].toMap().value("toneFilter").toInt() == DSD_TONE_FILTER_OFF
                                                && rows[2].toMap().value("toneList").toString().isEmpty());
        for (int i = 3; i < 6; ++i) {
            expect("a row without its own policy carries none",
                   !rows[i].toMap().value("toneFilter").isValid()
                       && rows[i].toMap().value("toneList").toString().isEmpty());
        }
    }
    model.remove(row);
}

static void
test_channel_bundle() {
    QTemporaryDir source;
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QFile file(source.filePath(name));
        expect("write bundle fixture", file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
        return source.filePath(name);
    };
    const QString keys = write("keys.csv", "keyid,value\n02,ABCDE\n");
    const QString map = write("map.csv", "tg_dec,keyid_hex\n123,02\n");
    const QString channels =
        write("channels.csv",
              "channel,frequency_hz,mode,keys_hex_csv,options\n1,461000000,dmr,keys.csv,--dmr-tg-key-csv 'map.csv' "
              "--squelch-db -60\n");
    auto result = importAndWait(model, channels, "Bundle.csv", "chan");
    expect("bundle imports with local companions", result.value("ok").toBool());
    const QString stored = result.value("path").toString();
    const int row = model.rowForPath(stored);
    expect("bundle registered", row >= 0);
    if (row < 0) {
        return;
    }
    const QString root = model.get(row).value("bundleRoot").toString();
    expect("bundle has private ownership", !root.isEmpty());
    const auto review = model.channelProfiles(row);
    const auto profileRows = review.value("rows").toList();
    expect("bundle row review succeeds", review.value("ok").toBool() && profileRows.size() == 1);
    if (!profileRows.isEmpty()) {
        const auto profile = profileRows.first().toMap();
        expect("row review identifies collection and mapping",
               profile.value("keySource").toInt() == 2 && profile.value("mappings").toInt() == 1);
        /* #521: the row's own squelch reaches the review; an inheriting row carries none. */
        expect("row review reports the row squelch", profile.value("squelchDb").toInt() == -60);
        expect("a level squelch carries no auto margin", !profile.value("squelchMarginDb").isValid());
        expect("row review never contains key material",
               !QJsonDocument::fromVariant(profile).toJson().contains("ABCDE"));
    }
    QFile primary(stored);
    expect("open stored bundle", primary.open(QIODevice::ReadOnly));
    const QByteArray original = primary.readAll();
    primary.close();
    expect("companion paths rewritten", original.contains("/0.csv") && original.contains("/1.csv"));
    QFile::remove(keys);
    QFile::remove(map);
    result = importAndWait(model, channels, "Bundle.csv", "chan", rowPath(model, row), QVariantMap());
    expect("missing companions are requested",
           !result.value("ok").toBool() && result.value("error").toString() == "companions");
    expect("missing update keeps previous file", primary.open(QIODevice::ReadOnly) && primary.readAll() == original);
    primary.close();
    const QString replacementKey = write("replacement.csv", "keyid,value\n02,12345\n");
    const QString badMap = write("invalid.csv", "tg_dec,keyid_hex\ninvalid,02\n");
    result = importAndWait(model, channels, "Bundle.csv", "chan", rowPath(model, row),
                           {{"keys.csv", replacementKey}, {"map.csv", badMap}});
    expect("invalid companion rejects whole update", !result.value("ok").toBool());
    // The parser's reason names the companion by the file the user picked, never by its staging copy.
    const QString why = result.value("detail").toString();
    expect("an invalid companion's reason names the picked file",
           why.contains("invalid.csv") && !why.contains("import-staging"));
    expect("failed update keeps previous bundle", primary.open(QIODevice::ReadOnly) && primary.readAll() == original);
    primary.close();
    model.remove(row);
    expect("removing bundle frees companions", !QDir(root).exists());
}

/* Issue #637: a spreadsheet saves an options cell holding a double quote wrapped in double quotes, each quote inside
   doubled. The bundle import finds the companion the loader reads in it and rewrites it in place, leaving a cell the
   channel-map loader still takes, with its service name intact. */
static void
test_channel_bundle_quoted_options() {
    QTemporaryDir source;
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QFile file(source.filePath(name));
        expect("write quoted bundle fixture", file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
        return source.filePath(name);
    };
    (void)write("my keys.csv", "keyid,value\n02,ABCDE\n");
    const QByteArray text = "channel,frequency_hz,mode,options\n"
                            "1,464125000,dmr,\"--dcc-name \"\"7=St Mary's\"\" -K \"\"my keys.csv\"\"\"\n";
    const QString channels = write("quoted.csv", text);
    expect("quoted cell names its companion",
           dsd_qt::csv_companion_references(QString::fromUtf8(text)) == QStringList{"my keys.csv"});
    const auto result = importAndWait(model, channels, "Quoted.csv", "chan");
    expect("quoted bundle imports", result.value("ok").toBool());
    const QString stored = result.value("path").toString();
    const int row = model.rowForPath(stored);
    expect("quoted bundle registered", row >= 0);
    if (row < 0) {
        return;
    }
    QFile primary(stored);
    expect("open stored quoted bundle", primary.open(QIODevice::ReadOnly));
    const QByteArray rewritten = primary.readAll();
    primary.close();
    expect("the cell keeps its quoting and service name", rewritten.contains(",\"--dcc-name \"\"7=St Mary's\"\" -K ")
                                                              && !rewritten.contains("my keys.csv")
                                                              && rewritten.trimmed().endsWith("\""));
    dsd_csv_validation stats{};
    expect("the rewritten map still loads", dsd_csv_validate_chan_file(stored.toUtf8().constData(), &stats) == 0
                                                && stats.accepted == 1 && stats.skipped == 0);
    model.remove(row);
}

static void
test_target_bundle() {
    QTemporaryDir source;
    TestHost host;
    dsd_qt::ImportService service(host.importTransport());
    dsd_qt::ImportedFilesModel model(&host, &service);
    expect("nested fixture directory", QDir(source.path()).mkpath("sub"));
    const auto write = [&](const QString& name, const QByteArray& text) {
        const auto path = source.filePath(name);
        expect("write target fixture", write_file(path, text));
        return path;
    };
    const auto channels = write(
        "map, one.csv",
        "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,sub/key.csv\n-1,851012500,p25,unused-missing.csv\n");
    const auto keys = write("sub/key.csv", "id,value\n01,ABCDEF0123\n");
    const auto groups = write("groups file.csv", "id,mode,name\n123,A,Dispatch\n");
    const auto dmr = write("dmr.csv", "tg_dec,keyid_hex\n123,01\n");
    const auto band = write("band.csv", "iden,base_hz,spacing_hz\n0,851000000,12500\n");
    const QByteArray header = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,OPTIONS,modulation,rtl_"
                              "gain,p25_bandplan_csv\r\n";
    const QByteArray csv = header
                           + "site 1,p25-trunk,851012500,\"map, one.csv\",,,\"note, keep\",--enc-follow "
                             "--scan-max-visit-ms 20000 --squelch-db -60,auto,0,band.csv\r\n"
                             "plant,dmr-conventional,461112500,,250,1200,private,-H 0123456789 --no-force-key -G "
                             "\"groups file.csv\" --dmr-tg-key-csv='dmr.csv',gfsk,18,\r\n";
    const auto targets = write("targets.csv", csv);
    auto result = importAndWait(model, targets, "Targets.csv", "trunkTargets");
    expect("target bundle imports", result.value("ok").toBool());
    if (!result.value("ok").toBool()) {
        DSD_FPRINTF(stderr, "%s\n", qPrintable(result.value("detail").toString()));
        return;
    }
    const QString path = result.value("path").toString();
    const int row = model.rowForPath(path);
    expect("target count", result.value("accepted").toInt() == 2);
    const auto preview = model.targetPreview(path);
    const auto rows = preview.value("rows").toList();
    expect("target preview", preview.value("ok").toBool() && rows.size() == 2);
    if (rows.size() == 2) {
        expect("preserves id and omitted timing",
               rows[0].toMap().value("id") == "site 1" && rows[0].toMap().value("dwellMs").toInt() == -1);
        expect("preserves explicit auto and timing",
               rows[0].toMap().value("modulation") == "auto" && rows[1].toMap().value("dwellMs").toInt() == 250);
        /* #521: a target's own squelch previews as dB; one that inherits previews as nothing. */
        expect("previews the target squelch", rows[0].toMap().value("squelchDb").toInt() == -60);
        expect("an inheriting target previews no squelch", !rows[1].toMap().value("squelchDb").isValid());
    }
    expect("preview contains no keys", !QJsonDocument::fromVariant(preview).toJson().contains("0123456789"));
    expect("metadata contains no keys",
           !read_file(dsd_qt::json_store_path("imported_files.json")).contains("0123456789"));
    const auto original = read_file(path);
    expect("notes and CRLF preserved", original.contains("\"note, keep\"") && original.contains("\r\n"));
    expect("private target file", !(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
    dsd_trunk_scan_target_list parsed{};
    char error[512] = {};
    expect("rewritten file parses",
           dsd_trunk_scan_load_targets_csv(path.toUtf8().constData(), nullptr, &parsed, error, sizeof error) == 0);
    if (parsed.count == 2) {
        expect("scoped visit limit retained", parsed.targets[0].row_options.max_visit_ms == 20000);
        expect("explicit no force retained", (parsed.targets[1].row_options.present & DSD_SCAN_OPT_FORCE)
                                                 && parsed.targets[1].row_options.force == 0);
    }
    dsd_trunk_scan_target_list_reset(&parsed);
    host.running = true;
    expect("active replacement refused",
           !importAndWait(model, targets, "Targets.csv", "trunkTargets", rowPath(model, row), QVariantMap())
                .value("ok")
                .toBool());
    expect("active removal refused", !model.remove(row));
    host.running = false;
    write_file(band, "iden,base_hz,spacing_hz\n");
    result = importAndWait(model, targets, "Targets.csv", "trunkTargets", rowPath(model, row), QVariantMap());
    expect("empty bandplan rejects replacement",
           !result.value("ok").toBool() && result.value("detail").toString().contains("p25_bandplan_csv"));
    expect("rejected replacement preserves bytes", read_file(path) == original);
    write_file(band, "iden,base_hz,spacing_hz\n0,851000000,12500\n");
    result = importAndWait(model, targets, "Targets.csv", "trunkTargets", rowPath(model, row), QVariantMap());
    expect("replacement keeps stable path", result.value("ok").toBool() && result.value("path") == path);
    const QString root = model.get(row).value("bundleRoot").toString();
    waitForWorker(service);
    expect("old revisions reclaimed", QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == 1);

    // SAF references convey no sibling access, even when our host can copy them.
    host.documents.insert("content://targets", targets);
    result = importAndWait(model, "content://targets", "Android.csv", "trunkTargets");
    expect("SAF asks for companions", result.value("error") == "companions");
    host.documents.insert("content://channels", channels);
    const QVariantMap sources{
        {"map, one.csv", "content://channels"}, {"groups file.csv", groups}, {"dmr.csv", dmr}, {"band.csv", band}};
    QVariantMap selected;
    const auto labels = result.value("requiredLabels").toMap();
    for (auto i = labels.cbegin(); i != labels.cend(); ++i) {
        selected[i.key()] = sources.value(i.value().toString());
    }
    result = importAndWait(model, "content://targets", "Android.csv", "trunkTargets", QString(), selected);
    const auto nestedLabels = result.value("requiredLabels").toMap();
    expect("nested SAF reference has context",
           nestedLabels.values().contains(QStringLiteral("map, one.csv → sub/key.csv")));
    for (auto i = nestedLabels.cbegin(); i != nestedLabels.cend(); ++i) {
        if (i.value() == QStringLiteral("map, one.csv → sub/key.csv")) {
            selected[i.key()] = keys;
        }
    }
    result = importAndWait(model, "content://targets", "Android.csv", "trunkTargets", QString(), selected);
    expect("complete SAF bundle imports", result.value("ok").toBool());
    const QString androidPath = result.value("path").toString();

    // A retained channel map may refer to a leaf in another revision. GC must
    // follow the map, not assume every reachable file shares its directory.
    dsd_trunk_scan_target_list relocated{};
    expect("inspect revision fixture",
           dsd_trunk_scan_load_targets_csv(path.toUtf8().constData(), nullptr, &relocated, error, sizeof error) == 0);
    QString movedLeaf;
    const QString leafRevision = root + "/11111111-1111-1111-1111-111111111111";
    const QString orphanRevision = root + "/22222222-2222-2222-2222-222222222222";
    if (relocated.count > 0) {
        const QString storedMap = QString::fromUtf8(relocated.targets[0].chan_csv);
        QByteArray mapBytes = read_file(storedMap);
        const QByteArray leafName = mapBytes.split('\n').value(1).split(',').last();
        const QString oldLeaf = QFileInfo(storedMap).dir().filePath(QString::fromUtf8(leafName));
        expect("create revision fixture", QDir().mkpath(leafRevision) && QDir().mkpath(orphanRevision));
        movedLeaf = leafRevision + "/key.csv";
        expect("move nested leaf", QFile::rename(oldLeaf, movedLeaf));
        mapBytes.replace(',' + leafName + '\n', ",../11111111-1111-1111-1111-111111111111/key.csv\n");
        expect("rewrite cross-revision map", write_file(storedMap, mapBytes));
    }
    dsd_trunk_scan_target_list_reset(&relocated);
    QDir(source.path()).removeRecursively();
    int count = 0;
    uint32_t first = 0;
    expect("bundle outlives original documents",
           dsd_app_trunk_scan_validate_bundle(path.toUtf8().constData(), &count, &first, error, sizeof error) == 0
               && count == 2 && first == 851012500);
    dsd_qt::ImportedFilesModel reloaded(&host, &service);
    waitForWorker(service);
    expect("GC retains transitive dependencies", !movedLeaf.isEmpty() && QFile::exists(movedLeaf));
    expect("GC removes unreachable revisions", !QDir(orphanRevision).exists());
    expect("target import survives restart",
           reloaded.rowForPath(path) >= 0 && reloaded.targetPreview(path).value("ok").toBool());
    model.remove(model.rowForPath(androidPath));
    model.remove(model.rowForPath(path));
}

// Library metadata is advisory: only a visible selection may bind a companion.
static void
test_library_companions() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    UriTestHost host;
    QTemporaryDir source;
    dsd_qt::ImportedFilesModel model(&host);
    const QString channel = source.filePath("map.csv");
    const QString keys = source.filePath("keys.csv");
    const QString targets = source.filePath("targets.csv");
    expect("library channel fixture", write_file(channel, "channel,frequency,mode\n1,851012500,p25\n"));
    expect("library key fixture", write_file(keys, "id,value\n1,12345\n"));
    expect("library target fixture",
           write_file(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_dec_csv\none,p25-"
                               "trunk,851012500,map.csv,,,,keys.csv\n"));
    const auto map = importAndWait(model, QUrl::fromLocalFile(channel).toString(), "map.csv", "chan");
    const auto hex = importAndWait(model, QUrl::fromLocalFile(keys).toString(), "keys.csv", "keysHex");
    const QString mapPath = map.value("path").toString();
    const QString hexPath = hex.value("path").toString();
    host.documents.insert("content://targets", targets);
    const auto before = read_file(dsd_qt::json_store_path("imported_files.json"));
    auto pending = importAndWait(model, "content://targets", "Targets.csv", "trunkTargets");
    expect("library does not auto-link", pending.value("error") == "companions");
    const auto details = pending.value("requiredDetails").toMap();
    QString mapKey, keyKey;
    for (auto i = details.cbegin(); i != details.cend(); ++i) {
        const auto detail = i.value().toMap();
        if (detail.value("type") == "chan") {
            mapKey = i.key();
            expect("channel candidate uses stored path", detail.value("preselected") == mapPath);
            expect("channel candidate typed", detail.value("candidates").toList().size() == 1);
        } else if (detail.value("type") == "keysDec") {
            keyKey = i.key();
            expect("hex excluded from decimal candidates", detail.value("candidates").toList().isEmpty());
            expect("hex mismatch surfaced", detail.value("warning").toString().contains("expects decimal keys"));
        }
    }
    expect("typed requirements available", !mapKey.isEmpty() && !keyKey.isEmpty());
    expect("cancelled discovery leaves metadata unchanged",
           read_file(dsd_qt::json_store_path("imported_files.json")) == before);
    expect("cancelled discovery leaves library unchanged", model.count() == 2 && QFile::exists(mapPath));
    expect("cancelled discovery leaves no staged revision",
           QDir(dsd_qt::json_store_path("imports/bundles")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    auto rejected = importAndWait(model, "content://targets", "Targets.csv", "trunkTargets", QString(),
                                  {{mapKey, mapPath}, {keyKey, hexPath}});
    expect("explicit library key type mismatch refused",
           rejected.value("error") == "companions" && rejected.value("required").toStringList() == QStringList{keyKey});
    expect("target mismatch explains actual and expected kinds inline",
           rejected.value("requiredDetails")
               .toMap()
               .value(keyKey)
               .toMap()
               .value("warning")
               .toString()
               .contains("keys.csv is a hex key file; this bundle expects decimal keys"));
    const auto dec = importAndWait(model, QUrl::fromLocalFile(keys).toString(), "decimal.csv", "keysDec");
    host.references.clear();
    const auto imported = importAndWait(model, "content://targets", "Targets.csv", "trunkTargets", QString(),
                                        {{mapKey, mapPath}, {keyKey, dec.value("path")}});
    expect("library selections feed stager", imported.value("ok").toBool());
    expect("library channel and trunk leaf reach host as file URLs",
           host.references
               == QStringList{"content://targets", QUrl::fromLocalFile(mapPath).toString(),
                              QUrl::fromLocalFile(dec.value("path").toString()).toString()});
    const QString stored = imported.value("path").toString();
    const auto original = read_file(stored);
    const int row = model.rowForPath(stored);
    rejected = importAndWait(model, "content://targets", "Targets.csv", "trunkTargets", rowPath(model, row),
                             {{mapKey, mapPath}, {keyKey, hexPath}});
    expect("rejected library replacement preserves target",
           !rejected.value("ok").toBool() && read_file(stored) == original);
    expect("original documents removed", QDir(source.path()).removeRecursively());
    expect("library-owned source removable independently", model.remove(model.rowForPath(mapPath)));
    expect("bundle owns independent private copies",
           dsd_app_trunk_scan_validate_bundle(stored.toUtf8().constData(), nullptr, nullptr, nullptr, 0) == 0);
}

static void
test_library_candidate_ranking() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    QTemporaryDir source;
    dsd_qt::ImportedFilesModel model(&host);
    const QString channels = source.filePath("map.csv");
    const QString keys = source.filePath("keys.csv");
    expect("ranking channel", write_file(channels, "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,keys.csv\n"));
    expect("ranking keys", write_file(keys, "id,value\n1,12345\n"));
    // A nested SAF map keeps its opaque identity and typed leaf requirement.
    const QString targets = source.filePath("targets.csv");
    expect("ranking target",
           write_file(
               targets,
               "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\none,p25-trunk,851012500,map.csv,,,\n"));
    host.documents.insert("content://targets", targets);
    host.documents.insert("content://map", channels);
    auto result = importAndWait(model, "content://targets", "Targets.csv", "trunkTargets");
    const QString slot = result.value("required").toStringList().value(0);
    const auto dec = importAndWait(model, keys, "keys.csv", "keysDec");
    importAndWait(model, keys, "another.csv", "keysDec");
    importAndWait(model, keys, "keys.csv", "keysHex");
    result =
        importAndWait(model, "content://targets", "Targets.csv", "trunkTargets", QString(), {{slot, "content://map"}});
    const auto details = result.value("requiredDetails").toMap();
    const auto missing = result.value("required").toStringList();
    expect("nested requirement offered", missing.size() == 1);
    const auto detail = details.value(missing.value(0)).toMap();
    expect("nested role retained", detail.value("type") == "keysDec");
    const auto candidates = detail.value("candidates").toList();
    expect("only role-compatible choices", candidates.size() == 2);
    expect("basename ranks first",
           !candidates.isEmpty() && candidates.first().toMap().value("path") == dec.value("path"));
    expect("multiple candidates never preselected", detail.value("preselected").toString().isEmpty());
    expect("nested label remains contextual",
           result.value("requiredLabels").toMap().values().contains(QStringLiteral("map.csv → keys.csv")));
}

static void
test_duplicate_library_names() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    QTemporaryDir source;
    dsd_qt::ImportedFilesModel model(&host);
    // A pick is stored under its own name: the map and the key file are both picked as same.csv.
    const QString channels = source.filePath("map/same.csv");
    const QString keys = source.filePath("keys/same.csv");
    expect("duplicate keys fixture", QDir(source.path()).mkpath("map") && QDir(source.path()).mkpath("keys")
                                         && write_file(keys, "id,value\n1,12345\n")
                                         && write_file(source.filePath("map/leaf.csv"), "id,value\n1,12345\n"));
    expect("duplicate channels fixture",
           write_file(channels, "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,leaf.csv\n"));
    // Bundle primaries live under distinct roots, so equal library basenames
    // really occur even though plain library imports uniquify their paths.
    const auto first = importAndWait(model, channels, "same.csv", "chan");
    const auto second = importAndWait(model, channels, "same.csv", "chan");
    const auto hex = importAndWait(model, keys, "same.csv", "keysHex");
    expect("duplicate named bundles imported", first.value("ok").toBool() && second.value("ok").toBool());
    const QString targets = source.filePath("targets.csv");
    expect("duplicate target fixture",
           write_file(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv\n"
                               "one,p25-trunk,851012500,same.csv,,,,same.csv\n"));
    host.documents.insert("content://targets", targets);
    const auto result = importAndWait(model, "content://targets", "Targets.csv", "trunkTargets");
    const auto details = result.value("requiredDetails").toMap();
    expect("same path different roles have distinct slots", result.value("required").toStringList().size() == 2);
    for (auto i = details.cbegin(); i != details.cend(); ++i) {
        const auto detail = i.value().toMap();
        const auto candidates = detail.value("candidates").toList();
        if (detail.value("type") == "chan") {
            expect("equal channel basenames both offered", candidates.size() == 2);
            expect("ambiguous names not preselected", detail.value("preselected").toString().isEmpty());
            expect("ambiguous names surfaced", detail.value("warning").toString().contains("Several imported files"));
        } else {
            expect("key role excludes both same-named channels", candidates.size() == 1);
            expect("key role uses correct private path", detail.value("preselected") == hex.value("path"));
        }
    }
}

static void
test_library_warning_display_names() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    QTemporaryDir source;
    dsd_qt::ImportedFilesModel model(&host);
    const QString keys = source.filePath("keys.csv");
    const QString channels = source.filePath("channels.csv");
    expect("warning keys fixture", write_file(keys, "id,value\n1,12345\n"));
    expect("warning channels fixture",
           write_file(channels, "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,missing/shared.csv\n"));
    const auto dec = importAndWait(model, keys, "shared.csv", "keysDec");
    const auto hex = importAndWait(model, keys, "stored-hex.csv", "keysHex");
    // Persist a display name that differs from the private filename.
    auto rows = dsd_qt::json_store_load_array("imported_files.json");
    auto row = rows.at(model.rowForPath(hex.value("path").toString())).toObject();
    row.insert("name", "shared.csv");
    rows.replace(model.rowForPath(hex.value("path").toString()), row);
    expect("warning display name saved", dsd_qt::json_store_save_array("imported_files.json", rows));
    dsd_qt::ImportedFilesModel reloaded(&host);
    // A file can disappear after the model has loaded its library metadata.
    expect("same-kind stored file removed", QFile::remove(dec.value("path").toString()));
    const auto pending = importAndWait(reloaded, channels, "Map.csv", "chan");
    const auto slot = pending.value("required").toStringList().value(0);
    const QString warning = "shared.csv is a hex key file; this bundle expects decimal keys";
    expect("missing same-kind file adds no blank warning",
           pending.value("requiredDetails").toMap().value(slot).toMap().value("warning") == warning);
    const auto rejected = importAndWait(reloaded, channels, "Map.csv", "chan", QString(), {{slot, hex.value("path")}});
    expect("picked mismatch uses the library display name once",
           rejected.value("requiredDetails").toMap().value(slot).toMap().value("warning") == warning);
}

static void
test_channel_companion_roles() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    UriTestHost host;
    QTemporaryDir source;
    dsd_qt::ImportedFilesModel model(&host);
    const QString channels = source.filePath("channels.csv");
    // A pick is stored under its own name.
    const QString decimal = source.filePath("decimal.csv");
    const QString keys = source.filePath("hex.csv");
    expect("channel role keys", write_file(decimal, "id,value\n1,12345\n") && write_file(keys, "id,value\n1,12345\n"));
    const auto dec = importAndWait(model, QUrl::fromLocalFile(decimal).toString(), "decimal.csv", "keysDec");
    const auto hex = importAndWait(model, QUrl::fromLocalFile(keys).toString(), "hex.csv", "keysHex");
    expect("channel role fixture",
           write_file(channels,
                      "channel,frequency,mode,options\n1,851012500,p25,-k same.csv\n2,852012500,p25,-K same.csv\n"));
    host.documents.insert("content://channels", channels);
    const auto pending = importAndWait(model, "content://channels", "Map.csv", "chan");
    const auto details = pending.value("requiredDetails").toMap();
    expect("channel roles get separate slots", pending.value("required").toStringList().size() == 2);
    QVariantMap selected;
    for (auto i = details.cbegin(); i != details.cend(); ++i) {
        selected.insert(i.key(), i.value().toMap().value("type") == "keysDec" ? dec.value("path") : hex.value("path"));
    }
    host.references.clear();
    const auto imported = importAndWait(model, "content://channels", "Map.csv", "chan", QString(), selected);
    expect("channel role selections import", imported.value("ok").toBool());
    expect("library channel companions reach host as file URLs",
           host.references
               == QStringList{"content://channels", QUrl::fromLocalFile(dec.value("path").toString()).toString(),
                              QUrl::fromLocalFile(hex.value("path").toString()).toString()});
    expect("channel role copies remain separate",
           read_file(imported.value("path").toString()).contains("/0.csv")
               && read_file(imported.value("path").toString()).contains("/1.csv"));
    for (auto i = selected.begin(); i != selected.end(); ++i) {
        i.value() = hex.value("path");
    }
    const auto rejected = importAndWait(model, "content://channels", "Map.csv", "chan", QString(), selected);
    const auto missing = rejected.value("required").toStringList();
    expect("channel type mismatch requests only the wrong slot",
           rejected.value("error") == "companions" && missing.size() == 1);
    expect("channel mismatch explains actual and expected kinds inline",
           rejected.value("requiredDetails")
               .toMap()
               .value(missing.value(0))
               .toMap()
               .value("warning")
               .toString()
               .contains("hex.csv is a hex key file; this bundle expects decimal keys"));
}

static void
test_channel_companion_uri_forms() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    UriTestHost host;
    QTemporaryDir source;
    dsd_qt::ImportedFilesModel model(&host);
    const QString channels = source.filePath("channels.csv");
    const QString keyCsvPath = source.filePath("keys # 100%.csv");
    expect("URI channel fixture",
           write_file(channels, "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,missing.csv\n"));
    expect("URI key fixture", write_file(keyCsvPath, "id,value\n1,12345\n"));
    expect("URI host rejects scheme-less input",
           importAndWait(model, keyCsvPath, "keys.csv", "keysDec").value("error") == "read" && model.rowCount() == 0);
    const QString fileUri = QUrl::fromLocalFile(keyCsvPath).toString(QUrl::FullyEncoded);
    const QString contentUri = "content://test.documents/document/keys%3A1?version=2";
    host.documents.insert("content://channels", channels);
    host.documents.insert(contentUri, keyCsvPath);
    const auto pending = importAndWait(model, "content://channels", "Map.csv", "chan");
    const QString slot = pending.value("required").toStringList().value(0);
    expect("URI channel requirement", !slot.isEmpty());
    for (const auto& selected : {keyCsvPath, fileUri, contentUri}) {
        host.references.clear();
        const auto imported =
            importAndWait(model, "content://channels", "Map.csv", "chan", QString(), {{slot, selected}});
        expect("channel companion URI form imports", imported.value("ok").toBool());
        const QString expected = selected == keyCsvPath ? QUrl::fromLocalFile(keyCsvPath).toString() : selected;
        expect("channel normalizes paths and preserves existing URIs",
               host.references == QStringList{"content://channels", expected});
        const QString path = imported.value("path").toString();
        const auto relative = QString::fromUtf8(read_file(path)).split('\n').value(1).split(',').value(3);
        expect("channel URI copy retains bytes",
               read_file(QFileInfo(path).dir().filePath(relative)) == read_file(keyCsvPath));
    }
}

static void
test_nested_companion_uri_forms() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    UriTestHost host;
    QTemporaryDir source;
    dsd_qt::ImportedFilesModel model(&host);
    const QString targets = source.filePath("targets.csv");
    const QString channels = source.filePath("map # 100%.csv");
    const QString keyCsvPath = source.filePath("keys # 100%.csv");
    expect("nested URI target fixture",
           write_file(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                               "one,p25-trunk,851012500,map.csv,,,\n"));
    expect("nested URI channel fixture",
           write_file(channels, "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,missing.csv\n"));
    expect("nested URI key fixture", write_file(keyCsvPath, "id,value\n1,12345\n"));
    const QString channelUri = QUrl::fromLocalFile(channels).toString(QUrl::FullyEncoded);
    const QString keyUri = QUrl::fromLocalFile(keyCsvPath).toString(QUrl::FullyEncoded);
    const QString contentMap = "content://test.documents/document/map%3A1?version=2";
    const QString contentKeys = "content://test.documents/document/keys%3A1?version=2";
    host.documents.insert("content://targets", targets);
    host.documents.insert(contentMap, channels);
    host.documents.insert(contentKeys, keyCsvPath);
    const auto pending = importAndWait(model, "content://targets", "Targets.csv", "trunkTargets");
    const QString mapSlot = pending.value("required").toStringList().value(0);
    expect("nested URI map requirement", !mapSlot.isEmpty());
    const QStringList mapForms{channels, channelUri, contentMap};
    const QStringList keyForms{keyCsvPath, keyUri, contentKeys};
    for (int i = 0; i < mapForms.size(); ++i) {
        const auto nested = importAndWait(model, "content://targets", "Targets.csv", "trunkTargets", QString(),
                                          {{mapSlot, mapForms[i]}});
        const auto missing = nested.value("required").toStringList();
        expect("nested URI leaf requirement", nested.value("error") == "companions" && missing.size() == 1);
        const auto identity = QJsonDocument::fromJson(missing.value(0).toUtf8()).array();
        expect("nested identity retains original selection",
               identity == QJsonArray{mapForms[i], "missing.csv", "keysDec"});
        host.references.clear();
        const auto imported = importAndWait(model, "content://targets", "Targets.csv", "trunkTargets", QString(),
                                            {{mapSlot, mapForms[i]}, {missing.value(0), keyForms[i]}});
        expect("nested companion URI forms import", imported.value("ok").toBool());
        expect("nested copies normalize paths and preserve existing URIs",
               host.references
                   == QStringList{"content://targets", i == 0 ? QUrl::fromLocalFile(channels).toString() : mapForms[i],
                                  i == 0 ? QUrl::fromLocalFile(keyCsvPath).toString() : keyForms[i]});
        expect("nested URI bundle validates",
               dsd_app_trunk_scan_validate_bundle(imported.value("path").toString().toUtf8().constData(), nullptr,
                                                  nullptr, nullptr, 0)
                   == 0);
    }
}

static void
test_target_primary_collision(const char* label, const QByteArray& csv, const QString& companion,
                              const QByteArray& companionBytes) {
    QTemporaryDir source;
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    const QString targets = source.filePath("targets.csv");
    expect("write collision primary", write_file(targets, csv));
    expect("write collision companion", write_file(source.filePath(companion), companionBytes));
    const auto imported = importAndWait(model, targets, "Targets.csv", "trunkTargets");
    expect("collision baseline imports", imported.value("ok").toBool());
    expect("collision baseline target count", imported.value("accepted").toInt() == 1);
    const QString path = imported.value("path").toString();
    const int row = model.rowForPath(path);
    expect("collision baseline registered", row >= 0);
    if (row < 0) {
        return;
    }
    const QByteArray original = read_file(path);
    const QString root = model.get(row).value("bundleRoot").toString();
    const QStringList revisions = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    expect("collision baseline has one revision", revisions.size() == 1);
    const QString revision = QDir(root).filePath(revisions.value(0));
    const QString storedCompanion = revision + "/0.csv";
    expect("collision baseline companion stored", read_file(storedCompanion) == companionBytes);

    QByteArray replacement = csv;
    replacement.replace(companion.toUtf8(), "targets.csv");
    const QString update = source.filePath("replacement.csv");
    expect("write colliding replacement", write_file(update, replacement));
    // SAF supplies only the selected document, with no local sibling access.
    host.documents.insert("content://replacement", update);
    const auto result = importAndWait(model, "content://replacement", "Replacement.csv", "trunkTargets",
                                      rowPath(model, row), QVariantMap());
    expect(label, !result.value("ok").toBool());
    expect("collision requests companions", result.value("error").toString() == "companions");
    expect("collision reports missing reference", !result.value("required").toStringList().isEmpty());
    expect("collision preserves stored primary", read_file(path) == original);
    expect("collision preserves previous revision", QDir(revision).exists());
    expect("collision preserves previous companion", QFileInfo(storedCompanion).isFile());
    expect("collision preserves companion bytes", read_file(storedCompanion) == companionBytes);
    model.remove(row);
}

static void
test_target_channel_primary_collision() {
    test_target_primary_collision("chan_csv must not bind to stored primary",
                                  "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                                  "one,p25-trunk,851012500,map.csv,,,\n",
                                  "map.csv", "channel,frequency,mode\n1,851012500,p25\n");
}

static void
test_target_key_primary_collision() {
    test_target_primary_collision("-K must not bind to stored primary",
                                  "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                                  "one,p25-conventional,851012500,,,,,-K keys.csv\n",
                                  "keys.csv", "id,value\n01,ABCDEF0123\n");
}

static QString
stored_target_channel(const QString& path) {
    dsd_trunk_scan_target_list parsed{};
    char error[512] = {};
    expect("detached target parses",
           dsd_trunk_scan_load_targets_csv(path.toUtf8().constData(), nullptr, &parsed, error, sizeof error) == 0);
    expect("detached target count", parsed.count == 1);
    const QString channel = parsed.count == 1 ? QString::fromUtf8(parsed.targets[0].chan_csv) : QString();
    dsd_trunk_scan_target_list_reset(&parsed);
    return channel;
}

static void
test_target_detached_copy() {
    QTemporaryDir source;
    QTemporaryDir detached;
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    const QByteArray keys = "id,value\n01,ABCDEF0123\n";
    const QString targets = source.filePath("targets.csv");
    expect("write detached keys", write_file(source.filePath("keys.csv"), keys));
    expect("write detached channel map",
           write_file(source.filePath("map.csv"), "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,keys.csv\n"));
    expect("write detached targets",
           write_file(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                               "one,p25-trunk,851012500,map.csv,250,,\n"));
    const auto imported = importAndWait(model, targets, "Targets.csv", "trunkTargets");
    expect("detached baseline imports", imported.value("ok").toBool());
    const QString path = imported.value("path").toString();
    const int row = model.rowForPath(path);
    expect("detached baseline registered", row >= 0);
    if (row < 0) {
        return;
    }
    const QString root = model.get(row).value("bundleRoot").toString();
    const QString originalMap = stored_target_channel(path);
    const QByteArray original = read_file(path);
    expect("stored primary names revision map",
           original.contains(QDir(root).relativeFilePath(originalMap).toUtf8()) && original.contains("/0.csv"));
    expect("stored map names nested key", read_file(originalMap).contains(",1.csv\n"));
    const QString copy = detached.filePath("edited.csv");
    expect("copy primary outside bundle", QFile::copy(path, copy));
    expect("detached directory is outside bundle", !copy.startsWith(root + '/'));
    QByteArray edited = read_file(copy);
    edited.replace(",250,", ",450,");
    expect("detached edit changes dwell", edited != original && edited.contains(",450,"));
    expect("write detached edit", write_file(copy, edited));
    expect("remove original sources", QDir(source.path()).removeRecursively());

    const auto result = importAndWait(model, copy, "Edited.csv", "trunkTargets", rowPath(model, row), QVariantMap());
    expect("detached replacement reuses companions", result.value("ok").toBool());
    expect("detached replacement keeps primary path", result.value("path").toString() == path);
    expect("detached replacement retains dwell edit", read_file(path).contains(",450,"));
    int count = 0;
    char error[512] = {};
    expect("detached replacement bundle validates",
           dsd_app_trunk_scan_validate_bundle(path.toUtf8().constData(), &count, nullptr, error, sizeof error) == 0);
    expect("detached replacement validated count", count == 1);
    const QString storedMap = stored_target_channel(path);
    expect("detached replacement channel remains reachable", QFileInfo(storedMap).isFile());
    const QByteArray keyName = read_file(storedMap).split('\n').value(1).split(',').last();
    const QString storedKey = QFileInfo(storedMap).dir().filePath(QString::fromUtf8(keyName));
    expect("detached replacement nested key remains reachable", QFileInfo(storedKey).isFile());
    expect("detached replacement preserves nested key bytes", read_file(storedKey) == keys);
    model.remove(row);
}

/* The labels (the paths as the CSV writes them) of the companions @p result still asks for. */
static QStringList
requiredLabels(const QVariantMap& result) {
    QStringList labels;
    const QVariantMap names = result.value("requiredLabels").toMap();
    for (const QString& key : result.value("required").toStringList()) {
        labels.append(names.value(key).toString());
    }
    labels.sort();
    return labels;
}

/* Issue #830: where the host browses the whole device, a CSV finds companions by path only inside its own folder
   tree, by a relative name, as a regular file whose real path stays there; the rest stay required on the sheet. An
   explicit choice is taken wherever it is, and a host without the browser finds every file it names, as before. */
static void
test_confined_companion_lookup() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    const QTemporaryDir tmp;
    const QString folder = tmp.filePath("browse/lists");
    const QString elsewhere = tmp.filePath("elsewhere");
    expect("confinement folders", QDir().mkpath(folder + "/sub") && QDir().mkpath(elsewhere));
    const QByteArray keys = "id,value\n1,12345\n";
    const QString outsideAbsolute = elsewhere + "/abs.csv";
    const QString insideAbsolute = folder + "/inside.csv";
    bool fixtures = write_file(folder + "/groups.csv", keys) && write_file(folder + "/sub/keys.csv", keys)
                    && write_file(tmp.filePath("browse/outside.csv"), keys) && write_file(outsideAbsolute, keys)
                    && write_file(insideAbsolute, keys) && write_file(elsewhere + "/target.csv", keys)
                    && write_file(folder + "/sub/real.csv", keys);
    QString csv = "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,groups.csv\n2,851025000,p25,sub/keys.csv\n"
                  "3,851037500,p25,../outside.csv\n4,851050000,p25,"
                  + outsideAbsolute + "\n5,851062500,p25," + insideAbsolute + "\n";
    QStringList confinedOut = {QStringLiteral("../outside.csv"), outsideAbsolute, insideAbsolute};
#ifndef _WIN32
    fixtures = fixtures && QFile::link(elsewhere + "/target.csv", folder + "/link.csv")
               && QFile::link(folder + "/sub/real.csv", folder + "/inlink.csv");
    csv += "6,851075000,p25,link.csv\n7,851087500,p25,inlink.csv\n";
    confinedOut.append(QStringLiteral("link.csv"));
#endif
    confinedOut.sort();
    const QString primary = folder + "/chan.csv";
    expect("confinement fixtures", fixtures && write_file(primary, csv.toUtf8()));

    TestHost host;
    host.browser = true;
    dsd_qt::ImportedFilesModel model(&host);
    QVariantMap result = importAndWait(model, QUrl::fromLocalFile(primary).toString(), "chan.csv", "chan");
    expect("confined: the companions out of the CSV's tree stay required",
           result.value("error") == "companions" && requiredLabels(result) == confinedOut);
    // An explicit choice outside the tree is the user's: taken as it is.
    QVariantMap chosen;
    for (const QString& key : result.value("required").toStringList()) {
        chosen.insert(key, outsideAbsolute);
    }
    result = importAndWait(model, QUrl::fromLocalFile(primary).toString(), "chan.csv", "chan", QString(), chosen);
    expect("confined: explicit choices outside are used", result.value("ok").toBool());
    const int stored = model.rowForPath(result.value("path").toString());

    // A target list's nested channel map: its companions stay in the target list's tree too.
    expect("nested fixtures",
           write_file(tmp.filePath("browse/x.csv"), "id,value\n01,ABCDEF0123\n")
               && write_file(folder + "/hk.csv", "id,value\n01,ABCDEF0123\n")
               && write_file(folder + "/nested.csv",
                             "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,../x.csv\n2,851025000,p25,hk.csv\n")
               && write_file(folder + "/targets.csv", "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                                                      "one,p25-trunk,851012500,nested.csv,250,,\n"));
    result =
        importAndWait(model, QUrl::fromLocalFile(folder + "/targets.csv").toString(), "targets.csv", "trunkTargets");
    const QStringList nested = requiredLabels(result);
    expect("confined: a nested map's escape stays required, its neighbour is found",
           result.value("error") == "companions" && nested.size() == 1 && nested.value(0).endsWith("→ ../x.csv"));
    expect("contained target fixtures",
           write_file(folder + "/map2.csv", "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,hk.csv\n")
               && write_file(folder + "/targets2.csv", "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                                                       "one,p25-trunk,851012500,map2.csv,250,,\n"));
    result =
        importAndWait(model, QUrl::fromLocalFile(folder + "/targets2.csv").toString(), "targets2.csv", "trunkTargets");
    expect("confined: a target list inside its tree imports", result.value("ok").toBool());
    const QString storedTargets = result.value("path").toString();
    const QString detachedTargets = tmp.filePath("detached-targets.csv");
    expect("detached target copy", QFile::copy(storedTargets, detachedTargets));
    host.documents.insert("content://detached-targets", detachedTargets);
    result = importAndWait(model, "content://detached-targets", "targets2.csv", "trunkTargets", storedTargets);
    expect("confined: a stored target revision's own companions are found", result.value("ok").toBool());

    // A stored revision's update from a document with no folder: only names inside the bundle's root are found.
    const QString document = tmp.filePath("update.csv");
    expect("update fixture", write_file(document, "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,"
                                                      + outsideAbsolute.toUtf8() + "\n"));
    host.documents.insert("content://update", document);
    result = importAndWait(model, "content://update", "chan.csv", "chan", rowPath(model, stored));
    expect("confined: a stored revision's update naming a file outside its root asks for it",
           result.value("error") == "companions" && requiredLabels(result) == QStringList{outsideAbsolute});
    const QString detached = tmp.filePath("detached.csv");
    expect("detached copy", QFile::copy(rowPath(model, stored), detached));
    host.documents.insert("content://detached", detached);
    result = importAndWait(model, "content://detached", "chan.csv", "chan", rowPath(model, stored));
    expect("confined: a stored revision's own companions are found", result.value("ok").toBool());

    // Without the browser (the desktop) every file the CSV names is found, as before.
    host.browser = false;
    result = importAndWait(model, QUrl::fromLocalFile(primary).toString(), "chan.csv", "chan");
    expect("unconfined: every companion is found", result.value("ok").toBool());
    result =
        importAndWait(model, QUrl::fromLocalFile(folder + "/targets.csv").toString(), "targets.csv", "trunkTargets");
    expect("unconfined: a nested map's companions are found", result.value("ok").toBool());
    result = importAndWait(model, "content://update", "chan.csv", "chan", rowPath(model, stored));
    expect("unconfined: a stored revision's update finds a file outside its root", result.value("ok").toBool());
    while (model.rowCount() > 0) {
        model.remove(0);
    }
}

static void
test_target_errors_and_opaque_columns() {
    TestHost host;
    QTemporaryDir source;
    dsd_qt::ImportedFilesModel model(&host);
    const int before = model.count();
    const QByteArray header = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes";
    const QByteArray row = "target,dmr-conventional,461000000,,,,";
    const QString path = source.filePath("targets.csv");
    const QByteArray secret = "REDACTION-SENTINEL-NOT-HEX";
    for (const auto& column : {QByteArray("options"), QByteArray("single_key_hex")}) {
        const auto value = column == "options" ? "-H " + secret : secret;
        expect("write bad-key fixture", write_file(path, header + ',' + column + '\n' + row + ',' + value + '\n'));
        const auto result = importAndWait(model, path, "Targets.csv", "trunkTargets");
        expect("bad-key import fails atomically", !result.value("ok").toBool() && model.count() == before);
        const auto detail = result.value("detail").toString();
        expect("bad-key error provides row without key",
               detail.contains("row 2") && !detail.contains(QString::fromUtf8(secret)));
    }
    // A channel map the target list names that its parser refuses: the reason never shows a staging path.
    expect("write bad channel companion",
           write_file(source.filePath("bad-chan.csv"), "channel,frequency_hz,mode\n1,461000000,warp\n")
               && write_file(path, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,"
                                   "notes\nscan,dmr-trunk,461000000,bad-chan.csv,,,\n"));
    const QString badChan = importAndWait(model, path, "Targets.csv", "trunkTargets").value("detail").toString();
    expect("a refused companion's reason names no staging path",
           badChan.contains("chan_csv companion") && !badChan.contains("import-staging"));
    expect("write opaque optional column", write_file(path, header + ",chan_csv\n" + row + ",unused-missing.csv\n"));
    const auto result = importAndWait(model, path, "Opaque.csv", "trunkTargets");
    expect("optional chan_csv remains opaque", result.value("ok").toBool());
    const QString stored = result.value("path").toString();
    expect("opaque bytes preserved", read_file(stored).endsWith(",unused-missing.csv\n"));
    model.remove(model.rowForPath(stored));
}

static dsd_trunk_tune_result
acceptTune(dsd_opts*, dsd_state*, long int, int, uint64_t) {
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
test_example_targets() {
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    const auto result = importAndWait(model, QStringLiteral(DSD_NEO_TEST_EXAMPLES_DIR "/trunk_scan_targets.csv"),
                                      "Examples.csv", "trunkTargets");
    expect("shipped example imports all targets",
           result.value("ok").toBool() && result.value("accepted").toInt() == 12);
    if (!result.value("ok").toBool()) {
        return;
    }
    const QString path = result.value("path").toString();
    const auto rows = model.targetPreview(path).value("rows").toList();
    /* Issue #526: the analog nfm-conventional and am-conventional targets preview with their canonical types and their
       own channel widths and squelch, which the import's copy keeps; a digital target carries no width. */
    expect("shipped row order retained",
           rows.size() == 12 && rows[0].toMap().value("id") == "county-p25"
               && rows[7].toMap().value("id") == "field-nxdn48" && rows[8].toMap().value("id") == "fire-nfm"
               && rows[8].toMap().value("type") == "nfm-conventional" && rows[9].toMap().value("id") == "tower-am"
               && rows[9].toMap().value("type") == "am-conventional" && rows[10].toMap().value("id") == "Mall"
               && rows[11].toMap().value("id") == "20R Local");
    if (rows.size() == 12) {
        expect("previews the nfm target's own width", rows[8].toMap().value("bandwidthHz").toInt() == 12500);
        expect("previews the am target's own width and auto squelch",
               rows[9].toMap().value("bandwidthHz").toInt() == 8333
                   && rows[9].toMap().value("squelchMarginDb").toInt() == 6);
        expect("an analog target previews no modulation", rows[9].toMap().value("modulation").toString().isEmpty());
        expect("a digital target previews no width", !rows[0].toMap().value("bandwidthHz").isValid());
        /* Issue #527: the nfm target's own tone policy previews as its mode and displayed list; the am and digital
           targets run the configured one. */
        expect("previews the nfm target's own tone policy",
               rows[8].toMap().value("toneFilter").toInt() == DSD_TONE_FILTER_ALLOW
                   && rows[8].toMap().value("toneList") == "156.7 Hz/D023N");
        expect("the am and digital targets carry no tone policy",
               !rows[9].toMap().value("toneFilter").isValid() && !rows[0].toMap().value("toneFilter").isValid());
        /* Issue #637: the reporter's shared-frequency rows preview their naming rules and the NFM row's tone list. */
        expect("the shared DMR row previews its names", !rows[10].toMap().value("receiveSummary").toString().isEmpty());
        expect("the shared NFM row previews its tones and name",
               rows[11].toMap().value("toneList") == "97.4 Hz/141.3 Hz"
                   && !rows[11].toMap().value("receiveSummary").toString().isEmpty());
    }
    auto* opts = static_cast<dsd_opts*>(std::calloc(1, sizeof(dsd_opts)));
    auto* state = static_cast<dsd_state*>(std::calloc(1, sizeof(dsd_state)));
    expect("allocate example engine state", opts && state);
    if (!opts || !state) {
        std::free(opts);
        std::free(state);
        return;
    }
    opts->trunk_scan_enabled = 1;
    opts->use_rigctl = 1;
    /* No rigctl peer: the retune fails as it does without one. A zeroed socket field would be descriptor 0, and a
       retune would write its command to stdin and then wait on stdin for the reply. */
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->rtl_dsp_bw_khz = 48;
    opts->scan_max_visit_ms = 40000;
    DSD_SNPRINTF(opts->trunk_scan_lists[0].path, sizeof opts->trunk_scan_lists[0].path, "%s",
                 path.toUtf8().constData());
    opts->trunk_scan_list_count = 1;
    dsd_trunk_tuning_hooks hooks{};
    hooks.tune_to_freq_request = acceptTune;
    hooks.tune_to_cc_request = acceptTune;
    dsd_trunk_tuning_hooks_set(hooks);
    char error[512] = {};
    const bool initialized = dsd_engine_trunk_scan_init(opts, state, error, sizeof error) == 0;
    expect("imported example initializes", initialized);
    if (initialized) {
        expect("engine owns all twelve imported targets", dsd_engine_trunk_scan_target_count(state) == 12);
        expect("first target visit limit applies", opts->scan_max_visit_ms == 20000);
        expect("advance imported target",
               dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        expect("next target inherits baseline limit", opts->scan_max_visit_ms == 40000);
    }
    dsd_engine_trunk_scan_shutdown(opts, state);
    dsd_trunk_tuning_hooks_set({});
    dsd_trunk_scan_hooks_set(nullptr);
    dsd_state_trunk_lcn_free(state);
    dsd_state_ext_free_all(state);
    DSD_SECURE_ZERO(state, sizeof *state);
    std::free(state);
    std::free(opts);
    model.remove(model.rowForPath(path));
}

/* Issue #829: a host without a picker answers every request "unsupported", but only after the request returned, so a
   caller that stores the id first sees the answer; each request gets a larger id. */
static void
test_document_picker_defaults() {
    TestHost host;
    expect("desktop has no document picker", !host.documentPickerSupported());
    QList<qint64> unavailableIds;
    QStringList reasons;
    int otherAnswers = 0;
    QObject::connect(&host, &dsd_qt::DecoderHost::documentPickUnavailable, &host,
                     [&unavailableIds, &reasons](qint64 id, const QString& reason) {
                         unavailableIds.append(id);
                         reasons.append(reason);
                     });
    QObject::connect(&host, &dsd_qt::DecoderHost::documentPicked, &host,
                     [&otherAnswers](qint64, const QString&, const QString&, bool) { otherAnswers++; });
    QObject::connect(&host, &dsd_qt::DecoderHost::documentPickCancelled, &host,
                     [&otherAnswers](qint64) { otherAnswers++; });

    const qint64 openId = host.requestOpenDocument();
    expect("first request id is 1", openId == 1);
    expect("no answer before the open request returns", unavailableIds.isEmpty());
    const qint64 saveId =
        host.requestSaveDocument(QStringLiteral("dsd-neo-backup.dsdbak"), QStringLiteral("application/octet-stream"));
    expect("save request id is larger", saveId > openId);
    expect("no answer before the save request returns", unavailableIds.isEmpty());
    QCoreApplication::processEvents();
    expect("each request answered once, in order, with its own id", unavailableIds == QList<qint64>({openId, saveId}));
    expect("answered unsupported",
           reasons == QStringList({QStringLiteral("unsupported"), QStringLiteral("unsupported")}));
    expect("no pick or cancel answer", otherAnswers == 0);

    host.cancelDocumentRequest(saveId);
    host.discardPendingDownload(QStringLiteral("content://media/external/downloads/1"));
    expect("no saved name on a host without Downloads",
           host.savedDocumentName(QStringLiteral("content://media/external/downloads/1")).isEmpty());
    const qint64 nextId = host.requestOpenDocument();
    expect("ids keep rising after a cancel", nextId > saveId);
    QCoreApplication::processEvents();
    expect("the later request is answered too", unavailableIds.size() == 3 && unavailableIds.last() == nextId);

    /* Cancelled before its answer ran: no answer follows, and the request after it still gets its own. */
    const qint64 cancelledId = host.requestSaveDocument(QStringLiteral("a.zip"), QStringLiteral("application/zip"));
    host.cancelDocumentRequest(cancelledId);
    const qint64 keptId = host.requestOpenDocument();
    QCoreApplication::processEvents();
    expect("a request cancelled before its answer gets none",
           unavailableIds.size() == 4 && !unavailableIds.contains(cancelledId) && unavailableIds.last() == keptId);
    expect("the property publishes the capability",
           host.property("documentPickerSupported").isValid() && !host.property("documentPickerSupported").toBool());
}

namespace {
/* A host that offers documents as a platform's poll does (#831). */
class OfferingHost : public TestHost {
  public:
    using dsd_qt::DecoderHost::offerIncomingDocument;
};
} // namespace

/* Issue #831: one incoming document waits at a time, a newer one replacing one not taken, and is taken once. The base
   host never offers one, and has no picker request pending. */
static void
test_incoming_document_slot() {
    OfferingHost host;
    int changes = 0;
    QObject::connect(&host, &dsd_qt::DecoderHost::incomingDocumentChanged, &host, [&changes] { changes++; });
    expect("nothing waits at first",
           !host.incomingDocumentPending() && !host.property("incomingDocumentPending").toBool());
    expect("no picker request pending on the base host",
           !host.documentRequestPending() && host.property("documentRequestPending").isValid());
    expect("an empty take is empty", host.takeIncomingDocument().isEmpty());
    expect("an empty take tells nothing", changes == 0);
    host.offerIncomingDocument(QString());
    expect("an empty reference is no document", changes == 0 && !host.incomingDocumentPending());

    // A mixed-case authority and escapes, which must come back as they were (never through QUrl).
    const QString first = QStringLiteral("content://com.Example.Provider/document/primary%3ADownload%2Fa%20b.csv");
    const QString second = QStringLiteral("content://com.Other.Files/tree/x/document/b.dsdbak");
    host.offerIncomingDocument(first);
    expect("an offer waits and is told", host.incomingDocumentPending() && changes == 1);
    host.offerIncomingDocument(second);
    expect("a newer offer is told", changes == 2 && host.property("incomingDocumentPending").toBool());
    expect("the newer document replaces the one not taken", host.takeIncomingDocument() == second);
    expect("a take empties the slot and is told", !host.incomingDocumentPending() && changes == 3);
    expect("it is taken once", host.takeIncomingDocument().isEmpty() && changes == 3);
    host.offerIncomingDocument(first);
    expect("a later offer comes back as it was sent", host.takeIncomingDocument() == first && changes == 5);
    (void)host.requestOpenDocument();
    expect("a refused request is not pending", !host.documentRequestPending());
    QCoreApplication::processEvents();
}

namespace {

/* Every file browser answer a host gave, in order, as {signal, id, result}; an access outcome is {outcome}. */
struct FileAnswers {
    QVariantList answers;

    explicit FileAnswers(dsd_qt::DecoderHost& host) {
        QObject::connect(&host, &dsd_qt::DecoderHost::fileRootsListed, &host,
                         [this](qint64 id, const QVariantMap& result) { note("roots", id, result); });
        QObject::connect(&host, &dsd_qt::DecoderHost::folderListed, &host,
                         [this](qint64 id, const QVariantMap& result) { note("folder", id, result); });
        QObject::connect(&host, &dsd_qt::DecoderHost::fileHandoffChecked, &host,
                         [this](qint64 id, const QVariantMap& result) { note("handoff", id, result); });
        QObject::connect(
            &host, &dsd_qt::DecoderHost::fileAccessRequestDone, &host,
            [this](qint64 id, const QString& outcome) { note("access", id, {{QStringLiteral("outcome"), outcome}}); });
    }

    void
    note(const char* signal, qint64 id, const QVariantMap& result) {
        answers.append(QVariantMap{{QStringLiteral("signal"), QString::fromLatin1(signal)},
                                   {QStringLiteral("id"), id},
                                   {QStringLiteral("result"), result}});
    }

    /* How many answers request @p id got. */
    int
    count(qint64 id) const {
        int n = 0;
        for (const QVariant& answer : answers) {
            n += answer.toMap().value(QStringLiteral("id")).toLongLong() == id ? 1 : 0;
        }
        return n;
    }

    /* Whether request @p id got exactly one answer, on @p signal, with @p result. */
    bool
    once(qint64 id, const char* signal, const QVariantMap& result) const {
        if (count(id) != 1) {
            return false;
        }
        for (const QVariant& answer : answers) {
            const QVariantMap entry = answer.toMap();
            if (entry.value(QStringLiteral("id")).toLongLong() == id) {
                return entry.value(QStringLiteral("signal")).toString() == QLatin1String(signal)
                       && entry.value(QStringLiteral("result")).toMap() == result;
            }
        }
        return false;
    }
};

QVariantMap
status_map(const char* status) {
    return {{QStringLiteral("status"), QString::fromLatin1(status)}};
}

/* Every roots answer has the roots' shape, a failure's too: no generation and no roots. */
QVariantMap
roots_map(const char* status) {
    return {{QStringLiteral("status"), QString::fromLatin1(status)},
            {QStringLiteral("generation"), qint64(0)},
            {QStringLiteral("roots"), QVariantList()}};
}

} // namespace

/* Issue #830: a host without a file browser opens sessions and answers every request "unsupported", queued and once;
   cancel, session close and a superseding listing turn a pending answer into "cancelled". */
static void
test_file_browser_defaults() {
    TestHost host;
    expect("the base host has no file browser", !host.fileBrowserSupported());
    expect("the property publishes the browser capability",
           host.property("fileBrowserSupported").isValid() && !host.property("fileBrowserSupported").toBool());
    const QVariantMap access{{QStringLiteral("declared"), false},
                             {QStringLiteral("granted"), false},
                             {QStringLiteral("route"), QStringLiteral("none")},
                             {QStringLiteral("apiLevel"), 0},
                             {QStringLiteral("rootsGeneration"), 0}};
    expect("the base host declares no file access", host.fileAccess() == access);
    expect("the property publishes the file access", host.property("fileAccess").toMap() == access);
    host.refreshFileAccess();
    expect("a refresh changes nothing on the base host", host.fileAccess() == access);
    FileAnswers answers(host);
    int accessChanges = 0;
    QObject::connect(&host, &dsd_qt::DecoderHost::fileAccessChanged, &host, [&accessChanges] { accessChanges++; });

    const qint64 s = host.openFileBrowserSession();
    expect("a session id is positive", s > 0);
    const qint64 r = host.requestFileRoots(s);
    const qint64 l = host.requestFolderListing(
        s, QStringLiteral("inbox"), 1, QStringLiteral("/x"),
        {{QStringLiteral("kind"), QStringLiteral("csv")}, {QStringLiteral("showAll"), false}});
    const qint64 p = host.requestMorePage(s, 7, 500);
    const qint64 h = host.requestFileHandoff(s, QStringLiteral("inbox"), 1, QStringLiteral("file:///x/a.csv"));
    const qint64 a = host.requestFileAccess(s);
    expect("each file request id is larger than the last", r > s && l > r && p > l && h > p && a > h);
    expect("no file answer before the requests return", answers.answers.isEmpty());
    expect("a second access request joins the live one", host.requestFileAccess(s) == a);
    QCoreApplication::processEvents();
    const QVariantMap unsupportedRoots{{QStringLiteral("status"), QStringLiteral("unsupported")},
                                       {QStringLiteral("generation"), 0},
                                       {QStringLiteral("roots"), QVariantList()}};
    expect("roots answered unsupported once", answers.once(r, "roots", unsupportedRoots));
    expect("the listing the page superseded is cancelled once", answers.once(l, "folder", status_map("cancelled")));
    expect("the page answered unsupported once", answers.once(p, "folder", status_map("unsupported")));
    expect("the hand-off answered unsupported once",
           answers.once(h, "handoff",
                        {{QStringLiteral("ok"), false}, {QStringLiteral("reason"), QStringLiteral("unsupported")}}));
    expect("the access request answered unsupported once",
           answers.once(a, "access", {{QStringLiteral("outcome"), QStringLiteral("unsupported")}}));
    expect("five answers in all", answers.answers.size() == 5);
    QCoreApplication::processEvents();
    expect("nothing answers twice", answers.answers.size() == 5);
    expect("the access request ends once answered: the next one is new", host.requestFileAccess(s) > a);
    QCoreApplication::processEvents();

    /* Cancelled before its answer ran: one "cancelled", never the "unsupported" it had queued. */
    answers.answers.clear();
    const qint64 x = host.requestFileRoots(s);
    host.cancelFileRequest(x);
    host.cancelFileRequest(x);
    const qint64 accessX = host.requestFileAccess(s);
    host.cancelFileRequest(accessX);
    expect("an access request cancelled is not joined", host.requestFileAccess(s) > accessX);
    QCoreApplication::processEvents();
    expect("a cancelled request is answered cancelled once", answers.once(x, "roots", roots_map("cancelled")));
    expect("a cancelled access request is answered cancelled once",
           answers.once(accessX, "access", {{QStringLiteral("outcome"), QStringLiteral("cancelled")}}));
    host.cancelFileRequest(x);
    host.cancelFileRequest(999999);
    QCoreApplication::processEvents();
    expect("cancelling an answered or unknown id answers nothing", answers.count(x) == 1 && answers.count(999999) == 0);

    /* An access request from a closed or unknown session never joins the pending one: it gets its own id, cancelled,
       and the pending request keeps its answer. */
    answers.answers.clear();
    const qint64 pending = host.requestFileAccess(s);
    const qint64 gone = host.openFileBrowserSession();
    host.closeFileBrowserSession(gone);
    const qint64 fromClosed = host.requestFileAccess(gone);
    const qint64 fromUnknown = host.requestFileAccess(-1);
    expect("an access request from a closed or unknown session is its own",
           fromClosed > pending && fromUnknown > fromClosed && host.requestFileAccess(s) == pending);
    QCoreApplication::processEvents();
    expect("the pending access request keeps its own answer",
           answers.once(pending, "access", {{QStringLiteral("outcome"), QStringLiteral("unsupported")}}));
    expect("an access request from a closed or unknown session is cancelled",
           answers.once(fromClosed, "access", {{QStringLiteral("outcome"), QStringLiteral("cancelled")}})
               && answers.once(fromUnknown, "access", {{QStringLiteral("outcome"), QStringLiteral("cancelled")}}));

    /* Closing the session cancels its live requests, and a request naming it afterwards is cancelled. */
    answers.answers.clear();
    const qint64 other = host.openFileBrowserSession();
    const qint64 kept = host.requestFileRoots(other);
    const qint64 first = host.requestFileRoots(s);
    const qint64 second = host.requestFileHandoff(s, QStringLiteral("saved"), 2, QStringLiteral("content://x/1"));
    host.closeFileBrowserSession(s);
    const qint64 late = host.requestFolderListing(s, QStringLiteral("inbox"), 1, QStringLiteral("/x"), {});
    const qint64 unknown = host.requestFileRoots(other + 1000);
    QCoreApplication::processEvents();
    expect(
        "closing a session cancels each live request once",
        answers.once(first, "roots", roots_map("cancelled"))
            && answers.once(second, "handoff",
                            {{QStringLiteral("ok"), false}, {QStringLiteral("reason"), QStringLiteral("cancelled")}}));
    expect("a request for a closed session is cancelled", answers.once(late, "folder", status_map("cancelled")));
    expect("a request for an unknown session is cancelled", answers.once(unknown, "roots", roots_map("cancelled")));
    expect("another session's request keeps its own answer", answers.once(kept, "roots", unsupportedRoots));
    host.closeFileBrowserSession(other);
    QCoreApplication::processEvents();
    expect("closing a session with nothing live answers nothing", answers.answers.size() == 5);
    expect("file access never changed on the base host", accessChanges == 0);
}

namespace {

/* A host whose file requests stay live until settled, as a platform host's do until its record comes: the protected
   helpers under test, and the retirements it is told of. */
class LiveFileHost : public TestHost {
  public:
    using DecoderHost::FileRequestKind;
    QList<qint64> retired;

    qint64
    begin(qint64 sessionId, FileRequestKind kind) {
        return beginFileRequest(sessionId, kind);
    }

    bool
    live(qint64 id) const {
        return fileRequestLive(id);
    }

    bool
    settle(qint64 id, const QVariantMap& result) {
        return settleFileRequest(id, result);
    }

    void
    settleLater(qint64 id, const QVariantMap& result) {
        settleFileRequestLater(id, result);
    }

    qint64
    liveAccess() const {
        return liveFileAccessRequest();
    }

  protected:
    void
    fileRequestRetired(qint64 requestId) override {
        retired.append(requestId);
    }
};

} // namespace

/* Issue #830: the helpers a platform host answers through. A cancel, a session close and a superseding listing retire
   a live request once (fileRequestRetired()) and make a late record a drop (settleFileRequest() false); one access
   request is live app-wide, and only an open session joins it. */
static void
test_file_browser_live_requests() {
    using Kind = LiveFileHost::FileRequestKind;
    LiveFileHost host;
    FileAnswers answers(host);
    const QVariantMap ok = status_map("ok");
    const qint64 s = host.openFileBrowserSession();

    const qint64 r = host.begin(s, Kind::Roots);
    expect("a begun request is live", host.live(r));
    host.cancelFileRequest(r);
    expect("a live cancel is retired once", host.retired == QList<qint64>({r}) && !host.live(r));
    expect("a record after the cancel is dropped", !host.settle(r, ok));
    host.cancelFileRequest(r);
    expect("a second cancel retires nothing more", host.retired.size() == 1);

    const qint64 l = host.begin(s, Kind::Listing);
    const qint64 p = host.begin(s, Kind::Page);
    expect("a page retires the session's live listing", host.retired == QList<qint64>({r, l}) && host.live(p));
    expect("a record for the superseded listing is dropped", !host.settle(l, ok));
    expect("the live page is answered now", host.settle(p, ok) && answers.once(p, "folder", ok));
    expect("a page answers only once", !host.settle(p, ok));

    const qint64 h = host.begin(s, Kind::Handoff);
    const qint64 a = host.begin(s, Kind::Access);
    expect("the access request is the one live", host.liveAccess() == a);
    expect("a second access begin joins it", host.begin(s, Kind::Access) == a && host.live(a));
    const qint64 gone = host.openFileBrowserSession();
    host.closeFileBrowserSession(gone);
    const qint64 fromClosed = host.begin(gone, Kind::Access);
    const qint64 fromUnknown = host.begin(-1, Kind::Access);
    expect("an access begin from a closed or unknown session is its own and not live",
           fromClosed > gone && fromUnknown > fromClosed && !host.live(fromClosed) && !host.live(fromUnknown));
    expect("and leaves the live access request alone", host.liveAccess() == a && host.live(a));
    expect("a request for a session not open is never retired on the platform", host.retired.size() == 2);

    host.closeFileBrowserSession(s);
    expect("closing the session retires each live request once",
           host.retired == QList<qint64>({r, l, h, a}) && !host.live(h) && !host.live(a));
    expect("records after the close are dropped",
           !host.settle(h, ok) && !host.settle(a, {{QStringLiteral("outcome"), QStringLiteral("granted")}}));
    expect("no access request is live after the close", host.liveAccess() == 0);

    /* A queued answer is not live: a cancel turns it into "cancelled" without a platform retirement. */
    const qint64 s3 = host.openFileBrowserSession();
    const qint64 q = host.begin(s3, Kind::Roots);
    host.settleLater(q, ok);
    expect("a request with a queued answer is not live", !host.live(q) && !host.settle(q, ok));
    host.cancelFileRequest(q);
    expect("cancelling a queued answer retires nothing on the platform", host.retired.size() == 4);
    const qint64 a2 = host.begin(s3, Kind::Access);
    expect("a new access request after the close", a2 > q && host.liveAccess() == a2);
    expect("an access record settles it now",
           host.settle(a2, {{QStringLiteral("outcome"), QStringLiteral("granted")}})
               && answers.once(a2, "access", {{QStringLiteral("outcome"), QStringLiteral("granted")}})
               && host.liveAccess() == 0);

    QCoreApplication::processEvents();
    const QVariantMap cancelled = status_map("cancelled");
    expect("each retired request is answered cancelled once",
           answers.once(r, "roots", roots_map("cancelled")) && answers.once(l, "folder", cancelled)
               && answers.once(h, "handoff",
                               {{QStringLiteral("ok"), false}, {QStringLiteral("reason"), QStringLiteral("cancelled")}})
               && answers.once(a, "access", {{QStringLiteral("outcome"), QStringLiteral("cancelled")}}));
    expect("requests for a session not open are answered cancelled",
           answers.once(fromClosed, "access", {{QStringLiteral("outcome"), QStringLiteral("cancelled")}})
               && answers.once(fromUnknown, "access", {{QStringLiteral("outcome"), QStringLiteral("cancelled")}}));
    expect("a queued answer cancelled before it ran is cancelled once",
           answers.once(q, "roots", roots_map("cancelled")));
    expect("the settled page is not answered again", answers.count(p) == 1);
    expect("nine answers in all", answers.answers.size() == 9);
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    /* Isolated, disposable storage — nothing this test writes may touch a real
     * profile (same arrangement as test_ui_qt_persistence). */
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-imported-files-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();

    {
        TestHost host;
        expect("desktop has no location/share capability", !host.locationSupported() && !host.shareSupported());
        expect("desktop has no local device failure", host.localDeviceFailureKind() == 0);
        bool answered = false;
        QObject::connect(&host, &dsd_qt::DecoderHost::locationResult, &host,
                         [&answered](qint64 id, bool fixOk, double, double, double, qint64, bool geocodeOk,
                                     const QString&, const QString&, const QString& error) {
                             answered = id == 42 && !fixOk && !geocodeOk && !error.isEmpty();
                         });
        host.requestCurrentLocation(42);
        expect("unsupported location answers with same request id", answered);
        host.cancelLocationRequest(42);
        host.shareDiagnostics("content", "title");
        host.hostDiagnostic("test lifecycle line");
        expect("initialization signal is available", host.metaObject()->indexOfSignal("sessionInitialized()") >= 0);
    }
    test_metadata_failure_keeps_file();
    test_user_replace_lets_go_first();
    test_update_waits_for_decoder_rewrite(false);
    test_update_waits_for_decoder_rewrite(true);
    test_failed_update_leaves_the_file(false);
    test_failed_update_leaves_the_file(true);
    test_remove_finishes_on_retry();
    test_async_import_outcomes();
    test_async_replace_stale();
    test_group_replace_busy();
    test_replace_failures();
    test_replace_double_failure();
    test_cancel_does_not_wait_for_the_copy();
    test_inspect_document();
    test_cancel_inspect();
    test_register_unsaved_export();
    test_entries_changed_on_replace();
    test_stored_names_kept();
    test_async_channel_bundles();
    test_async_companion_retry();
    test_async_nested_target_bundle();
    test_async_bundle_limits_and_failures();
    test_async_target_write_failure();
    test_startup_revision_cleanup();
    test_startup_cleanup_asks_the_live_engine();
    test_reason_under_ascii_logging();
    test_journal_recovery();
    test_journal_recovery_keeps_named_revisions();
    test_cleanup_keeps_revisions_other_rows_reach();
#ifndef _WIN32
    test_cleanup_matches_linked_spellings();
#endif
    test_channel_bundle();
    test_channel_bundle_quoted_options();
    test_channel_review_bandwidth();
    test_channel_review_tone_filter();
    test_target_bundle();
    test_library_companions();
    test_library_candidate_ranking();
    test_duplicate_library_names();
    test_library_warning_display_names();
    test_channel_companion_roles();
    test_channel_companion_uri_forms();
    test_nested_companion_uri_forms();
    test_target_channel_primary_collision();
    test_target_key_primary_collision();
    test_target_detached_copy();
    test_confined_companion_lookup();
    test_target_errors_and_opaque_columns();
    test_example_targets();
    test_export_registration_in_place();
    test_document_picker_defaults();
    test_incoming_document_slot();
    test_file_browser_defaults();
    test_file_browser_live_requests();
    test_group_list_counts_read_the_file_now();
    test_imported_files_model();
    test_update_rejects_invalid_pick();
    test_replace_by_stored_path();
    test_replace_validates_before_touching_the_stored_file();
    test_generated_import_and_refresh();
    test_p25_bandplan_kind();
    test_src_kind();
    test_legacy_store_without_provenance();
    test_store_past_int_range();
    test_unreadable_index_is_kept();
#ifndef _WIN32
    test_unsearchable_copy_is_not_pruned();
#endif

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
