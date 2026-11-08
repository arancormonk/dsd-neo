// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the shared stored-import naming rule (sanitized_import_name,
 * unique_import_destination). */

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <cstdio>

#include "import_names.h"

using dsd_qt::sanitized_import_name;
using dsd_qt::unique_import_destination;

namespace {

int failures = 0;

void
check_at(bool ok, int line) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "test_ui_qt_import_names.cpp:%d: check failed\n", line);
    }
}

#define CHECK(cond) check_at((cond), __LINE__)

void
touch(const QDir& dir, const QString& name) {
    QFile f(dir.filePath(name));
    CHECK(f.open(QIODevice::WriteOnly));
}

void
test_names() {
    CHECK(sanitized_import_name(QStringLiteral("a.csv"), QStringLiteral("b.csv")) == QStringLiteral("a.csv"));
    CHECK(sanitized_import_name(QString(), QStringLiteral("raw%3A%2Fstorage%2Femulated%2F0%2Fcapture.wav"))
          == QStringLiteral("capture.wav"));
    CHECK(sanitized_import_name(QStringLiteral("  "), QStringLiteral("x.csv")) == QStringLiteral("x.csv"));
    CHECK(sanitized_import_name(QStringLiteral("a/b/c.csv"), QString()) == QStringLiteral("c.csv"));
    CHECK(sanitized_import_name(QString(), QString()) == QStringLiteral("import.bin"));
    CHECK(sanitized_import_name(QStringLiteral("."), QString()) == QStringLiteral("import.bin"));
    CHECK(sanitized_import_name(QStringLiteral(".."), QString()) == QStringLiteral("import.bin"));
    CHECK(sanitized_import_name(QStringLiteral("x/.."), QString()) == QStringLiteral("import.bin"));
    CHECK(sanitized_import_name(QStringLiteral(".import-list.tmp"), QString()) == QStringLiteral(".import-list.tmp"));
    CHECK(sanitized_import_name(QStringLiteral(".staged-x.csv"), QString()) == QStringLiteral(".staged-x.csv"));
    CHECK(sanitized_import_name(QStringLiteral("name. "), QString()) == QStringLiteral("name"));
}

void
test_long_name() {
    QString base;
    for (int i = 0; i < 75; i++) {
        base += QString::fromUcs4(U"\U0001F600"); // 4 bytes each, 300 bytes
    }
    const QString out = sanitized_import_name(base + QStringLiteral(".csv"), QString());
    const QByteArray utf8 = out.toUtf8();
    CHECK(utf8.size() <= 247);
    CHECK(out.endsWith(QStringLiteral(".csv")));
    CHECK(QString::fromUtf8(utf8) == out);
    CHECK(!out.contains(QChar(0xFFFD)));
    CHECK(out.size() > 10);
    // A name that already fits is untouched.
    const QString fits = QString(243, QLatin1Char('a')) + QStringLiteral(".csv");
    CHECK(sanitized_import_name(fits, QString()) == fits);
}

void
test_unique() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QDir dir(tmp.path());
    const QSet<QString> none;
    CHECK(unique_import_destination(dir, QStringLiteral("chan.csv"), none) == dir.filePath(QStringLiteral("chan.csv")));
    touch(dir, QStringLiteral("chan.csv"));
    CHECK(unique_import_destination(dir, QStringLiteral("chan.csv"), none)
          == dir.filePath(QStringLiteral("chan (2).csv")));

    const QString reservedName = QStringLiteral("talkgroups-1234.csv");
    QSet<QString> reserved;
    reserved.insert(dir.absoluteFilePath(reservedName));
    CHECK(unique_import_destination(dir, reservedName, reserved)
          == dir.filePath(QStringLiteral("talkgroups-1234 (2).csv")));

    CHECK(dir.mkdir(QStringLiteral("bundles")));
    CHECK(unique_import_destination(dir, QStringLiteral("bundles"), none)
          == dir.filePath(QStringLiteral("bundles (2)")));

    const QString dangling = dir.filePath(QStringLiteral("link.csv"));
    if (QFile::link(dir.filePath(QStringLiteral("missing-target")), dangling) && QFileInfo(dangling).isSymLink()) {
        CHECK(unique_import_destination(dir, QStringLiteral("link.csv"), none)
              == dir.filePath(QStringLiteral("link (2).csv")));
    }

    // Exhausted: (2) through (999) all taken.
    touch(dir, QStringLiteral("full"));
    for (int i = 2; i <= 999; i++) {
        touch(dir, QStringLiteral("full (%1)").arg(i));
    }
    CHECK(unique_import_destination(dir, QStringLiteral("full"), none).isEmpty());
}

} // namespace

int
main() {
    test_names();
    test_long_name();
    test_unique();
    return failures ? 1 : 0;
}
