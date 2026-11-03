// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QByteArrayView>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>
#include <cstdio>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/runtime/path_policy.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <qsystemdetection.h>
#include <qtenvironmentvariables.h>
#include <stdint.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "decoder_host.h"
#include "json_store.h"
#include "recording_sessions.h"
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

namespace {

struct InspectedTargets {
    QStringList types;
    QList<int> widths;
    QList<int> squelchDb;
    QList<int> toneFilters;
    QStringList toneLists;
};

} // namespace

static void
collectTarget(const dsd_app_scan_csv_target* target, void* context) {
    auto* seen = static_cast<InspectedTargets*>(context);
    seen->types << QString::fromUtf8(target->type);
    seen->widths << target->bandwidth_hz;
    seen->squelchDb << (target->squelch_db_set ? target->squelch_db : 1);
    seen->toneFilters << target->tone_filter;
    seen->toneLists << QString::fromUtf8(target->tone_list);
}

/* The targets the engine's own parser reads from @p path, as the target preview reads them. */
static InspectedTargets
inspectTargets(const QString& path) {
    InspectedTargets seen;
    char error[256] = {};
    const dsd_app_scan_csv_callbacks callbacks{collectTarget, nullptr, &seen, nullptr};
    check(dsd_app_scan_csv_inspect(path.toUtf8().constData(), nullptr, 0, &callbacks, error, sizeof error) == 0);
    return seen;
}

/* Issue #526: a manual list mixing a digital, an nfm and an am frequency entry with a saved AM system round-trips: the
 * entries keep their protocols through the saved lists, and the list builds into a target CSV the engine's own parser
 * accepts with the canonical analog types, and no tone policy of their own (the editor sets none, so the configured one
 * applies). A CSV-backed list mixing analog and digital targets reaches --trunk-scan as the file itself, its options --
 * each width spelled for its kind, the squelch, an nfm target's tone policy (issue #527) -- intact. */
