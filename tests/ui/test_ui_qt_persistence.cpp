// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the Qt frontend's persistence layer — json_store's load/save
 * behavior on missing and corrupt input, SavedSystemsModel's field-map round
 * trip and reload path, and AppPrefs defaults and persistence. Registered only
 * when the Qt frontend is enabled (DSD_ENABLE_QT_UI), since these link Qt. */

#include <QAbstractListModel>
#include <QAnyStringView>
#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QMetaProperty>
#include <QMetaType>
#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <functional>
#include <stdio.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/log.h>
#include "app_prefs.h"
#include "dsd-neo/core/safe_api.h"
#include "json_store.h"
#include "listen_pins.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"

using dsd_qt::AppPrefs;
using dsd_qt::json_store_load_array;
using dsd_qt::json_store_path;
using dsd_qt::json_store_save_array;
using dsd_qt::ListenPins;
using dsd_qt::SavedSystemsModel;
using dsd_qt::ScanListsModel;

namespace {

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

void
test_json_store(void) {
    expect("missing file loads as empty array", json_store_load_array(QStringLiteral("absent.json")).isEmpty());

    QDir().mkpath(json_store_path(QStringLiteral("blocked.json")));
    expect("save reports an unwritable destination", !json_store_save_array(QStringLiteral("blocked.json"), {}));
    QJsonArray array;
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), QStringLiteral("Hamilton Co P25"));
    obj.insert(QStringLiteral("port"), 1234);
    array.append(obj);
    json_store_save_array(QStringLiteral("roundtrip.json"), array);
    const QJsonArray loaded = json_store_load_array(QStringLiteral("roundtrip.json"));
    expect("round trip keeps the row", loaded.size() == 1);
    expect("round trip keeps fields",
           loaded.at(0).toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Hamilton Co P25")
               && loaded.at(0).toObject().value(QStringLiteral("port")).toInt() == 1234);

    /* Persisted state is external input: a partially written or hand-mangled
     * file must read as empty, never crash or hand garbage rows to a model. */
    QFile corrupt(json_store_path(QStringLiteral("corrupt.json")));
    expect("corrupt fixture opens", corrupt.open(QIODevice::WriteOnly));
    corrupt.write("this is not json {]");
    corrupt.close();
    expect("corrupt file loads as empty array", json_store_load_array(QStringLiteral("corrupt.json")).isEmpty());

    QFile wrongShape(json_store_path(QStringLiteral("object.json")));
    expect("object fixture opens", wrongShape.open(QIODevice::WriteOnly));
    wrongShape.write("{\"not\":\"an array\"}");
    wrongShape.close();
    expect("non-array document loads as empty array", json_store_load_array(QStringLiteral("object.json")).isEmpty());

    /* Overwrite, not append: a shrunk list must not leave the old tail behind. */
    json_store_save_array(QStringLiteral("roundtrip.json"), QJsonArray());
    expect("save replaces the previous content", json_store_load_array(QStringLiteral("roundtrip.json")).isEmpty());
}

void
test_private_key_store() {
#ifndef _WIN32
    const mode_t previous = umask(0);
    const QString name = QStringLiteral("private-keys.json");
    const QByteArray path = QFile::encodeName(json_store_path(name));
    const QJsonArray rows{QJsonObject{{"encKeyValue", "123456789A"}}};
    expect("new private key store saved", json_store_save_array(name, rows));
    struct stat info = {};
    expect("new key store is owner read/write only",
           stat(path.constData(), &info) == 0 && (info.st_mode & 0777) == 0600);
    expect("public replacement fixture prepared", chmod(path.constData(), 0666) == 0);
    expect("replacement private key store saved", json_store_save_array(name, rows));
    expect("replacement key store is owner read/write only",
           stat(path.constData(), &info) == 0 && (info.st_mode & 0777) == 0600);
    expect("permissions preserve key contents", json_store_load_array(name) == rows);
    umask(previous);
#endif
}

QVariantMap
full_system_map(void) {
    QVariantMap map;
    map.insert(QStringLiteral("name"), QStringLiteral("Hamilton Co P25"));
    map.insert(QStringLiteral("sourceType"), QStringLiteral("usb"));
    map.insert(QStringLiteral("host"), QStringLiteral("192.168.1.10"));
    map.insert(QStringLiteral("port"), 1234);
    map.insert(QStringLiteral("freqMhz"), QStringLiteral("851.375"));
    map.insert(QStringLiteral("decodeFlag"), QStringLiteral("-ft"));
    map.insert(QStringLiteral("trunking"), true);
    map.insert(QStringLiteral("gainDb"), 36);
    map.insert(QStringLiteral("ppm"), QStringLiteral("-2"));
    map.insert(QStringLiteral("hangtime"), QStringLiteral("1.5"));
    map.insert(QStringLiteral("bandwidthKhz"), 12);
    map.insert(QStringLiteral("biasTee"), true);
    map.insert(QStringLiteral("extraArgs"), QStringLiteral("-C chan.csv"));
    map.insert(QStringLiteral("filePath"), QString());
    return map;
}

void
test_saved_systems(void) {
    {
        SavedSystemsModel model;
        expect("fresh model is empty", model.count() == 0);

        model.add(full_system_map());
        expect("add lands", model.count() == 1);
        const QVariantMap got = model.get(0);
        expect("identity round-trips",
               got.value(QStringLiteral("name")).toString() == QStringLiteral("Hamilton Co P25")
                   && got.value(QStringLiteral("sourceType")).toString() == QStringLiteral("usb")
                   && got.value(QStringLiteral("freqMhz")).toString() == QStringLiteral("851.375")
                   && got.value(QStringLiteral("decodeFlag")).toString() == QStringLiteral("-ft"));
        expect("tuning overrides round-trip",
               got.value(QStringLiteral("gainDb")).toInt() == 36
                   && got.value(QStringLiteral("ppm")).toString() == QStringLiteral("-2")
                   && got.value(QStringLiteral("hangtime")).toString() == QStringLiteral("1.5")
                   && got.value(QStringLiteral("bandwidthKhz")).toInt() == 12
                   && got.value(QStringLiteral("biasTee")).toInt() == 1
                   && got.value(QStringLiteral("extraArgs")).toString() == QStringLiteral("-C chan.csv"));

        /* Absent keys keep the documented sentinels; junk types collapse to
         * QVariant's conversions, never to uninitialized fields. */
        QVariantMap sparse;
        sparse.insert(QStringLiteral("name"), QStringLiteral("Sparse"));
        sparse.insert(QStringLiteral("port"), QStringLiteral("abc"));
        model.add(sparse);
        const QVariantMap sparseGot = model.get(1);
        expect("junk port reads as 0", sparseGot.value(QStringLiteral("port")).toInt() == 0);
        expect("absent overrides keep sentinels",
               sparseGot.value(QStringLiteral("gainDb")).toInt() == -1
                   && sparseGot.value(QStringLiteral("bandwidthKhz")).toInt() == -1
                   && sparseGot.value(QStringLiteral("biasTee")).toInt() == -1
                   && sparseGot.value(QStringLiteral("lastHeard")).toLongLong() == 0);
        /* Absent trunking is OFF, the same reading session_args gives it - it
         * appends -T only for a present true. The store defaulting it ON meant a
         * map that never mentioned trunking was saved as call-following on while
         * the args built from that same map left it off. */
        expect("an absent trunking key reads as off, as it does everywhere else",
               !sparseGot.value(QStringLiteral("trunking")).toBool());

        /* A partial update must not blank fields it does not mention. */
        QVariantMap rename;
        rename.insert(QStringLiteral("name"), QStringLiteral("Renamed"));
        model.update(0, rename);
        expect("partial update keeps other fields",
               model.get(0).value(QStringLiteral("name")).toString() == QStringLiteral("Renamed")
                   && model.get(0).value(QStringLiteral("gainDb")).toInt() == 36);

        model.touch(1);
        expect("touch stamps lastHeard", model.get(1).value(QStringLiteral("lastHeard")).toLongLong() > 0);
        expect("mostRecentRow follows the touch", model.mostRecentRow() == 1);

        /* The legacy simulcast chip flags are rewritten on load, not on save. */
        QVariantMap legacy;
        legacy.insert(QStringLiteral("name"), QStringLiteral("Legacy LSM"));
        legacy.insert(QStringLiteral("decodeFlag"), QStringLiteral("-f1 -mq"));
        model.add(legacy);

        /* Bias tee migrates from the legacy bool: true was an explicit choice,
         * false the untouched default whose behavior was to follow the app-wide
         * pref — it must read as follow (-1), never as a frozen off. */
        QVariantMap legacyBias;
        legacyBias.insert(QStringLiteral("name"), QStringLiteral("Legacy bias"));
        legacyBias.insert(QStringLiteral("biasTee"), false);
        model.add(legacyBias);
        expect("legacy bias-tee false reads as follow-default",
               model.get(3).value(QStringLiteral("biasTee")).toInt() == -1);
        QVariantMap explicitOff;
        explicitOff.insert(QStringLiteral("biasTee"), 0);
        model.update(3, explicitOff);
        expect("explicit bias-tee off persists as off", model.get(3).value(QStringLiteral("biasTee")).toInt() == 0);
    }

    auto legacyRows = json_store_load_array(QStringLiteral("saved_systems.json"));
    auto legacyRow = legacyRows.at(1).toObject();
    legacyRow.remove(QStringLiteral("hangtime"));
    legacyRows.replace(1, legacyRow);
    expect("legacy system stored without hang time",
           json_store_save_array(QStringLiteral("saved_systems.json"), legacyRows));

    /* A second instance is the Activity-restart path: everything above must
     * come back from disk, including the legacy decode-flag migration. */
    SavedSystemsModel reloaded;
    expect("reload restores every row", reloaded.count() == 4);
    expect("hang time persists through partial edits and reload",
           reloaded.get(0).value("hangtime") == "1.5"
               && reloaded.data(reloaded.index(0), SavedSystemsModel::HangtimeRole) == "1.5");
    expect("legacy hang time follows app default", reloaded.get(1).value("hangtime").toString().isEmpty());
    expect("reload restores fields",
           reloaded.get(0).value(QStringLiteral("name")).toString() == QStringLiteral("Renamed")
               && reloaded.get(0).value(QStringLiteral("extraArgs")).toString() == QStringLiteral("-C chan.csv"));
    expect("legacy '-f1 -mq' migrates to '-mq' on load",
           reloaded.get(2).value(QStringLiteral("decodeFlag")).toString() == QStringLiteral("-mq"));
    expect("explicit bias-tee off survives the reload", reloaded.get(3).value(QStringLiteral("biasTee")).toInt() == 0);

    reloaded.remove(0);
    reloaded.remove(0);
    reloaded.remove(0);
    reloaded.remove(0);
    expect("remove empties the model", reloaded.count() == 0);
    SavedSystemsModel emptied;
    expect("removal persists", emptied.count() == 0);
}

/* A restored systems file whose whole numbers are past int's range (a backup keeps a large JSON number as a 64-bit
 * integer): each reads as the field's default, as an absent key does, never as its low 32 bits. Main read port 1234,
 * gain 36 dB, bandwidth 12 kHz, bias tee on, force key 1, RadioReference system 77 site 5 and a "hex" key type. */
void
test_saved_systems_past_int_range(void) {
    const qint64 past = qint64(1) << 32;
    QJsonObject row;
    row.insert(QStringLiteral("name"), QStringLiteral("Restored"));
    row.insert(QStringLiteral("sourceType"), QStringLiteral("rtltcp"));
    row.insert(QStringLiteral("port"), past + 1234);
    row.insert(QStringLiteral("gainDb"), past + 36);
    row.insert(QStringLiteral("bandwidthKhz"), past + 12);
    row.insert(QStringLiteral("biasTee"), past + 1);
    row.insert(QStringLiteral("encForceKey"), past + 1);
    row.insert(QStringLiteral("rrSid"), past + 77);
    row.insert(QStringLiteral("rrSiteId"), past + 5);
    row.insert(QStringLiteral("encKeyType"), past + 1);
    expect("an oversized systems file saves", json_store_save_array(QStringLiteral("saved_systems.json"), {row}));
    {
        SavedSystemsModel model;
        const QVariantMap got = model.get(0);
        expect("an oversized row loads", model.count() == 1);
        expect("an oversized port reads as 0", got.value(QStringLiteral("port")).toInt() == 0);
        expect("oversized tuning overrides follow the app",
               got.value(QStringLiteral("gainDb")).toInt() == -1
                   && got.value(QStringLiteral("bandwidthKhz")).toInt() == -1
                   && got.value(QStringLiteral("biasTee")).toInt() == -1);
        expect("an oversized force key reads as none", got.value(QStringLiteral("encForceKey")).toInt() == 0);
        expect("an oversized RadioReference origin reads as none",
               got.value(QStringLiteral("rrSid")).toInt() == 0 && got.value(QStringLiteral("rrSiteId")).toInt() == 0);
        expect("an oversized legacy key type is no key type",
               got.value(QStringLiteral("encKeyType")).toString() != QStringLiteral("hex"));
        model.remove(0);
    }
}

