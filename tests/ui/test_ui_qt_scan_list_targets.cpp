// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <array>
#include <cstdio>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "json_store.h"
#include "saved_systems_model.h"
#include "scan_entry_name.h"
#include "scan_list_targets.h"
#include "scan_protocol_catalog.h"
#include "session_args.h"
using namespace dsd_qt;
static int failures;
/* A 64-digit hex key: the AES width every class that takes -H takes. */
#define kHex64 "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF"

static void
check_at(bool ok, int line) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "scan-list assertion failed at line %d\n", line);
    }
}

#define check(...) check_at((__VA_ARGS__), __LINE__)

/* Issue #890: the scan-list protocol catalog, in picker order, with each protocol's decode flag, engine type, label,
   width and capabilities; lookups never split a type at its first hyphen and keep the decode flags' case. */
static void
test_protocol_catalog() {
    const QStringList ids{"p25", "dmr", "nxdn", "nxdn48", "dpmr", "dstar", "ysf", "m17", "tetra_dmo", "nfm", "am"};
    const QStringList labels{"P25",        "DMR", "NXDN96",    "NXDN48",       "dPMR",       "D-STAR",
                             "YSF (C4FM)", "M17", "TETRA DMO", "NFM (analog)", "AM (analog)"};
    const QStringList flags{"-ft", "-fs", "-fn", "-fi", "-fm", "-fd", "-fy", "-fz", "-fT", "-fA", "-fM"};
    const QList<int> widths{12500, 12500, 12500, 6250, 6250, 6250, 12500, 12500, 25000, 12500, 6000};
    const QVariantList variant = scan_protocols_variant();
    check(variant.size() == ids.size() && scan_protocols().size() == static_cast<size_t>(ids.size()));
    for (qsizetype i = 0; i < ids.size() && i < variant.size(); ++i) {
        const QVariantMap p = variant.at(i).toMap();
        check(p.value("id").toString() == ids.at(i) && p.value("label").toString() == labels.at(i)
              && p.value("decodeFlag").toString() == flags.at(i) && p.value("widthHz").toInt() == widths.at(i));
        const ScanProtocol* byFlag = scan_protocol_for_flag(flags.at(i));
        check(byFlag != nullptr && QLatin1String(byFlag->id) == ids.at(i));
        check(scan_protocol_by_id(ids.at(i)) == byFlag);
        check(scan_protocol_for_type(p.value("type").toString()) == byFlag);
        check(scan_entry_protocol_label(ids.at(i)) == labels.at(i));
    }
    // The new families are conventional only, take no modulation, and follow the key matrix.
    for (const char* id : {"dpmr", "dstar", "ysf", "m17", "tetra_dmo"}) {
        const ScanProtocol* p = scan_protocol_by_id(QString::fromLatin1(id));
        check(p != nullptr && !p->trunking && !p->modulation && !p->analog);
    }
    check(scan_protocol_by_id("dpmr")->keys == ScanProtocolKeys::Scrambler);
    check(scan_protocol_by_id("m17")->keys == ScanProtocolKeys::Session);
    for (const char* id : {"dstar", "ysf", "tetra_dmo", "nfm", "am"}) {
        check(scan_protocol_by_id(QString::fromLatin1(id))->keys == ScanProtocolKeys::None);
    }
    for (const char* id : {"p25", "dmr", "nxdn", "nxdn48"}) {
        check(scan_protocol_by_id(QString::fromLatin1(id))->keys == ScanProtocolKeys::Full);
    }
    // Types are read whole: tetra-dmo-conventional is TETRA DMO, and no new family has a trunked spelling.
    check(scan_protocol_type(*scan_protocol_by_id("tetra_dmo"), false) == "tetra-dmo-conventional");
    check(scan_entry_type_label("tetra-dmo-conventional") == "TETRA DMO");
    check(scan_entry_type_label("DPMR-Conventional") == "dPMR");
    check(scan_entry_type_label("nxdn-trunk") == "NXDN96" && scan_entry_type_label("nxdn48-trunk") == "NXDN48");
    for (const char* type : {"tetra-dmo-trunk", "ysf-trunk", "dstar-trunk", "tetra-conventional", "tetra", "edacs"}) {
        check(scan_protocol_for_type(QString::fromLatin1(type)) == nullptr);
    }
    check(scan_entry_protocol_label("edacs").isEmpty() && scan_entry_protocol_label("TETRA_DMO").isEmpty());
    // Decode flags keep their case: -fm dPMR, -fM AM, -ft P25, -fT TETRA; P25's compound aliases are P25.
    check(QLatin1String(scan_protocol_for_flag("-fm")->id) == QLatin1String("dpmr"));
    check(QLatin1String(scan_protocol_for_flag("-fM")->id) == QLatin1String("am"));
    check(QLatin1String(scan_protocol_for_flag(" -fT ")->id) == QLatin1String("tetra_dmo"));
    for (const char* p25 : {"-ft", "-f1", "-mq", "-^", "-ft -^", "-mq -^", "-^ -ft", "-^ -mq"}) {
        check(scan_protocol_for_flag(QString::fromLatin1(p25)) == scan_protocol_by_id("p25"));
    }
    for (const char* none : {"", "-fa", "-FT", "-fp", "-fe", "-fT -xd", "-fs -Y"}) {
        check(scan_protocol_for_flag(QString::fromLatin1(none)) == nullptr);
    }
    // A channel map's mode names the scan class; the frequency-entry id and D-STAR's spelled-out name also count.
    check(scan_protocol_for_mode("nxdn96") == scan_protocol_by_id("nxdn"));
    check(scan_protocol_for_mode("TETRA-DMO") == scan_protocol_by_id("tetra_dmo"));
    check(scan_protocol_for_mode("d-star") == scan_protocol_by_id("dstar"));
    check(scan_protocol_for_mode("-fm") == scan_protocol_by_id("dpmr")
          && scan_protocol_for_mode("-fM") == scan_protocol_by_id("am"));
    check(scan_entry_protocol_for_flag("-fz") == "m17" && scan_entry_protocol_for_flag("-fa").isEmpty());
}

/* An NXDN48 or NXDN96 row's keys are the switches the engine's scoped options give its class (scan_options.c): a saved
   NXDN system's Basic Privacy key, DMR talkgroup key map or algorithm fallback (DMR's only) is refused here by name, so
   the list never reaches the engine with a row it would refuse; every switch the class takes still reaches the row. */