static void
checkMixedAnalogRoundTrip(const QTemporaryDir& dir) {
    SavedSystemsModel systems;
    check(systems.add({{"name", "Tower"}, {"decodeFlag", "-fM"}, {"freqMhz", "118.3"}}));
    const QString towerUid = systems.get(systems.count() - 1).value("uid").toString();
    QVariantList entries;
    const QStringList protocols{"p25", "nfm", "am"};
    const QStringList freqs{"851.5", "154.43", "121.5"};
    for (int i = 0; i < protocols.size(); ++i) {
        entries << QVariantMap{{"kind", "freq"},
                               {"name", protocols[i]},
                               {"protocol", protocols[i]},
                               {"freqMhz", freqs[i]},
                               {"enabled", true}};
    }
    entries << QVariantMap{{"kind", "system"}, {"systemUid", towerUid}, {"enabled", true}};
    ScanListsModel model;
    check(model.add({{"name", "Mixed"}, {"sourceType", "usb"}, {"entries", entries}}));
    const QString uid = model.get(model.count() - 1).value("uid").toString();
    ScanListsModel reloaded;
    const int row = reloaded.rowForUid(uid);
    check(row >= 0);
    const auto saved = reloaded.get(row).value("entries").toList();
    check(saved.size() == 4 && saved.value(1).toMap().value("protocol") == "nfm"
          && saved.value(2).toMap().value("protocol") == "am");
    ScanListStarter starter(nullptr, &systems);
    const auto built = starter.build(reloaded.get(row));
    check(built.value("ok").toBool() && built.value("targetCount").toInt() == 4);
    const QString path = dsd_qt::scan_list_csv_path(uid, reloaded.get(row).value("name").toString(), 1);
    int count = 0;
    char error[256] = {};
    check(dsd_app_trunk_scan_validate_targets_csv(path.toUtf8().constData(), &count, error, sizeof error) == 0
          && count == 4);
    const auto generated = inspectTargets(path);
    check(generated.types == QStringList{"p25-conventional", "nfm-conventional", "am-conventional", "am-conventional"});
    check(generated.toneFilters == QList<int>{-1, -1, -1, -1});
    check(model.remove(model.rowForUid(uid)));

    const QString targetPath = dir.filePath("analog-targets.csv");
    const QByteArray body = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                            "tower,am-conventional,118300000,,1500,2000,,--am-bandwidth-hz 8333 --squelch-db -55\n"
                            "fire,nfm-conventional,154430000,,,,,--nfm-bandwidth-hz 12500 --tone-allow 100.0/D023N\n"
                            "site,p25-conventional,851500000,,,,,--squelch-db -60\n";
    QFile targets(targetPath);
    check(targets.open(QIODevice::WriteOnly) && targets.write(body) == body.size());
    targets.close();
    check(model.add(
        {{"name", "Analog CSV"}, {"sourceType", "usb"}, {"targetSource", "csv"}, {"targetsCsvPath", targetPath}}));
    ScanListStarter csvStarter(nullptr, nullptr);
    csvStarter.setTargetFileLookup([&targetPath](const QString& candidate) { return candidate == targetPath; });
    const auto builtCsv = csvStarter.build(model.get(model.count() - 1));
    const auto args = builtCsv.value("args").toStringList();
    // Issue #697: the list runs from its own copy, named after the list; the imported file is left as it was.
    const QVariantMap csvList = model.get(model.count() - 1);
    check(builtCsv.value("ok").toBool() && builtCsv.value("targetCount").toInt() == 3
          && args.value(args.indexOf("--trunk-scan") + 1)
                 == scan_list_csv_path(csvList.value("uid").toString(), "Analog CSV", 1));
    check(targets.open(QIODevice::ReadOnly) && targets.readAll() == body);
    targets.close();
    /* A copy that is not at its path once written (on Windows, one a code page wrote under another name, which the
       folder's cleanup then removes) leaves the list on the imported file: the starter never names a missing file. */
    scan_list_set_copy_written_hook_for_test([](const QString& copy) { QFile::remove(copy); });
    const auto missingCopy = csvStarter.build(csvList);
    scan_list_set_copy_written_hook_for_test({});
    const auto missingArgs = missingCopy.value("args").toStringList();
    check(missingCopy.value("ok").toBool() && missingCopy.value("csvPath").toString() == targetPath
          && missingArgs.value(missingArgs.indexOf("--trunk-scan") + 1) == targetPath && QFile::exists(targetPath));
    const auto preview = inspectTargets(targetPath);
    check(preview.types == QStringList{"am-conventional", "nfm-conventional", "p25-conventional"});
    check(preview.widths == QList<int>{8333, 12500, -1} && preview.squelchDb == QList<int>{-55, 1, -60});
    check(preview.toneFilters == QList<int>{-1, DSD_TONE_FILTER_ALLOW, -1}
          && preview.toneLists == QStringList{"", "100.0 Hz/D023N", ""});
    check(model.remove(model.count() - 1));
}

/* Issue #526: the editor hides decryption for analog entries and keeps what it held, so nothing it held can stop the
 * list: an am frequency entry left on "Use a profile" with no profile selected, and a saved AM system whose own
 * profile no provider can resolve, still build (this starter has no profile provider at all). The same choices on a
 * digital entry are still checked. */
static void
checkAnalogEntriesIgnoreHiddenDecryption() {
    SavedSystemsModel systems;
    check(systems.add({{"name", "Tower"}, {"decodeFlag", "-fM"}, {"freqMhz", "118.3"}}));
    const QString towerUid = systems.get(systems.count() - 1).value("uid").toString();
    check(systems.update(systems.count() - 1, {{"decryptionProfileUid", "missing-profile"}}));
    const QVariantList entries{
        QVariantMap{{"uid", "guard"},
                    {"kind", "freq"},
                    {"protocol", "am"},
                    {"freqMhz", "121.5"},
                    {"decryptionMode", "profile"},
                    {"decryptionProfileUid", ""},
                    {"enabled", true}},
        QVariantMap{{"uid", "tower"}, {"kind", "system"}, {"systemUid", towerUid}, {"enabled", true}}};
    const QVariantMap list{{"uid", "analog-decryption"}, {"name", "Air"}, {"sourceType", "usb"}, {"entries", entries}};
    ScanListStarter starter(nullptr, &systems);
    const auto validated = starter.validate(list);
    check(validated.value("ok").toBool() && validated.value("targetCount").toInt() == 2);
    if (!validated.value("ok").toBool()) {
        std::fprintf(stderr, "analog entries with hidden decryption: %s\n",
                     qPrintable(validated.value("error").toString()));
    }
    // A digital entry on the same choice is still held to it.
    QVariantList digital = entries;
    auto p25 = digital[0].toMap();
    p25["protocol"] = "p25";
    digital[0] = p25;
    check(
        !starter
             .validate(QVariantMap{
                 {"uid", "digital-decryption"}, {"name", "P25"}, {"sourceType", "usb"}, {"entries", QVariantList{p25}}})
             .value("ok")
             .toBool());
}

