// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: what the licences screen shows. Where the source code of the Qt version the app is built with is, from
 * Qt's own version macros; then the list of every file the package's notice index names, in the index's order, so a
 * notice the packaging adds reaches the screen with no change to the app; and one listed file's text at a time. */

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLatin1Char>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtGlobal>
#include <cstdio>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <initializer_list>

#include "decoder_host.h"

namespace {

int failures = 0;

void
check_at(bool ok, int line, const char* why) {
    if (!ok) {
        ++failures;
        DSD_FPRINTF(stderr, "test_ui_qt_license_notices.cpp:%d: %s\n", line, why);
    }
}

#define CHECK(cond, why) check_at((cond), __LINE__, (why))

/* A host whose package keeps its notices in the folder a case names. */
class NoticeHost : public dsd_qt::DecoderHost {
  public:
    QString folder;

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

  protected:
    QString
    noticeDirectory() const override {
        return folder;
    }
};

bool
write_file(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(path.left(path.lastIndexOf(QLatin1Char('/'))));
#ifdef _WIN32
    /* The Windows helper opens a UTF-8 path by its UTF-16 name; the ANSI code page may not hold the temp folder's. */
    const QByteArray native = path.toUtf8();
#else
    const QByteArray native = QFile::encodeName(path);
#endif
    FILE* file = dsd_fopen_private(native.constData(), "wb");
    if (file == nullptr) {
        return false;
    }
    const size_t size = static_cast<size_t>(bytes.size());
    const bool written = std::fwrite(bytes.constData(), 1, size, file) == size;
    return std::fclose(file) == 0 && written;
}

QString
qt_version() {
    return QStringLiteral(QT_VERSION_STR);
}

QString
archive_url(const QString& module) {
    const QString series = QStringLiteral("%1.%2").arg(QT_VERSION_MAJOR).arg(QT_VERSION_MINOR);
    return QStringLiteral("https://download.qt.io/archive/qt/%1/%2/submodules/%3-everywhere-src-%2.tar.xz")
        .arg(series, qt_version(), module);
}

void
test_qt_source_section() {
    NoticeHost host;
    const QString text = host.qtSourceNotice();
    CHECK(text.startsWith(QStringLiteral("Qt source code\n")), "the section is headed as where Qt's source code is");
    CHECK(text.contains(QStringLiteral("This app includes Qt %1 ").arg(qt_version())),
          "it names the Qt version built with");
    CHECK(text.contains(QStringLiteral("GNU Lesser General Public License version 3")),
          "and the license Qt is used under");
    CHECK(text.contains(QStringLiteral("licenses/LGPL-3.0-only.txt")), "and where that license's text is");
    CHECK(text.contains(QStringLiteral("no patches")), "and that the app patches nothing in Qt");
    for (const QString& module : {QStringLiteral("qtbase"), QStringLiteral("qtdeclarative"), QStringLiteral("qtsvg")}) {
        CHECK(text.contains(archive_url(module)), "each Qt module's source archive is named at that version");
    }
    CHECK(text.contains(QStringLiteral("licenses/Qt-SOURCE.txt")),
          "and the file in the list that gives the same directions as the release, written from the build's Qt kit");
    CHECK(text.contains(QStringLiteral("Public Suffix List")),
          "which name the source form of the Public Suffix List at the commit the app carries");
    CHECK(text.size() < 2000, "the section is short: no notice file's text is part of it");
}

void
test_files_listed_and_read_one_at_a_time() {
    QTemporaryDir dir;
    NoticeHost host;
    host.folder = dir.path() + QStringLiteral("/doc");
    CHECK(write_file(host.folder + QStringLiteral("/LICENSE"), "GNU GENERAL PUBLIC LICENSE fixture\n"), "fixture");
    CHECK(write_file(host.folder + QStringLiteral("/licenses/Qt-NOTICES.txt"), "Qt notice fixture\n"), "fixture");
    CHECK(write_file(host.folder + QStringLiteral("/licenses/Added-NOTICES.txt"), "Added notice fixture\n"), "fixture");
    CHECK(write_file(host.folder + QStringLiteral("/licenses/unlisted.txt"), "Unlisted fixture\n"), "fixture");
    CHECK(write_file(dir.path() + QStringLiteral("/outside.txt"), "Outside fixture\n"), "fixture");
    CHECK(write_file(host.folder + QStringLiteral("/notice-index.txt"),
                     "LICENSE\nlicenses/Qt-NOTICES.txt\n\nlicenses/gone.txt\n../outside.txt\n"
                     "licenses/Added-NOTICES.txt\n"),
          "fixture");

    CHECK(host.licenseFiles()
              == QStringList({QStringLiteral("LICENSE"), QStringLiteral("licenses/Qt-NOTICES.txt"),
                              QStringLiteral("licenses/gone.txt"), QStringLiteral("../outside.txt"),
                              QStringLiteral("licenses/Added-NOTICES.txt")}),
          "the list is every name the index gives, in its order, blank lines left out");
    CHECK(host.licenseFileText(QStringLiteral("LICENSE")) == QStringLiteral("GNU GENERAL PUBLIC LICENSE fixture\n"),
          "a listed file reads as its text");
    CHECK(host.licenseFileText(QStringLiteral("licenses/Added-NOTICES.txt"))
              == QStringLiteral("Added notice fixture\n"),
          "a notice the index adds is read with no change to the app");
    const QString missing = QStringLiteral("This file is missing from the package.");
    CHECK(host.licenseFileText(QStringLiteral("licenses/gone.txt")) == missing,
          "a file the index names but the package lacks reads as missing");
    CHECK(host.licenseFileText(QStringLiteral("../outside.txt")) == missing, "a path out of the folder is never read");
    CHECK(host.licenseFileText(QStringLiteral("licenses/unlisted.txt")) == missing,
          "only a file the index names is read");
    CHECK(host.licenseFileText(QString()) == missing, "nor is an empty name");
}

void
test_no_index() {
    QTemporaryDir dir;
    NoticeHost host;
    host.folder = dir.path();
    CHECK(write_file(host.folder + QStringLiteral("/LICENSE"), "GNU GENERAL PUBLIC LICENSE fixture\n"), "fixture");
    CHECK(host.licenseFiles().isEmpty(), "a package without its notice index lists no file");
    CHECK(host.licenseFileText(QStringLiteral("LICENSE")) == QStringLiteral("This file is missing from the package."),
          "and reads none");
    host.folder.clear();
    CHECK(host.licenseFiles().isEmpty(), "nor does a host whose package carries no notice folder");
    CHECK(host.qtSourceNotice().startsWith(QStringLiteral("Qt source code\n")), "Qt's source is named either way");
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_qt_source_section();
    test_files_listed_and_read_one_at_a_time();
    test_no_index();
    return failures ? 1 : 0;
}