void
test_saved_systems_csv_fields(void) {
    const QString groupPath = QStringLiteral("/data/imports/county.csv");
    const QString planPath = QStringLiteral("/data/imports/band plan.csv");
    const QString srcPath = QStringLiteral("/data/imports/radio IDs.csv");
    {
        SavedSystemsModel model;
        QVariantMap sys = full_system_map();
        sys.insert(QStringLiteral("chanCsvPath"), QStringLiteral("/data/imports/chan map.csv"));
        sys.insert(QStringLiteral("groupCsvPath"), groupPath);
        sys.insert(QStringLiteral("keyCsvPath"), QStringLiteral("/data/imports/keys.csv"));
        sys.insert(QStringLiteral("keyCsvHex"), true);
        sys.insert(QStringLiteral("p25BandplanCsvPath"), planPath);
        sys.insert(QStringLiteral("srcCsvPath"), srcPath);
        model.add(sys);

        /* Legacy row: fields absent must read as no CSV, not junk. */
        QVariantMap legacy;
        legacy.insert(QStringLiteral("name"), QStringLiteral("Legacy"));
        model.add(legacy);

        QVariantMap second = full_system_map();
        second.insert(QStringLiteral("name"), QStringLiteral("Butler Co DMR"));
        second.insert(QStringLiteral("groupCsvPath"), groupPath);
        model.add(second);
    }

    SavedSystemsModel model;
    const QVariantMap got = model.get(0);
    expect("csv fields round-trip",
           got.value(QStringLiteral("chanCsvPath")).toString() == QStringLiteral("/data/imports/chan map.csv")
               && got.value(QStringLiteral("groupCsvPath")).toString() == groupPath
               && got.value(QStringLiteral("keyCsvPath")).toString() == QStringLiteral("/data/imports/keys.csv")
               && got.value(QStringLiteral("keyCsvHex")).toBool()
               && got.value(QStringLiteral("p25BandplanCsvPath")).toString() == planPath
               && got.value(QStringLiteral("srcCsvPath")).toString() == srcPath);
    expect("legacy row reads empty csv fields",
           model.get(1).value(QStringLiteral("chanCsvPath")).toString().isEmpty()
               && model.get(1).value(QStringLiteral("groupCsvPath")).toString().isEmpty()
               && model.get(1).value(QStringLiteral("keyCsvPath")).toString().isEmpty()
               && !model.get(1).value(QStringLiteral("keyCsvHex")).toBool()
               && model.get(1).value(QStringLiteral("p25BandplanCsvPath")).toString().isEmpty()
               && model.get(1).value(QStringLiteral("srcCsvPath")).toString().isEmpty());

    /* The delete-with-in-use-warning flow: which systems reference a stored
     * file, and clearing that reference everywhere when the file goes away. */
    const QStringList users = model.systemsReferencingPath(groupPath);
    expect("referencing systems are named", users.size() == 2 && users.contains(QStringLiteral("Hamilton Co P25"))
                                                && users.contains(QStringLiteral("Butler Co DMR")));
    expect("unreferenced path names nobody",
           model.systemsReferencingPath(QStringLiteral("/data/imports/nope.csv")).isEmpty());

    model.clearCsvPath(groupPath);
    expect("clear blanks every matching field",
           model.get(0).value(QStringLiteral("groupCsvPath")).toString().isEmpty()
               && model.get(2).value(QStringLiteral("groupCsvPath")).toString().isEmpty());
    expect("clear leaves other csv fields alone",
           model.get(0).value(QStringLiteral("chanCsvPath")).toString() == QStringLiteral("/data/imports/chan map.csv")
               && model.get(0).value(QStringLiteral("p25BandplanCsvPath")).toString() == planPath);

    /* The band plan field is part of the same delete flow: a library removal
     * must name the system using it and then blank the reference. */
    const QStringList planUsers = model.systemsReferencingPath(planPath);
    expect("bandplan users are named", planUsers.size() == 1 && planUsers.contains(QStringLiteral("Hamilton Co P25")));
    model.clearCsvPath(planPath);
    expect("clear blanks the bandplan field",
           model.get(0).value(QStringLiteral("p25BandplanCsvPath")).toString().isEmpty());

    const QStringList srcUsers = model.systemsReferencingPath(srcPath);
    expect("source users are named", srcUsers.size() == 1 && srcUsers.contains(QStringLiteral("Hamilton Co P25")));
    model.clearCsvPath(srcPath);
    expect("clear blanks source aliases", model.get(0).value(QStringLiteral("srcCsvPath")).toString().isEmpty());

    SavedSystemsModel reloaded;
    expect("clear persists", reloaded.get(0).value(QStringLiteral("groupCsvPath")).toString().isEmpty()
                                 && reloaded.get(0).value(QStringLiteral("p25BandplanCsvPath")).toString().isEmpty()
                                 && reloaded.get(0).value(QStringLiteral("srcCsvPath")).toString().isEmpty());
    reloaded.remove(0);
    reloaded.remove(0);
    reloaded.remove(0);
}

void
test_app_prefs(void) {
    {
        AppPrefs prefs;
        expect("default appearance follows the system", prefs.appearance() == AppPrefs::FollowSystem);
        expect("onboarding starts not-done", !prefs.onboardingDone());
        expect("the monitor's Signal & site readings start closed", !prefs.monitorDetailsExpanded());
        expect("background listening defaults on", prefs.backgroundListening());
        expect("skip-encrypted defaults on", prefs.skipEncrypted());
        expect("quick lockouts default to saving", prefs.persistTgLockouts());
        prefs.setPersistTgLockouts(false);
        expect("auto-ppm defaults off", !prefs.autoPpm());
        expect("hang time defaults to two seconds", prefs.hangtimeSec() == 2.0);
        prefs.setHangtimeSec(99);
        expect("hang time is clamped", prefs.hangtimeSec() == 30.0);
        prefs.setHangtimeSec(-1);
        expect("hang time has a zero lower bound", prefs.hangtimeSec() == 0.0);
        prefs.setHangtimeSec(3.5);
        expect("bias tee defaults off", !prefs.biasTee());
        expect("gain defaults to 30 dB", prefs.gainDb() == 30);
        expect("ppm defaults to 0", prefs.ppm() == 0);
        expect("bandwidth defaults to 48 kHz", prefs.bandwidthKhz() == 48);
        expect("extra args default empty", prefs.extraArgs().isEmpty());
        /* Both empty on a fresh install, which is what makes the RadioReference
         * screen open on its credentials gate. An empty app key means "use the
         * key baked in at build time, if this build carries one". */
        expect("RadioReference username defaults empty", prefs.rrUsername().isEmpty());
        expect("RadioReference app key defaults empty", prefs.rrAppKey().isEmpty());
        /* Empty is what makes the first Explore tap ask where to point the radio
         * instead of starting on a guess. */
        expect("explore source starts unchosen", prefs.exploreSourceType().isEmpty());
        expect("explore frequency starts unchosen", prefs.exploreFreqMhz().isEmpty());
        expect("explore port defaults to rtl_tcp's", prefs.explorePort() == 1234);

        /* The getter validates: an out-of-range stored mode must read as the
         * default, not drive a switch statement off the end. */
        prefs.setAppearance(9);
        expect("out-of-range appearance reads as default", prefs.appearance() == AppPrefs::FollowSystem);

        /* Same discipline for the explore source. Only a tuner can be explored, so
         * a stored "udp" is not a source to start on — it reads as unchosen and
         * sends the user back to the setup sheet. */
        prefs.setExploreSourceType(QStringLiteral("udp"));
        expect("a non-tuner explore source reads as unchosen", prefs.exploreSourceType().isEmpty());
        prefs.setExplorePort(70000);
        expect("an out-of-range explore port reads as the default", prefs.explorePort() == 1234);

        prefs.setGainDb(42);
        prefs.setExtraArgs(QStringLiteral("--enc-lockout"));
        prefs.setOnboardingDone(true);
        prefs.setExploreSourceType(QStringLiteral("rtltcp"));
        prefs.setExploreHost(QStringLiteral("10.0.2.2"));
        prefs.setExplorePort(1234);
        /* Text, not a number: the trailing zeros say which channel this was. */
        prefs.setExploreFreqMhz(QStringLiteral("769.76875"));
        prefs.setRrUsername(QStringLiteral("someuser"));
        prefs.setRrAppKey(QStringLiteral("user-supplied-key"));
    }

    AppPrefs reloaded;
    expect("gain persists across instances", reloaded.gainDb() == 42);
    expect("temporary lockout preference persists", !reloaded.persistTgLockouts());
    expect("hang time persists across instances", reloaded.hangtimeSec() == 3.5);
    expect("extra args persist across instances", reloaded.extraArgs() == QStringLiteral("--enc-lockout"));
    expect("onboarding flag persists", reloaded.onboardingDone());
    expect("explore source persists", reloaded.exploreSourceType() == QStringLiteral("rtltcp"));
    expect("explore host persists", reloaded.exploreHost() == QStringLiteral("10.0.2.2"));
    expect("explore port persists", reloaded.explorePort() == 1234);
    expect("explore frequency persists with its digits intact",
           reloaded.exploreFreqMhz() == QStringLiteral("769.76875"));
    expect("RadioReference username persists", reloaded.rrUsername() == QStringLiteral("someuser"));
    expect("RadioReference app key persists", reloaded.rrAppKey() == QStringLiteral("user-supplied-key"));
    /* There is deliberately no password preference: it is held in memory for the
     * session and re-prompted next launch, so it can never reach a settings file
     * or a device backup. */
    expect("no RadioReference password preference exists", reloaded.metaObject()->indexOfProperty("rrPassword") == -1);
}

QSettings
prefs_file() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("dsd-neo"),
                     QStringLiteral("dsd-neo-app"));
}

void
clear_prefs_file() {
    QSettings s = prefs_file();
    s.clear();
    s.sync();
}

/*
 * The radio Explore and Signal search start on survives a restart for every tuner
 * source the setup offers. An Airspy used to be stored and read back as empty, so
 * Explore asked for the radio again every time and Signal search fell back to USB.
 */
void
test_receiver_prefs() {
    {
        AppPrefs prefs;
        int explore = 0;
        QObject::connect(&prefs, &AppPrefs::exploreChanged, &prefs, [&explore]() { ++explore; });
        prefs.setExploreSourceType(QStringLiteral("airspy"));
        expect("an Airspy explore source reads back at once", prefs.exploreSourceType() == QStringLiteral("airspy"));
        expect("choosing Airspy for exploring notifies", explore == 1);
    }
    {
        AppPrefs reloaded;
        expect("an Airspy explore source persists", reloaded.exploreSourceType() == QStringLiteral("airspy"));
        reloaded.setExploreSourceType(QStringLiteral("usb"));
    }

    /* Signal search remembers its own radio, apart from Explore's. */
    {
        AppPrefs prefs;
        expect("search source starts unchosen", prefs.searchSourceType().isEmpty());
        expect("search host starts empty", prefs.searchHost().isEmpty());
        expect("search port defaults to rtl_tcp's", prefs.searchPort() == 1234);
        const QString exploreSource = prefs.exploreSourceType();
        const QString exploreHost = prefs.exploreHost();
        const int explorePort = prefs.explorePort();
        int search = 0;
        int explore = 0;
        QObject::connect(&prefs, &AppPrefs::searchChanged, &prefs, [&search]() { ++search; });
        QObject::connect(&prefs, &AppPrefs::exploreChanged, &prefs, [&explore]() { ++explore; });
        prefs.setSearchSourceType(QStringLiteral("udp"));
        expect("a non-tuner search source reads as unchosen", prefs.searchSourceType().isEmpty());
        prefs.setSearchPort(0);
        expect("an out-of-range search port reads as the default", prefs.searchPort() == 1234);
        search = 0;
        for (const char* kind : {"usb", "rtltcp", "airspy"}) {
            prefs.setSearchSourceType(QString::fromLatin1(kind));
            expect("every tuner source is a search source", prefs.searchSourceType() == QString::fromLatin1(kind));
        }
        expect("each search source change notifies once", search == 3);
        prefs.setSearchSourceType(QStringLiteral("airspy"));
        expect("an unchanged search source does not notify", search == 3);
        prefs.setSearchHost(QStringLiteral("10.0.2.2"));
        prefs.setSearchPort(1235);
        prefs.setSearchPort(1235);
        expect("search host and port notify on change only", search == 5);
        expect("search writes never touch Explore", explore == 0 && prefs.exploreSourceType() == exploreSource
                                                        && prefs.exploreHost() == exploreHost
                                                        && prefs.explorePort() == explorePort);
    }
    {
        AppPrefs reloaded;
        expect("an Airspy search source persists", reloaded.searchSourceType() == QStringLiteral("airspy"));
        expect("the search host persists", reloaded.searchHost() == QStringLiteral("10.0.2.2"));
        expect("the search port persists", reloaded.searchPort() == 1235);
        /* The setter compares with what is stored, so the corrective write lands. */
        prefs_file().setValue(QStringLiteral("search/sourceType"), QStringLiteral("file"));
        AppPrefs stale;
        expect("a stored non-tuner search source reads as unchosen", stale.searchSourceType().isEmpty());
        stale.setSearchSourceType(QString());
        AppPrefs corrected;
        expect("clearing a stored non-tuner search source lands",
               corrected.searchSourceType().isEmpty()
                   && prefs_file().value(QStringLiteral("search/sourceType")).toString().isEmpty());
        corrected.setSearchHost(QString());
        corrected.setSearchPort(1234);
    }
}

void
test_units_preference() {
    {
        AppPrefs prefs;
        expect("units default to imperial", !prefs.metricUnits());
        int changes = 0;
        QObject::connect(&prefs, &AppPrefs::metricUnitsChanged, &prefs, [&changes]() { ++changes; });
        prefs.setMetricUnits(false);
        expect("unchanged units do not notify", changes == 0);
        prefs.setMetricUnits(true);
        expect("metric selection updates and notifies", prefs.metricUnits() && changes == 1);
        prefs.setMetricUnits(true);
        expect("repeated metric selection does not notify", changes == 1);
    }
    {
        AppPrefs reloaded;
        expect("metric units persist across instances", reloaded.metricUnits());
        reloaded.setMetricUnits(false);
    }
    AppPrefs restored;
    expect("switching back to imperial persists", !restored.metricUnits());
}

/* Issue #708: call recording's preferences. Off by default; the storage limit is one of the choices Settings offers
 * (decimal MB, 0 for no limit), 1 GB unless chosen, and a damaged value reads as the default. */
void
test_recording_prefs() {
    QSettings stored(QSettings::IniFormat, QSettings::UserScope, "dsd-neo", "dsd-neo-app");
    const QString limitKey = QStringLiteral("recording/limitMb");
    stored.remove(QStringLiteral("recording/recordCalls"));
    stored.remove(QStringLiteral("recording/recordUnknownOnly"));
    stored.remove(limitKey);
    {
        AppPrefs prefs;
        int limitChanges = 0;
        QObject::connect(&prefs, &AppPrefs::recordingLimitMbChanged, &prefs, [&limitChanges]() { ++limitChanges; });
        expect("call recording is off by default", !prefs.recordCalls() && !prefs.recordUnknownOnly());
        expect("the storage limit defaults to 1 GB", prefs.recordingLimitMb() == 1000);
        expect("the limits Settings offers",
               prefs.recordingLimitChoices() == QVariantList({250, 500, 1000, 2000, 5000, 0}));
        prefs.setRecordCalls(true);
        prefs.setRecordUnknownOnly(true);
        prefs.setRecordingLimitMb(0);
        expect("no limit is a choice", prefs.recordingLimitMb() == 0 && limitChanges == 1);
        prefs.setRecordingLimitMb(0);
        expect("a repeated limit does not notify", limitChanges == 1);
        prefs.setRecordingLimitMb(750);
        expect("an off-list limit is written as 1 GB",
               prefs.recordingLimitMb() == 1000 && stored.value(limitKey).toInt() == 1000 && limitChanges == 2);
        prefs.setRecordingLimitMb(2000);
    }
    {
        AppPrefs reloaded;
        expect("the switches persist", reloaded.recordCalls() && reloaded.recordUnknownOnly());
        expect("the limit persists", reloaded.recordingLimitMb() == 2000);
        stored.setValue(limitKey, -5);
        expect("a negative stored limit reads as 1 GB", reloaded.recordingLimitMb() == 1000);
        stored.setValue(limitKey, QStringLiteral("abc"));
        expect("an unparsable stored limit reads as 1 GB", reloaded.recordingLimitMb() == 1000);
        int limitChanges = 0;
        QObject::connect(&reloaded, &AppPrefs::recordingLimitMbChanged, &reloaded,
                         [&limitChanges]() { ++limitChanges; });
        reloaded.setRecordingLimitMb(1000);
        expect("a corrective limit write replaces the damage",
               stored.value(limitKey).toString() == QStringLiteral("1000") && limitChanges == 1);
    }
    stored.remove(QStringLiteral("recording/recordCalls"));
    stored.remove(QStringLiteral("recording/recordUnknownOnly"));
    stored.remove(limitKey);
}