static void
test_nxdn_row_keys() {
    const QVariantMap entry{{"uid", "entry"}, {"kind", "system"}, {"systemUid", "sys"}, {"enabled", true}};
    const QVariantMap list{{"uid", "list"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}};
    for (const char* flag : {"-fi", "-fn"}) {
        for (const bool trunk : {false, true}) {
            QVariantMap sys{
                {"uid", "sys"}, {"name", "Site"}, {"decodeFlag", flag}, {"freqMhz", "451.5"}, {"trunking", trunk}};
            const QString site = QStringLiteral("Site: ") + QLatin1String(scan_protocol_for_flag(flag)->label);
            auto build = [&]() { return scan_list_targets(list, QVariantList{sys}); };
            check(build().ok);
            // Refused, each by name.
            sys["encKeyType"] = "basic";
            sys["encKeyValue"] = "7";
            check(!build().ok
                  && build().error
                         == site
                                + QStringLiteral(" entries take no Motorola Basic Privacy key, only a Hytera / AES, "
                                                 "RC4 / DES or scrambler key. Choose another key type for this saved "
                                                 "system or its decryption profile."));
            sys.remove("encKeyType");
            sys.remove("encKeyValue");
            sys["dmrTgKeyCsvPath"] = "/imports/tg.csv";
            check(!build().ok
                  && build().error
                         == site
                                + QStringLiteral(" entries take no DMR talkgroup key map. Remove it from this saved "
                                                 "system's decryption profile."));
            sys.remove("dmrTgKeyCsvPath");
            const QString algorithm = site
                                      + QStringLiteral(" entries take privacy forcing only, not an algorithm "
                                                       "fallback. Change the Force key setting of this saved "
                                                       "system or its decryption profile.");
            sys["encKeyType"] = "scrambler";
            sys["encKeyValue"] = "17";
            sys["encForceKey"] = 2;
            check(!build().ok && build().error == algorithm);
            sys["encForceKey"] = 0;
            for (const int force : {2, 0x21, 255}) {
                sys["decryptionForce"] = force;
                check(!build().ok && build().error == algorithm);
            }
            sys.remove("decryptionForce");
            // Taken: -R with privacy forcing, -H, -1, the key files, a profile's reference, no forcing, clear keys, -G.
            sys["encForceKey"] = 1;
            check(build().ok && build().csv.endsWith(",\"Site\",-R 17 -4\n"));
            sys.remove("encForceKey");
            for (const auto& key : {qMakePair(QStringLiteral("hex"), QByteArray("-H ")),
                                    qMakePair(QStringLiteral("rc4"), QByteArray("-1 "))}) {
                sys["encKeyType"] = key.first;
                sys["encKeyValue"] = key.first == QLatin1String("hex") ? kHex64 : "0123456789";
                check(build().ok && build().csv.contains(",\"Site\"," + key.second));
            }
            // The key width the engine's NXDN class takes: an AES key of 64 hex digits, no other.
            for (const char* narrow : {"0000001F00", "0123456789ABCDEF0123456789ABCDEF"}) {
                sys["encKeyType"] = "hex";
                sys["encKeyValue"] = narrow;
                check(!build().ok && build().error == QStringLiteral("Site: NXDN AES needs 64 hex digits."));
            }
            sys.remove("encKeyType");
            sys.remove("encKeyValue");
            sys["keysHexCsvPath"] = "/imports/hex.csv";
            sys["keysDecCsvPath"] = "/imports/dec.csv";
            sys["decryptionProfileRef"] = "nxdn-ref";
            sys["decryptionForce"] = 0;
            sys["groupCsvPath"] = "/imports/groups.csv";
            check(build().ok
                  && build().csv.endsWith(",\"Site\",-G \"/imports/groups.csv\" -K \"/imports/hex.csv\" -k "
                                          "\"/imports/dec.csv\" --no-force-key --key-profile-ref nxdn-ref\n"));
            sys["decryptionForce"] = 1;
            sys["decryptionClearKeys"] = true;
            check(build().ok && build().csv.contains("--no-decryption-keys -4 --key-profile-ref nxdn-ref"));
            // The legacy key column, too.
            sys["keyCsvPath"] = "/imports/keys.csv";
            sys["keyCsvHex"] = true;
            check(build().ok && build().csv.contains(",/imports/keys.csv,,"));
        }
    }
}

/* Issue #890: saved systems and frequency entries of YSF, D-STAR, dPMR, M17 and TETRA DMO become conventional targets,
   trunking refused; their rows take no modulation (a stale saved one is refused) and keys only by the matrix. */
