// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the Qt frontend's persistence layer — json_store's load/save
 * behavior on missing and corrupt input, SavedSystemsModel's field-map round
 * trip and reload path, and AppPrefs defaults and persistence. Registered only
 * when the Qt frontend is enabled (DSD_ENABLE_QT_UI), since these link Qt. */

#include <QAnyStringView>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QMetaProperty>
#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <initializer_list>
#include <stdio.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <dsd-neo/runtime/log.h>
#include "app_prefs.h"
#include "dsd-neo/core/safe_api.h"
#include "json_store.h"
#include "saved_systems_model.h"

using dsd_qt::AppPrefs;
using dsd_qt::json_store_load_array;
using dsd_qt::json_store_path;
using dsd_qt::json_store_save_array;
using dsd_qt::SavedSystemsModel;

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
        expect("background listening defaults on", prefs.backgroundListening());
        expect("skip-encrypted defaults on", prefs.skipEncrypted());
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

void
test_migration_write_failure() {
    const QString store = QStringLiteral("saved_systems.json");
    expect("legacy fixture saved", json_store_save_array(store, QJsonArray{QJsonObject{{"name", "legacy"}}}));
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto permissions = QFile::permissions(directory);
    expect("migration directory made read only", QFile::setPermissions(directory, QFile::ReadOwner | QFile::ExeOwner));
    int warnings = 0;
    dsd_neo_log_set_tap(
        [](dsd_neo_log_level_t level, const char* text, void* context) {
            if (level == LOG_LEVEL_WARN && QString::fromUtf8(text).contains("migration could not be persisted")) {
                ++*static_cast<int*>(context);
            }
        },
        &warnings);
    SavedSystemsModel model;
    dsd_neo_log_set_tap(nullptr, nullptr);
    expect("migration warns once when write fails", warnings == 1);
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

} // namespace

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
    test_saved_systems_csv_fields();
    test_app_prefs();
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
    test_key_persistence();
    test_foundation_key_type_migration();
    test_site_provenance_edits();
    test_add_with_retained_key();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
