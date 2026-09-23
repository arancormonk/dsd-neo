// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #521: a scan row or target can carry its own squelch. While it is on air
// the panel shows the row's threshold first, badges it as the row's, names the
// configured default beside it, and its buttons edit that default -- never the
// row, whose value would be gone as soon as the scanner moved on.
Item {
    width: 420
    height: 900

    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/RadioSheet.qml"
    }

    TestCase {
        name: "RadioSquelchOverride"
        when: windowShown

        readonly property var sheet: loader.item

        function init() {
            verify(sheet !== null, "RadioSheet must load");
            testContext.setHostRunning(true);
            testContext.resetCommands();
            sheet.open();
        }

        function cleanup() {
            testContext.setMetric("squelchRowOverride", false);
            testContext.setMetric("configuredSquelchDb", -120.0);
            testContext.setMetric("effectiveSquelchDb", -120.0);
            testContext.setMetric("configuredSquelchOff", false);
            testContext.setMetric("effectiveSquelchOff", false);
            testContext.setMetric("squelchReadout", "-120.0 dB");
            testContext.setMetric("squelchDb", -120.0);
            testContext.setMetric("squelchOff", false);
            if (sheet) {
                sheet.forgetRequests();
                sheet.visible = false;
            }
            testContext.setHostRunning(false);
        }

        // What the engine publishes for a row override: 0 dB reads as off here, as it does
        // for every level the dB forms can set. The readout is the app-control text.
        function rowOverride(effectiveDb, configuredDb, readout) {
            testContext.setMetric("effectiveSquelchDb", effectiveDb);
            testContext.setMetric("effectiveSquelchOff", effectiveDb >= 0);
            testContext.setMetric("squelchDb", effectiveDb >= 0 ? -120.0 : effectiveDb);
            testContext.setMetric("squelchOff", effectiveDb >= 0);
            testContext.setMetric("configuredSquelchDb", configuredDb);
            testContext.setMetric("configuredSquelchOff", configuredDb >= 0);
            testContext.setMetric("squelchReadout", readout || "");
            testContext.setMetric("squelchRowOverride", true);
            tryVerify(function () { return metrics.squelchRowOverride === true });
        }

        function test_no_override_shows_one_value_and_no_badge() {
            testContext.setMetric("squelchDb", -80.0);
            testContext.setMetric("configuredSquelchDb", -80.0);
            testContext.setMetric("effectiveSquelchDb", -80.0);
            tryVerify(function () { return metrics.squelchDb === -80.0 });
            compare(findChild(sheet, "radioSquelchValue").text, "-80 dB");
            verify(!findChild(sheet, "radioSquelchRowNote").visible, "no row, no badge");
        }

        function test_row_override_reads_row_first_and_names_the_default() {
            rowOverride(-60.0, -80.0, "-60.0 dB (row; default -80.0 dB)");
            compare(findChild(sheet, "radioSquelchValue").text, "-60 dB");
            verify(findChild(sheet, "radioSquelchRowNote").visible);
            verify(findChild(sheet, "radioSquelchRowBadge").visible);
            compare(findChild(sheet, "radioSquelchDefault").text, "default -80 dB");
            // Read aloud as the terminal prints it.
            compare(findChild(sheet, "radioSquelchValue").Accessible.name, "-60.0 dB (row; default -80.0 dB)");
        }

        function test_full_scale_default_is_not_off() {
            // A legacy linear default at full scale reads 0 dB, like off, but it gates
            // everything; the engine's off flag decides, not the sign of the reading.
            rowOverride(-60.0, 0.0, "-60.0 dB (row; default 0.0 dB)");
            testContext.setMetric("configuredSquelchOff", false);
            tryVerify(function () { return metrics.configuredSquelchOff === false });
            compare(findChild(sheet, "radioSquelchDefault").text, "default 0 dB");
            testContext.setMetric("effectiveSquelchDb", 0.0);
            testContext.setMetric("effectiveSquelchOff", false);
            tryVerify(function () { return metrics.effectiveSquelchOff === false });
            compare(findChild(sheet, "radioSquelchValue").text, "0 dB");
        }

        function test_buttons_edit_the_configured_default_not_the_row() {
            rowOverride(-60.0, -80.0);
            findChild(sheet, "radioSquelchUp").clicked();
            verify(Math.abs(testContext.lastSquelchDb() - (-75)) < 0.001,
                   "stepped from the row's -60 instead of the default's -80");
            // The row still owns the reading; only the named default moved.
            compare(findChild(sheet, "radioSquelchValue").text, "-60 dB");
            compare(findChild(sheet, "radioSquelchDefault").text, "default -75 dB");
            findChild(sheet, "radioSquelchDown").clicked();
            verify(Math.abs(testContext.lastSquelchDb() - (-80)) < 0.001);
        }

        function test_off_on_either_side_reads_as_off() {
            rowOverride(0.0, -80.0);
            compare(findChild(sheet, "radioSquelchValue").text, "off");
            compare(findChild(sheet, "radioSquelchDefault").text, "default -80 dB");
            sheet.forgetRequests();
            rowOverride(-45.0, 0.0);
            compare(findChild(sheet, "radioSquelchValue").text, "-45 dB");
            compare(findChild(sheet, "radioSquelchDefault").text, "default off");
            // Up from a configured off lands on the floor, as it does without a row.
            findChild(sheet, "radioSquelchUp").clicked();
            verify(Math.abs(testContext.lastSquelchDb() - (-120)) < 0.001);
        }
    }
}
