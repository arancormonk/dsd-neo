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
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <stdio.h>

#include "decoder_host.h"
#include "dsd-neo/core/safe_api.h"
#include "imported_files_model.h"
#include "json_store.h"

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

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
    bool
    isRunning() const override {
        return false;
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

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    /* Isolated, disposable storage — nothing this test writes may touch a real
     * profile (same arrangement as test_ui_qt_persistence). */
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-imported-files-%1").arg(QCoreApplication::applicationPid()));
    QStandardPaths::setTestModeEnabled(true);
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();

    test_import_document();
    test_imported_files_model();
    test_update_rejects_invalid_pick();
    test_replace_validates_before_touching_the_stored_file();
    test_generated_import_and_refresh();
    test_legacy_store_without_provenance();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
