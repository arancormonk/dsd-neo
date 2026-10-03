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