static void
test_simplex_digital_targets() {
    QVariantMap entry{{"uid", "entry"}, {"kind", "system"}, {"systemUid", "sys"}, {"enabled", true}};
    QVariantMap list{{"uid", "list"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}};
    QVariantMap sys{{"uid", "sys"}, {"name", "Simplex"}, {"decodeFlag", "-fy"}, {"freqMhz", "145.5"}};
    auto build = [&]() { return scan_list_targets(list, QVariantList{sys}); };
    const QList<QPair<QString, QString>> families{
        {"-fy", "ysf"}, {"-fd", "dstar"}, {"-fm", "dpmr"}, {"-fz", "m17"}, {"-fT", "tetra-dmo"}};
    for (const auto& family : families) {
        sys["decodeFlag"] = family.first;
        sys["trunking"] = false;
        const auto conventional = build();
        check(conventional.ok
              && conventional.csv.endsWith(
                  (QStringLiteral("entry,%1-conventional,145500000,,,,,,,,,,\"Simplex\",\n").arg(family.second))
                      .toUtf8()));
        const QString label = QString::fromLatin1(scan_protocol_for_flag(family.first)->label);
        sys["trunking"] = true;
        const auto trunked = build();
        check(!trunked.ok
              && trunked.error
                     == QStringLiteral("Simplex: %1 entries are conventional only; turn trunking off for this saved "
                                       "system.")
                            .arg(label));
        sys["trunking"] = false;
        // A channel map has no trunked type to go to either.
        sys["chanCsvPath"] = "/imports/channels.csv";
        check(!build().ok
              && build().error
                     == QStringLiteral("Simplex: %1 entries take no channel map; remove it from this saved system.")
                            .arg(label));
        sys.remove("chanCsvPath");
        // No modulation: one left in the entry from another protocol is not written, and a modulation saved in the
        // system's Radio settings is refused.
        entry["modulation"] = "gfsk";
        list["entries"] = QVariantList{entry};
        check(build().ok && !build().csv.contains("gfsk"));
        entry.remove("modulation");
        list["entries"] = QVariantList{entry};
        sys["radioSettings"] = QVariantMap{{"modulation", 0}};
        check(!build().ok
              && build().error
                     == QStringLiteral("Simplex: %1 entries take no modulation: clear the modulation saved in this "
                                       "system's Radio settings.")
                            .arg(label));
        // A digital row runs no dynamic squelch: a saved auto squelch goes to it as off (issue #899 B13, rowSquelch()).
        sys["radioSettings"] = QVariantMap{{"squelch", "auto+12"}};
        check(build().ok && build().csv.endsWith(",\"Simplex\",--squelch off\n"));
        sys.remove("radioSettings");
        // An unnamed system is named for its protocol.
        sys["name"] = "";
        check(build().ok && build().csv.contains((",\"" + label + QString::fromUtf8(" · 145.5 MHz\",")).toUtf8()));
        sys["name"] = "Simplex";
    }
    // Frequency entries of each family, in one list, beside P25.
    QVariantList entries;
    const QStringList protocols{"ysf", "dstar", "dpmr", "m17", "tetra_dmo", "p25"};
    for (int i = 0; i < protocols.size(); ++i) {
        entries << QVariantMap{
            {"uid", QStringLiteral("f%1").arg(i)}, {"kind", "freq"},       {"protocol", protocols[i]},
            {"freqMhz", QString::number(430 + i)}, {"modulation", "c4fm"}, {"enabled", true}};
    }
    list["entries"] = entries;
    const auto mixed = build();
    check(mixed.ok && mixed.targetCount == 6);
    check(mixed.csv.contains("f0,ysf-conventional,430000000,,,,,,,,,,\"YSF (C4FM) · 430 MHz\",\n")
          && mixed.csv.contains("f1,dstar-conventional,431000000,,,,,,,,,,\"D-STAR · 431 MHz\",\n")
          && mixed.csv.contains("f2,dpmr-conventional,432000000,,,,,,,,,,\"dPMR · 432 MHz\",\n")
          && mixed.csv.contains("f3,m17-conventional,433000000,,,,,,,,,,\"M17 · 433 MHz\",\n")
          && mixed.csv.contains("f4,tetra-dmo-conventional,434000000,,,,,,,,,,\"TETRA DMO · 434 MHz\",\n")
          && mixed.csv.contains("f5,p25-conventional,435000000,,,,,,,,c4fm,,\"P25 · 435 MHz\",\n"));
    list["entries"] = QVariantList{entry};

    // Keys. YSF, D-STAR and TETRA DMO decrypt nothing: a saved system's key settings have nothing to act on, so none
    // reaches the row, and none of its key files has to exist or fit a cell. Its talkgroup file still goes.
    for (const char* flag : {"-fy", "-fd", "-fT"}) {
        sys["decodeFlag"] = QString::fromLatin1(flag);
        sys["encKeyType"] = "basic";
        sys["encKeyValue"] = "7";
        sys["encForceKey"] = 1;
        sys["keyCsvPath"] = "/imports/old,keys.csv";
        sys["keysHexCsvPath"] = "/imports/hex.csv";
        sys["dmrTgKeyCsvPath"] = "/imports/tg.csv";
        sys["decryptionProfileRef"] = "profile-ref";
        sys["groupCsvPath"] = "/imports/groups.csv";
        const auto keyless = build();
        check(keyless.ok && keyless.paths == QStringList{"/imports/groups.csv"});
        check(keyless.csv.endsWith(",\"Simplex\",-G \"/imports/groups.csv\"\n") && !keyless.csv.contains("old,keys")
              && !keyless.csv.contains("hex.csv") && !keyless.csv.contains("tg.csv") && !keyless.csv.contains("-b")
              && !keyless.csv.contains("-4") && !keyless.csv.contains("profile-ref"));
        for (const auto& field : {"encKeyType", "encKeyValue", "encForceKey", "keyCsvPath", "keysHexCsvPath",
                                  "dmrTgKeyCsvPath", "decryptionProfileRef", "groupCsvPath"}) {
            sys.remove(field);
        }
    }
    // dPMR takes its scoped -R scrambler key and nothing else; a legacy NXDN / dPMR scrambler system keeps working,
    // force choice and all.
    sys["decodeFlag"] = "-fm";
    sys["encKeyType"] = "scrambler";
    sys["encKeyValue"] = "17";
    sys["encForceKey"] = 1;
    check(build().ok && build().csv.endsWith(",\"Simplex\",-R 17 -4\n"));
    sys.remove("encForceKey");
    for (const auto& material : {qMakePair(QStringLiteral("basic"), QStringLiteral("7")),
                                 qMakePair(QStringLiteral("hex"), QStringLiteral("0000001F00")),
                                 qMakePair(QStringLiteral("rc4"), QStringLiteral("0123456789")),
                                 qMakePair(QStringLiteral("m17Scrambler"), QStringLiteral("123"))}) {
        sys["encKeyType"] = material.first;
        sys["encKeyValue"] = material.second;
        const QString refusal =
            material.first == QLatin1String("m17Scrambler")
                ? QStringLiteral("Simplex: Invalid direct key shape for the selected type.")
                : QStringLiteral("Simplex: dPMR entries take no %1 key, only a scrambler key. Choose another key type "
                                 "for this saved system or its decryption profile.")
                      .arg(material.first == QLatin1String("basic") ? QStringLiteral("Motorola Basic Privacy")
                           : material.first == QLatin1String("hex") ? QStringLiteral("Hytera / AES")
                                                                    : QStringLiteral("RC4 / DES"));
        check(!build().ok && build().error == refusal);
    }
    sys.remove("encKeyType");
    sys.remove("encKeyValue");
    for (const auto& field : {"keyCsvPath", "keysHexCsvPath", "keysDecCsvPath", "dmrTgKeyCsvPath"}) {
        sys[field] = "/imports/keys.csv";
        check(!build().ok
              && build().error
                     == (QByteArray(field) == "dmrTgKeyCsvPath"
                             ? QStringLiteral("Simplex: dPMR entries take no DMR talkgroup key map. Remove it from "
                                              "this saved system's decryption profile.")
                             : QStringLiteral("Simplex: dPMR entries take no key file. Remove it from this saved "
                                              "system or its decryption profile.")));
        sys.remove(field);
    }
    // Forcing: privacy forcing (-4, direct choice 1 or a profile's 1) and no forcing pass; an algorithm fallback (the
    // direct choice 2, -0, or a profile's algorithm, --dmr-force-algid), which only a DMR row takes, is refused.
    const QString algorithmRefusal = QStringLiteral("Simplex: dPMR entries take privacy forcing only, not an algorithm "
                                                    "fallback. Change the Force key setting of this saved system or "
                                                    "its decryption profile.");
    sys["encKeyType"] = "scrambler";
    sys["encKeyValue"] = "17";
    sys["encForceKey"] = 2;
    check(!build().ok && build().error == algorithmRefusal);
    sys["encForceKey"] = 0;
    for (const int force : {2, 0x21, 255}) {
        sys["decryptionForce"] = force;
        check(!build().ok && build().error == algorithmRefusal);
    }
    sys["decryptionForce"] = 1;
    check(build().ok && build().csv.endsWith(",\"Simplex\",-R 17 -4\n"));
    sys["decryptionForce"] = 0;
    check(build().ok && build().csv.endsWith(",\"Simplex\",-R 17 --no-force-key\n"));
    // A profile force past int's range adds no option, so it forces nothing.
    sys["decryptionForce"] = (qlonglong(1) << 32) + 2;
    check(build().ok && build().csv.endsWith(",\"Simplex\",-R 17\n"));
    sys.remove("decryptionForce");
    sys.remove("encForceKey");
    sys.remove("encKeyType");
    sys.remove("encKeyValue");
    // The same choices still reach a DMR row, the one that takes them.
    sys["decodeFlag"] = "-fs";
    sys["encForceKey"] = 2;
    check(build().ok && build().csv.endsWith(",\"Simplex\",-0\n"));
    sys["encForceKey"] = 0;
    sys["decryptionForce"] = 0x21;
    check(build().ok && build().csv.contains("--dmr-force-algid 21"));
    sys.remove("decryptionForce");
    sys.remove("encForceKey");
    sys["decodeFlag"] = "-fm";
    // A no-keys profile on dPMR clears the session's keys for the row, as on any keyed protocol.
    sys["decryptionClearKeys"] = true;
    sys["decryptionProfileRef"] = "none-ref";
    check(build().ok && build().csv.contains("--no-decryption-keys")
          && build().csv.contains("--key-profile-ref none-ref"));
    sys.remove("decryptionClearKeys");
    sys.remove("decryptionProfileRef");
    // M17 takes none of its own: the session's keys apply, and a saved system that carries any is refused, so a key
    // the person set is never silently dropped.
    sys["decodeFlag"] = "-fz";
    const QString m17Refusal =
        QStringLiteral("Simplex: M17 entries use the session's keys, never their own. Remove the "
                       "key or decryption profile from this saved system, or set the key in the "
                       "app's decoder options.");
    const QList<QPair<QString, QVariant>> m17Keys{{"encKeyType", "m17Scrambler"},
                                                  {"encKeyValue", "123"},
                                                  {"keyCsvPath", "/imports/keys.csv"},
                                                  {"keysHexCsvPath", "/imports/hex.csv"},
                                                  {"keysDecCsvPath", "/imports/dec.csv"},
                                                  {"decryptionProfileRef", "m17-ref"},
                                                  {"decryptionSelectionMode", "direct"},
                                                  {"decryptionClearKeys", true},
                                                  {"encForceKey", 1},
                                                  {"decryptionForce", 1}};
    for (const auto& key : m17Keys) {
        sys[key.first] = key.second;
        check(!build().ok && build().error == m17Refusal);
        sys.remove(key.first);
    }
    sys["groupCsvPath"] = "/imports/groups.csv";
    check(build().ok && build().csv.endsWith(",\"Simplex\",-G \"/imports/groups.csv\"\n"));
    sys.remove("groupCsvPath");

    // The entry's own decryption choice applies only where the editor offers it.
    const QVariantMap freq{{"kind", "freq"}};
    for (const char* id : {"p25", "dmr", "nxdn", "nxdn48", "dpmr"}) {
        QVariantMap e = freq;
        e["protocol"] = QString::fromLatin1(id);
        check(scan_list_entry_takes_keys(e, QString()));
    }
    for (const char* id : {"dstar", "ysf", "m17", "tetra_dmo", "nfm", "am"}) {
        QVariantMap e = freq;
        e["protocol"] = QString::fromLatin1(id);
        check(!scan_list_entry_takes_keys(e, QString()));
    }
    const QVariantMap systemEntry{{"kind", "system"}};
    check(!scan_list_entry_takes_keys(systemEntry, "-fT") && scan_list_entry_takes_keys(systemEntry, "-fm")
          && scan_list_entry_takes_keys(systemEntry, "-ft -^"));
    check(scan_list_entry_protocol(systemEntry, "-fz") == scan_protocol_by_id("m17")
          && scan_list_entry_protocol(freq, "-fz") == nullptr);
}