/* Issue #526: the editor's own DSP-rate row diagnostic. An RTL-SDR list at a DSP bandwidth that cannot filter the AM
 * default names the am entries it would skip at every visit, as a warning beside the targets ready (the rest of the
 * list still scans); a bandwidth that fits, an nfm entry at the unset NFM default and an Airspy list, whose device sets
 * the rate, say nothing. A CSV-backed list is held the same way, each analog target at its own width. */
static void
checkAnalogRateDiagnostic(const QTemporaryDir& dir) {
    const QVariantList entries{
        QVariantMap{{"uid", "ops"}, {"kind", "freq"}, {"protocol", "nfm"}, {"freqMhz", "154.43"}, {"enabled", true}},
        QVariantMap{{"uid", "guard"},
                    {"kind", "freq"},
                    {"name", "Guard"},
                    {"protocol", "am"},
                    {"freqMhz", "121.5"},
                    {"enabled", true}}};
    QVariantMap list{
        {"uid", "analog-rate"}, {"name", "Air"}, {"sourceType", "usb"}, {"bandwidthKhz", 6}, {"entries", entries}};
    ScanListStarter starter(nullptr, nullptr);
    const auto narrow = starter.validate(list);
    const QString warning = narrow.value("warnings").toStringList().join("\n");
    check(narrow.value("ok").toBool() && narrow.value("targetCount").toInt() == 2);
    check(warning.contains("Skipped at every visit at this bandwidth: Guard: AM bandwidth 6 kHz does not fit the 6 kHz "
                           "DSP rate")
          && !warning.contains("more") && !warning.contains("NFM"));
    // A restored bandwidth past int's range is no bandwidth: its low 32 bits (6 kHz) name no rate.
    list["bandwidthKhz"] = (qlonglong(1) << 32) + 6;
    check(!starter.validate(list).value("warnings").toStringList().join("\n").contains("6 kHz DSP rate"));
    list["bandwidthKhz"] = 12;
    check(starter.validate(list).value("warnings").toStringList().isEmpty());
    // An entry without a width of its own runs the width of its kind the app's Extra arguments configure, in either
    // spelling, the last one winning as on the command line.
    AppPrefs prefs;
    prefs.setExtraArgs(QStringLiteral("--am-bandwidth-hz 20000"));
    ScanListStarter configured(&prefs, nullptr);
    const auto wideAm = configured.validate(list);
    const QString wideAmWarning = wideAm.value("warnings").toStringList().join("\n");
    check(wideAm.value("ok").toBool());
    check(wideAmWarning.contains("Guard: AM bandwidth 20 kHz does not fit the 12 kHz DSP rate")
          && !wideAmWarning.contains("more"));
    prefs.setExtraArgs(QStringLiteral("--am-bandwidth-hz=20000 --nfm-bandwidth-hz 25000"));
    const QString bothWarning = configured.validate(list).value("warnings").toStringList().join("\n");
    check(bothWarning.contains("154.43 MHz: NFM bandwidth 25 kHz does not fit the 12 kHz DSP rate")
          && bothWarning.contains("(and 1 more)"));
    if (!bothWarning.contains("(and 1 more)")) {
        std::fprintf(stderr, "configured-width rate diagnostic: '%s'\n", qPrintable(bothWarning));
    }
    prefs.setExtraArgs(QStringLiteral("--am-bandwidth-hz 20000 --am-bandwidth-hz=6000"));
    check(configured.validate(list).value("warnings").toStringList().isEmpty());
    prefs.setExtraArgs(QString());
    list["bandwidthKhz"] = 6;
    list["sourceType"] = "airspy";
    check(starter.validate(list).value("warnings").toStringList().isEmpty());

    const QString targetPath = dir.filePath("rate-targets.csv");
    QFile targets(targetPath);
    check(targets.open(QIODevice::WriteOnly));
    targets.write("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                  "fire,nfm-conventional,154430000,,,,,--nfm-bandwidth-hz 12500\n"
                  "inh,nfm-conventional,155100000,,,,,\n"
                  "tower,am-conventional,118300000,,,,,--am-bandwidth-hz 15000\n"
                  "site,p25-conventional,851500000,,,,,\n");
    targets.close();
    ScanListStarter csvStarter(nullptr, nullptr);
    csvStarter.setTargetFileLookup([&targetPath](const QString& candidate) { return candidate == targetPath; });
    QVariantMap csvList{{"uid", "analog-rate-csv"},
                        {"name", "CSV"},
                        {"sourceType", "rtltcp"},
                        {"host", "127.0.0.1"},
                        {"port", 1234},
                        {"bandwidthKhz", 12},
                        {"targetSource", "csv"},
                        {"targetsCsvPath", targetPath}};
    const auto csvNarrow = csvStarter.validate(csvList);
    const QString csvWarning = csvNarrow.value("warnings").toStringList().join("\n");
    check(csvNarrow.value("ok").toBool() && csvNarrow.value("targetCount").toInt() == 4);
    check(csvWarning.contains("Skipped at every visit at this bandwidth: Target fire: NFM bandwidth 12.5 kHz does not "
                              "fit the 12 kHz DSP rate")
          && csvWarning.contains("(and 1 more)"));
    if (!csvWarning.contains("Target fire")) {
        std::fprintf(stderr, "CSV rate diagnostic: '%s' (%s)\n", qPrintable(csvWarning),
                     qPrintable(csvNarrow.value("error").toString()));
    }
    csvList["bandwidthKhz"] = 24;
    check(csvStarter.validate(csvList).value("warnings").toStringList().isEmpty());
}

