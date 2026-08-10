// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The layers over a running session — the wizard, and the spectrum — cover the
// monitor completely, and both carry a full-width button along the bottom. So
// does the monitor: "Stop listening" sits at the same rect underneath them.
//
// TapHandlers never take exclusive grabs, so covering the monitor is not enough
// to stop a tap reaching it; only `enabled` does that. Without it one tap on the
// layer above runs both handlers, and the button the user actually pressed
// finishes by ending the session. That shipped: "Explore from here" — the one
// way out of a view-only spectrum — stopped the session instead of offering to
// hand the tuner over.
//
// This asserts the guard, not the geometry: the rects are free to move apart,
// but nothing underneath an opaque layer should be taking taps regardless.
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

        name: "MainOverlayLayering"
        when: windowShown

        property var app: null
        property var monitor: null

        function initTestCase() {
            tc.app = appLoader.item
            verify(tc.app !== null, "Main.qml failed to load")
            tc.monitor = findChild(tc.app, "monitorScreen")
            verify(tc.monitor !== null, "the monitor screen is missing")
        }

        function init() {
            tc.app.spectrumOpen = false
            tc.app.wizardOpen = false
        }

        // The baseline the other cases are measured against: with a live session
        // and nothing over it, the monitor is the screen and takes its own taps.
        function test_01_the_monitor_takes_taps_when_it_is_the_top_layer() {
            tryVerify(function () { return tc.monitor.enabled },
                      2000, "the monitor was inert with nothing covering it")
        }

        function test_02_the_spectrum_stops_taps_reaching_the_monitor() {
            tc.app.spectrumOpen = true
            tryVerify(function () { return !tc.monitor.enabled },
                      2000, "a tap on the spectrum also reaches the monitor underneath")

            // And closing it hands the screen back, or the monitor would be dead
            // to touch for the rest of the session.
            tc.app.spectrumOpen = false
            tryVerify(function () { return tc.monitor.enabled },
                      2000, "the monitor stayed inert after the spectrum closed")
        }

        function test_03_the_wizard_stops_taps_reaching_the_monitor() {
            tc.app.wizardOpen = true
            tryVerify(function () { return !tc.monitor.enabled },
                      2000, "a tap on the wizard also reaches the monitor underneath")

            tc.app.wizardOpen = false
            tryVerify(function () { return tc.monitor.enabled },
                      2000, "the monitor stayed inert after the wizard closed")
        }

        // The wizard opens over the spectrum ("Save as a system"), so both are up
        // at once. Whichever closes first must not re-arm the monitor while the
        // other is still covering it.
        function test_04_closing_one_layer_leaves_the_other_still_covering() {
            tc.app.spectrumOpen = true
            tc.app.wizardOpen = true
            tryVerify(function () { return !tc.monitor.enabled }, 2000)

            tc.app.wizardOpen = false
            tryVerify(function () { return !tc.monitor.enabled },
                      2000, "closing the wizard re-armed the monitor under the spectrum")

            tc.app.spectrumOpen = false
            tryVerify(function () { return tc.monitor.enabled }, 2000)
        }
    }
}
