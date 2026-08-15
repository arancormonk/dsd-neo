// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the Qt frontend's persistence layer — json_store's load/save
 * behavior on missing and corrupt input, SavedSystemsModel's field-map round
 * trip and reload path, and AppPrefs defaults and persistence. Registered only
 * when the Qt frontend is enabled (DSD_ENABLE_QT_UI), since these link Qt. */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantMap>
#include <stdio.h>

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
                   && sparseGot.value(QStringLiteral("trunking")).toBool()
                   && sparseGot.value(QStringLiteral("lastHeard")).toLongLong() == 0);

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

    /* A second instance is the Activity-restart path: everything above must
     * come back from disk, including the legacy decode-flag migration. */
    SavedSystemsModel reloaded;
    expect("reload restores every row", reloaded.count() == 4);
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
    {
        SavedSystemsModel model;
        QVariantMap sys = full_system_map();
        sys.insert(QStringLiteral("chanCsvPath"), QStringLiteral("/data/imports/chan map.csv"));
        sys.insert(QStringLiteral("groupCsvPath"), groupPath);
        sys.insert(QStringLiteral("keyCsvPath"), QStringLiteral("/data/imports/keys.csv"));
        sys.insert(QStringLiteral("keyCsvHex"), true);
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
               && got.value(QStringLiteral("keyCsvHex")).toBool());
    expect("legacy row reads empty csv fields",
           model.get(1).value(QStringLiteral("chanCsvPath")).toString().isEmpty()
               && model.get(1).value(QStringLiteral("groupCsvPath")).toString().isEmpty()
               && model.get(1).value(QStringLiteral("keyCsvPath")).toString().isEmpty()
               && !model.get(1).value(QStringLiteral("keyCsvHex")).toBool());

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
    expect("clear leaves other csv fields alone", model.get(0).value(QStringLiteral("chanCsvPath")).toString()
                                                      == QStringLiteral("/data/imports/chan map.csv"));

    SavedSystemsModel reloaded;
    expect("clear persists", reloaded.get(0).value(QStringLiteral("groupCsvPath")).toString().isEmpty());
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
        expect("bias tee defaults off", !prefs.biasTee());
        expect("gain defaults to 30 dB", prefs.gainDb() == 30);
        expect("ppm defaults to 0", prefs.ppm() == 0);
        expect("bandwidth defaults to 48 kHz", prefs.bandwidthKhz() == 48);
        expect("extra args default empty", prefs.extraArgs().isEmpty());
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
    }

    AppPrefs reloaded;
    expect("gain persists across instances", reloaded.gainDb() == 42);
    expect("extra args persist across instances", reloaded.extraArgs() == QStringLiteral("--enc-lockout"));
    expect("onboarding flag persists", reloaded.onboardingDone());
    expect("explore source persists", reloaded.exploreSourceType() == QStringLiteral("rtltcp"));
    expect("explore host persists", reloaded.exploreHost() == QStringLiteral("10.0.2.2"));
    expect("explore port persists", reloaded.explorePort() == 1234);
    expect("explore frequency persists with its digits intact",
           reloaded.exploreFreqMhz() == QStringLiteral("769.76875"));
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
    QStandardPaths::setTestModeEnabled(true);
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        DSD_FPRINTF(stderr, "FAIL: could not create settings dir\n");
        return 1;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    test_json_store();
    test_saved_systems();
    test_saved_systems_csv_fields();
    test_app_prefs();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