/* Issue #708: a scan list's start asks the call recording store for the list's folder before the host starts, names
 * it in the argv, and frees it when the start did not happen; a draft asks for none. */
static void
checkRecordingSessions(ScanListStarter& starter, const QVariantMap& list) {
    struct Sessions : RecordingSessions {
        QStringList calls;

        QString
        beginSession(const QString& kind, const QString& uid, const QString& name) override {
            calls.append(QStringLiteral("begin %1 %2 %3").arg(kind, uid, name));
            return QStringLiteral("/recordings/folder");
        }

        void
        endSession() override {
            calls.append(QStringLiteral("end"));
        }
    } sessions;

    struct Host : DecoderHost {
        QStringList* calls = nullptr;
        QStringList received;
        bool accept = true;

        bool
        isRunning() const override {
            return false;
        }

        QString
        statusText() const override {
            return {};
        }

        bool
        start(const QStringList& argv) override {
            received = argv;
            calls->append(QStringLiteral("start"));
            return accept;
        }

        void
        stop() override {}
    } host;

    host.calls = &sessions.calls;
    starter.setRecordingSessions(&sessions);
    const QString begin =
        QStringLiteral("begin scan %1 %2").arg(list.value("uid").toString(), list.value("name").toString());
    check(starter.start(list, &host).value("started").toBool());
    check(sessions.calls == QStringList({begin, QStringLiteral("start")}));
    const qsizetype folderAt = host.received.indexOf(QStringLiteral("-7"));
    check(folderAt >= 0 && host.received.value(folderAt + 1) == QStringLiteral("/recordings/folder")
          && host.received.count(QStringLiteral("--wav-journal")) == 1);
    sessions.calls.clear();
    host.accept = false;
    check(!starter.start(list, &host).value("started").toBool());
    check(sessions.calls == QStringList({begin, QStringLiteral("start"), QStringLiteral("end")}));
    // A draft cannot start: no folder is made for it.
    sessions.calls.clear();
    host.accept = true;
    QVariantMap draft = list;
    draft.insert(QStringLiteral("isDraft"), true);
    const auto refused = starter.start(draft, &host);
    check(!refused.value("ok").toBool() && !refused.value("started").toBool() && sessions.calls.isEmpty());
    starter.setRecordingSessions(nullptr);
}

