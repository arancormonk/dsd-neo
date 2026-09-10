// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cstdio>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <initializer_list>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "json_store.h"
#include "saved_systems_model.h"
#include "scan_list_starter.h"
#include "scan_lists_model.h"
using namespace dsd_qt;
static int failures;

static void
check(bool ok) {
    if (!ok) {
        ++failures;
        std::fputs("scan-list roundtrip assertion failed\n", stderr);
    }
}

int
main(int argc, char** argv) {
    QTemporaryDir dir;
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("dsd-neo-test"));
    app.setApplicationName(QStringLiteral("scan-list-roundtrip-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QVariantMap entry{
        {"kind", "freq"}, {"name", "Simplex"}, {"protocol", "p25"}, {"freqMhz", "851.5"}, {"enabled", true}};
    QVariantMap list{{"name", "Local"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}};
    ScanListsModel model;
    model.add(list);
    list = model.get(0);
    const QString uid = list.value("uid").toString();
    check(!uid.isEmpty() && !list.value("entries").toList().first().toMap().value("uid").toString().isEmpty());
    model.update(0, {{"name", "Updated"}, {"uid", "replacement"}});
    model.touch(0);
    ScanListsModel loaded;
    check(loaded.count() == 1 && loaded.get(0).value("uid") == uid
          && loaded.get(0).value("lastHeard").toLongLong() > 0);
    ScanListStarter starter(nullptr, nullptr);
    auto result = starter.build(list);
    check(result.value("ok").toBool() && result.value("targetCount").toInt() == 1);
    const QString path = json_store_path("scan_lists/" + uid + ".csv");
    int count = 0;
    char error[256] = {};
    check(dsd_app_trunk_scan_validate_targets_csv(path.toUtf8().constData(), &count, error, sizeof error) == 0
          && count == 1);
    QFile file(path);
    check(file.open(QIODevice::ReadOnly));
    auto csv = file.readAll();
    file.close();
    auto duplicate = csv.mid(csv.indexOf('\n') + 1);
    duplicate.replace(list.value("entries").toList().first().toMap().value("uid").toString().toUtf8(), "duplicate");
    check(file.open(QIODevice::Append));
    file.write(duplicate);
    file.close();
    check(dsd_app_trunk_scan_validate_targets_csv(path.toUtf8().constData(), &count, error, sizeof error) != 0
          && error[0] && count == 0);
    list["groupCsvPath"] = dir.path() + "/absent.csv";
    check(!starter.build(list).value("ok").toBool());
    // Relative saved paths keep their standalone working-directory meaning.
    const QString previousDirectory = QDir::currentPath();
    check(QDir::setCurrent(dir.path()));
    QFile groups("groups.csv");
    check(groups.open(QIODevice::WriteOnly));
    groups.write("id,mode,name\n123,A,Global\n");
    groups.close();
    list["groupCsvPath"] = "groups.csv";
    const auto relative = starter.build(list);
    check(relative.value("ok").toBool() && relative.value("args").toStringList().contains(dir.path() + "/groups.csv"));
    check(QDir::setCurrent(previousDirectory));
    // The facade's protocol-specific rejection reaches the caller safely.
    SavedSystemsModel systems;
    systems.add(
        {{"name", "P25"}, {"decodeFlag", "-ft"}, {"freqMhz", "851.5"}, {"encKeyType", "basic"}, {"encKeyValue", "7"}});
    list.remove("groupCsvPath");
    list["entries"] = QVariantList{QVariantMap{
        {"uid", "keyed"}, {"kind", "system"}, {"systemUid", systems.get(0).value("uid")}, {"enabled", true}}};
    ScanListStarter keyedStarter(nullptr, &systems);
    const auto rejected = keyedStarter.build(list);
    check(!rejected.value("ok").toBool() && rejected.value("error").toString().startsWith("Scan list rejected: "));
    check(!QFile::exists(path));
    // INT-3 redacts QML maps; a supported saved key must still reach the private CSV.
    const QString directKey = QStringLiteral("123456789A");
    systems.update(0, {{"encKeyType", "rc4"}, {"encKeyValue", directKey}});
    check(!systems.get(0).contains("encKeyValue"));
    check(keyedStarter.build(list).value("ok").toBool());
    check(file.open(QIODevice::ReadOnly));
    check(file.readAll().contains(QByteArray("-1 ") + directKey.toUtf8()));
    file.close();
    // A list replaces each system's source aliases; unused paths cannot block it.
    QFile aliases(dir.filePath("sources.csv"));
    check(aliases.open(QIODevice::WriteOnly));
    aliases.write("id,name\n123,Dispatch\n");
    aliases.close();
    list["srcCsvPath"] = aliases.fileName();
    for (const auto& unused : {dir.filePath("missing.csv"), QStringLiteral("invalid,\"\npath.csv")}) {
        systems.update(0, {{"srcCsvPath", unused}});
        const auto built = keyedStarter.build(list);
        check(built.value("ok").toBool() && !built.value("warnings").toStringList().isEmpty());
        check(built.value("args").toStringList().contains(aliases.fileName()));
    }
    list["srcCsvPath"] = dir.filePath("missing-effective.csv");
    check(!keyedStarter.build(list).value("ok").toBool());
    list["srcCsvPath"] = "invalid,effective.csv";
    check(!keyedStarter.build(list).value("ok").toBool());
    list.remove("srcCsvPath");
    systems.remove(0);
    // Removing a list also removes its private generated session input.
    check(starter.build(model.get(0)).value("ok").toBool());
    model.remove(0);
    check(!QFile::exists(path));
    check(model.count() == 0);
    QDir(dataDir).removeRecursively();
    return failures ? 1 : 0;
}
