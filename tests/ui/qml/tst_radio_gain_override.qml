// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #518 follow-up: under --trunk-scan a target can carry its own rtl_gain. While
// it is on air the panel shows the target's gain, badges it as the target's, names the
// configured gain beside it, and its buttons edit that configured gain -- the one every
// other target runs and the scan restores at each switch.
Item {
    width: 420
    height: 900

    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/RadioSheet.qml"
    }

    TestCase {
        name: "RadioGainOverride"
        when: windowShown

        readonly property var sheet: loader.item

        function init() {
            verify(sheet !== null, "RadioSheet must load");
            testContext.setHostRunning(true);
            testContext.resetCommands();
            sheet.open();
        }

        function cleanup() {
            testContext.setMetric("tunerGainRowOverride", false);
            testContext.setMetric("configuredTunerGainDb", 30);
            testContext.setMetric("tunerGainDb", 30);
            if (sheet) {
                sheet.forgetRequests();
                sheet.visible = false;
            }
            testContext.setHostRunning(false);
        }

        function targetOverride(effectiveDb, configuredDb) {
            testContext.setMetric("tunerGainDb", effectiveDb);
            testContext.setMetric("configuredTunerGainDb", configuredDb);
            testContext.setMetric("tunerGainRowOverride", true);
            tryVerify(function () { return metrics.tunerGainRowOverride === true });
        }

        // Issue #743: a radio input DSD-neo cannot tune shows no gain or PPM rows,
        // and says why where they were: an I/Q stream names its sending program,
        // an I/Q replay what was recorded. Squelch stays.
        function test_fixed_input_hides_the_tuner_rows() {
            var gain = findChild(sheet, "radioGainRows");
            var ppm = findChild(sheet, "radioPpmRows");
            var note = findChild(sheet, "radioTunerNote");
            verify(gain !== null && ppm !== null && note !== null, "a tuner row is missing");
            verify(gain.visible && ppm.visible && !note.visible, "a live tuner hid its rows");
            try {
                testContext.setMetric("tunable", false);
                tryVerify(function () { return !gain.visible && !ppm.visible && note.visible }, 2000,
                          "a stream kept its tuner rows");
                compare(note.text, "An I/Q stream has no tuner controls: adjust the radio in the sending program.");
                verify(findChild(sheet, "radioSquelchValue").visible, "the squelch went with the tuner rows");
                testContext.setMetric("replayInput", true);
                tryVerify(function () { return note.text === "An I/Q replay has no tuner controls: it plays what was recorded." });
                testContext.setMetric("replayInput", false);
                testContext.setMetric("radioInput", false);
                tryVerify(function () { return gain.visible && ppm.visible && !note.visible }, 2000,
                          "audio input lost the rows that set up the radio");
            } finally {
                testContext.setMetric("replayInput", false);
                testContext.setMetric("radioInput", true);
                testContext.setMetric("tunable", true);
            }
        }

        function test_no_override_shows_one_value_and_no_badge() {
            testContext.setMetric("tunerGainDb", 22);
            testContext.setMetric("configuredTunerGainDb", 22);
            tryVerify(function () { return metrics.tunerGainDb === 22 });
            compare(findChild(sheet, "radioGainValue").text, "22 dB");
            verify(!findChild(sheet, "radioGainRowNote").visible, "no target gain, no badge");
        }

        function test_target_gain_reads_first_and_names_the_default() {
            targetOverride(10, 20);
            compare(findChild(sheet, "radioGainValue").text, "10 dB");
            verify(findChild(sheet, "radioGainRowNote").visible);
            verify(findChild(sheet, "radioGainRowBadge").visible);
            compare(findChild(sheet, "radioGainDefault").text, "default 20 dB");
        }

        function test_buttons_edit_the_configured_gain_not_the_target() {
            targetOverride(10, 20);
            findChild(sheet, "radioGainUp").clicked();
            compare(testContext.lastGainDb(), 21, "stepped from the target's 10 instead of the default's 20");
            // The target still owns the reading; only the named default moved.
            compare(findChild(sheet, "radioGainValue").text, "10 dB");
            compare(findChild(sheet, "radioGainDefault").text, "default 21 dB");
        }

        // Gain steps here are live only: the line under the RTL gain row says so. An Airspy shows its own
        // controls instead of that row, and so no line.
        function test_session_note_under_the_rtl_gain() {
            var note = findChild(sheet, "radioGainSessionNote");
            verify(note !== null);
            verify(note.visible);
            compare(note.text, "Gain changes here last until you stop listening.");
            compare(note.Accessible.name, note.text, "the note reads by itself");
            testContext.setMetric("airspy", {"gain_mode": "sensitivity", "sensitivity_gain": 10});
            tryVerify(function () { return sheet.airspyActive });
            verify(!note.visible, "no RTL gain row, no note");
            testContext.setMetric("airspy", {});
            tryVerify(function () { return !sheet.airspyActive });
            verify(note.visible);
        }

        function test_agc_on_either_side_reads_as_auto() {
            targetOverride(0, 18);
            compare(findChild(sheet, "radioGainValue").text, "auto");
            compare(findChild(sheet, "radioGainDefault").text, "default 18 dB");
            sheet.forgetRequests();
            targetOverride(12, 0);
            compare(findChild(sheet, "radioGainValue").text, "12 dB");
            compare(findChild(sheet, "radioGainDefault").text, "default auto");
        }
    }
}
