// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the Qt frontend's imported-files layer — DecoderHost's desktop
 * importDocument() default (copy into the app's imports dir, unique-ify on
 * collision, atomic replace on update) and the ImportedFilesModel library the
 * wizard and imports screen share. Registered only when the Qt frontend is
 * enabled (DSD_ENABLE_QT_UI), since these link Qt. */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <stdio.h>

#include "decoder_host.h"
#include "dsd-neo/core/safe_api.h"
#include "imported_files_model.h"

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
    }
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

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
