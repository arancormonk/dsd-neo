// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the Qt frontend's imported-files layer — DecoderHost's desktop
 * importDocument() default (copy into the app's imports dir, unique-ify on
 * collision, atomic replace on update) and the ImportedFilesModel library the
 * wizard and imports screen share. Registered only when the Qt frontend is
 * enabled (DSD_ENABLE_QT_UI), since these link Qt. */

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
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <stdio.h>
#include <utility>
#include "../test_support/qt_test_paths.h"

#include <cstdlib>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_options.h>
#include <initializer_list>
#include <qflags.h>
#include <stdint.h>
extern "C" {
#include <dsd-neo/core/state.h>
}
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include "decoder_host.h"
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

class TestHost : public dsd_qt::DecoderHost {
  public:
    bool running = false;
    QMap<QString, QString> documents;

    QString
    importDocument(const QString& reference, const QString& name, const QString& replace = QString()) override {
        return DecoderHost::importDocument(documents.value(reference, reference), name, replace);
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
    QStringList references;

    QString
    importDocument(const QString& reference, const QString& name, const QString& replace = QString()) override {
        references.append(reference);
        if (QUrl(reference).scheme().isEmpty()) {
            return {};
        }
        return TestHost::importDocument(reference, name, replace);
    }
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

void
test_import_document(void) {
    TestHost host;
    QTemporaryDir sourceDir;
    expect("source dir created", sourceDir.isValid());
    const QString importsDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/imports");

    const QString sourceA = sourceDir.filePath(QStringLiteral("chan.csv"));
    expect("source A written", write_file(sourceA, "channel,freq\n1,851000000\n"));
    const QString imported = host.importDocument(QUrl::fromLocalFile(sourceA).toString(), QStringLiteral("chan.csv"));
    expect("import returns a path inside imports dir", imported.startsWith(importsDir));
    expect("import keeps the display name", imported.endsWith(QStringLiteral("/chan.csv")));
    expect("imported copy has the source content", read_file(imported) == "channel,freq\n1,851000000\n");

    /* A different document with the same display name must not overwrite the
     * first import — two agencies both ship a "chan.csv". */
    const QString sourceB = sourceDir.filePath(QStringLiteral("other/chan.csv"));
    expect("source B dir created", QDir(sourceDir.path()).mkpath(QStringLiteral("other")));
    expect("source B written", write_file(sourceB, "channel,freq\n2,852000000\n"));
    const QString importedB = host.importDocument(QUrl::fromLocalFile(sourceB).toString(), QStringLiteral("chan.csv"));
    expect("collision returns a distinct path", !importedB.isEmpty() && importedB != imported);
    expect("collision keeps the extension", importedB.endsWith(QStringLiteral(".csv")));
    expect("first import untouched by collision", read_file(imported) == "channel,freq\n1,851000000\n");
    expect("collision copy has its own content", read_file(importedB) == "channel,freq\n2,852000000\n");

    /* Update-in-place: replacePath re-uses the stored file so saved systems
     * keep pointing at the same path. */
    const QString sourceC = sourceDir.filePath(QStringLiteral("updated.csv"));
    expect("source C written", write_file(sourceC, "channel,freq\n9,860000000\n"));
    const QString replaced =
        host.importDocument(QUrl::fromLocalFile(sourceC).toString(), QStringLiteral("updated.csv"), imported);
    expect("replace returns the replaced path", replaced == imported);
    expect("replace updates the content", read_file(imported) == "channel,freq\n9,860000000\n");

    /* A replace target inside the imports dir that no longer exists is still a
     * write target: QFileInfo::canonicalPath() gives up on a missing leaf, and
     * falling back to a fresh unique copy would strand a file no library row
     * references while the update reports failure. */
    expect("stored copy removable", QFile::remove(imported));
    const QString recreated =
        host.importDocument(QUrl::fromLocalFile(sourceC).toString(), QStringLiteral("updated.csv"), imported);
    expect("missing replace target is recreated in place", recreated == imported);
    expect("recreated copy has the source content", read_file(imported) == "channel,freq\n9,860000000\n");

    /* replacePath outside the imports dir is not a write target — treat it as
     * a fresh import instead of scribbling wherever the caller points. */
    const QString outside = sourceDir.filePath(QStringLiteral("outside.csv"));
    expect("outside target written", write_file(outside, "original\n"));
    const QString redirected =
        host.importDocument(QUrl::fromLocalFile(sourceC).toString(), QStringLiteral("updated.csv"), outside);
    expect("outside replace lands in imports dir", redirected.startsWith(importsDir));
    expect("outside file untouched", read_file(outside) == "original\n");

    const QString missing = host.importDocument(QUrl::fromLocalFile(sourceDir.filePath("absent.csv")).toString(),
                                                QStringLiteral("absent.csv"));
    expect("missing source returns empty", missing.isEmpty());
}

void
test_imported_files_model(void) {
    /* Fresh app-data tree: the imports directory is shared state, and copies
     * left by the importDocument tests would unique-ify this test's names. */
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

        const QVariantMap imported = model.importFile(QUrl::fromLocalFile(groupSrc).toString(),
                                                      QStringLiteral("county.csv"), QStringLiteral("group"));
        expect("group import ok", imported.value(QStringLiteral("ok")).toBool());
        expect("group import counts accepted", imported.value(QStringLiteral("accepted")).toInt() == 2);
        expect("group import counts skipped", imported.value(QStringLiteral("skipped")).toInt() == 1);
        expect("group import has no error", imported.value(QStringLiteral("error")).toString().isEmpty());
        storedGroupPath = imported.value(QStringLiteral("path")).toString();
        expect("group import stores a path", !storedGroupPath.isEmpty() && QFile::exists(storedGroupPath));
        expect("group import adds a row", model.rowCount() == 1);

        const QVariantMap chan = model.importFile(QUrl::fromLocalFile(chanSrc).toString(), QStringLiteral("chan.csv"),
                                                  QStringLiteral("chan"));
        expect("chan import ok", chan.value(QStringLiteral("ok")).toBool());
        expect("chan import counts accepted", chan.value(QStringLiteral("accepted")).toInt() == 1);

        /* A parseable file with zero usable rows is kept — recoverable via
         * update — but flagged so the UI can warn instead of silently storing
         * a talkgroup list that names nothing. */
        const QVariantMap empty = model.importFile(QUrl::fromLocalFile(emptySrc).toString(),
                                                   QStringLiteral("header_only.csv"), QStringLiteral("group"));
        expect("empty import kept", empty.value(QStringLiteral("ok")).toBool());
        expect("empty import flagged", empty.value(QStringLiteral("error")).toString() == QStringLiteral("empty"));
        expect("empty import accepted zero", empty.value(QStringLiteral("accepted")).toInt() == 0);

        const QVariantMap bad = model.importFile(QUrl::fromLocalFile(sourceDir.filePath("absent.csv")).toString(),
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
            model.updateFile(0, QUrl::fromLocalFile(updatedSrc).toString(), QStringLiteral("updated.csv"));
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
 * updateFile() replaces the row's stored file. Validating only after the replace
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
    const QVariantMap imported = model.importFile(QUrl::fromLocalFile(groupSrc).toString(), QStringLiteral("group.csv"),
                                                  QStringLiteral("group"));
    expect("update-guard import ok", imported.value(QStringLiteral("ok")).toBool());
    const QString storedPath = imported.value(QStringLiteral("path")).toString();
    expect("update-guard stored copy matches", read_file(storedPath) == good);

    /* A directory is not a readable regular file, so the row's type validator
     * cannot parse it. */
    const QString unreadable = sourceDir.filePath(QStringLiteral("a-directory"));
    expect("update-guard directory created", QDir().mkpath(unreadable));

    const int before = model.get(0).value(QStringLiteral("accepted")).toInt();
    const QVariantMap rejected =
        model.updateFile(0, QUrl::fromLocalFile(unreadable).toString(), QStringLiteral("group.csv"));
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
                                              : model.updateFile(0, QUrl::fromLocalFile(replacementSrc).toString(),
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
        const QVariantMap picked = model.importFile(QUrl::fromLocalFile(pickSrc).toString(),
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
            model.updateFile(0, QUrl::fromLocalFile(ownSrc).toString(), QStringLiteral("mine.csv"));
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
    const QVariantMap plan = model.importFile(QUrl::fromLocalFile(planSrc).toString(), QStringLiteral("bandplan.csv"),
                                              QStringLiteral("p25Bandplan"));
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
    const QVariantMap wrong = model.importFile(QUrl::fromLocalFile(chanSrc).toString(), QStringLiteral("chan.csv"),
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
        model.importFile(QUrl::fromLocalFile(srcPath).toString(), QStringLiteral("src.csv"), QStringLiteral("src"));
    expect("src import ok", result.value(QStringLiteral("ok")).toBool());
    expect("src import has no error", result.value(QStringLiteral("error")).toString().isEmpty());
    expect("src import counts exact and range rows", result.value(QStringLiteral("accepted")).toInt() == 2);
    expect("src import stores its kind", result.value(QStringLiteral("type")).toString() == QStringLiteral("src"));

    const QVariantList entries = model.entriesForType(QStringLiteral("src"));
    expect("type filter finds the src row", entries.size() == 1);
    expect("src is not offered as talkgroups", model.entriesForType(QStringLiteral("group")).isEmpty());

    // A header-only source list is retained with an empty warning.
    const QVariantMap wrong =
        model.importFile(QUrl::fromLocalFile(emptyPath).toString(), QStringLiteral("empty.csv"), QStringLiteral("src"));
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
    const auto imported = model.importFile(original, "atomic.csv", "group");
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
           !model.updateFile(row, replacement, "replacement.csv").value("ok").toBool());
    expect("metadata failure preserves original bytes", read_file(path) == before);
    expect("metadata failure preserves counts", model.get(row).value("accepted").toInt() == 1);
    expect("unblock metadata write", QDir().rmdir(store) && QFile::rename(store + ".held", store));
    model.remove(row);
}

} // namespace

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
              "channel,frequency_hz,mode,keys_hex_csv,options\n1,461000000,dmr,keys.csv,--dmr-tg-key-csv 'map.csv'\n");
    auto result = model.importBundle(channels, "Bundle.csv", "chan", {});
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
    result = model.importBundle(channels, "Bundle.csv", "chan", {}, row);
    expect("missing companions are requested",
           !result.value("ok").toBool() && result.value("error").toString() == "companions");
    expect("missing update keeps previous file", primary.open(QIODevice::ReadOnly) && primary.readAll() == original);
    primary.close();
    const QString replacementKey = write("replacement.csv", "keyid,value\n02,12345\n");
    const QString badMap = write("invalid.csv", "tg_dec,keyid_hex\ninvalid,02\n");
    result =
        model.importBundle(channels, "Bundle.csv", "chan", {{"keys.csv", replacementKey}, {"map.csv", badMap}}, row);
    expect("invalid companion rejects whole update", !result.value("ok").toBool());
    expect("failed update keeps previous bundle", primary.open(QIODevice::ReadOnly) && primary.readAll() == original);
    primary.close();
    model.remove(row);
    expect("removing bundle frees companions", !QDir(root).exists());
}

static void
test_target_bundle() {
    QTemporaryDir source;
    TestHost host;
    dsd_qt::ImportedFilesModel model(&host);
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
                             "--scan-max-visit-ms 20000,auto,0,band.csv\r\n"
                             "plant,dmr-conventional,461112500,,250,1200,private,-H 0123456789 --no-force-key -G "
                             "\"groups file.csv\" --dmr-tg-key-csv='dmr.csv',gfsk,18,\r\n";
    const auto targets = write("targets.csv", csv);
    auto result = model.importFile(targets, "Targets.csv", "trunkTargets");
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
           !model.importBundle(targets, "Targets.csv", "trunkTargets", {}, row).value("ok").toBool());
    expect("active removal refused", !model.remove(row));
    host.running = false;
    write_file(band, "iden,base_hz,spacing_hz\n");
    result = model.importBundle(targets, "Targets.csv", "trunkTargets", {}, row);
    expect("empty bandplan rejects replacement",
           !result.value("ok").toBool() && result.value("detail").toString().contains("p25_bandplan_csv"));
    expect("rejected replacement preserves bytes", read_file(path) == original);
    write_file(band, "iden,base_hz,spacing_hz\n0,851000000,12500\n");
    result = model.importBundle(targets, "Targets.csv", "trunkTargets", {}, row);
    expect("replacement keeps stable path", result.value("ok").toBool() && result.value("path") == path);
    const QString root = model.get(row).value("bundleRoot").toString();
    expect("old revisions reclaimed", QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == 1);

    // SAF references convey no sibling access, even when our host can copy them.
    host.documents.insert("content://targets", targets);
    result = model.importBundle("content://targets", "Android.csv", "trunkTargets", {});
    expect("SAF asks for companions", result.value("error") == "companions");
    host.documents.insert("content://channels", channels);
    const QVariantMap sources{
        {"map, one.csv", "content://channels"}, {"groups file.csv", groups}, {"dmr.csv", dmr}, {"band.csv", band}};
    QVariantMap selected;
    const auto labels = result.value("requiredLabels").toMap();
    for (auto i = labels.cbegin(); i != labels.cend(); ++i) {
        selected[i.key()] = sources.value(i.value().toString());
    }
    result = model.importBundle("content://targets", "Android.csv", "trunkTargets", selected);
    const auto nestedLabels = result.value("requiredLabels").toMap();
    expect("nested SAF reference has context",
           nestedLabels.values().contains(QStringLiteral("map, one.csv → sub/key.csv")));
    for (auto i = nestedLabels.cbegin(); i != nestedLabels.cend(); ++i) {
        if (i.value() == QStringLiteral("map, one.csv → sub/key.csv")) {
            selected[i.key()] = keys;
        }
    }
    result = model.importBundle("content://targets", "Android.csv", "trunkTargets", selected);
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
    dsd_qt::ImportedFilesModel reloaded(&host);
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
    const auto map = model.importFile(QUrl::fromLocalFile(channel).toString(), "map.csv", "chan");
    const auto hex = model.importFile(QUrl::fromLocalFile(keys).toString(), "keys.csv", "keysHex");
    const QString mapPath = map.value("path").toString();
    const QString hexPath = hex.value("path").toString();
    host.documents.insert("content://targets", targets);
    const auto before = read_file(dsd_qt::json_store_path("imported_files.json"));
    auto pending = model.importBundle("content://targets", "Targets.csv", "trunkTargets", {});
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
    auto rejected =
        model.importBundle("content://targets", "Targets.csv", "trunkTargets", {{mapKey, mapPath}, {keyKey, hexPath}});
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
    const auto dec = model.importFile(QUrl::fromLocalFile(keys).toString(), "decimal.csv", "keysDec");
    host.references.clear();
    const auto imported = model.importBundle("content://targets", "Targets.csv", "trunkTargets",
                                             {{mapKey, mapPath}, {keyKey, dec.value("path")}});
    expect("library selections feed stager", imported.value("ok").toBool());
    expect("library channel and trunk leaf reach host as file URLs",
           host.references
               == QStringList{"content://targets", QUrl::fromLocalFile(mapPath).toString(),
                              QUrl::fromLocalFile(dec.value("path").toString()).toString()});
    const QString stored = imported.value("path").toString();
    const auto original = read_file(stored);
    const int row = model.rowForPath(stored);
    rejected = model.importBundle("content://targets", "Targets.csv", "trunkTargets",
                                  {{mapKey, mapPath}, {keyKey, hexPath}}, row);
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
    auto result = model.importBundle("content://targets", "Targets.csv", "trunkTargets", {});
    const QString slot = result.value("required").toStringList().value(0);
    const auto dec = model.importFile(keys, "keys.csv", "keysDec");
    model.importFile(keys, "another.csv", "keysDec");
    model.importFile(keys, "keys.csv", "keysHex");
    result = model.importBundle("content://targets", "Targets.csv", "trunkTargets", {{slot, "content://map"}});
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
    const QString channels = source.filePath("channels.csv");
    const QString keys = source.filePath("leaf.csv");
    expect("duplicate keys fixture", write_file(keys, "id,value\n1,12345\n"));
    expect("duplicate channels fixture",
           write_file(channels, "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,leaf.csv\n"));
    // Bundle primaries live under distinct roots, so equal library basenames
    // really occur even though plain library imports uniquify their paths.
    const auto first = model.importBundle(channels, "same.csv", "chan", {});
    const auto second = model.importBundle(channels, "same.csv", "chan", {});
    const auto hex = model.importFile(keys, "same.csv", "keysHex");
    expect("duplicate named bundles imported", first.value("ok").toBool() && second.value("ok").toBool());
    const QString targets = source.filePath("targets.csv");
    expect("duplicate target fixture",
           write_file(targets, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv\n"
                               "one,p25-trunk,851012500,same.csv,,,,same.csv\n"));
    host.documents.insert("content://targets", targets);
    const auto result = model.importBundle("content://targets", "Targets.csv", "trunkTargets", {});
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
    const auto dec = model.importFile(keys, "shared.csv", "keysDec");
    const auto hex = model.importFile(keys, "stored-hex.csv", "keysHex");
    // Persist a display name that differs from the private filename.
    auto rows = dsd_qt::json_store_load_array("imported_files.json");
    auto row = rows.at(model.rowForPath(hex.value("path").toString())).toObject();
    row.insert("name", "shared.csv");
    rows.replace(model.rowForPath(hex.value("path").toString()), row);
    expect("warning display name saved", dsd_qt::json_store_save_array("imported_files.json", rows));
    dsd_qt::ImportedFilesModel reloaded(&host);
    // A file can disappear after the model has loaded its library metadata.
    expect("same-kind stored file removed", QFile::remove(dec.value("path").toString()));
    const auto pending = reloaded.importBundle(channels, "Map.csv", "chan", {});
    const auto slot = pending.value("required").toStringList().value(0);
    const QString warning = "shared.csv is a hex key file; this bundle expects decimal keys";
    expect("missing same-kind file adds no blank warning",
           pending.value("requiredDetails").toMap().value(slot).toMap().value("warning") == warning);
    const auto rejected = reloaded.importBundle(channels, "Map.csv", "chan", {{slot, hex.value("path")}});
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
    const QString keys = source.filePath("keys.csv");
    expect("channel role keys", write_file(keys, "id,value\n1,12345\n"));
    const auto dec = model.importFile(QUrl::fromLocalFile(keys).toString(), "decimal.csv", "keysDec");
    const auto hex = model.importFile(QUrl::fromLocalFile(keys).toString(), "hex.csv", "keysHex");
    expect("channel role fixture",
           write_file(channels,
                      "channel,frequency,mode,options\n1,851012500,p25,-k same.csv\n2,852012500,p25,-K same.csv\n"));
    host.documents.insert("content://channels", channels);
    const auto pending = model.importBundle("content://channels", "Map.csv", "chan", {});
    const auto details = pending.value("requiredDetails").toMap();
    expect("channel roles get separate slots", pending.value("required").toStringList().size() == 2);
    QVariantMap selected;
    for (auto i = details.cbegin(); i != details.cend(); ++i) {
        selected.insert(i.key(), i.value().toMap().value("type") == "keysDec" ? dec.value("path") : hex.value("path"));
    }
    host.references.clear();
    const auto imported = model.importBundle("content://channels", "Map.csv", "chan", selected);
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
    const auto rejected = model.importBundle("content://channels", "Map.csv", "chan", selected);
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
    expect("URI host rejects scheme-less input", host.importDocument(keyCsvPath, "keys.csv").isEmpty());
    const QString fileUri = QUrl::fromLocalFile(keyCsvPath).toString(QUrl::FullyEncoded);
    const QString contentUri = "content://test.documents/document/keys%3A1?version=2";
    host.documents.insert("content://channels", channels);
    host.documents.insert(contentUri, keyCsvPath);
    const auto pending = model.importBundle("content://channels", "Map.csv", "chan", {});
    const QString slot = pending.value("required").toStringList().value(0);
    expect("URI channel requirement", !slot.isEmpty());
    for (const auto& selected : {keyCsvPath, fileUri, contentUri}) {
        host.references.clear();
        const auto imported = model.importBundle("content://channels", "Map.csv", "chan", {{slot, selected}});
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
    const auto pending = model.importBundle("content://targets", "Targets.csv", "trunkTargets", {});
    const QString mapSlot = pending.value("required").toStringList().value(0);
    expect("nested URI map requirement", !mapSlot.isEmpty());
    const QStringList mapForms{channels, channelUri, contentMap};
    const QStringList keyForms{keyCsvPath, keyUri, contentKeys};
    for (int i = 0; i < mapForms.size(); ++i) {
        const auto nested =
            model.importBundle("content://targets", "Targets.csv", "trunkTargets", {{mapSlot, mapForms[i]}});
        const auto missing = nested.value("required").toStringList();
        expect("nested URI leaf requirement", nested.value("error") == "companions" && missing.size() == 1);
        const auto identity = QJsonDocument::fromJson(missing.value(0).toUtf8()).array();
        expect("nested identity retains original selection",
               identity == QJsonArray{mapForms[i], "missing.csv", "keysDec"});
        host.references.clear();
        const auto imported = model.importBundle("content://targets", "Targets.csv", "trunkTargets",
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
    const auto imported = model.importFile(targets, "Targets.csv", "trunkTargets");
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
    const auto result = model.importBundle("content://replacement", "Replacement.csv", "trunkTargets", {}, row);
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
    const auto imported = model.importFile(targets, "Targets.csv", "trunkTargets");
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

    const auto result = model.importBundle(copy, "Edited.csv", "trunkTargets", {}, row);
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
        const auto result = model.importFile(path, "Targets.csv", "trunkTargets");
        expect("bad-key import fails atomically", !result.value("ok").toBool() && model.count() == before);
        const auto detail = result.value("detail").toString();
        expect("bad-key error provides row without key",
               detail.contains("row 2") && !detail.contains(QString::fromUtf8(secret)));
    }
    expect("write opaque optional column", write_file(path, header + ",chan_csv\n" + row + ",unused-missing.csv\n"));
    const auto result = model.importFile(path, "Opaque.csv", "trunkTargets");
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
    const auto result = model.importFile(QStringLiteral(DSD_NEO_TEST_EXAMPLES_DIR "/trunk_scan_targets.csv"),
                                         "Examples.csv", "trunkTargets");
    expect("shipped example imports all targets", result.value("ok").toBool() && result.value("accepted").toInt() == 8);
    if (!result.value("ok").toBool()) {
        return;
    }
    const QString path = result.value("path").toString();
    const auto rows = model.targetPreview(path).value("rows").toList();
    expect("shipped row order retained", rows.size() == 8 && rows[0].toMap().value("id") == "county-p25"
                                             && rows[7].toMap().value("id") == "field-nxdn48");
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
    opts->rtl_dsp_bw_khz = 48;
    opts->scan_max_visit_ms = 40000;
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", path.toUtf8().constData());
    dsd_trunk_tuning_hooks hooks{};
    hooks.tune_to_freq_request = acceptTune;
    hooks.tune_to_cc_request = acceptTune;
    dsd_trunk_tuning_hooks_set(hooks);
    char error[512] = {};
    const bool initialized = dsd_engine_trunk_scan_init(opts, state, error, sizeof error) == 0;
    expect("imported example initializes", initialized);
    if (initialized) {
        expect("engine owns all eight imported targets", dsd_engine_trunk_scan_target_count(state) == 8);
        expect("first target visit limit applies", opts->scan_max_visit_ms == 20000);
        expect("advance imported target",
               dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE) == 0);
        expect("next target inherits baseline limit", opts->scan_max_visit_ms == 40000);
    }
    dsd_engine_trunk_scan_shutdown(opts, state);
    dsd_trunk_tuning_hooks_set({});
    dsd_trunk_scan_hooks_set({});
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
    test_channel_bundle();
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
    test_import_document();
    test_imported_files_model();
    test_update_rejects_invalid_pick();
    test_replace_validates_before_touching_the_stored_file();
    test_generated_import_and_refresh();
    test_p25_bandplan_kind();
    test_src_kind();
    test_legacy_store_without_provenance();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
