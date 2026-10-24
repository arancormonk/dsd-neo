// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

// Settings → Decoding → Relaxed CRC: an app preference every app-started session gets (-F), applied live to a running
// session through the relaxed-CRC command, whose own snapshot stays the switch's truth until it confirms.
Item {
    width: 420
    height: 900
    Loader { id: screenLoader; anchors.fill: parent }

    TestCase {
        name: "SettingsRelaxedCrc"
        when: windowShown

        function openScreen() {
            screenLoader.source = uiDir + "/SettingsScreen.qml";
            verify(screenLoader.item !== null);
            return findChild(screenLoader.item, "relaxedCrcToggle");
        }

        function note() {
            return findChild(screenLoader.item, "relaxedCrcChannelNote");
        }

        function init() {
            testContext.useLifecycleHost(true);
            testContext.resetCommands();
            prefs.relaxedCrc = false;
            testContext.setMetric("optionsKnown", true);
            testContext.setMetric("relaxedCrcConfigured", false);
            testContext.setMetric("relaxedCrcEffective", false);
            testContext.setMetric("relaxedCrcRowOverride", false);
        }

        function cleanup() {
            screenLoader.source = "";
            prefs.relaxedCrc = false;
            testContext.useLifecycleHost(false);
            testContext.resetCommands();
            testContext.setMetric("optionsKnown", false);
            testContext.setMetric("relaxedCrcConfigured", false);
            testContext.setMetric("relaxedCrcEffective", false);
            testContext.setMetric("relaxedCrcRowOverride", false);
        }

        function test_copy() {
            var toggle = openScreen();
            verify(toggle !== null, "the Decoding panel has a Relaxed CRC row");
            compare(toggle.title, "Relaxed CRC");
            compare(toggle.subtitle, "Accepts control and data whose CRC check failed, and marks them. Needed for DMR RAS (Restricted Access to System) systems. Applies now and from the next start; a CRC option in Extra decoder arguments wins.");
            verify(!note().visible);
        }

        function test_stopped_preference_persists() {
            var toggle = openScreen();
            compare(toggle.checked, false);
            toggle.activate();
            compare(prefs.relaxedCrc, true);
            compare(toggle.checked, true);
            compare(commands.relaxedCrcRequests().length, 0);
            screenLoader.source = "";
            toggle = openScreen();
            compare(toggle.checked, true);
            toggle.activate();
            compare(prefs.relaxedCrc, false);
        }

        function test_running_session_applies_live() {
            testContext.setLifecyclePhase(2);
            var toggle = openScreen();
            toggle.activate();
            compare(commands.relaxedCrcRequests(), [true]);
            compare(prefs.relaxedCrc, true);
            compare(toggle.checked, false); // the session's snapshot stays the truth until it confirms
            compare(toggle.enabled, false);
            testContext.setMetric("relaxedCrcConfigured", true);
            tryCompare(toggle, "checked", true);
            tryCompare(toggle, "enabled", true);
            // The running session decides what the row shows, not the stored preference.
            prefs.relaxedCrc = false;
            compare(toggle.checked, true);
            toggle.activate();
            compare(commands.relaxedCrcRequests(), [true, false]);
        }

        function test_refused_change_keeps_preference() {
            testContext.setLifecyclePhase(2);
            var toggle = openScreen();
            commands.setRelaxedCrcAccepted(false);
            toggle.activate();
            compare(prefs.relaxedCrc, false);
            compare(toggle.checked, false);
            compare(findChild(screenLoader.item, "relaxedCrcError").text,
                    "Could not change the running decoder's setting. Try again.");
            testContext.setLifecyclePhase(3);
            compare(toggle.enabled, false);
            compare(findChild(screenLoader.item, "relaxedCrcError").visible, false); // the session it was about is gone
            toggle.activate();
            compare(commands.relaxedCrcRequests().length, 1);
        }

        function test_scan_row_override_shows_this_channel() {
            testContext.setLifecyclePhase(2);
            testContext.setMetric("relaxedCrcConfigured", true);
            testContext.setMetric("relaxedCrcRowOverride", true);
            testContext.setMetric("relaxedCrcEffective", false);
            var toggle = openScreen();
            compare(toggle.checked, true);
            verify(note().visible);
            compare(note().text, "This channel: Off (scan row)");
            testContext.setMetric("relaxedCrcEffective", true);
            compare(note().text, "This channel: On (scan row)");
            testContext.setMetric("relaxedCrcRowOverride", false);
            verify(!note().visible);
            // Stopped, there is no channel on air to speak of.
            testContext.setMetric("relaxedCrcRowOverride", true);
            testContext.setLifecyclePhase(0);
            verify(!note().visible);
        }
    }
}