// Issue #697: a manual list's CSV is named after the list, so the engine names the list (toasts, logs, the API) as
// the app does, never by its UUID: unsafe characters made "_", blanks and trailing dots dropped, device names kept
// apart (with or without an extension), no '$' or leading '~' for the engine's path expansion to read, cut to 31 UTF-8
// bytes on a character, and "list N" for nothing left.
static void
checkCsvStem() {
    const struct {
        const char* name;
        const char* stem;
    } cases[] = {
        {"Security", "Security"},
        {"Fire v2.1", "Fire v2.1"},
        {"a/b\\c<d>e:f\"g|h?i*j", "a_b_c_d_e_f_g_h_i_j"},
        {"tab\there\x01", "tab_here_"},
        {"  padded  ", "padded"},
        {"dots...", "dots"},
        {"..", "list 3"},
        {"", "list 3"},
        {"   ", "list 3"},
        {"CON", "CON_"},
        {"lpt1", "lpt1_"},
        {"Console", "Console"},
        /* A device name is one whatever follows its first dot, blanks before the dot not counting. */
        {"CON.local", "CON_.local"},
        {"LPT1.backup", "LPT1_.backup"},
        {"aux .txt", "aux_ .txt"},
        {"Console.local", "Console.local"},
        /* COM and LPT with a superscript 1, 2 or 3 are device names too; a superscript 4 is not. */
        {"COM\xc2\xb9", "COM\xc2\xb9_"},
        {"com\xc2\xb2.log", "com\xc2\xb2_.log"},
        {"LPT\xc2\xb3", "LPT\xc2\xb3_"},
        {"lpt\xc2\xb9 .txt", "lpt\xc2\xb9_ .txt"},
        {"COM\xe2\x81\xb4", "COM\xe2\x81\xb4"},
        /* Nothing the engine's path expansion reads: no '$', and no '~' to start. */
        {"Fire $5", "Fire _5"},
        {"$HOME", "_HOME"},
        {"${X}", "_{X}"},
        {"~home", "_home"},
        {"  ~home", "_home"},
        {"a~b", "a~b"},
        {"0123456789012345678901234567890123456789", "0123456789012345678901234567890"},
        /* 15 two-byte characters fit in 30 bytes; the 16th would make 32. */
        {"\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3"
         "\xa9"
         "\xc3\xa9\xc3\xa9",
         "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3"
         "\xa9"
         "\xc3\xa9"},
        /* A four-byte character is never split: 7 of them are 28 bytes, the 8th would be 32. */
        {"\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93"
         "\xbb"
         "\xf0\x9f\x93\xbb",
         "\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb\xf0\x9f\x93"
         "\xbb"},
    };

    for (const auto& c : cases) {
        const QString got = scan_list_csv_stem(QString::fromUtf8(c.name), 3);
        if (got != QString::fromUtf8(c.stem)) {
            std::fprintf(stderr, "stem of '%s': got '%s' want '%s'\n", c.name, got.toUtf8().constData(), c.stem);
        }
        check(got == QString::fromUtf8(c.stem));
        check(got.toUtf8().size() <= 31);
    }
    check(scan_list_csv_path("u-1", "Security", 1).endsWith("/scan_lists/u-1/Security.csv"));
}

// A list named with characters the engine's path expansion reads ("Fire $5") runs: its CSV, as saved, is the file the
// engine's opener reads at that path, not "Fire " and an empty variable.
static void
checkCsvPathReadsAsSaved() {
    QVariantMap entry{
        {"kind", "freq"}, {"name", "Simplex"}, {"protocol", "p25"}, {"freqMhz", "851.5"}, {"enabled", true}};
    ScanListsModel model;
    check(model.add({{"name", "Fire $5"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}}));
    const int row = model.count() - 1;
    ScanListStarter starter(nullptr, nullptr);
    const auto built = starter.build(model.get(row));
    const QString path = built.value("csvPath").toString();
    check(built.value("ok").toBool() && QFile::exists(path));
    char opened[2048] = {};
    const bool reads = dsd_path_resolve_user_read_file(path.toUtf8().constData(), opened, sizeof opened) == 0;
    if (!reads || QString::fromUtf8(opened) != path) {
        std::fprintf(stderr, "'%s' opens as '%s'\n", path.toUtf8().constData(), reads ? opened : "(nothing)");
    }
    check(reads && QString::fromUtf8(opened) == path);
    check(model.remove(row));
}