namespace {
/* One key or force setting a saved system can hold: the fields that carry it, the switch it puts in a row
   (ScanKeySwitch), that switch as the engine's scoped options take it alone, and how the generated row shows it. */
struct KeySetting {
    const char* name;
    QVariantMap fields;
    unsigned bit;
    const char* engineText;
    QRegularExpression rowShows;
};

QList<KeySetting>
keySettings() {
    const auto row = [](const char* pattern) { return QRegularExpression(QString::fromLatin1(pattern)); };
    return {
        {"basic key", {{"encKeyType", "basic"}, {"encKeyValue", "7"}}, ScanKeyBasic, "-b 7", row(",-b 7\n")},
        /* 64 hex digits, the width every class that takes -H takes (P25 also 32, DMR also 10 and 32). */
        {"hex key",
         {{"encKeyType", "hex"}, {"encKeyValue", kHex64}},
         ScanKeyHex,
         "-H " kHex64,
         row(",-H " kHex64 "\n")},
        {"rc4 key",
         {{"encKeyType", "rc4"}, {"encKeyValue", "0123456789"}},
         ScanKeyRc4,
         "-1 0123456789",
         row(",-1 0123456789\n")},
        {"scrambler key",
         {{"encKeyType", "scrambler"}, {"encKeyValue", "17"}},
         ScanKeyScrambler,
         "-R 17",
         row(",-R 17\n")},
        {"hex key file",
         {{"keysHexCsvPath", "/imports/hex.csv"}},
         ScanKeyHexFile,
         "-K /imports/hex.csv",
         row(",-K \"/imports/hex.csv\"\n")},
        {"decimal key file",
         {{"keysDecCsvPath", "/imports/dec.csv"}},
         ScanKeyDecFile,
         "-k /imports/dec.csv",
         row(",-k \"/imports/dec.csv\"\n")},
        {"legacy hex key column",
         {{"keyCsvPath", "/imports/keys.csv"}, {"keyCsvHex", true}},
         ScanKeyHexFile,
         "-K /imports/keys.csv",
         row(",/imports/keys.csv,,")},
        {"legacy decimal key column",
         {{"keyCsvPath", "/imports/keys.csv"}, {"keyCsvHex", false}},
         ScanKeyDecFile,
         "-k /imports/keys.csv",
         row(",,/imports/keys.csv,")},
        {"DMR talkgroup key map",
         {{"dmrTgKeyCsvPath", "/imports/tg.csv"}},
         ScanKeyDmrMap,
         "--dmr-tg-key-csv /imports/tg.csv",
         row(",--dmr-tg-key-csv \"/imports/tg.csv\"\n")},
        {"no DMR talkgroup key map",
         {{"dmrTgKeyClear", true}},
         ScanKeyDmrMapClear,
         "--dmr-tg-key-clear",
         row(",--dmr-tg-key-clear\n")},
        {"no keys",
         {{"decryptionClearKeys", true}},
         ScanKeyClear,
         "--no-decryption-keys",
         row(",--no-decryption-keys\n")},
        {"profile reference",
         {{"decryptionProfileRef", "ref-1"}},
         ScanKeyProfileRef,
         "--key-profile-ref ref-1",
         row(",--key-profile-ref ref-1\n")},
        {"direct privacy forcing", {{"encForceKey", 1}}, ScanKeyForcePrivacy, "-4", row(",-4\n")},
        {"profile privacy forcing", {{"decryptionForce", 1}}, ScanKeyForcePrivacy, "-4", row(",-4\n")},
        {"Basic Privacy fallback", {{"encForceKey", 2}}, ScanKeyForceBasic, "-0", row(",-0\n")},
        {"algorithm fallback",
         {{"decryptionForce", 0x21}},
         ScanKeyForceAlgid,
         "--dmr-force-algid 21",
         row(",--dmr-force-algid 21\n")},
        {"no forcing", {{"decryptionForce", 0}}, ScanKeyNoForce, "--no-force-key", row(",--no-force-key\n")},
        {"talkgroup file",
         {{"groupCsvPath", "/imports/groups.csv"}},
         ScanKeyGroup,
         "-G /imports/groups.csv",
         row(",-G \"/imports/groups.csv\"\n")},
    };
}

/* Whether the engine's scoped options take @p text alone on a conventional row of scan class @p mode. */
bool
engineTakes(const char* text, dsd_scan_mode mode) {
    dsd_scan_options parsed;
    DSD_MEMSET(&parsed, 0, sizeof parsed);
    char error[192];
    const bool taken =
        dsd_scan_options_parse(text, static_cast<unsigned int>(mode), 1, &parsed, error, sizeof error) == 0;
    DSD_SECURE_ZERO(&parsed, sizeof parsed);
    return taken;
}
} // namespace

/* Issue #890: one table says which key and force switches each protocol's row takes (ScanProtocol::keySwitches), and
   it is the engine's. Every protocol × every key or force setting a saved system can hold: the table agrees with the
   engine's own scoped-option parser (dsd_scan_options_parse()) for the protocol's class, and Qt's answer follows the
   table: a switch the row takes reaches it, one it does not is refused by name before anything is written (or, for a
   protocol whose editor hides decryption, never written), so a generated list can never carry a switch the engine
   would refuse it for. Every protocol's scan class is one the engine knows, and every cell must agree. */
static void
test_key_switch_table_matches_engine() {
    // The walk covers every switch: a new ScanKeySwitch bit with no setting here fails it.
    unsigned covered = 0U;
    for (const KeySetting& setting : keySettings()) {
        covered |= setting.bit;
    }
    check(covered == kScanKeyAllSwitches);
    if (covered != kScanKeyAllSwitches) {
        std::fprintf(stderr, "keySettings() misses switch bits 0x%x\n", kScanKeyAllSwitches & ~covered);
    }
    // Every protocol's table names only real switches.
    for (const ScanProtocol& protocol : scan_protocols()) {
        check((protocol.keySwitches & ~kScanKeyAllSwitches) == 0U);
    }
    const QVariantMap entry{{"uid", "entry"}, {"kind", "system"}, {"systemUid", "sys"}, {"enabled", true}};
    const QVariantMap list{{"uid", "list"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}};
    for (const ScanProtocol& protocol : scan_protocols()) {
        dsd_scan_mode mode = DSD_SCAN_MODE_INHERIT;
        const bool classKnown = dsd_scan_mode_parse(protocol.scanClass, &mode) == 0;
        check(classKnown);
        if (!classKnown) {
            std::fprintf(stderr, "%s row: the engine has no scan class '%s'\n", protocol.id, protocol.scanClass);
            continue;
        }
        for (const KeySetting& setting : keySettings()) {
            const bool tableTakes = (protocol.keySwitches & setting.bit) != 0U;
            // The table is the engine's.
            const bool agrees = engineTakes(setting.engineText, mode) == tableTakes;
            check(agrees);
            if (!agrees) {
                std::fprintf(stderr, "%s row, %s: the engine %s it, the table %s\n", protocol.id, setting.name,
                             tableTakes ? "refuses" : "takes", tableTakes ? "takes it" : "does not");
            }
            // Qt follows the table.
            QVariantMap sys{{"uid", "sys"}, {"name", "Row"}, {"decodeFlag", protocol.decodeFlag}, {"freqMhz", "451.5"}};
            for (auto it = setting.fields.cbegin(); it != setting.fields.cend(); ++it) {
                sys.insert(it.key(), it.value());
            }
            const auto built = scan_list_targets(list, QVariantList{sys});
            const bool shown = built.ok && setting.rowShows.match(QString::fromUtf8(built.csv)).hasMatch();
            /* Refused where the row has keys of its own (or M17's, the session's); a reset is never refused, and a
               protocol whose editor hides decryption, or an analog one, passes none of it. */
            const bool hidden = protocol.keys == ScanProtocolKeys::None || setting.bit == ScanKeyDmrMapClear
                                || setting.bit == ScanKeyGroup;
            const bool answer = tableTakes ? shown : !shown && (hidden ? built.ok : !built.ok);
            check(answer);
            if (!answer) {
                std::fprintf(stderr, "%s row, %s: table %s, Qt %s (%s)\n", protocol.id, setting.name,
                             tableTakes ? "takes it" : "does not",
                             built.ok ? (shown ? "wrote it" : "dropped it") : "refused it", qPrintable(built.error));
            }
        }
    }
}

/* A decryption profile made for another protocol is named as that, before any switch it carries is judged: a DMR
   Basic Privacy profile left on a system switched to P25 says the profile does not match, not that P25 takes no Basic
   Privacy key. A vendor keystream profile likewise. */
