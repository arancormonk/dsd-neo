// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the licences screen's text. It names where the source code of the Qt version the app is built with is,
 * from Qt's own version macros, and then shows every file the package's notice index names, in the index's order, so a
 * notice the packaging adds reaches the screen with no change to the app. */

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QLatin1Char>
#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtGlobal>
#include <cstdio>

#include "decoder_host.h"

namespace {

int failures = 0;

void
check_at(bool ok, int line, const char* why) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "test_ui_qt_license_notices.cpp:%d: %s\n", line, why);
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
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
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
test_qt_source_comes_first() {
    NoticeHost host;
    const QString text = host.licenseNotices();
    CHECK(text.startsWith(QStringLiteral("Qt source code\n")), "the screen opens with where Qt's source code is");
    CHECK(text.contains(QStringLiteral("This app includes Qt %1 ").arg(qt_version())),
          "it names the Qt version built with");
    CHECK(text.contains(QStringLiteral("GNU Lesser General Public License version 3")),
          "and the licence Qt is used under");
    CHECK(text.contains(QStringLiteral("licenses/LGPL-3.0-only.txt")), "and where that licence's text is");
    CHECK(text.contains(QStringLiteral("no patches")), "and that the app patches nothing in Qt");
    for (const QString& module : {QStringLiteral("qtbase"), QStringLiteral("qtdeclarative"), QStringLiteral("qtsvg")}) {
        CHECK(text.contains(archive_url(module)), "each Qt module's source archive is named at that version");
    }
}

void
test_every_indexed_file_in_index_order() {
    QTemporaryDir dir;
    NoticeHost host;
    host.folder = dir.path() + QStringLiteral("/doc");
    CHECK(write_file(host.folder + QStringLiteral("/LICENSE"), "GNU GENERAL PUBLIC LICENSE fixture\n"), "fixture");
    CHECK(write_file(host.folder + QStringLiteral("/licenses/Qt-NOTICES.txt"), "Qt notice fixture\n"), "fixture");
    CHECK(write_file(host.folder + QStringLiteral("/licenses/Added-NOTICES.txt"), "Added notice fixture\n"), "fixture");
    CHECK(write_file(dir.path() + QStringLiteral("/outside.txt"), "Outside fixture\n"), "fixture");
    CHECK(
        write_file(host.folder + QStringLiteral("/notice-index.txt"),
                   "LICENSE\nlicenses/Qt-NOTICES.txt\nlicenses/gone.txt\n../outside.txt\nlicenses/Added-NOTICES.txt\n"),
        "fixture");

    const QString text = host.licenseNotices();
    const auto at = [&text](const char* needle) { return text.indexOf(QLatin1String(needle)); };
    CHECK(at("LICENSE\n\nGNU GENERAL PUBLIC LICENSE fixture") > at("Qt source code"), "the project's licence follows");
    CHECK(at("licenses/Qt-NOTICES.txt\n\nQt notice fixture") > at("GNU GENERAL PUBLIC LICENSE fixture"),
          "Qt's notices follow in the index's order");
    CHECK(at("licenses/gone.txt\n\nThis file is missing from the package.") > at("Qt notice fixture"),
          "a file the index names but the package lacks is shown as missing, not skipped");
    CHECK(at("Outside fixture") < 0, "a path out of the folder is never read");
    CHECK(at("../outside.txt\n\nThis file is missing from the package.") > 0, "it is shown as missing");
    CHECK(at("licenses/Added-NOTICES.txt\n\nAdded notice fixture") > at("licenses/gone.txt"),
          "a notice the index adds is shown with no change to the app");
}

void
test_no_index() {
    QTemporaryDir dir;
    NoticeHost host;
    host.folder = dir.path();
    const QString text = host.licenseNotices();
    CHECK(text.startsWith(QStringLiteral("Qt source code\n")), "Qt's source is named even without the files");
    CHECK(text.endsWith(QStringLiteral("The licence and notice files are missing from this package.")),
          "a package without its notice index says the files are missing");
    host.folder.clear();
    CHECK(host.licenseNotices().endsWith(QStringLiteral("The licence and notice files are missing from this package.")),
          "so does a host whose package carries no notice folder");
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_qt_source_comes_first();
    test_every_indexed_file_in_index_order();
    test_no_index();
    return failures ? 1 : 0;
}