/* Issue #756: the analog pause hold. Off (200, the decoder's default) unless chosen; any value the decoder takes is
 * kept as it is, offered or not, so a restored or older value shows as itself; anything else reads and writes as off.
 */
void
test_analog_max_gap_pref() {
    QSettings stored(QSettings::IniFormat, QSettings::UserScope, "dsd-neo", "dsd-neo-app");
    const QString key = QStringLiteral("recording/analogMaxGapMs");
    stored.remove(key);
    {
        AppPrefs prefs;
        int changes = 0;
        QObject::connect(&prefs, &AppPrefs::analogMaxGapMsChanged, &prefs, [&changes]() { ++changes; });
        expect("the pause hold is off by default", prefs.analogMaxGapMs() == 200 && !stored.contains(key));
        expect("the holds Settings offers",
               prefs.analogMaxGapChoices() == QVariantList({200, 500, 1000, 2000, 3000, 5000, 10000}));
        prefs.setAnalogMaxGapMs(2000);
        expect("a choice is saved",
               prefs.analogMaxGapMs() == 2000 && stored.value(key).toInt() == 2000 && changes == 1);
        prefs.setAnalogMaxGapMs(2000);
        expect("a repeated hold does not notify", changes == 1);
        prefs.setAnalogMaxGapMs(1500);
        expect("a value not offered is kept as it is", prefs.analogMaxGapMs() == 1500 && changes == 2);
        prefs.setAnalogMaxGapMs(0);
        expect("0 is kept (off)", prefs.analogMaxGapMs() == 0 && changes == 3);
        prefs.setAnalogMaxGapMs(10000);
        expect("the largest is kept", prefs.analogMaxGapMs() == 10000 && changes == 4);
        prefs.setAnalogMaxGapMs(10001);
        expect("one past the range is written as off",
               prefs.analogMaxGapMs() == 200 && stored.value(key).toInt() == 200 && changes == 5);
        prefs.setAnalogMaxGapMs(-1);
        expect("a negative hold is written as off, unchanged", prefs.analogMaxGapMs() == 200 && changes == 5);
        prefs.setAnalogMaxGapMs(3000);
    }
    {
        AppPrefs reloaded;
        expect("the hold persists", reloaded.analogMaxGapMs() == 3000);
        stored.setValue(key, 20000);
        expect("a stored hold past the range reads as off", reloaded.analogMaxGapMs() == 200);
        stored.setValue(key, QStringLiteral("abc"));
        expect("an unparsable stored hold reads as off", reloaded.analogMaxGapMs() == 200);
        int changes = 0;
        QObject::connect(&reloaded, &AppPrefs::analogMaxGapMsChanged, &reloaded, [&changes]() { ++changes; });
        reloaded.setAnalogMaxGapMs(200);
        expect("a corrective hold write replaces the damage",
               stored.value(key).toString() == QStringLiteral("200") && changes == 1);
        stored.setValue(key, 1234);
        expect("a stored custom hold reads as itself", reloaded.analogMaxGapMs() == 1234);
    }
    stored.remove(key);
}

/* Issue #668: the NFM pre-roll. The decoder's default unless chosen; any value the decoder takes (0..1000 ms) is kept
 * as it is, offered or not; anything else reads and writes as the default. */
void
test_analog_preroll_pref() {
    QSettings stored(QSettings::IniFormat, QSettings::UserScope, "dsd-neo", "dsd-neo-app");
    const QString key = QStringLiteral("recording/analogPrerollMs");
    const int fallback = DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS;
    stored.remove(key);
    {
        AppPrefs prefs;
        int changes = 0;
        QObject::connect(&prefs, &AppPrefs::analogPrerollMsChanged, &prefs, [&changes] { ++changes; });
        expect("the pre-roll is the decoder's default", prefs.analogPrerollMs() == fallback && !stored.contains(key));
        expect("the pre-rolls Settings offers",
               AppPrefs::analogPrerollChoices() == QVariantList({0, 100, 200, 300, 500, 1000}));
        prefs.setAnalogPrerollMs(300);
        expect("a choice is saved", prefs.analogPrerollMs() == 300 && stored.value(key).toInt() == 300 && changes == 1);
        prefs.setAnalogPrerollMs(300);
        expect("a repeated pre-roll does not notify", changes == 1);
        prefs.setAnalogPrerollMs(250);
        expect("a value not offered is kept as it is", prefs.analogPrerollMs() == 250 && changes == 2);
        prefs.setAnalogPrerollMs(1000);
        expect("the largest is kept", prefs.analogPrerollMs() == 1000 && changes == 3);
        prefs.setAnalogPrerollMs(0);
        expect("0 is kept (off)", prefs.analogPrerollMs() == 0 && stored.value(key).toInt() == 0 && changes == 4);
        prefs.setAnalogPrerollMs(500);
        prefs.setAnalogPrerollMs(1001);
        expect("one past the range is written as the default",
               prefs.analogPrerollMs() == fallback && stored.value(key).toInt() == fallback && changes == 6);
        prefs.setAnalogPrerollMs(500);
        prefs.setAnalogPrerollMs(-1);
        expect("a negative pre-roll is written as the default", prefs.analogPrerollMs() == fallback && changes == 8);
        prefs.setAnalogPrerollMs(100);
    }
    {
        AppPrefs reloaded;
        expect("the pre-roll persists", reloaded.analogPrerollMs() == 100);
        stored.setValue(key, 5000);
        expect("a stored pre-roll past the range reads as the default", reloaded.analogPrerollMs() == fallback);
        stored.setValue(key, QStringLiteral("abc"));
        expect("an unparsable stored pre-roll reads as the default", reloaded.analogPrerollMs() == fallback);
        int changes = 0;
        QObject::connect(&reloaded, &AppPrefs::analogPrerollMsChanged, &reloaded, [&changes] { ++changes; });
        reloaded.setAnalogPrerollMs(fallback);
        expect("a corrective pre-roll write replaces the damage",
               stored.value(key).toString() == QString::number(fallback) && changes == 1);
        stored.setValue(key, 750);
        expect("a stored custom pre-roll reads as itself", reloaded.analogPrerollMs() == 750);
    }
    stored.remove(key);
}

/* An integer preference read wide: a backup keeps a large JSON number as a 64-bit integer (or a double), and the ini
 * file hands it back as text, and QVariant::toInt() takes either one's low 32 bits and reports success. A value past
 * int's range reads as the preference's default, never as its low bits, and a write of those low bits replaces it. */
struct WideIntPref {
    const char* key;
    int (AppPrefs::*get)() const;
    void (AppPrefs::*set)(int);
    int fallback; /* what a damaged value reads as */
    int low;      /* a valid value, different from the fallback, that the oversized one truncates to */
};

const WideIntPref kWideIntPrefs[] = {
    {"ui/appearance", &AppPrefs::appearance, &AppPrefs::setAppearance, AppPrefs::FollowSystem, AppPrefs::Dark},
    {"listen/screenMode", &AppPrefs::screenMode, &AppPrefs::setScreenMode, 0, AppPrefs::ScreenOffBetweenCalls},
    {"listen/screenDelaySec", &AppPrefs::screenDelaySec, &AppPrefs::setScreenDelaySec, 30, 120},
    {"tuner/gainDb", &AppPrefs::gainDb, &AppPrefs::setGainDb, 30, 42},
    {"tuner/ppm", &AppPrefs::ppm, &AppPrefs::setPpm, 0, 5},
    {"tuner/bandwidthKhz", &AppPrefs::bandwidthKhz, &AppPrefs::setBandwidthKhz, 48, 24},
    {"explore/port", &AppPrefs::explorePort, &AppPrefs::setExplorePort, 1234, 80},
    {"search/port", &AppPrefs::searchPort, &AppPrefs::setSearchPort, 1234, 80},
    {"recording/limitMb", &AppPrefs::recordingLimitMb, &AppPrefs::setRecordingLimitMb, 1000, 2000},
    {"recording/analogMaxGapMs", &AppPrefs::analogMaxGapMs, &AppPrefs::setAnalogMaxGapMs, 200, 2000},
    {
        "recording/analogPrerollMs",
        &AppPrefs::analogPrerollMs,
        &AppPrefs::setAnalogPrerollMs,
        DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS,
        500,
    },
};

/* @p wide as a restored backup holds it (a JSON integer, a JSON number too big for one), or as the file reads back. */
QVariant
oversized_as(int form, qlonglong wide) {
    if (form == 0) {
        return QVariant(wide);
    }
    if (form == 1) {
        return QVariant(static_cast<double>(wide) * 1.0e10);
    }
    return QVariant(QString::number(wide));
}

void
test_prefs_past_int_range() {
    const qlonglong past = 1LL << 32;
    for (int form = 0; form < 3; form++) {
        QVariantMap incoming;
        for (const WideIntPref& pref : kWideIntPrefs) {
            incoming.insert(QLatin1String(pref.key), oversized_as(form, past + pref.low));
        }
        expect("a backup with oversized integers restores", AppPrefs::replacePortable(incoming));
        AppPrefs prefs;
        for (const WideIntPref& pref : kWideIntPrefs) {
            if ((prefs.*pref.get)() != pref.fallback) {
                DSD_FPRINTF(stderr, "FAIL: %s (form %d) read %d, not %d\n", pref.key, form, (prefs.*pref.get)(),
                            pref.fallback);
                g_failures++;
            }
        }
    }
    /* The setters compare with what is stored, read wide: writing the low bits is a change, so it lands. */
    AppPrefs prefs;
    QSettings stored = prefs_file();
    for (const WideIntPref& pref : kWideIntPrefs) {
        stored.setValue(QLatin1String(pref.key), past + pref.low);
        (prefs.*pref.set)(pref.low);
        if (stored.value(QLatin1String(pref.key)).toLongLong() != pref.low || (prefs.*pref.get)() != pref.low) {
            DSD_FPRINTF(stderr, "FAIL: %s: writing %d over its oversized value did not land\n", pref.key, pref.low);
            g_failures++;
        }
        stored.remove(QLatin1String(pref.key));
    }
}

/* Issue #574: the screen mode and its delay, which replaced the keep-awake switch.
 * The mode codes are a contract with the Android policy (ScreenPolicy.Mode). */
void
test_screen_policy_prefs() {
    QSettings stored(QSettings::IniFormat, QSettings::UserScope, "dsd-neo", "dsd-neo-app");
    const QString modeKey = QStringLiteral("listen/screenMode");
    const QString delayKey = QStringLiteral("listen/screenDelaySec");
    const QString legacyKey = QStringLiteral("listen/keepAwake");
    stored.remove(modeKey);
    stored.remove(delayKey);
    stored.remove(legacyKey);

    static_assert(AppPrefs::ScreenSystem == 0 && AppPrefs::ScreenAlwaysOn == 1 && AppPrefs::ScreenDimBetweenCalls == 2
                      && AppPrefs::ScreenOffBetweenCalls == 3,
                  "screen mode codes are a contract with ScreenPolicy.Mode");
    expect("screen mode is a registered enum", AppPrefs::staticMetaObject.indexOfEnumerator("ScreenMode") >= 0);
    {
        AppPrefs prefs;
        int changes = 0;
        QObject::connect(&prefs, &AppPrefs::screenPolicyChanged, &prefs, [&changes]() { ++changes; });
        expect("the keep-awake preference is gone", prefs.metaObject()->indexOfProperty("keepScreenAwake") == -1);
        expect("screen delay choices are the policy's",
               prefs.screenDelayChoices() == QVariantList({10, 30, 60, 120, 300}));
        expect("screen mode defaults to the system's", prefs.screenMode() == AppPrefs::ScreenSystem);
        expect("screen delay defaults to 30 s", prefs.screenDelaySec() == 30);

        /* Migration on read: an install that had the switch on keeps the screen on. */
        stored.setValue(legacyKey, true);
        expect("legacy keep-awake on reads as Always on", prefs.screenMode() == AppPrefs::ScreenAlwaysOn);
        stored.setValue(legacyKey, false);
        expect("legacy keep-awake off reads as System default", prefs.screenMode() == AppPrefs::ScreenSystem);
        stored.setValue(legacyKey, true);
        expect("migrating writes nothing", !stored.contains(modeKey));
        expect("migrating does not notify", changes == 0);

        /* Choosing what the migration already shows is not a change. */
        prefs.setScreenMode(AppPrefs::ScreenAlwaysOn);
        expect("choosing the migrated mode is not a change", changes == 0 && !stored.contains(modeKey));
        prefs.setScreenMode(AppPrefs::ScreenDimBetweenCalls);
        expect("a new mode reads back", prefs.screenMode() == AppPrefs::ScreenDimBetweenCalls);
        expect("a new mode notifies once", changes == 1);
        expect("a new mode lands on the new key", stored.value(modeKey).toInt() == AppPrefs::ScreenDimBetweenCalls);
        expect("the legacy key is left for a downgrade", stored.value(legacyKey).toBool());
        prefs.setScreenMode(AppPrefs::ScreenDimBetweenCalls);
        expect("a repeated mode does not notify", changes == 1);
        prefs.setScreenMode(9);
        expect("an out-of-range mode is written as System default",
               prefs.screenMode() == AppPrefs::ScreenSystem && stored.value(modeKey).toInt() == AppPrefs::ScreenSystem);
        expect("the out-of-range write notifies once", changes == 2);
        prefs.setScreenMode(AppPrefs::ScreenDimBetweenCalls);
        expect("the mode can be chosen again", changes == 3);

        prefs.setScreenDelaySec(120);
        expect("a listed delay is kept", prefs.screenDelaySec() == 120);
        expect("a new delay notifies once", changes == 4);
        prefs.setScreenDelaySec(120);
        expect("a repeated delay does not notify", changes == 4);
        prefs.setScreenDelaySec(45);
        expect("an off-list delay is written as 30 s",
               prefs.screenDelaySec() == 30 && stored.value(delayKey).toInt() == 30);
        expect("the off-list write notifies once", changes == 5);
        prefs.setScreenDelaySec(30);
        expect("re-choosing the corrected delay does not notify", changes == 5);
        prefs.setScreenDelaySec(120);
        expect("the delay can be chosen again", changes == 6);
    }
    {
        AppPrefs reloaded;
        expect("screen mode persists across instances", reloaded.screenMode() == AppPrefs::ScreenDimBetweenCalls);
        expect("screen delay persists across instances", reloaded.screenDelaySec() == 120);
        expect("the stored mode wins over the legacy key",
               stored.value(legacyKey).toBool() && reloaded.screenMode() != AppPrefs::ScreenAlwaysOn);

        int changes = 0;
        QObject::connect(&reloaded, &AppPrefs::screenPolicyChanged, &reloaded, [&changes]() { ++changes; });
        /* A damaged or hand-edited file reads as the defaults, never as a mode the
         * policy has no branch for. */
        stored.setValue(modeKey, 9);
        expect("an out-of-range stored mode reads as System default", reloaded.screenMode() == AppPrefs::ScreenSystem);
        stored.setValue(modeKey, QStringLiteral("abc"));
        expect("an unparsable stored mode reads as System default", reloaded.screenMode() == AppPrefs::ScreenSystem);
        stored.setValue(delayKey, 45);
        expect("an off-list stored delay reads as 30 s", reloaded.screenDelaySec() == 30);
        stored.setValue(delayKey, QStringLiteral("abc"));
        expect("an unparsable stored delay reads as 30 s", reloaded.screenDelaySec() == 30);
        expect("reading damaged values does not notify", changes == 0);

        /* The setters compare with what is stored, so the corrective write lands. */
        reloaded.setScreenMode(AppPrefs::ScreenSystem);
        expect("a corrective mode write replaces the damage",
               stored.value(modeKey).toString() == QStringLiteral("0") && changes == 1);
        reloaded.setScreenDelaySec(30);
        expect("a corrective delay write replaces the damage",
               stored.value(delayKey).toString() == QStringLiteral("30") && changes == 2);
    }
    stored.remove(modeKey);
    stored.remove(delayKey);
    stored.remove(legacyKey);
}

