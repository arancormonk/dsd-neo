// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #899: who owns a session's squelch, and what a live decode switch asks the app to send. The app's default comes
 * in on a switch to NFM or AM and goes out on a switch back to a digital mode only while the squelch is the app's: a
 * user edit, a saved or Advanced squelch, and a session whose rows or ranges own the squelch are never touched.
 */

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <cstdio>
#include <dsd-neo/core/safe_api.h>

#include "squelch_default_policy.h"

namespace {

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

using Action = dsd_qt::SquelchDefaultPolicy::Action;

void
test_fresh_policy_sends_nothing() {
    dsd_qt::SquelchDefaultPolicy policy;
    expect("no session: nothing to send on a switch to NFM", policy.decodeModeChanged(true) == Action::None);
    expect("nor on one to P25", policy.decodeModeChanged(false) == Action::None);
    expect("and no default", policy.defaultSetting().isEmpty());
    policy.userChangedSquelch();
    expect("a user edit with no session changes nothing", policy.decodeModeChanged(true) == Action::None);
}

void
test_digital_start() {
    dsd_qt::SquelchDefaultPolicy policy;
    policy.sessionStarted(QStringLiteral("noise+10"), false, false);
    expect("the default the session was built with", policy.defaultSetting() == QStringLiteral("noise+10"));
    expect("a digital switch first sends nothing", policy.decodeModeChanged(false) == Action::None);
    expect("a switch to NFM brings the default in", policy.decodeModeChanged(true) == Action::PushDefault);
    expect("NFM to AM keeps it, sending nothing", policy.decodeModeChanged(true) == Action::None);
    expect("a switch to P25 takes it out", policy.decodeModeChanged(false) == Action::PushOff);
    expect("P25 to DMR sends nothing", policy.decodeModeChanged(false) == Action::None);
    expect("NFM again brings it in again", policy.decodeModeChanged(true) == Action::PushDefault);
}

void
test_analog_start() {
    dsd_qt::SquelchDefaultPolicy policy;
    policy.sessionStarted(QStringLiteral("auto+15"), true, false);
    expect("the default is in force from the spec: NFM sends nothing", policy.decodeModeChanged(true) == Action::None);
    expect("a switch to P25 takes it out", policy.decodeModeChanged(false) == Action::PushOff);
    expect("back to AM brings it in", policy.decodeModeChanged(true) == Action::PushDefault
                                          && policy.defaultSetting() == QStringLiteral("auto+15"));
}

void
test_user_owns_it() {
    dsd_qt::SquelchDefaultPolicy policy;
    policy.sessionStarted(QStringLiteral("noise+10"), false, false);
    expect("the default comes in", policy.decodeModeChanged(true) == Action::PushDefault);
    policy.userChangedSquelch();
    expect("after a user edit a switch to P25 sends nothing", policy.decodeModeChanged(false) == Action::None);
    expect("nor one back to NFM", policy.decodeModeChanged(true) == Action::None);

    policy.sessionStarted(QStringLiteral("noise+10"), false, false);
    expect("a new session is the app's again", policy.decodeModeChanged(true) == Action::PushDefault);
    // The QML path: the Radio and Squelch sheets call it by name.
    expect("the sheets' call is invokable", QMetaObject::invokeMethod(&policy, "noteUserSquelchChange"));
    expect("and owns it for the user", policy.decodeModeChanged(false) == Action::None);

    policy.sessionStarted(QStringLiteral("noise+10"), false, false);
    policy.noteUserSquelchChange();
    expect("an edit before any switch keeps every switch off it", policy.decodeModeChanged(true) == Action::None);
}

void
test_session_owns_it() {
    dsd_qt::SquelchDefaultPolicy policy;
    policy.sessionStarted(QStringLiteral("noise+10"), false, true);
    expect("a saved or Advanced squelch: NFM sends nothing", policy.decodeModeChanged(true) == Action::None);
    expect("nor P25", policy.decodeModeChanged(false) == Action::None);
    expect("and there is no default to send", policy.defaultSetting().isEmpty());

    policy.sessionStartedWithoutDefault();
    expect("a scan list, a search or an audio source: NFM sends nothing",
           policy.decodeModeChanged(true) == Action::None);
    expect("nor P25", policy.decodeModeChanged(false) == Action::None);

    for (const QString& nothing :
         {QStringLiteral("off"), QString(), QStringLiteral("-60"), QStringLiteral("noise+99")}) {
        policy.sessionStarted(nothing, false, false);
        expect("a default that is off, or none the app takes, is never sent",
               policy.decodeModeChanged(true) == Action::None && policy.decodeModeChanged(false) == Action::None);
    }
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_fresh_policy_sends_nothing();
    test_digital_start();
    test_analog_start();
    test_user_owns_it();
    test_session_owns_it();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
