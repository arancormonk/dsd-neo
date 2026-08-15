// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The engine-facing context properties are plain maps (see qml_test_context.h),
// and QML answers a read of a key a map does not carry with `undefined` — no
// warning, no error. So a screen that grew a binding on a reading the fixture
// lacks would pass every case in this suite and render `undefined` or NaN on the
// phone. This is the case that makes that loud.
TestCase {
    name: "QmlContextFixtureIsComplete"

    function test_01_the_fixture_carries_every_reading_the_screens_read() {
        // The screens the other cases load, plus Theme.qml: the screens pull it
        // in as a singleton and it reads prefs itself.
        var missing = testContext.missingContextKeys(
            ["HistoryScreen.qml", "MonitorScreen.qml", "SpectrumScreen.qml", "ExploreSetupScreen.qml", "RadioSheet.qml",
             "ImportsScreen.qml", "WizardScreen.qml", "HomeScreen.qml", "SettingsScreen.qml", "Main.qml",
             "Theme.qml"])

        compare(missing.length, 0,
                "read by the screens, missing from the fixture: " + missing.join(", "))
    }
}