/* Saved-system migration warnings so far. The log tap is installed once and never replaced, so what it counts into
 * must live as long as the process. */
int g_migration_warnings = 0;

void
count_migration_warnings() {
    dsd_neo_log_set_tap(
        [](dsd_neo_log_level_t level, const char* text, void*) {
            if (level == LOG_LEVEL_WARN && QString::fromUtf8(text).contains("migration could not be persisted")) {
                ++g_migration_warnings;
            }
        },
        nullptr);
}

void
test_migration_write_failure() {
    const QString store = QStringLiteral("saved_systems.json");
    expect("legacy fixture saved", json_store_save_array(store, QJsonArray{QJsonObject{{"name", "legacy"}}}));
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto permissions = QFile::permissions(directory);
    expect("migration directory made read only", QFile::setPermissions(directory, QFile::ReadOwner | QFile::ExeOwner));
    count_migration_warnings();
    const int before = g_migration_warnings;
    SavedSystemsModel model;
    expect("migration warns once when write fails", g_migration_warnings - before == 1);
    expect("failed migration keeps readable row", model.count() == 1);
    expect("migration permissions restored", QFile::setPermissions(directory, permissions));
}

void
test_foundation_persistence() {
    const QString store = QStringLiteral("saved_systems.json");
    json_store_save_array(store, QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("legacy")}}});
    SavedSystemsModel legacy;
    const QString uid = legacy.get(0).value(QStringLiteral("uid")).toString();
    expect("legacy gets a UUID", !QUuid(uid).isNull());
    SavedSystemsModel migrated;
    expect("migration persists without an edit", migrated.get(0).value(QStringLiteral("uid")).toString() == uid);
    expect("UID lookup finds row", migrated.rowForUid(uid) == 0);
    expect("unknown UID never selects a row",
           migrated.rowForUid(QString()) == -1 && migrated.getByUid("missing").isEmpty());
    QVariantMap fields{{"encKeyType", "rc4"}, {"encKeyValue", "0011223344"},
                       {"encForceKey", 2},    {"rrSid", 123},
                       {"rrSiteId", 456},     {"siteName", "North"},
                       {"siteLat", 42.5},     {"siteLon", -87.5},
                       {"hasSitePos", true},  {"avoidSite", true}};
    migrated.update(0, fields);
    SavedSystemsModel reloaded;
    expect("get map hides key", !reloaded.get(0).contains("encKeyValue"));
    expect("UID map hides key", !reloaded.getByUid(uid).contains("encKeyValue"));
    expect("model roles hide key", !reloaded.roleNames().values().contains("encKeyValue"));
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        expect("optional field survives reload", (it.key() == "encKeyValue" ? QVariant(reloaded.keyValueForUid(uid))
                                                                            : reloaded.getByUid(uid).value(it.key()))
                                                     == it.value());
    }
    reloaded.add(reloaded.get(0));
    expect("copy gets a distinct identity", reloaded.get(1).value("uid") != uid);
    reloaded.remove(0);
    expect("deleted UID cannot address a shifted row", reloaded.rowForUid(uid) == -1);

    AppPrefs prefs;
    for (const char* property : {"lastLat", "lastLon", "lastFixAt"}) {
        expect("location properties require atomic fix publication",
               !prefs.metaObject()->property(prefs.metaObject()->indexOfProperty(property)).isWritable());
    }
    expect("attach defaults off", !prefs.autoStartOnAttach());
    prefs.setAutoStartOnAttach(true);
    prefs.setLastStartedKind("saved");
    prefs.setLastStartedUid(uid);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool coherentFix = false;
    const auto connection = QObject::connect(&prefs, &AppPrefs::locationChanged, &prefs, [&]() {
        coherentFix = prefs.lastLat() == 42.5 && prefs.lastLon() == -87.5 && prefs.lastFixAt() == now;
    });
    prefs.setLocationFix(42.5, -87.5, now, 125);
    expect("fix accuracy retained", prefs.lastAccuracyM() == 125);
    {
        dsd_qt::AppPrefs restored;
        expect("fix accuracy survives reload", restored.lastAccuracyM() == 125);
    }
    expect("fix notification sees all coordinates and timestamp", coherentFix);
    QObject::disconnect(connection);
    AppPrefs fresh;
    expect("attach and session prefs persist",
           fresh.autoStartOnAttach() && fresh.lastStartedKind() == "saved" && fresh.lastStartedUid() == uid);
    expect("fresh fix persists", fresh.lastLat() == 42.5 && fresh.lastLon() == -87.5 && fresh.lastFixAt() == now);
    prefs.setLastFixAt(now - 24LL * 60 * 60 * 1000 - 1);
    expect("expired fix reads absent", prefs.lastFixAt() == 0 && prefs.lastLat() == 0 && prefs.lastLon() == 0);
    QSettings stored(QSettings::IniFormat, QSettings::UserScope, "dsd-neo", "dsd-neo-app");
    expect("expired fix removed from disk", !stored.contains("location/lastLat") && !stored.contains("location/lastLon")
                                                && !stored.contains("location/lastFixAt")
                                                && !stored.contains("location/lastAccuracyM"));
}

void
test_key_persistence() {
    SavedSystemsModel model;
    while (model.count()) {
        model.remove(0);
    }
    const QStringList types{"", "basic", "hex", "rc4", "scrambler"};
    for (const auto& type : types) {
        QVariantMap row{{"name", "key persistence"},
                        {"encKeyType", type},
                        {"encKeyValue", type.isEmpty() ? QString() : QStringLiteral("0")},
                        {"encForceKey", 2}};
        model.add(row);
    }
    SavedSystemsModel fresh;
    expect("all saved key types reload", fresh.count() == types.size());
    for (int i = 0; i < types.size(); ++i) {
        const auto row = fresh.get(i);
        expect("key type round trip", row.value("encKeyType").toString() == types[i]);
        expect("key value round trip", fresh.keyValueForUid(row.value("uid").toString())
                                           == (types[i].isEmpty() ? QString() : QStringLiteral("0")));
        expect("force round trip", row.value("encForceKey").toInt() == 2);
    }
    const QString uid = fresh.get(1).value("uid").toString();
    auto edit = fresh.getByUid(uid);
    edit.insert("name", "Renamed keyed system");
    fresh.update(1, edit);
    dsd_qt::SavedSystemsModel kept;
    expect("redacted update preserves key in memory", fresh.keyValueForUid(uid) == "0");
    expect("redacted update preserves key on disk", kept.keyValueForUid(uid) == "0");
    expect("configured flag survives update", kept.getByUid(uid).value("encKeyConfigured").toBool());
    fresh.update(1, {{"encKeyType", ""}, {"encKeyValue", ""}, {"encForceKey", 0}});
    SavedSystemsModel cleared;
    expect("key clearing persists", cleared.get(1).value("encKeyType").toString().isEmpty()
                                        && cleared.keyValueForUid(cleared.get(1).value("uid").toString()).isEmpty()
                                        && cleared.get(1).value("encForceKey").toInt() == 0);
}

void
test_add_with_retained_key() {
    SavedSystemsModel model;
    while (model.count()) {
        model.remove(0);
    }
    const QString key = QString::number(17 * 3);
    model.add({{"name", "Private source"}, {"encKeyType", "basic"}, {"encKeyValue", key}});
    const QString sourceUid = model.get(0).value("uid").toString();
    auto site = model.get(0);
    expect("source map hides retained key", !site.contains("encKeyValue"));
    site.insert("rrSid", 12);
    site.insert("rrSiteId", 16863);
    expect("retained key copy accepts first site", model.addWithKeyFrom(sourceUid, site));
    site.insert("rrSiteId", 48391);
    expect("retained key copy accepts second site", model.addWithKeyFrom(sourceUid, site));
    SavedSystemsModel reloaded;
    expect("source and both sites persist", reloaded.count() == 3);
    for (int row = 1; row < reloaded.count(); ++row) {
        const auto stored = reloaded.get(row);
        const QString uid = stored.value("uid").toString();
        expect("copied site receives a new identity", uid != sourceUid && !uid.isEmpty());
        expect("private key copied and persisted", reloaded.keyValueForUid(uid) == key);
        expect("copied maps hide private key",
               !stored.contains("encKeyValue") && !reloaded.getByUid(uid).contains("encKeyValue"));
    }
    expect("model roles hide copied key", !reloaded.roleNames().values().contains("encKeyValue"));
    auto mismatched = site;
    mismatched.insert("encKeyType", "rc4");
    expect("retained copy rejects a different type", !model.addWithKeyFrom(sourceUid, mismatched));
    auto replacement = site;
    replacement.insert("encKeyValue", key);
    expect("replacement uses ordinary add", !model.addWithKeyFrom(sourceUid, replacement));
    expect("rejected copies do not append", model.count() == 3);
    model.remove(0);
    const int before = model.count();
    expect("deleted source cannot copy from shifted row", !model.addWithKeyFrom(sourceUid, site));
    expect("failed copy creates no incomplete row", model.count() == before);
}

void
test_site_provenance_edits() {
    SavedSystemsModel model;
    while (model.count()) {
        model.remove(0);
    }
    const QVariantMap site{{"rrSid", 12},        {"rrSiteId", 16863},   {"siteLat", 41.65503}, {"siteLon", -91.60244},
                           {"hasSitePos", true}, {"siteName", "North"}, {"freqMhz", "851"}};
    for (const char* key :
         {"freqMhz", "decodeFlag", "chanCsvPath", "groupCsvPath", "keyCsvPath", "p25BandplanCsvPath", "srcCsvPath"}) {
        model.add(site);
        const int row = model.count() - 1;
        model.update(row, {{key, "changed"}});
        SavedSystemsModel reloaded;
        expect("site editing clears both persisted ids",
               reloaded.get(row).value("rrSid").toInt() == 0 && reloaded.get(row).value("rrSiteId").toInt() == 0);
    }
    auto incomplete = site;
    incomplete.remove("siteLon");
    model.add(incomplete);
    expect("missing coordinate is not a position", !model.get(model.count() - 1).value("hasSitePos").toBool());
    model.add(site);
    model.update(model.count() - 1, {{"siteLat", "invalid"}});
    expect("malformed coordinates cannot become equator", !model.get(model.count() - 1).value("hasSitePos").toBool());
}

/* A directory where the systems file goes fails every save, whatever the permissions the test runs with. */
QString
systems_store_path() {
    return json_store_path(QStringLiteral("saved_systems.json"));
}

bool
block_systems_store() {
    const QString path = systems_store_path();
    if (!QFile::rename(path, path + ".saved")) {
        return false;
    }
    if (!QDir().mkpath(path)) {
        // Put the store back, so the tests after this one still read and write it.
        QFile::rename(path + ".saved", path);
        return false;
    }
    return true;
}

bool
unblock_systems_store() {
    const QString path = systems_store_path();
    return QDir().rmdir(path) && QFile::rename(path + ".saved", path);
}

/* Every change signal @p model sends, by name, in order, into @p log (declared before the model, so it outlives it). */
void
record_signals(SavedSystemsModel& model, QStringList& log) {
    const auto note = [&model, &log](auto signal, const char* name) {
        QObject::connect(&model, signal, &model, [&log, name]() { log.append(QString::fromLatin1(name)); });
    };
    note(&SavedSystemsModel::rowsAboutToBeRemoved, "rowsAboutToBeRemoved");
    note(&SavedSystemsModel::rowsRemoved, "rowsRemoved");
    note(&SavedSystemsModel::dataChanged, "dataChanged");
    note(&SavedSystemsModel::sitesChanged, "sitesChanged");
    note(&SavedSystemsModel::countChanged, "countChanged");
    note(&SavedSystemsModel::mostRecentRowChanged, "mostRecentRowChanged");
}

void
empty_systems(SavedSystemsModel& model) {
    // A removal can fail now (a store left blocked); stop there rather than retry forever.
    while (model.count() && model.remove(0)) {}
    expect("the systems empty", model.count() == 0);
}

/* A removal whose save fails changes nothing: the card stays, and anything that follows rows out (a pinned shortcut)
 * keeps it, because the system is still there on the next launch. A saved removal signals rows, sites, count, then
 * the most recent row. */
