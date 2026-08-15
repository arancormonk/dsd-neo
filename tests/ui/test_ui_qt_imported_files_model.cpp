// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the Qt frontend's imported-files layer — DecoderHost's desktop
 * importDocument() default (copy into the app's imports dir, unique-ify on
 * collision, atomic replace on update). Registered only when the Qt frontend
 * is enabled (DSD_ENABLE_QT_UI), since these link Qt. */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <stdio.h>

#include "decoder_host.h"
#include "dsd-neo/core/safe_api.h"

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

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
