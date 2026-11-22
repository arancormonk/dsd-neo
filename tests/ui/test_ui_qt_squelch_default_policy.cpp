// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #899: who owns a session's squelch, and what a live decode switch asks the decoder to do with it. The app's
 * default comes in on a switch to NFM or AM and goes out on a switch back to a digital mode only while the squelch is
 * the app's: a user edit, a saved or Advanced squelch, and a session whose rows or ranges own the squelch are never
 * touched. The decoder's own checks (the switch taken, the configured squelch) are UI_QT_SESSION_ARGS_PARSE's.
 */

#include <QCoreApplication>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <cstdio>
#include <dsd-neo/core/safe_api.h>
#include <initializer_list>

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
test_app_owns_it() {
    dsd_qt::SquelchDefaultPolicy policy;
    policy.sessionStarted(QStringLiteral("noise+10"), false);
    expect("the default the session was built with", policy.defaultSetting() == QStringLiteral("noise+10"));
    // The decoder decides whether each applies (the switch taken, the configured squelch the app expects).
    expect("a switch to NFM or AM asks for the default over an off squelch",
           policy.decodeModeChanged(true) == Action::DefaultIfOff);
    expect("a switch to a digital mode asks for off over the default",
           policy.decodeModeChanged(false) == Action::OffIfDefault);
    policy.sessionStarted(QStringLiteral("auto+15"), false);
    expect("the default of the latest start", policy.defaultSetting() == QStringLiteral("auto+15")
                                                  && policy.decodeModeChanged(true) == Action::DefaultIfOff);
}

void
test_user_owns_it() {
    dsd_qt::SquelchDefaultPolicy policy;
    policy.sessionStarted(QStringLiteral("noise+10"), false);
    policy.userChangedSquelch();
    expect("after a user edit a switch to P25 asks for nothing", policy.decodeModeChanged(false) == Action::None);
    expect("nor one back to NFM", policy.decodeModeChanged(true) == Action::None);
    expect("and there is no default to send", policy.defaultSetting().isEmpty());

    policy.sessionStarted(QStringLiteral("noise+10"), false);
    expect("a new session is the app's again", policy.decodeModeChanged(true) == Action::DefaultIfOff);
    // The QML path: the Radio and Squelch sheets call it by name.
    expect("the sheets' call is invokable", QMetaObject::invokeMethod(&policy, "noteUserSquelchChange"));
    expect("and owns it for the user", policy.decodeModeChanged(false) == Action::None);
}

void
test_session_owns_it() {
    dsd_qt::SquelchDefaultPolicy policy;
    policy.sessionStarted(QStringLiteral("noise+10"), true);
    expect("a saved or Advanced squelch: NFM asks for nothing", policy.decodeModeChanged(true) == Action::None);
    expect("nor P25", policy.decodeModeChanged(false) == Action::None);
    expect("and there is no default to send", policy.defaultSetting().isEmpty());

    policy.sessionStartedWithoutDefault();
    expect("a scan list, a search or an audio source: NFM asks for nothing",
           policy.decodeModeChanged(true) == Action::None);
    expect("nor P25", policy.decodeModeChanged(false) == Action::None);

    for (const QString& nothing :
         {QStringLiteral("off"), QString(), QStringLiteral("-60"), QStringLiteral("noise+99")}) {
        policy.sessionStarted(nothing, false);
        expect("a default that is off, or none the app takes, is never sent",
               policy.decodeModeChanged(true) == Action::None && policy.decodeModeChanged(false) == Action::None);
    }
}

/* Issue #899 (I3): the owner travels with the accepted start, so a UI recreated over a running session takes it back;
   anything it cannot read as the app's or the user's is the session's, and nothing is ever sent for it. */
void
test_record_and_adopt() {
    dsd_qt::SquelchDefaultPolicy policy;
    int changes = 0;
    QObject::connect(&policy, &dsd_qt::SquelchDefaultPolicy::changed, &policy, [&changes]() { ++changes; });
    expect("no session: no record", policy.record().isEmpty());
    policy.sessionStarted(QStringLiteral("noise+10"), false);
    expect("an app-owned start records its default", policy.record() == QStringLiteral("app:noise+10") && changes == 1);
    policy.sessionStarted(QStringLiteral("noise+10"), false);
    expect("the same record again does not notify", changes == 1);
    policy.userChangedSquelch();
    expect("a user edit records the user", policy.record() == QStringLiteral("user") && changes == 2);
    policy.sessionStartedWithoutDefault();
    expect("a session's own squelch records the session", policy.record() == QStringLiteral("session") && changes == 3);

    for (const QString& record : {QStringLiteral("app:noise+10"), QStringLiteral("app:auto+15")}) {
        dsd_qt::SquelchDefaultPolicy fresh;
        fresh.adopt(record);
        expect("a fresh UI adopting the app's record follows decode switches",
               fresh.record() == record && fresh.decodeModeChanged(true) == Action::DefaultIfOff
                   && fresh.decodeModeChanged(false) == Action::OffIfDefault
                   && fresh.defaultSetting() == record.mid(4));
    }
    {
        dsd_qt::SquelchDefaultPolicy fresh;
        QMetaObject::invokeMethod(&fresh, "adopt", Q_ARG(QString, QStringLiteral("user")));
        expect("the user's record is the user's (QML's call)",
               fresh.record() == QStringLiteral("user") && fresh.decodeModeChanged(true) == Action::None);
    }
    for (const QString& record : {QString(), QStringLiteral("session"), QStringLiteral("app:off"),
                                  QStringLiteral("app:-60"), QStringLiteral("app:noise+99"), QStringLiteral("app:"),
                                  QStringLiteral("garbage"), QStringLiteral("APP:noise+10")}) {
        dsd_qt::SquelchDefaultPolicy fresh;
        fresh.adopt(record);
        expect("no record, or one that is not the app's or the user's, adopts as the session's",
               fresh.record() == QStringLiteral("session") && fresh.decodeModeChanged(true) == Action::None
                   && fresh.decodeModeChanged(false) == Action::None);
    }
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_fresh_policy_sends_nothing();
    test_app_owns_it();
    test_user_owns_it();
    test_session_owns_it();
    test_record_and_adopt();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
