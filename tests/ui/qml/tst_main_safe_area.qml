// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// From Android 15 the app window is edge-to-edge: the status and navigation
// bars are transparent overlays over the window, and keeping content out from
// under them is the app's job. The insets reach QML as SafeArea margins, and
// every full-window layer in Main.qml is expected to stay inside them.
//
// Offscreen there are no system bars, so the platform margins are zero and a
// UI that ignored them would pass by accident. SafeArea's writable
// additionalMargins feed the same sum the platform insets do, which makes the
// enforcement testable: raise the margins, and the layers must move.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: appLoader

        anchors.fill: parent
        source: uiDir + "/Main.qml"
    }

    TestCase {
        id: tc

        name: "MainSafeArea"
        when: windowShown

        property var app: null

        function initTestCase() {
            tc.app = appLoader.item
            verify(tc.app !== null, "Main.qml failed to load")
        }

        function test_the_ui_stays_inside_the_safe_area() {
            tc.app.SafeArea.additionalMargins.top = 48
            tc.app.SafeArea.additionalMargins.left = 12
            tc.app.SafeArea.additionalMargins.right = 16
            tc.app.SafeArea.additionalMargins.bottom = 32

            // The window's margins are the sum of the platform insets (zero
            // offscreen) and the additional margins; if this read-back fails,
            // the simulated insets never took, not the layout under test.
            tryVerify(function () { return tc.app.SafeArea.margins.top === 48 },
                      2000, "additional safe-area margins did not register")

            // The container every UI layer anchors to.
            var safe = findChild(tc.app, "safeArea")
            verify(safe !== null, "Main.qml has no safe-area container")
            tryCompare(safe, "y", 48)
            compare(safe.x, 12)
            compare(safe.width, tc.app.width - 12 - 16)
            compare(safe.height, tc.app.height - 48 - 32)

            // And one full-window layer as a user-visible sample of the rest.
            var monitor = findChild(tc.app, "monitorScreen")
            verify(monitor !== null, "the monitor screen is missing")
            tryCompare(monitor, "y", 48)
            compare(monitor.height, tc.app.height - 48 - 32)
        }
    }
}
