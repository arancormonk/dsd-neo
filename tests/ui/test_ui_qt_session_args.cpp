// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the session-args builder — the ':'-delimited rtl input specs and
 * flag set a saved system starts with. These strings silently mistune a session
 * when malformed, which is why they are assembled in C++ under test rather than
 * in QML-side JavaScript. The bias-tee tri-state matrix is the load-bearing
 * case: a per-system explicit Off must survive an app-wide On. */

#include <QStringList>
#include <QVariantMap>

#include "dsd-neo/core/safe_api.h"
#include "session_args.h"

using dsd_qt::session_args_build;
using dsd_qt::session_args_freq_valid;
using dsd_qt::SessionArgPrefs;
using dsd_qt::SessionArgsError;

namespace {

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

QVariantMap
usb_system(void) {
    QVariantMap sys;
    sys.insert(QStringLiteral("sourceType"), QStringLiteral("usb"));
    sys.insert(QStringLiteral("freqMhz"), QStringLiteral("851.375"));
    sys.insert(QStringLiteral("trunking"), false);
    return sys;
}

QString
input_spec(const QStringList& args) {
    const qsizetype i = args.indexOf(QStringLiteral("-i"));
    return (i >= 0 && i + 1 < args.size()) ? args.at(i + 1) : QString();
}

void
test_freq_validation(void) {
    expect("plain MHz parses", session_args_freq_valid(QStringLiteral("851.375")));
    expect("surrounding whitespace is tolerated", session_args_freq_valid(QStringLiteral(" 851.375 ")));
    expect("trailing junk is refused", !session_args_freq_valid(QStringLiteral("851.375M")));
    expect("zero is refused", !session_args_freq_valid(QStringLiteral("0")));
    expect("negative is refused", !session_args_freq_valid(QStringLiteral("-851")));
    expect("empty is refused", !session_args_freq_valid(QString()));

    SessionArgsError error = SessionArgsError::None;
    QVariantMap sys = usb_system();
    sys.insert(QStringLiteral("freqMhz"), QStringLiteral("garbage"));
    expect("bad frequency refuses the build",
           session_args_build(sys, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::Frequency);
}

void
test_defaults_and_overrides(void) {
    SessionArgsError error = SessionArgsError::None;
    const QStringList defaults = session_args_build(usb_system(), SessionArgPrefs(), &error);
    expect("default build succeeds", error == SessionArgsError::None);
    expect("defaults fill the rtl spec", input_spec(defaults) == QStringLiteral("rtl:0:851.375M:30:0:48:0:2"));
    expect("frontend none is requested",
           defaults.mid(0, 2) == QStringList{QStringLiteral("--frontend"), QStringLiteral("none")});
    expect("pulse output is requested", defaults.contains(QStringLiteral("-o")));
    expect("skip-encrypted default adds the lockout", defaults.contains(QStringLiteral("--enc-lockout")));
    expect("auto-ppm stays off by default", !defaults.contains(QStringLiteral("--auto-ppm")));
    expect("no trunking flag without trunking", !defaults.contains(QStringLiteral("-T")));

    QVariantMap sys = usb_system();
    sys.insert(QStringLiteral("gainDb"), 36);
    sys.insert(QStringLiteral("ppm"), QStringLiteral("-2"));
    sys.insert(QStringLiteral("bandwidthKhz"), 12);
    sys.insert(QStringLiteral("trunking"), true);
    sys.insert(QStringLiteral("decodeFlag"), QStringLiteral("-f1 -mq"));
    sys.insert(QStringLiteral("extraArgs"), QStringLiteral("-C chan.csv"));
    SessionArgPrefs prefs;
    prefs.autoPpm = true;
    prefs.extraArgs = QStringLiteral("--wav-dir /tmp");
    const QStringList args = session_args_build(sys, prefs, &error);
    expect("override build succeeds", error == SessionArgsError::None);
    expect("overrides land in the rtl spec", input_spec(args) == QStringLiteral("rtl:0:851.375M:36:-2:12:0:2"));
    expect("multi-flag decode chip splits",
           args.contains(QStringLiteral("-f1")) && args.contains(QStringLiteral("-mq")));
    expect("trunking adds -T", args.contains(QStringLiteral("-T")));
    expect("auto-ppm pref adds the flag", args.contains(QStringLiteral("--auto-ppm")));
    expect("system and app extra args both land",
           args.contains(QStringLiteral("chan.csv")) && args.contains(QStringLiteral("--wav-dir")));
}

void
test_ppm_shapes(void) {
    SessionArgsError error = SessionArgsError::None;
    QVariantMap sys = usb_system();
    sys.insert(QStringLiteral("ppm"), QStringLiteral("+5"));
    const QStringList plus = session_args_build(sys, SessionArgPrefs(), &error);
    expect("explicit plus sign is normalized",
           error == SessionArgsError::None && input_spec(plus) == QStringLiteral("rtl:0:851.375M:30:5:48:0:2"));

    sys.insert(QStringLiteral("ppm"), QStringLiteral("abc"));
    expect("junk ppm refuses the build",
           session_args_build(sys, SessionArgPrefs(), &error).isEmpty() && error == SessionArgsError::Ppm);
}

void
test_bias_tee_tristate(void) {
    SessionArgsError error = SessionArgsError::None;
    SessionArgPrefs prefOn;
    prefOn.biasTee = true;
    SessionArgPrefs prefOff;

    QVariantMap sys = usb_system();
    // Follow (-1): the app-wide pref decides.
    sys.insert(QStringLiteral("biasTee"), -1);
    expect("follow + pref on powers the tee",
           input_spec(session_args_build(sys, prefOn, &error)).endsWith(QStringLiteral(":bias")));
    expect("follow + pref off leaves the tee off",
           !input_spec(session_args_build(sys, prefOff, &error)).endsWith(QStringLiteral(":bias")));

    // Explicit off (0) must survive a global on — the hardware-protection case.
    sys.insert(QStringLiteral("biasTee"), 0);
    expect("explicit off beats the app-wide on",
           !input_spec(session_args_build(sys, prefOn, &error)).endsWith(QStringLiteral(":bias")));

    // Explicit on (1) needs no global blessing.
    sys.insert(QStringLiteral("biasTee"), 1);
    expect("explicit on works with the pref off",
           input_spec(session_args_build(sys, prefOff, &error)).endsWith(QStringLiteral(":bias")));

    // Legacy bool stores: true was an explicit choice, false the untouched
    // default whose observed behavior followed the pref.
    sys.insert(QStringLiteral("biasTee"), true);
    expect("legacy true powers the tee",
           input_spec(session_args_build(sys, prefOff, &error)).endsWith(QStringLiteral(":bias")));
    sys.insert(QStringLiteral("biasTee"), false);
    expect("legacy false keeps following the pref",
           input_spec(session_args_build(sys, prefOn, &error)).endsWith(QStringLiteral(":bias")));
}

void
test_network_and_file_sources(void) {
    SessionArgsError error = SessionArgsError::None;

    QVariantMap rtltcp = usb_system();
    rtltcp.insert(QStringLiteral("sourceType"), QStringLiteral("rtltcp"));
    rtltcp.insert(QStringLiteral("host"), QStringLiteral("192.168.1.10"));
    rtltcp.insert(QStringLiteral("port"), 1234);
    rtltcp.insert(QStringLiteral("biasTee"), 1);
    expect("rtltcp spec carries host, port, tail and bias",
           input_spec(session_args_build(rtltcp, SessionArgPrefs(), &error))
               == QStringLiteral("rtltcp:192.168.1.10:1234:851.375M:30:0:48:0:2:bias"));

    QVariantMap udp;
    udp.insert(QStringLiteral("sourceType"), QStringLiteral("udp"));
    udp.insert(QStringLiteral("port"), 7355);
    expect("udp spec binds all interfaces",
           input_spec(session_args_build(udp, SessionArgPrefs(), &error)) == QStringLiteral("udp:0.0.0.0:7355"));
    expect("udp needs no frequency", error == SessionArgsError::None);

    QVariantMap tcp;
    tcp.insert(QStringLiteral("sourceType"), QStringLiteral("tcp"));
    tcp.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    tcp.insert(QStringLiteral("port"), 7355);
    expect("tcp spec carries host and port",
           input_spec(session_args_build(tcp, SessionArgPrefs(), &error)) == QStringLiteral("tcp:127.0.0.1:7355"));

    QVariantMap file;
    file.insert(QStringLiteral("sourceType"), QStringLiteral("file"));
    file.insert(QStringLiteral("filePath"), QStringLiteral("/sdcard/capture.wav"));
    expect("file source passes the path through",
           input_spec(session_args_build(file, SessionArgPrefs(), &error)) == QStringLiteral("/sdcard/capture.wav"));
}

} // namespace

int
main(void) {
    test_freq_validation();
    test_defaults_and_overrides();
    test_ppm_shapes();
    test_bias_tee_tristate();
    test_network_and_file_sources();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