// The list's folder holds its one CSV: a rename leaves no file under the old name, the file older versions wrote
// beside the folder goes at the next build, and removing the list removes the folder.
static void
checkCsvFolder() {
    QVariantMap entry{
        {"kind", "freq"}, {"name", "Simplex"}, {"protocol", "p25"}, {"freqMhz", "851.5"}, {"enabled", true}};
    ScanListsModel model;
    check(model.add({{"name", "Fire / EMS"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}}));
    const int row = model.count() - 1;
    const QString uid = model.get(row).value("uid").toString();
    const QString legacy = json_store_path("scan_lists/" + uid + ".csv");
    QDir().mkpath(QFileInfo(legacy).absolutePath());
    QFile old(legacy);
    check(old.open(QIODevice::WriteOnly) && old.write("id,type\n") > 0);
    old.close();
    ScanListStarter starter(nullptr, nullptr);
    const auto first = starter.build(model.get(row));
    check(first.value("ok").toBool() && first.value("csvPath").toString().endsWith("/" + uid + "/Fire _ EMS.csv"));
    const auto args = first.value("args").toStringList();
    check(args.value(args.indexOf("--trunk-scan") + 1) == first.value("csvPath").toString());
    check(QFile::exists(first.value("csvPath").toString()) && !QFile::exists(legacy));
    check(model.update(row, {{"name", "Fire"}}));
    const auto renamed = starter.build(model.get(row));
    check(renamed.value("ok").toBool() && renamed.value("csvPath").toString().endsWith("/" + uid + "/Fire.csv"));
    const QDir folder(json_store_path("scan_lists/" + uid));
    check(folder.entryList({"*.csv"}, QDir::Files) == QStringList{"Fire.csv"});
    check(model.remove(row));
    check(!folder.exists() && !QFile::exists(legacy));
}

namespace {
// A host that keeps the argv of its last start.
struct ArgvHost : DecoderHost {
    QStringList received;

    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return {};
    }

    bool
    start(const QStringList& argv) override {
        received = argv;
        return true;
    }

    void
    stop() override {}
};

dsd_trunk_tune_result
acceptTune(dsd_opts*, dsd_state*, long int, int, uint64_t) {
    return DSD_TRUNK_TUNE_RESULT_OK;
}

// An imported targets file as the import stores it: imports/bundles/<id>/targets.csv, its talkgroup file beside it
// and named relative to it.
QString
writeImportedBundle(const QString& root, const QString& id, uint32_t hz, const QByteArray& groupName) {
    const QString folder = root + "/" + id;
    QDir().mkpath(folder + "/rev");
    QFile groups(folder + "/rev/0.csv");
    check(groups.open(QIODevice::WriteOnly) && groups.write("id,mode,name\n123,B," + groupName + "\n") > 0);
    groups.close();
    QFile targets(folder + "/targets.csv");
    const QByteArray body = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n" + id.toUtf8()
                            + ",p25-conventional," + QByteArray::number(hz) + ",,,,,-G rev/0.csv\n";
    check(targets.open(QIODevice::WriteOnly) && targets.write(body) == body.size());
    targets.close();
    return targets.fileName();
}
} // namespace