void
test_remove_failure_keeps_row() {
    QStringList log;
    SavedSystemsModel model;
    empty_systems(model);
    expect("remove: first system", model.add({{"name", "First"}}));
    expect("remove: second system", model.add({{"name", "Second"}}));
    record_signals(model, log);
    expect("remove: store blocked", block_systems_store());
    expect("remove: a failed save reports failure", !model.remove(0));
    expect("remove: a failed save keeps the row",
           model.count() == 2 && model.get(0).value("name") == "First" && model.get(1).value("name") == "Second");
    expect("remove: a failed save sends no signal", log.isEmpty());
    expect("remove: store unblocked", unblock_systems_store());
    {
        SavedSystemsModel reloaded;
        expect("remove: a failed removal is still on disk", reloaded.count() == 2);
    }
    expect("remove: a saved removal reports success", model.remove(0));
    expect("remove: a saved removal drops the row", model.count() == 1 && model.get(0).value("name") == "Second");
    expect("remove: a saved removal signals rows, sites, count and recency in order",
           log
               == QStringList{"rowsAboutToBeRemoved", "rowsRemoved", "sitesChanged", "countChanged",
                              "mostRecentRowChanged"});
    expect("remove: a row past the end is refused", !model.remove(1) && !model.remove(-1));
    SavedSystemsModel reloaded;
    expect("remove: a saved removal persists", reloaded.count() == 1);
}

/* Recency is advisory, so touch() reports nothing, but a stamp that did not save stays out of memory: the most recent
 * row the app shows is the one the next launch shows. */
void
test_touch_failure_keeps_memory() {
    QStringList log;
    SavedSystemsModel model;
    empty_systems(model);
    expect("touch: system", model.add({{"name", "Heard"}}));
    record_signals(model, log);
    expect("touch: store blocked", block_systems_store());
    model.touch(0);
    expect("touch: a failed save keeps lastHeard", model.get(0).value("lastHeard").toLongLong() == 0);
    expect("touch: a failed save sends no signal", log.isEmpty());
    expect("touch: store unblocked", unblock_systems_store());
    model.touch(0);
    expect("touch: a saved stamp lands", model.get(0).value("lastHeard").toLongLong() > 0);
    SavedSystemsModel reloaded;
    expect("touch: a saved stamp persists", reloaded.get(0).value("lastHeard").toLongLong() > 0);
}

/* Clearing a removed library file's references saves first: a failure reports false and leaves every reference (and
 * the site's group) as it was, so a retry can finish it. A path nothing references needs no save. */
void
test_clear_csv_failure_keeps_memory() {
    const QString path = QStringLiteral("/data/imports/county.csv");
    QStringList log;
    SavedSystemsModel model;
    empty_systems(model);
    expect("clear: grouped site",
           model.add({{"name", "North"}, {"rrSid", 12}, {"rrSiteId", 1}, {"groupCsvPath", path}}));
    expect("clear: second system", model.add({{"name", "Butler"}, {"chanCsvPath", path}}));
    record_signals(model, log);
    expect("clear: store blocked", block_systems_store());
    expect("clear: a failed save reports failure", !model.clearCsvPath(path));
    expect("clear: a failed save keeps every reference",
           model.get(0).value("groupCsvPath") == path && model.get(1).value("chanCsvPath") == path);
    expect("clear: a failed save keeps the site in its group",
           model.get(0).value("rrSid").toInt() == 12 && model.get(0).value("rrSiteId").toInt() == 1);
    expect("clear: a failed save sends no signal", log.isEmpty());
    expect("clear: an unreferenced path needs no save", model.clearCsvPath(QStringLiteral("/data/imports/none.csv")));
    expect("clear: store unblocked", unblock_systems_store());
    expect("clear: a saved clear reports success", model.clearCsvPath(path));
    expect("clear: a saved clear blanks every reference",
           model.get(0).value("groupCsvPath").toString().isEmpty()
               && model.get(1).value("chanCsvPath").toString().isEmpty());
    SavedSystemsModel reloaded;
    expect("clear: a saved clear persists", reloaded.get(0).value("groupCsvPath").toString().isEmpty()
                                                && reloaded.get(1).value("chanCsvPath").toString().isEmpty());
}

/* The site chooser's Avoid reports a failed save, so the toggle can show the stored state. */
void
test_set_avoid_failure_reports() {
    SavedSystemsModel model;
    empty_systems(model);
    expect("avoid: north site", model.add({{"name", "North"}, {"rrSid", 12}, {"rrSiteId", 1}}));
    expect("avoid: south site", model.add({{"name", "South"}, {"rrSid", 12}, {"rrSiteId", 2}}));
    expect("avoid: store blocked", block_systems_store());
    expect("avoid: a failed save reports failure", !model.setAvoidSite(1, true));
    expect("avoid: a failed save leaves the site unavoided", !model.get(1).value("avoidSite").toBool());
    expect("avoid: store unblocked", unblock_systems_store());
    expect("avoid: a saved avoid reports success", model.setAvoidSite(1, true));
    SavedSystemsModel reloaded;
    expect("avoid: a saved avoid persists", reloaded.get(1).value("avoidSite").toBool());
    expect("avoid: a row past the end is refused", !model.setAvoidSite(2, true));
}

/* An avoid is lifted only in the group's site chooser, so a site that leaves its group (an edited frequency, decode
 * flag or file, or a removed library file) drops its avoid with the group; an edit that keeps the group keeps it. */
void
test_avoid_cleared_when_site_leaves_group() {
    const QString path = QStringLiteral("/data/imports/county.csv");
    SavedSystemsModel model;
    empty_systems(model);
    for (int site = 1; site <= 4; ++site) {
        expect("leave: site", model.add({{"name", QStringLiteral("Site %1").arg(site)},
                                         {"freqMhz", "851"},
                                         {"rrSid", 12},
                                         {"rrSiteId", site},
                                         {"groupCsvPath", path}}));
    }
    expect("leave: two sites avoided", model.setAvoidSite(1, true) && model.setAvoidSite(2, true));
    expect("leave: a rename", model.update(1, {{"name", "Renamed"}}));
    expect("leave: a rename keeps the group and the avoid",
           model.get(1).value("rrSid").toInt() == 12 && model.get(1).value("avoidSite").toBool());
    expect("leave: a frequency edit", model.update(1, {{"freqMhz", "852"}}));
    expect("leave: a frequency edit takes the site out of its group and drops its avoid",
           model.get(1).value("rrSid").toInt() == 0 && !model.get(1).value("avoidSite").toBool());
    expect("leave: the other avoided site keeps its avoid", model.get(2).value("avoidSite").toBool());
    // Listening for another protocol from the Radio controls saves the decode flag through the same edit.
    expect("leave: a third site avoided", model.setAvoidSite(3, true));
    expect("leave: a decoder change from the Radio controls",
           model.updateRadioSettings(model.get(3).value("uid").toString(), {{"decodeFlag", "-fs"}}));
    expect("leave: a decoder change takes the site out of its group and drops its avoid",
           model.get(3).value("rrSid").toInt() == 0 && !model.get(3).value("avoidSite").toBool());
    expect("leave: the library file goes", model.clearCsvPath(path));
    expect("leave: a removed library file drops the avoid of the sites it ungroups",
           model.get(2).value("rrSid").toInt() == 0 && !model.get(2).value("avoidSite").toBool());
    SavedSystemsModel reloaded;
    expect("leave: the dropped avoids persist", !reloaded.get(1).value("avoidSite").toBool()
                                                    && !reloaded.get(2).value("avoidSite").toBool()
                                                    && !reloaded.get(3).value("avoidSite").toBool());
}

/* Rows already stored with an avoid outside a group (saved before the rule above) lose it at load, and the load saves
 * that as it saves a UID repair; a lone RadioReference site keeps its avoid, since its "1 site" chooser lifts it. */