static void
test_profile_mismatch_named_first() {
    const QVariantMap entry{{"uid", "entry"}, {"kind", "system"}, {"systemUid", "sys"}, {"enabled", true}};
    const QVariantMap list{{"uid", "list"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}};
    QVariantMap sys{{"uid", "sys"},
                    {"name", "Site"},
                    {"decodeFlag", "-ft"},
                    {"freqMhz", "851.5"},
                    {"decryptionProtocol", "dmr"},
                    {"decryptionSelectionMode", "direct"},
                    {"decryptionProfileRef", "dmr-ref"},
                    {"decryptionForce", 0},
                    {"encKeyType", "basic"},
                    {"encKeyValue", "7"}};
    auto build = [&]() { return scan_list_targets(list, QVariantList{sys}); };
    check(!build().ok
          && build().error == QStringLiteral("Site: The decryption profile protocol does not match this scan entry."));
    // The same profile on the DMR system it was made for builds.
    sys["decodeFlag"] = "-fs";
    check(build().ok && build().csv.contains("-b 7"));
    // A vendor profile is named as that on a protocol whose row would refuse its switches too.
    sys["decodeFlag"] = "-ft";
    sys["decryptionProtocol"] = "mixed";
    sys["decryptionSelectionMode"] = "vendor";
    check(!build().ok
          && build().error
                 == QStringLiteral("Site: Vendor keystream profiles require a standalone session; scan restoration is "
                                   "unavailable."));
    // A P25 profile on P25 with a key P25 takes builds; one P25 cannot take is refused by its switch.
    sys["decryptionProtocol"] = "p25";
    sys["decryptionSelectionMode"] = "direct";
    check(!build().ok && build().error.contains("P25 entries take no Motorola Basic Privacy key"));
    sys["encKeyType"] = "rc4";
    sys["encKeyValue"] = "0123456789";
    check(build().ok && build().csv.contains("-1 0123456789"));
}

/* The options cell of the single row @p targets holds (its last CSV cell). */
static QString
optionsCell(const ScanListTargets& targets) {
    const QStringList lines = QString::fromUtf8(targets.csv).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.size() == 2 ? lines.at(1).section(QLatin1Char(','), 13)
                             : QStringLiteral("<rows: %1>").arg(lines.size());
}

/* Issue #899 (B13): every row's squelch is one its row type takes. A saved system's own setting goes to its row as
   the row runs it; a row with none carries none (it inherits; the decoder runs the app's default on an analog row,
   --squelch-analog-default), an analog row as much as a digital one. */
static void
rowSquelch() {
    QVariantMap entry{{"uid", "entry"}, {"kind", "system"}, {"systemUid", "sys"}, {"enabled", true}};
    QVariantMap list{{"uid", "list"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}};
    QVariantMap sys{{"uid", "sys"}, {"name", "Example"}, {"freqMhz", "851.5"}};
    const auto saved = [&](const QString& flag, const QString& squelch) {
        sys["decodeFlag"] = flag;
        sys["radioSettings"] = QVariantMap{{"squelch", squelch}};
        return scan_list_targets(list, QVariantList{sys});
    };

    const struct {
        const char* flag;
        const char* saved;
        const char* row;
        bool warns;
    } cases[] = {
        {"-fM", "noise+17", "--squelch auto+17", false},
        {"-fM", "noise", "--squelch auto+10", false},
        {"-fM", "auto+12", "--squelch auto+12", false},
        {"-fA", "noise+17", "--squelch noise+17", false},
        {"-fA", "AUTO+9", "--squelch auto+9", false},
        {"-ft", "auto+12", "--squelch off", false},
        {"-ft", "noise+8", "--squelch off", false},
        {"-fs", "noise+8", "--squelch off", false},
        {"-ft", "-60", "--squelch -60", false},
        {"-fA", "-110", "--squelch -110", false},
        {"-fA", "-65.5", "--squelch -66", true},
        {"-fA", "-0.25", "--squelch -1", true},
        {"-fA", "1", "--squelch -1", true},
        {"-fA", "-130", "--squelch -120", true},
        {"-fA", "off", "--squelch off", false},
        {"-fA", "0", "--squelch off", false},
    };

    for (const auto& c : cases) {
        const auto targets = saved(c.flag, c.saved);
        check(targets.ok && optionsCell(targets) == QLatin1String(c.row));
        check(targets.warnings.isEmpty() != c.warns);
    }
    sys.remove("radioSettings");

    for (const char* flag : {"-fA", "-fM", "-ft", "-fs"}) {
        sys["decodeFlag"] = QLatin1String(flag);
        const auto targets = scan_list_targets(list, QVariantList{sys});
        check(targets.ok && optionsCell(targets).isEmpty() && targets.warnings.isEmpty());
    }
    // A frequency entry carries none either, nfm, am or digital.
    for (const QString& protocol : {QStringLiteral("nfm"), QStringLiteral("am"), QStringLiteral("p25")}) {
        const QVariantMap freqEntry{
            {"uid", "f"}, {"kind", "freq"}, {"protocol", protocol}, {"freqMhz", "462.5625"}, {"enabled", true}};
        const auto targets = scan_list_targets(
            {{"uid", "list"}, {"sourceType", "usb"}, {"entries", QVariantList{freqEntry}}}, QVariantList{});
        check(targets.ok && optionsCell(targets).isEmpty());
    }
}

static void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
    }
    check(ok);
}

/* Issue #834: the Skip silent channels box owns --scan-skip-silent and --no-scan-skip-silent, but not how long the
   check reads. A list's extra options may set that length in either spelling, the space form's value as the next
   token; the decoder checks the range. */
