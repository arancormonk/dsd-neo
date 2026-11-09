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
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdio.h>
#include <utility>
#include "../test_support/qt_test_paths.h"

#include <cerrno>
#include <cstdlib>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/scan_options.h>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <qflags.h>
#include <stdint.h>
extern "C" {
#include <dsd-neo/core/state.h>
}
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include "csv_bundle_import.h"
#include "decoder_host.h"
#include "document_import_transport.h"
#include "import_service.h"
#include "imported_files_model.h"
#include "json_store.h"

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

/* The stored path of library row @p row, or one naming no row (a replace of it is refused as "path"). */
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
    expect("a replace of no row is refused", missing.value("error") == "path");
    const QVariantMap wrongType = importAndWait(model, repick, "repick.csv", "group", stored);
    expect("a replace as another type is refused", wrongType.value("error") == "path");
    expect("refusals change nothing", model.count() == 1 && read_file(stored) == read_file(groups));
    model.remove(model.rowForPath(stored));
}

/* Issue #832 (B11): the engine appends learned talkgroups to a group list and rewrites it on policy edits while it
 * runs, so replacing one is refused while the decoder runs: at the start and again at the commit. */
void
test_group_replace_busy() {
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
    QTemporaryDir source;
    const QString groups = source.filePath("groups.csv");
    const QString repick = source.filePath("repick.csv");
    const QByteArray kept("TG,Mode,Name\n1,D,One\n");
    expect("busy fixtures", write_file(groups, kept) && write_file(repick, "TG,Mode,Name\n2,D,Two\n3,D,Three\n"));
    const QString stored = importAndWait(model, groups, "groups.csv", "group").value("path").toString();
    const int row = model.rowForPath(stored);

    host.running = true;
    const QVariantMap atStart = importAndWait(model, repick, "repick.csv", "group", stored);
    expect("a group replace is refused at the start while decoding",
           atStart.value("error") == "busy"
               && atStart.value("detail") == "Stop the decoder before replacing a talkgroup list.");
    expect("a refused replace keeps the file", read_file(stored) == kept && model.get(row).value("accepted") == 1);

    host.running = false;
    QSignalSpy spy(&model, &dsd_qt::ImportedFilesModel::importFinished);
    const int id = model.beginImport(repick, "repick.csv", "group", {}, stored);
    host.running = true;
    const QVariantMap atCommit = waitForImport(spy, id);
    expect("a group replace is refused at the commit once decoding started", atCommit.value("error") == "busy");
    expect("a replace refused at the commit keeps the file",
           read_file(stored) == kept && model.get(row).value("accepted") == 1 && indexedAccepted(stored) == 1);

    host.running = false;
    const QVariantMap stopped = importAndWait(model, repick, "repick.csv", "group", stored);
    expect("a group replace runs once the decoder stopped",
           stopped.value("ok").toBool() && stopped.value("path") == stored && model.get(row).value("accepted") == 2);
    model.remove(row);
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
    test_remove_finishes_on_retry();
    test_async_import_outcomes();
    test_async_replace_stale();
    test_group_replace_busy();
    test_replace_failures();
    test_replace_double_failure();
    test_cancel_does_not_wait_for_the_copy();
    test_register_unsaved_export();
    test_entries_changed_on_replace();
    test_stored_names_kept();
    test_async_channel_bundles();
    test_async_companion_retry();
    test_async_nested_target_bundle();
    test_async_bundle_limits_and_failures();
    test_async_target_write_failure();
    test_startup_revision_cleanup();
    test_journal_recovery();
    test_journal_recovery_keeps_named_revisions();
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
    test_target_errors_and_opaque_columns();
    test_example_targets();
    test_export_registration_in_place();
    test_imported_files_model();
    test_update_rejects_invalid_pick();
    test_replace_validates_before_touching_the_stored_file();
    test_generated_import_and_refresh();
    test_p25_bandplan_kind();
    test_src_kind();
    test_legacy_store_without_provenance();
    test_store_past_int_range();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