void
test_load_migrates_orphaned_avoid() {
    const QString store = QStringLiteral("saved_systems.json");
    const auto row = [](const char* name, int sid, int site) {
        return QJsonObject{{"uid", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                           {"name", name},
                           {"rrSid", sid},
                           {"rrSiteId", site},
                           {"avoidSite", true}};
    };
    expect("orphan: fixture saved",
           json_store_save_array(store, QJsonArray{row("Orphan", 0, 0), row("No site", 12, 0), row("Lone", 12, 5)}));
    {
        SavedSystemsModel model;
        expect("orphan: an ungrouped row loads unavoided", !model.get(0).value("avoidSite").toBool());
        expect("orphan: a row with no site id loads unavoided", !model.get(1).value("avoidSite").toBool());
        expect("orphan: a lone RadioReference site keeps its avoid", model.get(2).value("avoidSite").toBool());
    }
    const QJsonArray stored = json_store_load_array(store);
    expect("orphan: the cleared avoids are saved without an edit",
           !stored.at(0).toObject().value("avoidSite").toBool() && !stored.at(1).toObject().value("avoidSite").toBool()
               && stored.at(2).toObject().value("avoidSite").toBool());

    expect("orphan: unsaved fixture", json_store_save_array(store, QJsonArray{row("Orphan", 0, 0)}));
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto permissions = QFile::permissions(directory);
    expect("orphan: data directory made read only",
           QFile::setPermissions(directory, QFile::ReadOwner | QFile::ExeOwner));
    count_migration_warnings();
    const int before = g_migration_warnings;
    {
        SavedSystemsModel model;
        expect("orphan: an unsaved clear still clears in memory", !model.get(0).value("avoidSite").toBool());
    }
    expect("orphan: an unsaved clear warns once", g_migration_warnings - before == 1);
    expect("orphan: data directory permissions restored", QFile::setPermissions(directory, permissions));
    SavedSystemsModel model;
    empty_systems(model);
}

void
test_foundation_key_type_migration() {
    QJsonArray rows;
    rows.append(QJsonObject{{"encKeyType", 0}, {"encKeyValue", ""}});
    rows.append(QJsonObject{{"encKeyType", 0}, {"encKeyValue", "0"}});
    rows.append(QJsonObject{{"encKeyType", 2}, {"encKeyValue", "0"}});
    json_store_save_array(QStringLiteral("saved_systems.json"), rows);
    SavedSystemsModel model;
    expect("foundation blank key remains absent", model.get(0).value("encKeyType").toString().isEmpty());
    expect("foundation basic type converts", model.get(1).value("encKeyType").toString() == "basic");
    expect("foundation rc4 type converts", model.get(2).value("encKeyType").toString() == "rc4");
}

struct PinnedKey {
    const char* property;
    QVariant value; // differs from the default and passes the setter's sanitiser
    const char* key;
};

void
test_prefs_key_table(void) {
    clear_prefs_file();
    QStringList names;
    for (const AppPrefs::KeySpec& k : AppPrefs::keyTable()) {
        names.append(QString::fromLatin1(k.name));
    }
    expect("key table has no duplicate names", names.removeDuplicates() == 0);

    const PinnedKey pinned[] = {
        {"appearance", 2, "ui/appearance"},
        {"metricUnits", true, "ui/metricUnits"},
        {"onboardingDone", true, "ui/onboardingDone"},
        {"notificationExplained", true, "ui/notificationExplained"},
        {"backgroundListening", false, "listen/background"},
        {"screenMode", 2, "listen/screenMode"},
        {"screenDelaySec", 60, "listen/screenDelaySec"},
        {"autoStartOnAttach", true, "listen/autoStartOnAttach"},
        {"lastStartedKind", QStringLiteral("scan"), "listen/lastStartedKind"},
        {"lastStartedUid", QStringLiteral("uid-1"), "listen/lastStartedUid"},
        {"skipEncrypted", false, "decode/skipEncrypted"},
        {"persistTgLockouts", false, "decode/persistTgLockouts"},
        {"hangtimeSec", 5.5, "decode/hangtimeSec"},
        {"autoPpm", true, "decode/autoPpm"},
        {"relaxedCrc", true, "decode/relaxedCrc"},
        {"extraArgs", QStringLiteral("-x"), "decode/extraArgs"},
        {"gainDb", 41, "tuner/gainDb"},
        {"ppm", 7, "tuner/ppm"},
        {"bandwidthKhz", 96, "tuner/bandwidthKhz"},
        {"biasTee", true, "tuner/biasTee"},
        {"rrUsername", QStringLiteral("user"), "rr/username"},
        {"rrAppKey", QStringLiteral("appkey"), "rr/appKey"},
        {"exploreSourceType", QStringLiteral("usb"), "explore/sourceType"},
        {"exploreHost", QStringLiteral("host"), "explore/host"},
        {"explorePort", 4321, "explore/port"},
        {"exploreFreqMhz", QStringLiteral("851.5"), "explore/freqMhz"},
        {"searchSourceType", QStringLiteral("airspy"), "search/sourceType"},
        {"searchHost", QStringLiteral("host"), "search/host"},
        {"searchPort", 4321, "search/port"},
        {"monitorDetailsExpanded", true, "monitor/detailsExpanded"},
        {"recordCalls", true, "recording/recordCalls"},
        {"recordUnknownOnly", true, "recording/recordUnknownOnly"},
        {"recordingLimitMb", 5000, "recording/limitMb"},
        {"analogMaxGapMs", 2000, "recording/analogMaxGapMs"},
        {"analogPrerollMs", 500, "recording/analogPrerollMs"},
        {"acceptedStartKind", QStringLiteral("scan"), "listen/acceptedStartKind"},
        {"acceptedStartUid", QStringLiteral("uid-2"), "listen/acceptedStartUid"},
        // Issue #697: a scan session's other lists, as accepted and as last started.
        {"acceptedStartCompanions", QStringList{QStringLiteral("uid-3"), QStringLiteral("uid-4")},
         "listen/acceptedStartCompanions"},
        {"lastStartedCompanions", QStringList{QStringLiteral("uid-3")}, "listen/lastStartedCompanions"},
        // Issue #644: the accepted scan session's targets.
        {"acceptedStartTargets",
         QVariantList{QVariantMap{{QStringLiteral("targetId"), QStringLiteral("t1")},
                                  {QStringLiteral("frequencyHz"), 851012500}}},
         "listen/acceptedStartTargets"},
        // Issue #801: the shortcuts pinned to Listen.
        {"pinned", QVariantList{QVariantMap{{QStringLiteral("kind"), QStringLiteral("search")}}}, "listen/pinned"},
    };
    QStringList covered;
    int writable = 0;
    {
        AppPrefs probe;
        const QMetaObject* mo = probe.metaObject();
        for (int i = mo->propertyOffset(); i < mo->propertyCount(); ++i) {
            writable += mo->property(i).isWritable() ? 1 : 0;
        }
    }
    expect("every writable property is pinned", writable == int(sizeof(pinned) / sizeof(pinned[0])));
    for (const PinnedKey& pin : pinned) {
        clear_prefs_file();
        {
            AppPrefs prefs;
            expect(pin.property, prefs.setProperty(pin.property, pin.value));
        }
        QSettings f = prefs_file();
        f.sync();
        expect(qPrintable(
                   QStringLiteral("%1 lands exactly on %2").arg(QLatin1String(pin.property), QLatin1String(pin.key))),
               f.allKeys() == QStringList{QString::fromLatin1(pin.key)});
        covered.append(QString::fromLatin1(pin.key));
    }
    // The location fix writes four keys together.
    clear_prefs_file();
    {
        AppPrefs prefs;
        prefs.setLocationFix(1.5, 2.5, QDateTime::currentMSecsSinceEpoch(), 7.0);
    }
    {
        QSettings f = prefs_file();
        f.sync();
        QStringList got = f.allKeys();
        got.sort();
        const QStringList want = {QStringLiteral("location/lastAccuracyM"), QStringLiteral("location/lastFixAt"),
                                  QStringLiteral("location/lastLat"), QStringLiteral("location/lastLon")};
        expect("setLocationFix writes exactly the four location keys", got == want);
        covered.append(want);
    }
    // The legacy keep-awake switch is read, never written: pin it through the migration.
    clear_prefs_file();
    {
        QSettings f = prefs_file();
        f.setValue(QStringLiteral("listen/keepAwake"), true);
        f.sync();
        AppPrefs prefs;
        expect("legacy listen/keepAwake drives the migrated screen mode",
               prefs.screenMode() == AppPrefs::ScreenAlwaysOn);
        covered.append(QStringLiteral("listen/keepAwake"));
    }
    QStringList sortedCovered = covered;
    QStringList sortedNames = names;
    sortedCovered.sort();
    sortedNames.sort();
    expect("pinned keys cover exactly the key table", sortedCovered == sortedNames);

    // Leave the state the replace checks below expect: one value per portable key.
    clear_prefs_file();
    {
        AppPrefs prefs;
        for (const PinnedKey& pin : pinned) {
            prefs.setProperty(pin.property, pin.value);
        }
        prefs.setLocationFix(1.5, 2.5, QDateTime::currentMSecsSinceEpoch(), 7.0);
    }
    QSettings file = prefs_file();
    file.sync();
    expect("the pinned writes persist all but the legacy key", file.allKeys().size() == names.size() - 1);

    QStringList local;
    for (const AppPrefs::KeySpec& k : AppPrefs::keyTable()) {
        if (!k.portable) {
            local.append(QString::fromLatin1(k.name));
        }
    }
    local.sort();
    const QStringList wantLocal = {
        QStringLiteral("listen/acceptedStartCompanions"),
        QStringLiteral("listen/acceptedStartKind"),
        QStringLiteral("listen/acceptedStartTargets"),
        QStringLiteral("listen/acceptedStartUid"),
        QStringLiteral("listen/lastStartedCompanions"),
        QStringLiteral("location/lastAccuracyM"),
        QStringLiteral("location/lastFixAt"),
        QStringLiteral("location/lastLat"),
        QStringLiteral("location/lastLon"),
        QStringLiteral("monitor/detailsExpanded"),
        QStringLiteral("ui/notificationExplained"),
        QStringLiteral("ui/onboardingDone"),
    };
    expect("device-local set is exactly the pinned one", local == wantLocal);
    const QStringList portable = AppPrefs::portableKeys();
    expect("portable keys are the table minus device-local",
           portable.size() + local.size() == names.size() && !portable.contains(QStringLiteral("ui/onboardingDone"))
               && portable.contains(QStringLiteral("listen/keepAwake"))
               && portable.contains(QStringLiteral("listen/lastStartedUid"))
               && portable.contains(QStringLiteral("rr/appKey"))
               && portable.contains(QStringLiteral("explore/sourceType"))
               && portable.contains(QStringLiteral("search/sourceType"))
               && portable.contains(QStringLiteral("search/host")) && portable.contains(QStringLiteral("search/port"))
               && portable.contains(QStringLiteral("recording/recordCalls"))
               && portable.contains(QStringLiteral("recording/recordUnknownOnly"))
               && portable.contains(QStringLiteral("recording/limitMb"))
               && portable.contains(QStringLiteral("recording/analogMaxGapMs"))
               && portable.contains(QStringLiteral("recording/analogPrerollMs"))
               && portable.contains(QStringLiteral("listen/pinned")));

    const QVariantMap before = AppPrefs::readPortable();
    expect("readPortable returns only portable keys",
           !before.isEmpty() && !before.contains(QStringLiteral("location/lastLat"))
               && !before.contains(QStringLiteral("ui/onboardingDone"))
               && !before.contains(QStringLiteral("monitor/detailsExpanded")));
    for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
        expect("readPortable key is portable", portable.contains(it.key()));
    }
    expect("readPortable reads stored values", before.value(QStringLiteral("rr/username")).toString() == "user"
                                                   && before.value(QStringLiteral("tuner/gainDb")).toInt() == 41);
    expect("readPortable omits absent keys", !before.contains(QStringLiteral("listen/keepAwake")));
    expect("readPortable carries the remembered radios",
           before.value(QStringLiteral("explore/sourceType")).toString() == "usb"
               && before.value(QStringLiteral("search/sourceType")).toString() == "airspy"
               && before.value(QStringLiteral("search/host")).toString() == "host"
               && before.value(QStringLiteral("search/port")).toInt() == 4321);

    file.setValue(QStringLiteral("ui/onboardingDone"), true);
    file.setValue(QStringLiteral("location/lastLat"), 9.0);
    file.setValue(QStringLiteral("monitor/detailsExpanded"), true);
    file.sync();

    QVariantMap incoming;
    incoming.insert(QStringLiteral("tuner/gainDb"), 42);
    incoming.insert(QStringLiteral("rr/username"), QStringLiteral("restored"));
    incoming.insert(QStringLiteral("listen/keepAwake"), true);
    incoming.insert(QStringLiteral("ui/onboardingDone"), false);
    incoming.insert(QStringLiteral("monitor/detailsExpanded"), false);
    incoming.insert(QStringLiteral("not/a/key"), 1);
    incoming.insert(QStringLiteral("search/sourceType"), QStringLiteral("airspy"));
    incoming.insert(QStringLiteral("search/port"), 5555);
    expect("replacePortable succeeds", AppPrefs::replacePortable(incoming));

    QSettings after = prefs_file();
    after.sync();
    expect("present portable keys are set", after.value(QStringLiteral("tuner/gainDb")).toInt() == 42
                                                && after.value(QStringLiteral("listen/keepAwake")).toBool());
    expect("absent portable keys are removed",
           !after.contains(QStringLiteral("tuner/ppm")) && !after.contains(QStringLiteral("explore/host")));
    expect("device-local keys are untouched", after.value(QStringLiteral("ui/onboardingDone")).toBool()
                                                  && after.value(QStringLiteral("monitor/detailsExpanded")).toBool()
                                                  && after.value(QStringLiteral("location/lastLat")).toInt() == 9);
    expect("unknown keys are ignored", !after.contains(QStringLiteral("not/a/key")));
    {
        AppPrefs fresh;
        expect("a fresh AppPrefs reads the replaced values",
               fresh.gainDb() == 42 && fresh.rrUsername() == "restored" && fresh.ppm() == 0);
    }
    const QVariantMap again = AppPrefs::readPortable();
    expect("readPortable round-trips replacePortable",
           again.size() == 5 && again.value(QStringLiteral("tuner/gainDb")).toInt() == 42
               && again.value(QStringLiteral("search/sourceType")).toString() == "airspy"
               && again.value(QStringLiteral("search/port")).toInt() == 5555);
    {
        AppPrefs fresh;
        expect("a restored search radio reads back", fresh.searchSourceType() == QStringLiteral("airspy")
                                                         && fresh.searchPort() == 5555 && fresh.searchHost().isEmpty());
    }
    expect("empty replace clears portable keys",
           AppPrefs::replacePortable(QVariantMap()) && AppPrefs::readPortable().isEmpty());
}

/* Issue #801: what `listen/pinned` holds after a write that is not the app's own. */
QVariantList
pinned_as_stored(const QVariant& raw) {
    clear_prefs_file();
    {
        QSettings f = prefs_file();
        f.setValue(QStringLiteral("listen/pinned"), raw);
        f.sync();
    }
    const AppPrefs prefs;
    return prefs.pinned();
}

QVariantMap
pin_of(const char* kind, const QString& uid = QString()) {
    return {{QStringLiteral("kind"), QLatin1String(kind)}, {QStringLiteral("uid"), uid}};
}

QString
pins_json(const QVariantList& pins) {
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(pins)).toJson(QJsonDocument::Compact));
}

QString
new_uid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/* The getter is the only validation a restored or hand-edited value gets (a backup carries the text as it is): text
 * that is no JSON array reads as no pins, and each pin keeps only a known kind with the UID it takes, once, at most
 * six of them. */