static void
test_skip_silent_length_extra(const QVariantMap& list) {
    SessionArgPrefs prefs;
    QString error;
    for (const auto& extra : {QStringLiteral("--scan-skip-silent-ms 50"), QStringLiteral("--scan-skip-silent-ms=50")}) {
        expect("the scan gate takes the skip-silent length", session_args_scan_extra_safe(extra));
        prefs.extraArgs = extra;
        const QStringList args = session_args_scan_build(list, "851.5", "/targets.csv", prefs, &error);
        const QStringList tokens = extra.split(QLatin1Char(' '));
        expect("a scan list with the skip-silent length starts, the length passed as typed",
               error.isEmpty() && args.size() > tokens.size() && args.mid(args.size() - tokens.size()) == tokens
                   && args.count(QStringLiteral("--scan-skip-silent")) == 1);
    }
    for (const auto& extra : {QStringLiteral("--scan-skip-silent-ms 50 --scan-skip-silent"),
                              QStringLiteral("--scan-skip-silent-ms=50 --no-scan-skip-silent"),
                              QStringLiteral("--scan-skip-silent=1"), QStringLiteral("--no-scan-skip-silent-ms=50")}) {
        expect("the scan gate still refuses the skip-silent switches beside the length",
               !session_args_scan_extra_safe(extra));
        prefs.extraArgs = extra;
        expect("a scan list carrying a skip-silent switch is refused",
               session_args_scan_build(list, "851.5", "/targets.csv", prefs, &error).isEmpty()
                   && error == session_args_error_text(SessionArgsError::UnsafeOption));
    }
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    dsd_test_qt_isolate_paths();
    QVariantMap entry{{"uid", "entry"}, {"kind", "system"}, {"systemUid", "sys"}, {"enabled", true}};
    QVariantMap list{{"uid", "list"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}};
    QVariantMap sys{{"uid", "sys"}, {"name", "Example"}, {"decodeFlag", "-ft"}, {"freqMhz", "851.5"}};
    auto build = [&]() { return scan_list_targets(list, QVariantList{sys}); };
    sys["radioSettings"] = QVariantMap{{"modulation", 1}, {"squelch", "auto+12"}};
    // Issue #899 (B13): a P25 row takes no auto squelch (the row parser refuses one), and the system ran it as off on
    // its own: the row says off, so a session level from the extra options cannot gate it either.
    check(build().ok && build().csv.contains(",cqpsk,,\"Example\",--squelch off"));
    entry["modulation"] = "c4fm";
    list["entries"] = QVariantList{entry};
    check(build().ok && build().csv.contains(",c4fm,,\"Example\",--squelch off"));
    entry.remove("modulation");
    list["entries"] = QVariantList{entry};
    sys["decodeFlag"] = "-fA";
    check(build().ok && build().csv.contains(",,,\"Example\",--squelch auto+12"));
    sys["radioSettings"] = QVariantMap{{"modulation", 8}};
    check(!build().ok);
    sys.remove("radioSettings");
    sys["decodeFlag"] = "-ft";
    auto result = build();
    check(result.ok && result.targetCount == 1 && result.csv.contains("p25-conventional,851500000"));
    // Issue #782: a saved system with no name is labelled by its protocol and frequency, never left to its UID.
    sys["name"] = "  ";
    check(build().ok && build().csv.contains(",\"P25 · 851.5 MHz\",") && !build().csv.contains("\"\","));
    sys["name"] = "Example";
    /* Issue #890: -fd, -fy, -fm, -fz and -fT map (test_simplex_digital_targets()); Auto, EDACS and compound flags do
       not. */
    for (const auto& flag :
         QStringList{"", "-Y", "-fa", "-fe", "-fp", "-ft -fT", "-mq -Y", "-mq -^ --show-keys", "-fs -^"}) {
        sys["decodeFlag"] = flag;
        check(!build().ok && build().error.contains("Example"));
    }
    const QStringList flags{"-ft", "-f1", "-mq", "-^", "-fs", "-fi", "-fn"};
    const QStringList types{"p25", "p25", "p25", "p25", "dmr", "nxdn48", "nxdn"};
    for (int i = 0; i < flags.size(); ++i) {
        sys["decodeFlag"] = flags[i];
        for (bool trunk : {false, true}) {
            sys["trunking"] = trunk;
            check(build().ok && build().csv.contains((types[i] + (trunk ? "-trunk" : "-conventional")).toUtf8()));
        }
    }
    // Issue #526: a saved analog system maps to its conventional target, never a trunked one, and so does a frequency
    // entry's analog protocol. An analog entry carries no modulation, keys, talkgroups or key-map files: the editor
    // hides them, and a saved analog system's have nothing to act on, so none reaches the CSV or has to exist.
    for (const auto& analog : {qMakePair(QStringLiteral("-fA"), QByteArray("nfm-conventional")),
                               qMakePair(QStringLiteral("-fM"), QByteArray("am-conventional"))}) {
        sys["decodeFlag"] = analog.first;
        sys["trunking"] = false;
        check(build().ok && build().csv.contains(analog.second + ",851500000,"));
        sys["trunking"] = true;
        check(!build().ok && build().error.contains("Example: Analog entries are conventional only"));
        sys["trunking"] = false;
        sys["encKeyType"] = "basic";
        sys["encKeyValue"] = "7";
        sys["encForceKey"] = 1;
        sys["groupCsvPath"] = "/imports/groups.csv";
        sys["keyCsvPath"] = "/imports/keys.csv";
        sys["dmrTgKeyCsvPath"] = "/imports/tg.csv";
        sys["decryptionClearKeys"] = true;
        entry["modulation"] = "gfsk";
        entry["gainDb"] = 20;
        list["entries"] = QVariantList{entry};
        const auto analogTargets = build();
        check(analogTargets.ok && analogTargets.paths.isEmpty());
        check(analogTargets.csv.endsWith(analog.second + ",851500000,,,,,,,,,20,\"Example\",\n"));
        check(!analogTargets.csv.contains("-b") && !analogTargets.csv.contains("-G")
              && !analogTargets.csv.contains("gfsk") && !analogTargets.csv.contains("keys.csv")
              && !analogTargets.csv.contains("-4"));
        // ...nor has to fit a CSV cell: a retained path with a comma, quote or line break that no analog entry passes
        // does not refuse the list.
        sys["keyCsvPath"] = "/imports/old,keys.csv";
        sys["groupCsvPath"] = "/imports/\"groups\".csv";
        sys["keysHexCsvPath"] = "/imports/hex\nkeys.csv";
        const auto unusedPaths = build();
        check(unusedPaths.ok && unusedPaths.paths.isEmpty());
        check(!unusedPaths.csv.contains("/imports/old") && !unusedPaths.csv.contains("/imports/\"groups")
              && !unusedPaths.csv.contains("/imports/hex"));
        sys.remove("keysHexCsvPath");
        for (const auto& field : {"encKeyType", "encKeyValue", "encForceKey", "groupCsvPath", "keyCsvPath",
                                  "dmrTgKeyCsvPath", "decryptionClearKeys"}) {
            sys.remove(field);
        }
        entry.remove("modulation");
        entry.remove("gainDb");
        list["entries"] = QVariantList{entry};
    }
    // Only the exact analog flags: the digital-only -fa (auto) and both together stay refused; a lowercase -fm is dPMR
    // (issue #890), a digital entry.
    for (const auto& flag : QStringList{"-fa", "-fA -fM", "-fa -fA"}) {
        sys["decodeFlag"] = flag;
        check(!build().ok);
    }
    sys["decodeFlag"] = "-fm";
    check(build().ok && build().csv.contains("dpmr-conventional,851500000,"));
    // A saved analog system with a channel map is told to remove it: no trunked type would take it either.
    sys["decodeFlag"] = "-fM";
    sys["trunking"] = false;
    sys["chanCsvPath"] = "/imports/chan.csv";
    check(!build().ok && build().error.contains("Example: Analog entries take no channel map; remove it"));
    check(!build().error.contains("trunked"));
    sys.remove("chanCsvPath");
    // Issue #890: a Signal search save keeps its measured width as its only Extra arguments. Exactly one width option of
    // the system's own analog kind, in whole Hz within the range the target's options parser takes, reaches the row's
    // options column (written as --name value, ahead of the squelch); anything else in Extra arguments stays refused.
    const QByteArray amRow = "am-conventional,851500000,,,,,,,,,,\"Example\",--am-bandwidth-hz 8333\n";
    for (const auto& extra :
         QStringList{"--am-bandwidth-hz 8333", "  --am-bandwidth-hz   8333 ", "--am-bandwidth-hz=8333"}) {
        sys["extraArgs"] = extra;
        check(build().ok && build().csv.endsWith(amRow));
    }
    sys["decodeFlag"] = "-fA";
    sys["extraArgs"] = "--nfm-bandwidth-hz 12500";
    sys["radioSettings"] = QVariantMap{{"squelch", "auto+12"}};
    check(build().ok
          && build().csv.endsWith("nfm-conventional,851500000,,,,,,,,,,\"Example\",--nfm-bandwidth-hz 12500 --squelch "
                                  "auto+12\n"));
    sys.remove("radioSettings");
    const QString refused = QStringLiteral("Example: This saved system has standalone Extra arguments.");
    const QList<QPair<QString, QString>> refusedExtras{{"-fA", "--am-bandwidth-hz 8333"},
                                                       {"-fM", "--nfm-bandwidth-hz 12500"},
                                                       {"-fA", "--nfm-bandwidth-hz 12500 -F"},
                                                       {"-fA", "-F --nfm-bandwidth-hz 12500"},
                                                       {"-fA", "--nfm-bandwidth-hz 12500 --nfm-bandwidth-hz 9000"},
                                                       {"-fA", "--nfm-bandwidth-hz"},
                                                       {"-fA", "--nfm-bandwidth-hz="},
                                                       {"-fA", "--nfm-bandwidth-hz 7999"},
                                                       {"-fA", "--nfm-bandwidth-hz 25001"},
                                                       {"-fA", "--nfm-bandwidth-hz 12500.5"},
                                                       {"-fA", "--nfm-bandwidth-hz '12500'"},
                                                       {"-fA", "--nfm-bandwidth-hz +12500"},
                                                       {"-fM", "--am-bandwidth-hz 4999"},
                                                       {"-fM", "--am-bandwidth-hz 20001"},
                                                       {"-ft", "--nfm-bandwidth-hz 12500"},
                                                       {"-fs", "--am-bandwidth-hz 8333"}};
    for (const auto& extra : refusedExtras) {
        sys["decodeFlag"] = extra.first;
        sys["extraArgs"] = extra.second;
        const auto refusedTargets = build();
        check(!refusedTargets.ok && refusedTargets.error.startsWith(refused));
        if (refusedTargets.ok) {
            std::fprintf(stderr, "accepted %s '%s'\n", qPrintable(extra.first), qPrintable(extra.second));
        }
    }
    sys.remove("extraArgs");
    sys["decodeFlag"] = "-fM";
    // The rate warning reads the same width: the option of the system's own kind, else none.
    check(scan_list_saved_system_width_hz("--am-bandwidth-hz 8333", DSD_ANALOG_DEMOD_AM) == 8333);
    check(scan_list_saved_system_width_hz("--nfm-bandwidth-hz=12500", DSD_ANALOG_DEMOD_FM) == 12500);
    check(scan_list_saved_system_width_hz("--nfm-bandwidth-hz 12500", DSD_ANALOG_DEMOD_AM) == 0);
    check(scan_list_saved_system_width_hz("--nfm-bandwidth-hz 12500 -F", DSD_ANALOG_DEMOD_FM) == 0);
    check(scan_list_saved_system_width_hz(QString(), DSD_ANALOG_DEMOD_FM) == 0);
    // The analog kind the starter and the editor share: the protocol of a frequency entry, the exact flag of a
    // saved system's.
    const QVariantMap systemEntry{{"kind", "system"}};
    check(scan_list_entry_analog_kind({{"kind", "freq"}, {"protocol", "nfm"}}, "-fs") == DSD_ANALOG_DEMOD_FM);
    check(scan_list_entry_analog_kind({{"kind", "freq"}, {"protocol", "am"}}, QString()) == DSD_ANALOG_DEMOD_AM);
    check(scan_list_entry_analog_kind({{"kind", "freq"}, {"protocol", "p25"}}, "-fA") == -1);
    check(scan_list_entry_analog_kind(systemEntry, " -fA ") == DSD_ANALOG_DEMOD_FM);
    check(scan_list_entry_analog_kind(systemEntry, "-fM") == DSD_ANALOG_DEMOD_AM);
    check(scan_list_entry_analog_kind(systemEntry, "-fA -fM") == -1);
    check(scan_list_entry_analog_kind(systemEntry, "-fs") == -1);
    // Frequency entries: nfm and am map to their conventional types beside digital ones.
    QVariantList mixed;
    const QStringList protocols{"p25", "nfm", "am", "dmr"};
    for (int i = 0; i < protocols.size(); ++i) {
        mixed << QVariantMap{
            {"uid", QStringLiteral("f%1").arg(i)},           {"kind", "freq"},       {"protocol", protocols[i]},
            {"freqMhz", QString::number(118.3 + i, 'f', 1)}, {"modulation", "c4fm"}, {"enabled", true}};
    }
    list["entries"] = mixed;
    const auto mixedTargets = build();
    check(mixedTargets.ok && mixedTargets.targetCount == 4);
    check(mixedTargets.csv.contains("f0,p25-conventional,118300000,,,,,,,,c4fm,,\"P25 · 118.3 MHz\",")
          && mixedTargets.csv.contains("f1,nfm-conventional,119300000,,,,,,,,,,\"NFM (analog) · 119.3 MHz\",")
          && mixedTargets.csv.contains("f2,am-conventional,120300000,,,,,,,,,,\"AM (analog) · 120.3 MHz\",")
          && mixedTargets.csv.contains("f3,dmr-conventional,121300000,,,,,,,,c4fm,,\"DMR · 121.3 MHz\","));
    // Issue #782: the `label` column names each target as the scan list does, never by its internal entry UID: an
    // entry by its name, else its protocol and frequency. A quote becomes an apostrophe (the parser does not unescape
    // doubled quotes) and a line break a space; a comma stays, inside the quoted cell.
    check(mixedTargets.csv.startsWith("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_hex_csv,"
                                      "keys_dec_csv,p25_bandplan_csv,modulation,rtl_gain,label,options\n"));
    QVariantMap named = mixed[0].toMap();
    named["name"] = "Tower \"A\",\nnorth";
    list["entries"] = QVariantList{named};
    check(build().ok && build().csv.contains(",c4fm,,\"Tower 'A', north\","));
    list["entries"] = QVariantList{entry};
    sys["trunking"] = true;
    sys["decodeFlag"] = "-mq";
    check(build().csv.contains("cqpsk"));
    for (const auto& flag : QStringList{"-mq -^", "-^ -mq", "  -mq   -^  ", "-ft -^", "-^ -ft"}) {
        sys["decodeFlag"] = flag;
        const auto imported = build();
        check(imported.ok && imported.csv.contains("p25-trunk") && imported.csv.contains("-^"));
        check(imported.csv.contains("cqpsk") == flag.contains("-mq"));
    }
    sys["decodeFlag"] = "-mq";
    sys["extraArgs"] = "-F";
    check(!build().ok);
    sys.remove("extraArgs");
    sys["freqMhz"] = "nan";
    check(!build().ok);
    sys["freqMhz"] = "851.5";
    sys["chanCsvPath"] = "/missing.csv";
    sys["trunking"] = false;
    check(!build().ok && build().error.contains("remove the channel map or use a trunked type"));
    sys.remove("chanCsvPath");
    sys["p25BandplanCsvPath"] = "/missing.csv";
    check(!build().ok);
    sys.remove("p25BandplanCsvPath");
    for (const auto& path : QStringList{"a,b", "a\"b", "a\nb", "a\rb"}) {
        sys["groupCsvPath"] = path;
        check(!build().ok);
    }
    sys.remove("groupCsvPath");
    // Basic Privacy and every force choice are DMR's (the engine's class masks, ScanProtocol::keySwitches).
    sys["decodeFlag"] = "-fs";
    sys["encKeyType"] = "basic";
    sys["encKeyValue"] = "7";
    check(build().ok && build().csv.contains("-b ") && !build().csv.contains("single_key"));
    sys["keyCsvPath"] = "/missing.csv";
    check(!build().ok);
    sys.remove("keyCsvPath");
    sys["encForceKey"] = 1;
    check(build().csv.contains("-4"));
    sys["encForceKey"] = 2;
    check(build().csv.contains("-0"));
    sys["encForceKey"] = 3;
    check(!build().ok);
    sys["encForceKey"] = 0;
    // A restored force key past int's range fails the 0..2 check rather than run as its low 32 bits (1 here), and a
    // profile force past it adds no option.
    sys["encForceKey"] = (qlonglong(1) << 32) + 1;
    check(!build().ok);
    sys["encForceKey"] = 0;
    sys["decryptionForce"] = (qlonglong(1) << 32) + 1;
    check(build().ok && !build().csv.contains("-4") && !build().csv.contains("--no-force-key"));
    sys.remove("decryptionForce");
    sys.remove("encKeyType");
    sys.remove("encKeyValue");
    // Every preserved column and every direct-key family, without printing values.
    sys["decodeFlag"] = "-ft";
    sys["trunking"] = true;
    sys["chanCsvPath"] = "/imports/channels.csv";
    sys["p25BandplanCsvPath"] = "/imports/bandplan.csv";
    sys["groupCsvPath"] = "/imports/group list.csv";
    sys["gainDb"] = 27;
    check(build().ok && build().csv.contains("/imports/channels.csv") && build().csv.contains("/imports/bandplan.csv")
          && build().csv.contains("-G \"/imports/group list.csv\"") && build().csv.contains(",27,"));
    // A P25 row takes the Hytera / AES and RC4 / DES keys; Basic Privacy (DMR's) and the scrambler (NXDN's and
    // dPMR's) are refused by name.
    const QStringList keyTypes{"basic", "hex", "rc4", "scrambler"};
    const QStringList keyValues{"7", "0000001F00", "0123456789", "17"};
    const QStringList keyFlags{"-b ", "-H ", "-1 ", "-R "};
    const QList<bool> p25Takes{false, false, true, false};
    for (int i = 0; i < keyTypes.size(); ++i) {
        sys["encKeyType"] = keyTypes[i];
        sys["encKeyValue"] = keyValues[i];
        check(build().ok == p25Takes[i] && (!p25Takes[i] || build().csv.contains(keyFlags[i].toUtf8())));
        check(p25Takes[i]
              || build().error.contains(i == 1 ? "P25 AES needs 32 or 64 hex digits." : "P25 entries take no "));
        sys["encKeyValue"] = "invalid";
        check(!build().ok);
    }
    // A 10-digit key (above) is DMR's Hytera key, never P25's: its AES keys are 32 or 64 hex digits, as the engine
    // reads them.
    for (const char* hex : {"0123456789ABCDEF0123456789ABCDEF", kHex64}) {
        sys["encKeyType"] = "hex";
        sys["encKeyValue"] = hex;
        check(build().ok && build().csv.contains(QByteArray("-H ") + hex));
    }
    sys.remove("encKeyType");
    sys.remove("encKeyValue");
    sys["keyCsvPath"] = "/imports/keys.csv";
    sys["keyCsvHex"] = true;
    check(build().csv.contains(",/imports/keys.csv,,/imports/bandplan.csv"));
    sys["keyCsvHex"] = false;
    check(build().csv.contains(",,/imports/keys.csv,/imports/bandplan.csv"));
    list["groupCsvPath"] = "/imports/global.csv";
    check(!build().csv.contains("/imports/global.csv"));
    sys["srcCsvPath"] = "/imports/system-src.csv";
    check(build().warnings.size() == 1 && !build().csv.contains("system-src"));
    list["srcCsvPath"] = sys.value("srcCsvPath");
    check(build().warnings.isEmpty());
    for (const auto& field : {"defaultDwellMs", "defaultHoldMs"}) {
        for (int value : {1, 249, 600001}) {
            list[field] = value;
            check(!build().ok);
        }
        list[field] = 250;
        check(build().ok);
        list[field] = 0;
    }
    for (const auto& field : {"dwellMs", "holdMs"}) {
        entry[field] = 249;
        list["entries"] = QVariantList{entry};
        check(!build().ok);
        entry[field] = 600000;
        list["entries"] = QVariantList{entry};
        check(build().ok);
        entry[field] = 0;
    }
    entry["gainDb"] = 42;
    entry["modulation"] = "c4fm";
    list["entries"] = QVariantList{entry};
    check(build().csv.contains(",c4fm,42,"));
    entry["gainDb"] = "invalid";
    list["entries"] = QVariantList{entry};
    check(!build().ok);
    entry["gainDb"] = 42;
    list["entries"] = QVariantList{entry};
    SessionArgPrefs prefs;
    prefs.extraArgs = "--show-keys";
    QString error;
    check(session_args_scan_build(list, "851.5", "/targets.csv", prefs, &error).isEmpty() && !error.isEmpty());
    prefs.extraArgs = "-F";
    prefs.hangtimeSec = 4.5;
    prefs.autoPpm = true;
    list["voiceOnly"] = true;
    list["skipSilent"] = true;
    list["defaultDwellMs"] = 250;
    list["defaultHoldMs"] = 1500;
    list["sourceType"] = "rtltcp";
    list["host"] = "localhost";
    list["port"] = 1234;
    list["gainDb"] = 38;
    list["ppm"] = "4";
    list["bandwidthKhz"] = 24;
    list["biasTee"] = 1;
    auto args = session_args_scan_build(list, "851.5", "/targets.csv", prefs, &error);
    check(args.last() == "-F");
    check(args.count("-t") == 1 && args.value(args.indexOf("-t") + 1) == "4.5");
    prefs.extraArgs = "-t 9 -F";
    const auto extraArgs = session_args_scan_build(list, "851.5", "/targets.csv", prefs, &error);
    check(error.isEmpty() && extraArgs.count("-t") == 2 && extraArgs.last() == "-F"
          && extraArgs.value(extraArgs.indexOf("-t") + 1) == "4.5"
          && extraArgs.value(extraArgs.lastIndexOf("-t") + 1) == "9");
    prefs.extraArgs = "-F";
    check(error.isEmpty() && args.contains("rtltcp:localhost:1234:851.5M:38:4:24:0:2:bias")
          && args.contains("--trunk-scan") && args.contains("--scan-voice-only") && args.contains("--scan-skip-silent")
          && args.contains("--src-csv") && args.contains("--trunk-scan-dwell-ms")
          && args.contains("--trunk-scan-activity-hold-ms") && args.contains("--enc-lockout")
          && args.contains("--auto-ppm"));
    test_skip_silent_length_extra(list);
    for (const auto& field : {"gainDb", "bandwidthKhz", "biasTee"}) {
        const auto prior = list.value(field);
        list[field] = "invalid";
        check(session_args_scan_build(list, "851.5", "/targets.csv", prefs, &error).isEmpty());
        list[field] = prior;
    }
    list["host"] = "host:injection";
    check(session_args_scan_build(list, "851.5", "/targets.csv", prefs, &error).isEmpty());
    list["host"] = "localhost";
    list["entries"] = QVariantList{entry, entry};
    check(!build().ok);
    entry["systemUid"] = "missing";
    list["entries"] = QVariantList{entry};
    check(!build().ok);
    entry["enabled"] = false;
    list["entries"] = QVariantList{entry};
    check(!build().ok);
    for (const auto& token : QStringList{"-Cfoo", "-Y", "-T", "-i", "--trunk-scan=x", "--p25-bandplan", "--iq-replay",
                                         "--scan-voice-only", "--no-scan-voice-only", "--scan-skip-silent",
                                         "--no-scan-skip-silent", "--show-keys", "-ft", "-mq"}) {
        check(!session_args_scan_extra_safe(token));
    }
    check(session_args_scan_extra_safe("--enc-lockout"));
    for (const auto& token :
         QStringList{"-FT", "-FY", "-Fiother-input", "-FTZ", "-FCT.csv", "-Ff1", "-Fmq", "-Fv2", "-FZ"}) {
        check(!session_args_scan_extra_safe(token));
        prefs.extraArgs = token;
        check(session_args_scan_build(list, "851.5", "/targets.csv", prefs, &error).isEmpty());
        check(error.contains("each short option separately"));
    }
    check(session_args_scan_extra_safe("-GFT.csv")); // G consumes the attached filename, including T.
    prefs.extraArgs = "-F -e -v2";
    args = session_args_scan_build(list, "851.5", "/targets.csv", prefs, &error);
    check(session_args_scan_extra_safe(prefs.extraArgs) && error.isEmpty() && args.contains("-F") && args.contains("-e")
          && args.last() == "-v2");
    // A saved system stored at 60 dB (the system editor took 50..99 before) loads as 49 dB, so its scan target runs at
    // 49 dB; main loaded 60 and refused the list ("Gain must be 0..49 dB"). A map handed straight in with 60 is still
    // refused: the range is held where the store is read, not here.
    check(json_store_save_array(
        QStringLiteral("saved_systems.json"),
        QJsonArray{QJsonObject{{"name", "High"}, {"decodeFlag", "-ft"}, {"freqMhz", "851.5"}, {"gainDb", 60}}}));
    const QVariantMap saved = SavedSystemsModel().get(0);
    check(saved.value("gainDb").toInt() == 49);
    const QVariantMap savedEntry{
        {"uid", "saved-entry"}, {"kind", "system"}, {"systemUid", saved.value("uid")}, {"enabled", true}};
    const QVariantMap savedList{{"uid", "saved-list"}, {"sourceType", "usb"}, {"entries", QVariantList{savedEntry}}};
    const auto savedTargets = scan_list_targets(savedList, QVariantList{saved});
    check(savedTargets.ok && savedTargets.csv.contains(",49,\"High\""));
    QVariantMap raw = saved;
    raw["gainDb"] = 60;
    const auto rawTargets = scan_list_targets(savedList, QVariantList{raw});
    check(!rawTargets.ok && rawTargets.error.contains("Gain must be 0..49 dB"));
    rowSquelch();
    test_protocol_catalog();
    test_simplex_digital_targets();
    test_nxdn_row_keys();
    test_key_switch_table_matches_engine();
    test_profile_mismatch_named_first();
    return failures ? 1 : 0;
}