// Issue #697: every imported list is stored as targets.csv, so the decoder named each one "targets". Each now runs
// from its own copy named after the list, its companion files still found: two imported lists started together are
// named apart in the argv and by the real coordinator, each target with its own talkgroups.
static void
checkImportedListsNamedApart(const QTemporaryDir& dir) {
    const QString root = dir.filePath("imports/bundles");
    const QString hospitalFile = writeImportedBundle(root, "hospital-target", 852012500U, "Hospital TG");
    const QString parksFile = writeImportedBundle(root, "parks-target", 853012500U, "Parks TG");
    const QByteArray hospitalBefore = [&] {
        QFile f(hospitalFile);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    ScanListsModel model;
    const QVariantMap common{{"sourceType", "rtltcp"}, {"host", "127.0.0.1"}, {"port", 1234}, {"targetSource", "csv"}};
    QVariantMap hospital = common;
    hospital.insert("name", "Hospital");
    hospital.insert("targetsCsvPath", hospitalFile);
    QVariantMap parks = common;
    parks.insert("name", "Parks");
    parks.insert("targetsCsvPath", parksFile);
    check(model.add(hospital) && model.add(parks));
    hospital = model.get(model.count() - 2);
    parks = model.get(model.count() - 1);
    ScanListStarter starter(nullptr, nullptr);
    starter.setTargetFileLookup([&](const QString& path) { return path == hospitalFile || path == parksFile; });
    ArgvHost host;
    const auto started = starter.startLists({hospital, parks}, &host);
    check(started.value("started").toBool());
    QStringList paths;
    for (qsizetype i = 0; i + 1 < host.received.size(); ++i) {
        if (host.received.at(i) == "--trunk-scan") {
            paths << host.received.at(i + 1);
        }
    }
    check(paths
          == QStringList({scan_list_csv_path(hospital.value("uid").toString(), "Hospital", 1),
                          scan_list_csv_path(parks.value("uid").toString(), "Parks", 2)}));
    // The imported file is never written.
    QFile original(hospitalFile);
    check(original.open(QIODevice::ReadOnly) && original.readAll() == hospitalBefore);
    original.close();

    // The real coordinator over those copies: the lists named after the lists, and the target first in the
    // rotation holding its own talkgroups from the file its copy names. Run in both orders, so each copy's companion
    // is read.
    const struct {
        int first;
        const char* names[2];
        const char* group;
    } orders[] = {{0, {"Hospital", "Parks"}, "Hospital TG"}, {1, {"Parks", "Hospital"}, "Parks TG"}};

    for (const auto& order : orders) {
        auto* opts = new dsd_opts{};
        auto* state = new dsd_state{};
        initOpts(opts);
        initState(state);
        opts->trunk_scan_enabled = 1;
        opts->use_rigctl = 1;
        opts->trunk_scan_list_count = 2;
        for (int i = 0; i < 2; ++i) {
            DSD_SNPRINTF(opts->trunk_scan_lists[i].path, sizeof opts->trunk_scan_lists[i].path, "%s",
                         paths.value((order.first + i) % 2).toUtf8().constData());
        }
        dsd_trunk_tuning_hooks hooks{};
        hooks.tune_to_freq_request = acceptTune;
        hooks.tune_to_cc_request = acceptTune;
        dsd_trunk_tuning_hooks_set(hooks);
        char error[256]{};
        const bool initialized = dsd_engine_trunk_scan_init(opts, state, error, sizeof error) == 0;
        if (!initialized) {
            std::fprintf(stderr, "imported lists: %s\n", error);
        }
        check(initialized);
        if (initialized) {
            check(state->trunk_scan_list_count == 2
                  && QString::fromUtf8(state->trunk_scan_lists[0].name) == QLatin1String(order.names[0])
                  && QString::fromUtf8(state->trunk_scan_lists[1].name) == QLatin1String(order.names[1]));
            dsd_tg_policy_lookup found{};
            check(dsd_tg_policy_lookup_id(state, 123, &found) == 0
                  && QString::fromUtf8(found.entry.name) == QLatin1String(order.group));
        }
        dsd_engine_trunk_scan_shutdown(opts, state);
        dsd_trunk_tuning_hooks_set({});
        dsd_trunk_scan_hooks_set(nullptr);
        freeState(state);
        delete state;
        delete opts;
    }
    check(model.remove(model.count() - 1) && model.remove(model.count() - 1));
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
    checkCsvStem();
    checkCsvPathReadsAsSaved();
    checkCsvFolder();
    checkImportedListsNamedApart(dir);
    model.update(0, {{"name", "Updated"}, {"uid", "replacement"}});
    model.touch(0);
    ScanListsModel loaded;
    check(loaded.count() == 1 && loaded.get(0).value("uid") == uid
          && loaded.get(0).value("lastHeard").toLongLong() > 0);
    ScanListStarter starter(nullptr, nullptr);
#if defined(Q_OS_UNIX)
    // Android has no usable global /tmp. Validation and build must still work
    // with only the app's cache writable, including before that cache exists.
    const bool hadTmpDir = qEnvironmentVariableIsSet("TMPDIR");
    const QByteArray previousTmpDir = qgetenv("TMPDIR");
    qputenv("TMPDIR", (dir.path() + "/missing-temp").toUtf8());
    const auto validation = starter.validate(list);
    check(validation.value("ok").toBool() && validation.value("targetCount").toInt() == 1);
    const auto withoutTemp = starter.build(list);
    check(withoutTemp.value("ok").toBool() && withoutTemp.value("targetCount").toInt() == 1);
    if (hadTmpDir) {
        qputenv("TMPDIR", previousTmpDir);
    } else {
        qunsetenv("TMPDIR");
    }
#endif
    auto result = starter.build(list);
    check(result.value("ok").toBool() && result.value("targetCount").toInt() == 1);
    checkRecordingSessions(starter, list);
    const QString path = dsd_qt::scan_list_csv_path(uid, list.value("name").toString(), 1);
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
    check(file.open(QIODevice::ReadOnly));
    const auto lastValidCsv = file.readAll();
    file.close();
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
    check(file.open(QIODevice::ReadOnly));
    check(file.readAll() == lastValidCsv);
    file.close();
    // INT-3 redacts QML maps; a supported saved key must still reach the private CSV.
    const QString directKey = QStringLiteral("123456789A");
    systems.update(0, {{"encKeyType", "rc4"}, {"encKeyValue", directKey}});
    check(!systems.get(0).contains("encKeyValue"));
    check(keyedStarter.build(list).value("ok").toBool());
    check(file.open(QIODevice::ReadOnly));
    check(file.readAll().contains(QByteArray("-1 ") + directKey.toUtf8()));
    file.close();
    // RadioReference's combined P25 flags pass the real target parser with their scoped preference.
    systems.update(0, {{"decodeFlag", "-mq -^"}, {"trunking", true}});
    check(keyedStarter.build(list).value("ok").toBool());
    check(file.open(QIODevice::ReadOnly));
    const auto importedCsv = file.readAll();
    check(importedCsv.contains("p25-trunk") && importedCsv.contains("cqpsk") && importedCsv.contains("-^"));
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
    // CSV-backed lists preserve their durable input and bypass manual systems.
    const QString targetPath = dir.filePath("targets.csv");
    QFile targets(targetPath);
    check(targets.open(QIODevice::WriteOnly));
    targets.write("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                  "CSV ID,p25-conventional,851500000,,,,,--scan-max-visit-ms 20000\n");
    targets.close();
    check(model.add({{"name", "Imported"},
                     {"sourceType", "rtltcp"},
                     {"host", "localhost"},
                     {"port", 1234},
                     {"targetSource", "csv"},
                     {"targetsCsvPath", targetPath},
                     {"entries", QVariantList{entry}}}));
    auto imported = model.get(0);
    check(imported.value("entries").toList().isEmpty());
    check(!starter.build(imported).value("ok").toBool());
    starter.setTargetFileLookup([&targetPath](const QString& candidate) { return candidate == targetPath; });
    const auto builtCsv = starter.build(imported);
    check(builtCsv.value("ok").toBool() && builtCsv.value("targetCount").toInt() == 1);
    const auto importedArgs = builtCsv.value("args").toStringList();
    // Issue #697: an imported list runs from its own copy, named after the list, in its folder; nothing at the old
    // per-uid path.
    const QString importedCopy =
        scan_list_csv_path(imported.value("uid").toString(), imported.value("name").toString(), 1);
    check(importedArgs.value(importedArgs.indexOf("--trunk-scan") + 1) == importedCopy && QFile::exists(importedCopy)
          && importedCopy != targetPath);
    check(!QFile::exists(json_store_path("scan_lists/" + imported.value("uid").toString() + ".csv")));
    imported["entries"] = QVariantList{entry};
    check(!starter.build(imported).value("ok").toBool());
    imported["entries"] = QVariantList();
    imported["sourceType"] = "udp";
    check(!starter.build(imported).value("ok").toBool());
    imported["sourceType"] = "rtltcp";
    imported["defaultDwellMs"] = 1;
    check(!starter.build(imported).value("ok").toBool());
    ScanListsModel csvReloaded;
    check(csvReloaded.get(0).value("targetSource") == "csv"
          && csvReloaded.get(0).value("targetsCsvPath") == targetPath);
    check(model.listsReferencingPath(targetPath) == QStringList{"Imported"});
    check(model.clearCsvPath(targetPath));
    check(model.get(0).value("isDraft").toBool() && model.get(0).value("targetsCsvPath").toString().isEmpty());
    check(!starter.build(model.get(0)).value("ok").toBool());
    check(model.remove(0) && QFile::exists(targetPath));
    checkMixedAnalogRoundTrip(dir);
    checkAnalogEntriesIgnoreHiddenDecryption();
    checkAnalogRateDiagnostic(dir);
    QDir(dataDir).removeRecursively();
    return failures ? 1 : 0;
}
