// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include "scan_list_targets.h"
#include "session_args.h"
using namespace dsd_qt;
static int failures;

static void
check(bool ok) {
    if (!ok) {
        ++failures;
        std::fputs("scan-list assertion failed\n", stderr);
    }
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QVariantMap entry{{"uid", "entry"}, {"kind", "system"}, {"systemUid", "sys"}, {"enabled", true}};
    QVariantMap list{{"uid", "list"}, {"sourceType", "usb"}, {"entries", QVariantList{entry}}};
    QVariantMap sys{{"uid", "sys"}, {"name", "Example"}, {"decodeFlag", "-ft"}, {"freqMhz", "851.5"}};
    auto build = [&]() { return scan_list_targets(list, QVariantList{sys}); };
    auto result = build();
    check(result.ok && result.targetCount == 1 && result.csv.contains("p25-conventional,851500000"));
    for (const auto& flag : QStringList{"", "-Y", "-fd", "-fy", "-fm", "-fe"}) {
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
    sys["decodeFlag"] = "-mq";
    check(build().csv.contains("cqpsk"));
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
    // Every preserved column and every direct-key family, without printing values.
    sys["decodeFlag"] = "-ft";
    sys["trunking"] = true;
    sys["chanCsvPath"] = "/imports/channels.csv";
    sys["p25BandplanCsvPath"] = "/imports/bandplan.csv";
    sys["groupCsvPath"] = "/imports/group list.csv";
    sys["gainDb"] = 27;
    check(build().ok && build().csv.contains("/imports/channels.csv") && build().csv.contains("/imports/bandplan.csv")
          && build().csv.contains("-G \"/imports/group list.csv\"") && build().csv.contains(",27,"));
    const QStringList keyTypes{"basic", "hex", "rc4", "scrambler"};
    const QStringList keyValues{"7", "0000001F00", "0123456789", "17"};
    const QStringList keyFlags{"-b ", "-H ", "-1 ", "-R "};
    for (int i = 0; i < keyTypes.size(); ++i) {
        sys["encKeyType"] = keyTypes[i];
        sys["encKeyValue"] = keyValues[i];
        check(build().ok && build().csv.contains(keyFlags[i].toUtf8()));
        sys["encKeyValue"] = "invalid";
        check(!build().ok);
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
    prefs.autoPpm = true;
    list["voiceOnly"] = true;
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
    check(error.isEmpty() && args.contains("rtltcp:localhost:1234:851.5M:38:4:24:0:2:bias")
          && args.contains("--trunk-scan") && args.contains("--scan-voice-only") && args.contains("--src-csv")
          && args.contains("--trunk-scan-dwell-ms") && args.contains("--trunk-scan-activity-hold-ms")
          && args.contains("--enc-lockout") && args.contains("--auto-ppm"));
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
                                         "--scan-voice-only", "--no-scan-voice-only", "--show-keys", "-ft", "-mq"}) {
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
    return failures ? 1 : 0;
}
