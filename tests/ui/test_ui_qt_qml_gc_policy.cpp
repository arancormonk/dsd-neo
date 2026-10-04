// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The QML collector policy every process that builds a QML engine applies first.
 *
 * Qt's incremental collector can free the function storage of a QML object while the object is still
 * being created. A `Connections` element with a `function onX()` handler then crashed the QML suites
 * under load (QQmlConnections::connectSignalsToMethods). The policy runs each collection to
 * completion instead, unless the environment already holds a time limit Qt would honour.
 */

#include <QByteArray>
#include <QByteArrayView>
#include <cstdio>
#include <dsd-neo/core/safe_api.h>
#include <initializer_list>
#include <qtenvironmentvariables.h>
#include "qml_gc_policy.h"

static int failures;

static void
check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        DSD_FPRINTF(stderr, "qml gc policy: %s\n", what);
    }
}

static QByteArray
time_limit() {
    return qgetenv("QV4_GC_TIMELIMIT");
}

int
main() {
    qunsetenv("QV4_GC_TIMELIMIT");
    check(dsd_qt::applyQmlGcPolicy(), "an unset limit is not set by the policy");
    check(time_limit() == "0", "an unset limit does not become 0");

    // A second call (another engine in the same process) changes nothing.
    check(!dsd_qt::applyQmlGcPolicy(), "a limit of 0 is set again");
    check(time_limit() == "0", "a limit of 0 does not stay 0");

    // Someone chose a time limit on purpose: leave it, even one that brings the collector's
    // time slices back. The policy reads the variable with Qt's own parser (C-style base prefixes),
    // so every spelling Qt honours is kept as written.
    for (const char* explicit_limit : {"7", "0x10", "010", "-1"}) {
        qputenv("QV4_GC_TIMELIMIT", explicit_limit);
        check(!dsd_qt::applyQmlGcPolicy(), "an explicit limit is replaced");
        check(time_limit() == explicit_limit, "an explicit limit does not survive");
    }

    // Qt reads a value it cannot parse as an int as unset and slices at its default, so the policy
    // treats it as unset too: text, a unit, an octal prefix with an 8, a value past int.
    for (const char* unusable : {"", "fast", "5ms", "08", "99999999999"}) {
        qputenv("QV4_GC_TIMELIMIT", unusable);
        check(dsd_qt::applyQmlGcPolicy(), "a limit Qt ignores is not replaced");
        check(time_limit() == "0", "a limit Qt ignores does not become 0");
    }

    if (failures != 0) {
        DSD_FPRINTF(stderr, "%d qml gc policy check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