void
test_pinned_pref_sanitizes() {
    expect("pinned: nothing stored reads as none", pinned_as_stored(QVariant()).isEmpty());
    expect("pinned: text that is no JSON reads as none", pinned_as_stored(QStringLiteral("[{\"kind\":")).isEmpty());
    expect("pinned: a JSON object reads as none", pinned_as_stored(QStringLiteral("{\"kind\":\"search\"}")).isEmpty());
    // An unquoted hand edit: the ini reader splits it at its commas and strips its quotes.
    clear_prefs_file();
    {
        QFile ini(prefs_file().fileName());
        expect("pinned: ini file opens", ini.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ini.write("[listen]\npinned=[{\"kind\":\"search\"}, {\"kind\":\"explore\"}]\n");
    }
    expect("pinned: the unquoted edit reads back as a list",
           prefs_file().value(QStringLiteral("listen/pinned")).metaType().id() == QMetaType::QStringList);
    {
        const AppPrefs prefs;
        expect("pinned: an unquoted hand edit reads as none", prefs.pinned().isEmpty());
    }

    const QString uid = new_uid();
    const QString longScan(129, QLatin1Char('s'));
    const QString boundScan(128, QLatin1Char('b'));
    const QVariantList messy{
        pin_of("favorite", uid),
        pin_of("system", QStringLiteral("not-a-uuid")),
        pin_of("site", QStringLiteral("12")),
        pin_of("scan", QString()),
        pin_of("scan", longScan),
        QStringLiteral("search"),
        pin_of("system", QLatin1Char('{') + uid.toUpper() + QLatin1Char('}')),
        pin_of("system", uid),
        pin_of("search", QStringLiteral("ignored")),
        pin_of("scan", boundScan),
        pin_of("site", uid),
    };
    const QVariantList want{pin_of("system", uid), pin_of("search"), pin_of("scan", boundScan), pin_of("site", uid)};
    expect("pinned: unknown kinds, bad UIDs and repeats dropped, UUIDs in the stored form, no UID on singletons",
           pinned_as_stored(pins_json(messy)) == want);

    QVariantList seven{pin_of("search"), pin_of("explore")};
    for (int i = 0; i < 5; ++i) {
        seven.append(pin_of("scan", QStringLiteral("list-%1").arg(i)));
    }
    const QVariantList six = pinned_as_stored(pins_json(seven));
    expect("pinned: seven read as the first six", six.size() == AppPrefs::kPinnedCap && six == seven.mid(0, 6));
}

/* The setter writes compact JSON the scalar-only backup carries; a reload, a backup's readPortable() and a restore's
 * replacePortable() give the same pins back. Writing none removes the key. */
void
test_pinned_pref_round_trip() {
    clear_prefs_file();
    const QVariantList pins{pin_of("system", new_uid()), pin_of("scan", QStringLiteral("list-1")), pin_of("explore")};
    {
        AppPrefs prefs;
        int notified = 0;
        QObject::connect(&prefs, &AppPrefs::pinnedChanged, &prefs, [&notified]() { notified++; });
        prefs.setPinned(pins);
        prefs.setPinned(pins);
        expect("pinned: a write notifies once, a repeat not at all", notified == 1);
        prefs.setProperty("lastStartedKind", QStringLiteral("saved"));
    }
    {
        QSettings f = prefs_file();
        f.sync();
        expect("pinned: stored as one compact JSON string",
               f.value(QStringLiteral("listen/pinned")).metaType().id() == QMetaType::QString
                   && f.value(QStringLiteral("listen/pinned")).toString() == pins_json(pins));
    }
    const QVariantMap portable = AppPrefs::readPortable();
    expect("pinned: a backup reads the text",
           portable.value(QStringLiteral("listen/pinned")).toString() == pins_json(pins));
    clear_prefs_file();
    expect("pinned: a restore writes it back", AppPrefs::replacePortable(portable));
    {
        AppPrefs prefs;
        expect("pinned: a restore reads the same pins", prefs.pinned() == pins);
        prefs.setPinned({});
    }
    expect("pinned: none removes the key", !prefs_file().contains(QStringLiteral("listen/pinned")));
}

/* A pins fixture: the live prefs and both models emptied, then a ListenPins over them. */
struct PinsWorld {
    AppPrefs prefs;
    SavedSystemsModel systems;
    ScanListsModel lists;

    PinsWorld() {
        clear_prefs_file();
        empty_systems(systems);
        while (lists.count() && lists.remove(0)) {}
        expect("pins: the lists empty", lists.count() == 0);
    }

    QString
    system(const QString& name, int sid = 0, int siteId = 0, const QString& siteName = QString(),
           const QString& groupCsv = QString()) {
        expect("pins: add system", systems.add({{"name", name},
                                                {"freqMhz", QStringLiteral("851.%1").arg(systems.count())},
                                                {"rrSid", sid},
                                                {"rrSiteId", siteId},
                                                {"siteName", siteName},
                                                {"groupCsvPath", groupCsv}}));
        return systems.get(systems.count() - 1).value("uid").toString();
    }

    QString
    list(const QString& name) {
        expect("pins: add list", lists.add({{"name", name}}));
        return lists.get(lists.count() - 1).value("uid").toString();
    }
};

QVariantMap
entry_for(const ListenPins& pins, const QString& key) {
    for (const QVariant& entry : pins.entries()) {
        if (entry.toMap().value("key").toString() == key) {
            return entry.toMap();
        }
    }
    return {};
}

/* Notes whether @p pins changes its entries while @p saved() says the departing row is still in its model, or names
 * @p gone: a removal's prune must land once the row is gone, outside the model's removal. */
struct EntriesWatch {
    bool early = false;
    QMetaObject::Connection connection;

    EntriesWatch(ListenPins& pins, const std::function<bool()>& saved, const QString& gone) {
        connection = QObject::connect(&pins, &ListenPins::entriesChanged, &pins, [this, &pins, saved, gone]() {
            for (const QVariant& entry : pins.entries()) {
                early = early || entry.toMap().value("label").toString() == gone;
            }
            early = early || saved();
        });
    }

    ~EntriesWatch() { QObject::disconnect(connection); }

    EntriesWatch(const EntriesWatch&) = delete;
    EntriesWatch& operator=(const EntriesWatch&) = delete;
};

QStringList
pin_keys(const ListenPins& pins) {
    QStringList keys;
    for (const QVariant& entry : pins.entries()) {
        keys.append(entry.toMap().value("key").toString());
    }
    return keys;
}

/* A system pin stands for its card: a group's card is pinned through any of its sites, and a site in no group is its
 * own card, so a site pin of it pins that card too. Pinning is idempotent and never touches what Listen last
 * started. */
void
test_pin_unpin_is_pinned() {
    PinsWorld w;
    const QString solo = w.system("Solo");
    const QString lead = w.system("County", 7, 1, "North");
    const QString second = w.system("County", 7, 2, "South");
    const QString third = w.system("County", 7, 3, "East");
    const QString list = w.list("Patrol");
    w.prefs.setLastStartedKind(QStringLiteral("scan"));
    w.prefs.setLastStartedUid(QStringLiteral("last-uid"));
    ListenPins pins(&w.prefs, &w.systems, &w.lists);

    expect("pins: a system pins", pins.pin("system", solo).isEmpty() && pins.isPinned("system", solo));
    expect("pins: a system pin is no site pin", !pins.isPinned("site", solo));
    expect("pins: pinning again changes nothing", pins.pin("system", solo).isEmpty() && pins.count() == 1);

    expect("pins: a group pins through one of its sites", pins.pin("system", second).isEmpty());
    expect("pins: the group's card is pinned through its lead",
           pins.isPinned("system", lead) && pins.isPinned("system", third));
    expect("pins: pinning the card through another site is a repeat",
           pins.pin("system", lead).isEmpty() && pins.count() == 2);
    expect("pins: a group's system pin is no site pin", !pins.isPinned("site", second));
    pins.unpin("system", lead);
    expect("pins: unpinning the card through its lead removes the pin",
           !pins.isPinned("system", second) && pins.count() == 1);

    expect("pins: one site of a group pins", pins.pin("site", third).isEmpty() && pins.isPinned("site", third));
    expect("pins: a grouped site's pin does not pin its card",
           !pins.isPinned("system", lead) && !pins.isPinned("system", third));

    pins.unpin("system", solo);
    expect("pins: a standalone site pins", pins.pin("site", solo).isEmpty());
    expect("pins: a standalone site's pin pins its card", pins.isPinned("system", solo));
    pins.unpin("system", solo);
    expect("pins: unpinning the card removes its site pin", !pins.isPinned("site", solo) && pins.count() == 1);

    expect("pins: a scan list pins", pins.pin("scan", list).isEmpty() && pins.isPinned("scan", list));
    expect("pins: Signal search pins without a UID",
           pins.pin("search", QStringLiteral("any")).isEmpty() && pins.isPinned("search", QString()));
    expect("pins: Explore pins", pins.pin("explore", QString()).isEmpty() && pins.isPinned("explore", QString()));
    expect("pins: entries in pinned order, keyed kind:uid",
           pin_keys(pins)
               == QStringList{"site:" + third, "scan:" + list, QStringLiteral("search:"), QStringLiteral("explore:")});
    pins.unpinKey(QStringLiteral("search:"));
    pins.unpin("scan", list);
    expect("pins: unpinKey and unpin remove one each",
           pin_keys(pins) == QStringList{"site:" + third, QStringLiteral("explore:")});

    expect("pins: an unsaved system is not found",
           pins.pin("system", new_uid()) == ListenPins::text(ListenPins::Text::NotFoundRefusal));
    expect("pins: an unsaved list is not found",
           pins.pin("scan", QStringLiteral("gone")) == ListenPins::text(ListenPins::Text::NotFoundRefusal));
    expect("pins: an unknown kind is not found",
           pins.pin("tool", QString()) == ListenPins::text(ListenPins::Text::NotFoundRefusal));
    expect("pins: a refusal stores nothing", pins.count() == 2);
    expect("pins: pinning leaves what Listen last started",
           w.prefs.lastStartedKind() == "scan" && w.prefs.lastStartedUid() == "last-uid");
    AppPrefs reread;
    expect("pins: pins persist", reread.pinned().size() == 2);
}

/* Six is the cap, and pins whose targets are missing count toward it: they are never dropped to make room. */
void
test_pin_full_refuses() {
    PinsWorld w;
    QStringList uids;
    for (int i = 0; i < 4; ++i) {
        uids.append(w.system(QStringLiteral("System %1").arg(i)));
    }
    ListenPins pins(&w.prefs, &w.systems, &w.lists);
    int fullChanges = 0;
    QObject::connect(&pins, &ListenPins::fullChanged, &pins, [&fullChanges]() { fullChanges++; });
    for (const QString& uid : uids) {
        expect("full: pin a system", pins.pin("system", uid).isEmpty());
    }
    expect("full: pin search", pins.pin("search", QString()).isEmpty());
    expect("full: not full at five", !pins.full() && fullChanges == 0);
    expect("full: pin explore", pins.pin("explore", QString()).isEmpty());
    expect("full: full at six", pins.full() && pins.count() == ListenPins::kCap && fullChanges == 1);
    const QString extra = w.list("Patrol");
    expect("full: a seventh is refused",
           pins.pin("scan", extra) == ListenPins::text(ListenPins::Text::FullRefusal) && pins.count() == 6);
    expect("full: the refusal names the cap",
           ListenPins::text(ListenPins::Text::FullRefusal) == QStringLiteral("Pinned is full (6). Unpin one first."));
    expect("full: the views read the same cap", pins.property("cap").toInt() == 6 && ListenPins::cap() == 6);
    expect("full: pinning what is pinned still succeeds", pins.pin("system", uids.first()).isEmpty());

    QVariantList missing;
    for (int i = 0; i < 5; ++i) {
        missing.append(pin_of("system", new_uid()));
    }
    missing.append(pin_of("search"));
    w.prefs.setPinned(missing);
    expect("full: missing targets count", pins.full() && pins.count() == 6);
    expect("full: missing targets are not made room from",
           pins.pin("scan", extra) == ListenPins::text(ListenPins::Text::FullRefusal) && pins.count() == 6);
    pins.unpinKey(QStringLiteral("system:") + missing.first().toMap().value("uid").toString());
    expect("full: a missing pin unpins by key", !pins.full() && pins.count() == 5 && fullChanges == 2);
    expect("full: then the list pins", pins.pin("scan", extra).isEmpty() && pins.full());
}

/* Every source offers the same Pin to Listen: named for what it pins, Unpin once pinned (a group's card through any of
 * its sites), and disabled with the full reason while six are stored, except as Unpin. */
void
test_pin_action() {
    PinsWorld w;
    const QString lead = w.system("County", 7, 1, "North");
    const QString second = w.system("County", 7, 2, "South");
    const QString list = w.list("Patrol");
    ListenPins pins(&w.prefs, &w.systems, &w.lists);
    using T = ListenPins::Text;
    const auto offers = [&pins](const QString& kind, const QString& uid, const QString& text, const QString& name,
                                bool pinned) {
        const QVariantMap action = pins.action(kind, uid);
        return action.value("text").toString() == text && action.value("accessibleName").toString() == name
               && action.value("pinned").toBool() == pinned && action.value("enabled").toBool()
               && action.value("description").toString().isEmpty();
    };

    expect("action: the copy",
           ListenPins::actionText(false) == QStringLiteral("Pin to Listen")
               && ListenPins::actionText(true) == QStringLiteral("Unpin from Listen")
               && ListenPins::actionName(false, "Patrol") == QStringLiteral("Pin Patrol to Listen")
               && ListenPins::actionName(true, "Patrol") == QStringLiteral("Unpin Patrol from Listen"));
    expect("action: a system card names its system",
           offers("system", lead, ListenPins::actionText(false), ListenPins::actionName(false, "County"), false));
    expect("action: a site names its system and itself",
           offers("site", second, ListenPins::actionText(false),
                  ListenPins::actionName(false, ListenPins::text(T::SiteLabel, "County", "South")), false));
    expect("action: Signal search and Explore",
           offers("search", QString(), ListenPins::actionText(false), "Pin Signal search to Listen", false)
               && offers("explore", QString(), ListenPins::actionText(false), "Pin Explore to Listen", false));

    expect("action: pin the card through a site", pins.pin("system", second).isEmpty() && pins.count() == 1);
    expect("action: the card is Unpin through its lead",
           offers("system", lead, ListenPins::actionText(true), ListenPins::actionName(true, "County"), true));
    expect(
        "action: the card's sites are not pinned by it",
        offers("site", second, ListenPins::actionText(false), ListenPins::actionName(false, "County · South"), false));
    pins.unpin("system", lead);
    expect(
        "action: the card unpinned through its lead is Pin again",
        pins.count() == 0
            && offers("system", second, ListenPins::actionText(false), ListenPins::actionName(false, "County"), false));

    QVariantList six;
    for (int i = 0; i < 5; ++i) {
        six.append(pin_of("system", new_uid()));
    }
    six.append(pin_of("scan", list));
    w.prefs.setPinned(six);
    expect("full: six stored", pins.full());
    const QVariantMap refused = pins.action("explore", QString());
    expect("action: full, Pin is disabled with the reason",
           refused.value("text").toString() == ListenPins::actionText(false) && !refused.value("enabled").toBool()
               && refused.value("description").toString() == ListenPins::text(T::FullRefusal));
    expect("action: full, Unpin stays offered",
           offers("scan", list, ListenPins::actionText(true), "Unpin Patrol from Listen", true));
    pins.unpin("scan", list);
    expect("action: an unpin makes room", pins.count() == 5);
    expect("action: room again, Pin is offered",
           offers("explore", QString(), ListenPins::actionText(false), "Pin Explore to Listen", false));
}

/* A store that cannot be read loads as empty: pins stay stored, marked not found, through pinning something else, and
 * resolve again once the store loads. */
void
test_pins_survive_unreadable_store() {
    QString system;
    QString list;
    {
        PinsWorld w;
        system = w.system("Metro");
        list = w.list("Patrol");
        ListenPins pins(&w.prefs, &w.systems, &w.lists);
        expect("unreadable: pin system", pins.pin("system", system).isEmpty());
        expect("unreadable: pin list", pins.pin("scan", list).isEmpty());
    }
    const QStringList stores{QStringLiteral("saved_systems.json"), QStringLiteral("scan_lists.json")};
    for (const QString& store : stores) {
        const QString path = json_store_path(store);
        expect("unreadable: store set aside", QFile::copy(path, path + ".kept"));
        QFile broken(path);
        expect("unreadable: store broken", broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
        broken.write("{not json");
    }
    {
        AppPrefs prefs;
        SavedSystemsModel systems;
        ScanListsModel lists;
        ListenPins pins(&prefs, &systems, &lists);
        expect("unreadable: the stores load empty", systems.count() == 0 && lists.count() == 0);
        expect("unreadable: both pins kept", pins.count() == 2);
        expect("unreadable: neither is found", !entry_for(pins, "system:" + system).value("found").toBool()
                                                   && !entry_for(pins, "scan:" + list).value("found").toBool());
        expect("unreadable: Signal search still pins", pins.pin("search", QString()).isEmpty());
        expect("unreadable: and the others stay",
               pin_keys(pins) == QStringList{"system:" + system, "scan:" + list, QStringLiteral("search:")});
    }
    for (const QString& store : stores) {
        const QString path = json_store_path(store);
        expect("unreadable: store restored", QFile::remove(path) && QFile::rename(path + ".kept", path));
    }
    AppPrefs prefs;
    SavedSystemsModel systems;
    ScanListsModel lists;
    ListenPins pins(&prefs, &systems, &lists);
    expect("unreadable: found again once the stores load",
           entry_for(pins, "system:" + system).value("found").toBool()
               && entry_for(pins, "scan:" + list).value("found").toBool() && pins.count() == 3);
}

/* A removal the model saved drops the pins of what it removed, and nothing else. */
void
test_prune_on_remove() {
    PinsWorld w;
    const QString solo = w.system("Solo");
    const QString lead = w.system("County", 7, 1, "North");
    const QString site = w.system("County", 7, 2, "South");
    const QString list = w.list("Patrol");
    const QString other = w.list("Harbor");
    ListenPins pins(&w.prefs, &w.systems, &w.lists);
    for (const auto& pin : {std::make_pair("system", solo), std::make_pair("site", site), std::make_pair("scan", list),
                            std::make_pair("scan", other), std::make_pair("search", QString())}) {
        expect("prune: pin", pins.pin(pin.first, pin.second).isEmpty());
    }
    {
        EntriesWatch watch(pins, [&w, &solo]() { return w.systems.rowForUid(solo) >= 0; }, QStringLiteral("Solo"));
        expect("prune: remove the system", w.systems.remove(w.systems.rowForUid(solo)));
        expect("prune: the pins change only once the system is gone", !watch.early);
    }
    expect("prune: its pin goes", !pins.isPinned("system", solo) && pins.count() == 4);
    expect("prune: remove the site", w.systems.remove(w.systems.rowForUid(site)));
    expect("prune: its pin goes", !pins.isPinned("site", site) && pins.count() == 3);
    {
        EntriesWatch watch(pins, [&w, &list]() { return w.lists.rowForUid(list) >= 0; }, QStringLiteral("Patrol"));
        expect("prune: remove a list", w.lists.remove(w.lists.rowForUid(list)));
        expect("prune: the pins change only once the list is gone", !watch.early);
    }
    expect("prune: its pin goes, and only its",
           pin_keys(pins) == QStringList{"scan:" + other, QStringLiteral("search:")});
    AppPrefs reread;
    expect("prune: the prune is stored", reread.pinned().size() == 2);
    expect("prune: the rest of the group stays", w.systems.rowForUid(lead) >= 0);
}

/* A grouped system's pin names one of its sites; when that site goes the pin moves to one that stays, so the card is
 * still pinned, and goes with the group's last site. */
void
test_system_pin_rehomes_to_sibling() {
    PinsWorld w;
    // Named apart from its group, so a label taken from the departing lead would show.
    const QString lead = w.system("Old County", 7, 1, "North");
    w.system("Elsewhere");
    const QString second = w.system("County", 7, 2, "South");
    const QString third = w.system("County", 7, 3, "East");
    ListenPins pins(&w.prefs, &w.systems, &w.lists);
    expect("rehome: pin the card", pins.pin("system", lead).isEmpty());
    {
        EntriesWatch watch(
            pins, [&w, &lead]() { return w.systems.rowForUid(lead) >= 0; }, QStringLiteral("Old County"));
        expect("rehome: remove the pinned site", w.systems.remove(w.systems.rowForUid(lead)));
        expect("rehome: the pin moves only once the site is gone, never named after it", !watch.early);
    }
    expect("rehome: the pin moves to a remaining site",
           pin_keys(pins) == QStringList{"system:" + second} && pins.isPinned("system", third));
    expect("rehome: named by the new lead", entry_for(pins, "system:" + second).value("label") == "County");
    expect("rehome: remove the next", w.systems.remove(w.systems.rowForUid(second)));
    expect("rehome: the pin moves again", pin_keys(pins) == QStringList{"system:" + third});
    expect("rehome: remove the last", w.systems.remove(w.systems.rowForUid(third)));
    expect("rehome: the pin goes with the group", pins.count() == 0);
}

/* A system pin is the system, not the row it names: when that row is edited out of its group, or a library file it
 * names is removed, the pin moves to the group's lead while the group has sites, and stays with the row once it is the
 * group's last. */
void
test_system_pin_follows_group_when_row_leaves() {
    const QString path = QStringLiteral("/data/imports/county.csv");
    PinsWorld w;
    const QString lead = w.system("County", 7, 1, "North");
    const QString second = w.system("County", 7, 2, "South", path);
    const QString third = w.system("County", 7, 3, "East");
    const QString lone = w.system("Lone", 9, 1, "Only");
    ListenPins pins(&w.prefs, &w.systems, &w.lists);
    expect("follow: pin the card through its lead", pins.pin("system", lead).isEmpty());
    expect("follow: pin a one-site group", pins.pin("system", lone).isEmpty());

    expect("follow: edit the lead's frequency", w.systems.update(w.systems.rowForUid(lead), {{"freqMhz", "852.5"}}));
    expect("follow: the lead left its group", w.systems.siblingRows(w.systems.rowForUid(lead)).isEmpty());
    expect("follow: the pin names a remaining site",
           pin_keys(pins) == QStringList{"system:" + second, "system:" + lone});
    expect("follow: the group's card is pinned", pins.isPinned("system", second) && pins.isPinned("system", third));
    expect("follow: named as the system", entry_for(pins, "system:" + second).value("label") == "County");
    expect("follow: the edited row's own card is not pinned", !pins.isPinned("system", lead));
    {
        const AppPrefs reread;
        expect("follow: the move is stored", reread.pinned().first().toMap().value("uid").toString() == second);
    }

    expect("follow: the library file goes", w.systems.clearCsvPath(path));
    expect("follow: a removed file moves it again",
           pin_keys(pins) == QStringList{"system:" + third, "system:" + lone} && !pins.isPinned("system", second));

    expect("follow: edit the one-site group's frequency",
           w.systems.update(w.systems.rowForUid(lone), {{"freqMhz", "853.5"}}));
    expect("follow: a group's last site keeps its pin",
           entry_for(pins, "system:" + lone).value("label") == "Lone" && pins.isPinned("system", lone));
}

/* A site edited out of its group keeps its pin, and its new standalone card shows it pinned: the revision moves so a
 * binding that asks again sees it, though no entry changed. */
void
test_site_pin_after_leaving_group() {
    PinsWorld w;
    w.system("County", 7, 1, "North");
    const QString site = w.system("County", 7, 2, "South");
    const QString loose = w.system("Loose");
    ListenPins pins(&w.prefs, &w.systems, &w.lists);
    expect("leave: pin the site", pins.pin("site", site).isEmpty());
    expect("leave: the grouped site's card is the group's", !pins.isPinned("system", site));
    const QVariantList entries = pins.entries();
    int revision = pins.revision();
    expect("leave: a rename of an unpinned row", w.systems.update(w.systems.rowForUid(loose), {{"name", "Renamed"}}));
    expect("leave: moves the revision, not the entries", pins.revision() > revision && pins.entries() == entries);
    revision = pins.revision();
    expect("leave: edit its frequency", w.systems.update(w.systems.rowForUid(site), {{"freqMhz", "852.5"}}));
    expect("leave: the site left its group", w.systems.siblingRows(w.systems.rowForUid(site)).isEmpty());
    expect("leave: the revision moved", pins.revision() > revision);
    expect("leave: the pin is kept", pins.isPinned("site", site) && pins.count() == 1);
    expect("leave: its standalone card shows it pinned", pins.isPinned("system", site));
    pins.unpin("system", site);
    expect("leave: the card unpins it", pins.count() == 0);
}

/* Names follow what is saved: a rename, a site's name, a list's rename, and how a grouped system starts. */
void
test_labels_follow_rename_and_site() {
    PinsWorld w;
    const QString lead = w.system("County", 7, 1, "North");
    const QString second = w.system("County", 7, 2, "South");
    w.system("County", 7, 3, QString());
    const QString solo = w.system("Solo");
    const QString list = w.list("Patrol");
    ListenPins pins(&w.prefs, &w.systems, &w.lists);
    for (const auto& pin :
         {std::make_pair("system", second), std::make_pair("site", second), std::make_pair("system", solo),
          std::make_pair("scan", list), std::make_pair("search", QString())}) {
        expect("labels: pin", pins.pin(pin.first, pin.second).isEmpty());
    }
    const auto label = [&pins](const QString& key) { return entry_for(pins, key).value("label").toString(); };
    const auto name = [&pins](const QString& key) { return entry_for(pins, key).value("accessibleName").toString(); };
    using T = ListenPins::Text;
    expect("labels: a system by its lead", label("system:" + second) == "County");
    expect("labels: a group with several sites chooses",
           name("system:" + second) == ListenPins::text(T::ChooseSiteName, "County")
               && name("system:" + second) == QStringLiteral("Choose a site of County"));
    expect("labels: a site by system and site",
           label("site:" + second) == QStringLiteral("County · South")
               && name("site:" + second) == ListenPins::text(T::StartName, "County · South"));
    expect("labels: a standalone system starts",
           label("system:" + solo) == "Solo" && name("system:" + solo) == ListenPins::text(T::StartName, "Solo"));
    expect("labels: a scan list", label("scan:" + list) == "Patrol"
                                      && name("scan:" + list) == ListenPins::text(T::StartScanListName, "Patrol"));
    expect("labels: Signal search", label("search:") == ListenPins::text(T::SearchLabel)
                                        && name("search:") == ListenPins::text(T::OpenSearchName));
    expect("labels: every pin found", entry_for(pins, "system:" + second).value("found").toBool()
                                          && entry_for(pins, "scan:" + list).value("found").toBool());

    int changes = 0;
    QObject::connect(&pins, &ListenPins::entriesChanged, &pins, [&changes]() { changes++; });
    expect("labels: rename the lead", w.systems.update(w.systems.rowForUid(lead), {{"name", "Metro"}}));
    expect("labels: the system follows its lead", label("system:" + second) == "Metro" && changes == 1);
    expect("labels: rename the site's system",
           w.systems.update(w.systems.rowForUid(second), {{"name", "Metro"}, {"siteName", "Harbor"}}));
    expect("labels: the site follows", label("site:" + second) == QStringLiteral("Metro · Harbor"));
    expect("labels: rename the list", w.lists.update(w.lists.rowForUid(list), {{"name", "Night"}}));
    expect("labels: the list follows", label("scan:" + list) == "Night");

    expect("labels: avoid two of three sites", w.systems.setAvoidSite(0, true) && w.systems.setAvoidSite(2, true));
    expect("labels: one site to play starts it", name("system:" + second) == ListenPins::text(T::StartName, "Metro"));
    expect("labels: avoid the last", w.systems.setAvoidSite(w.systems.rowForUid(second), true));
    expect("labels: no site to play chooses", name("system:" + second) == ListenPins::text(T::ChooseSiteName, "Metro"));
    expect("labels: a site with no name of its own", w.systems.update(w.systems.rowForUid(second), {{"siteName", ""}}));
    expect("labels: is named by its system", label("site:" + second) == "Metro");

    expect("labels: remove the list", w.lists.remove(w.lists.rowForUid(list)));
    expect("labels: refreshed after a removal", entry_for(pins, "scan:" + list).isEmpty());
}

/* A removal whose save fails keeps the system, so it keeps its pin. */
void
test_failed_remove_keeps_pin() {
    PinsWorld w;
    const QString solo = w.system("Solo");
    ListenPins pins(&w.prefs, &w.systems, &w.lists);
    expect("failed remove: pin", pins.pin("system", solo).isEmpty());
    expect("failed remove: store blocked", block_systems_store());
    expect("failed remove: the removal fails", !w.systems.remove(0));
    expect("failed remove: the pin stays, found",
           pins.isPinned("system", solo) && entry_for(pins, "system:" + solo).value("found").toBool());
    expect("failed remove: store unblocked", unblock_systems_store());
    AppPrefs reread;
    expect("failed remove: the pin is still stored", reread.pinned().size() == 1);
}

/* Explore's pin says what a tap does: start, once its radio is chosen, else open its setup. */
void
test_explore_label_follows_configuration() {
    PinsWorld w;
    ListenPins pins(&w.prefs, &w.systems, &w.lists);
    expect("explore: pin", pins.pin("explore", QString()).isEmpty());
    const auto name = [&pins]() { return entry_for(pins, QStringLiteral("explore:")).value("accessibleName"); };
    expect("explore: labelled Explore", entry_for(pins, QStringLiteral("explore:")).value("label")
                                            == ListenPins::text(ListenPins::Text::ExploreLabel));
    expect("explore: unset asks for setup", name() == ListenPins::text(ListenPins::Text::SetUpExploreName));
    w.prefs.setExploreSourceType(QStringLiteral("rtltcp"));
    expect("explore: chosen starts", name() == ListenPins::text(ListenPins::Text::StartExploreName));
    w.prefs.setExploreSourceType(QString());
    expect("explore: cleared asks again", name() == ListenPins::text(ListenPins::Text::SetUpExploreName));
}
} // namespace

static void
test_radio_settings() {
    SavedSystemsModel model;
    while (model.count()) {
        model.remove(0);
    }
    expect("first system", model.add({{"name", "First"}, {"decodeFlag", "-f1"}}));
    expect(
        "radio system",
        model.add(
            {{"name", "Radio"}, {"decodeFlag", "-f1"}, {"gainDb", 36}, {"ppm", "-2"}, {"extraArgs", "--squelch -80"}}));
    const auto uid = model.get(1).value("uid").toString();
    model.remove(0);
    expect("live settings follow UID after row deletion",
           model.updateRadioSettings(uid, {{"squelch", "-65"}, {"modulation", 1}}));
    expect("decoder preset merges with the existing squelch", model.updateRadioSettings(uid, {{"decodeFlag", "-fA"}}));
    const QVariantMap expected{{"squelch", "-65"}, {"decodeFlag", "-fA"}};
    {
        SavedSystemsModel fresh;
        const auto saved = fresh.getByUid(uid);
        expect("Radio controls survive model/application reload", saved.value("radioSettings").toMap() == expected);
        expect("Listening for also updates wizard and list metadata", saved.value("decodeFlag") == "-fA");
        expect("unrelated saved controls and advanced options survive",
               saved.value("gainDb") == 36 && saved.value("ppm") == "-2"
                   && saved.value("extraArgs") == "--squelch -80");
    }
    for (const auto& invalid : QList<QVariantMap>{{{"squelch", "noise+31"}},
                                                  {{"modulation", 4}},
                                                  {{"modulation", 1.5}},
                                                  {{"decodeFlag", "-fA --show-keys"}},
                                                  {{"squelch", "-65,--show-keys"}},
                                                  {{"unknown", true}}}) {
        expect("invalid radio update rejected atomically", !model.updateRadioSettings(uid, invalid));
        expect("invalid update keeps stored controls", model.getByUid(uid).value("radioSettings").toMap() == expected);
    }
    const QString path = json_store_path(QStringLiteral("saved_systems.json"));
    expect("move store aside for failure test", QFile::rename(path, path + ".saved"));
    expect("block store with a directory", QDir().mkpath(path));
    expect("disk failure does not report success", !model.updateRadioSettings(uid, {{"squelch", "off"}}));
    expect("disk failure preserves model", model.getByUid(uid).value("radioSettings").toMap() == expected);
    expect("unblock store", QDir().rmdir(path) && QFile::rename(path + ".saved", path));
    expect("ordinary wizard edit", model.update(0, {{"name", "Renamed"}}));
    expect("ordinary edit preserves live controls", model.get(0).value("radioSettings").toMap() == expected);
    expect("wizard decoder edit", model.update(0, {{"decodeFlag", "-fs"}}));
    expect("wizard preset replaces live decoder and modulation, preserves squelch",
           model.get(0).value("radioSettings").toMap() == QVariantMap{{"squelch", "-65"}});
    model.remove(0);
    expect("removed system is never recreated", !model.updateRadioSettings(uid, {{"squelch", "off"}}));
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    /* Isolated, disposable storage: test mode points QStandardPaths at the
     * qttest tree, and QSettings is redirected into a QTemporaryDir. Nothing
     * this test writes may touch (or depend on) a real profile. */
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-persistence-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        DSD_FPRINTF(stderr, "FAIL: could not create settings dir\n");
        return 1;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    test_json_store();
    test_private_key_store();
    test_saved_systems();
    test_saved_systems_past_int_range();
    test_saved_systems_csv_fields();
    test_app_prefs();
    test_receiver_prefs();
    test_screen_policy_prefs();
    test_recording_prefs();
    test_analog_max_gap_pref();
    test_analog_preroll_pref();
    test_prefs_past_int_range();
    test_units_preference();
    test_migration_write_failure();
    test_foundation_persistence();
    // WP-S2: opting out preserves the last successful target, including scan lists.
    {
        AppPrefs prefs;
        prefs.setLastStartedKind("scan");
        prefs.setLastStartedUid("stable-scan-uid");
        prefs.setAutoStartOnAttach(false);
    }
    {
        AppPrefs restored;
        expect("attach opt-out persists", !restored.autoStartOnAttach());
        expect("opt-out retains scan target",
               restored.lastStartedKind() == "scan" && restored.lastStartedUid() == "stable-scan-uid");
    }
    test_prefs_key_table();
    test_key_persistence();
    test_foundation_key_type_migration();
    test_site_provenance_edits();
    test_remove_failure_keeps_row();
    test_touch_failure_keeps_memory();
    test_clear_csv_failure_keeps_memory();
    test_set_avoid_failure_reports();
    test_avoid_cleared_when_site_leaves_group();
    test_load_migrates_orphaned_avoid();
    test_add_with_retained_key();
    test_radio_settings();
    test_pinned_pref_sanitizes();
    test_pinned_pref_round_trip();
    test_pin_unpin_is_pinned();
    test_pin_full_refuses();
    test_pin_action();
    test_pins_survive_unreadable_store();
    test_prune_on_remove();
    test_system_pin_rehomes_to_sibling();
    test_system_pin_follows_group_when_row_leaves();
    test_site_pin_after_leaving_group();
    test_labels_follow_rename_and_site();
    test_failed_remove_keeps_pin();
    test_explore_label_follows_configuration();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
