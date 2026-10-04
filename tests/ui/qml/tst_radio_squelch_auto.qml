// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml/Util.js" as Util

// The dynamic squelches (issue #518 follow-up): the Radio sheet chooses between
// a threshold in dB, a margin over the noise floor the demodulator learns
// (Auto) and the FM quieting a radio's squelch opens at (Noise). In Auto and
// Noise the buttons step the margin, the floor, the quieting or the reason it
// is off reads below, and dB goes back to the level the setting kept beneath
// it. A "this channel" edit offers Auto only on a row that takes it (an nfm or
// am row) and Noise only on an nfm row; the AM monitor on its own has no Noise.
Item {
    width: 420
    height: 1200

    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/RadioSheet.qml"
    }

    TestCase {
        name: "RadioSquelchAuto"
        when: windowShown

        readonly property var sheet: loader.item
        readonly property int squelchField: 1
        readonly property int widthField: 2

        function init() {
            verify(sheet !== null, "RadioSheet must load");
            testContext.setHostRunning(true);
            testContext.resetCommands();
            testContext.setMetric("radioInput", true);
            sheet.open();
        }

        function cleanup() {
            onAir(false, 0);
            setNoise(false, false, 0, "");
            testContext.setMetric("squelchNoiseOffered", true);
            setAuto(false, false, 0, "");
            testContext.setMetric("configuredSquelchLevelOff", false);
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

        // The configured and in-force settings' auto flags, one margin for both (a level keeps its margin), and the
        // status of the one in force.
        function setAuto(configured, effective, margin, status) {
            testContext.setMetric("configuredSquelchAuto", configured);
            testContext.setMetric("configuredSquelchMarginDb", margin);
            testContext.setMetric("effectiveSquelchAuto", effective);
            testContext.setMetric("effectiveSquelchMarginDb", margin);
            testContext.setMetric("squelchAutoStatus", status);
            tryVerify(function () {
                return metrics.configuredSquelchAuto === configured && metrics.squelchAutoStatus === status;
            });
        }

        // The configured and in-force settings' noise flags, as setAuto() sets the auto ones.
        function setNoise(configured, effective, margin, status) {
            testContext.setMetric("configuredSquelchNoise", configured);
            testContext.setMetric("configuredSquelchMarginDb", margin);
            testContext.setMetric("effectiveSquelchNoise", effective);
            testContext.setMetric("effectiveSquelchMarginDb", margin);
            testContext.setMetric("squelchAutoStatus", status);
            tryVerify(function () {
                return metrics.configuredSquelchNoise === configured && metrics.squelchAutoStatus === status;
            });
        }

        // A trunk target "fire" on air taking the squelch edit, and the width edit when @p analog (an nfm row).
        function onAir(active, editable) {
            testContext.setScanRowContext(active ? {"active": true, "scanner": 1, "session": 7, "row": 2, "mode": 9,
                                                    "target": "fire", "label": "fire", "key": "1:7:2"}
                                                 : {"active": false});
            testContext.setMetric("scanRowLabel", active ? "fire" : "");
            testContext.setMetric("scanRowKey", active ? "1:7:2" : "");
            testContext.setMetric("scanRowEditable", editable);
            testContext.setMetric("scanRowListed", 0);
            testContext.setMetric("scanRowEdited", 0);
            testContext.setMetric("scanRowActive", active);
            tryVerify(function () { return metrics.scanRowActive === active });
        }

        function test_a_level_reads_db_and_no_status() {
            testContext.setMetric("squelchDb", -80.0);
            testContext.setMetric("configuredSquelchDb", -80.0);
            tryVerify(function () { return metrics.squelchDb === -80.0 });
            var mode = findChild(sheet, "radioSquelchMode");
            verify(mode.visible);
            compare(mode.currentIndex, 0);
            compare(findChild(sheet, "radioSquelchValue").text, "-80 dB");
            verify(!findChild(sheet, "radioSquelchAutoStatus").visible, "a level has no floor to show");
        }

        function test_choosing_auto_starts_at_the_default_margin() {
            testContext.setMetric("configuredSquelchDb", -80.0);
            findChild(sheet, "radioSquelchMode").selected(1);
            compare(testContext.squelchAutoCalls(), 1);
            compare(testContext.lastSquelchMarginDb(), 10);
            compare(testContext.squelchCalls(), 0, "no level request with it");
            // The request speaks for itself until the engine's reading arrives.
            compare(findChild(sheet, "radioSquelchMode").currentIndex, 1);
            compare(findChild(sheet, "radioSquelchValue").text, "auto +10 dB");
            // Choosing what is already chosen sends nothing.
            findChild(sheet, "radioSquelchMode").selected(1);
            compare(testContext.squelchAutoCalls(), 1);
        }

        function test_auto_reads_its_margin_and_floor() {
            setAuto(true, true, 12, "floor -78.3 dB");
            compare(findChild(sheet, "radioSquelchMode").currentIndex, 1);
            compare(findChild(sheet, "radioSquelchValue").text, "auto +12 dB");
            var status = findChild(sheet, "radioSquelchAutoStatus");
            verify(status.visible);
            compare(status.text, "floor -78.3 dB");
            compare(findChild(sheet, "radioSquelchValue").Accessible.name, "auto +12 dB (floor -78.3 dB)");
            setAuto(true, true, 12, "off on digital");
            compare(findChild(sheet, "radioSquelchAutoStatus").text, "off on digital");
        }

        function test_buttons_step_the_margin_in_auto() {
            setAuto(true, true, 10, "learning");
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.lastSquelchMarginDb(), 11);
            compare(testContext.squelchCalls(), 0, "the dB threshold is left alone");
            compare(findChild(sheet, "radioSquelchValue").text, "auto +11 dB");
            verify(!findChild(sheet, "radioSquelchAutoStatus").visible, "the status describes the old setting");
            findChild(sheet, "radioSquelchDown").clicked();
            findChild(sheet, "radioSquelchDown").clicked();
            compare(testContext.lastSquelchMarginDb(), 9);
            compare(testContext.squelchAutoCalls(), 3);
        }

        function test_margin_stops_at_its_range() {
            setAuto(true, true, 30, "learning");
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.squelchAutoCalls(), 0, "30 dB is the widest margin");
            sheet.forgetRequests();
            setAuto(true, true, 3, "learning");
            findChild(sheet, "radioSquelchDown").clicked();
            compare(testContext.squelchAutoCalls(), 0, "3 dB is the narrowest margin");
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.lastSquelchMarginDb(), 4);
        }

        function test_choosing_db_restores_the_level_beneath() {
            testContext.setMetric("configuredSquelchDb", -70.0);
            setAuto(true, true, 10, "learning");
            findChild(sheet, "radioSquelchMode").selected(0);
            // The engine puts back the level it kept, as stored; no dB value is sent for it.
            compare(testContext.restoreSquelchCalls(), 1);
            compare(testContext.squelchCalls(), 0);
            compare(findChild(sheet, "radioSquelchValue").text, "-70 dB");
            compare(findChild(sheet, "radioSquelchMode").currentIndex, 0);
            sheet.forgetRequests();
            // Off beneath the auto squelch reads off until the engine's reading arrives.
            testContext.setMetric("configuredSquelchDb", 0.0);
            testContext.setMetric("configuredSquelchLevelOff", true);
            tryVerify(function () { return metrics.configuredSquelchLevelOff === true });
            findChild(sheet, "radioSquelchMode").selected(0);
            compare(testContext.restoreSquelchCalls(), 2);
            compare(findChild(sheet, "radioSquelchValue").text, "off");
            sheet.forgetRequests();
            // A legacy full-scale level also reads 0 dB, and it gates everything: it is not off.
            testContext.setMetric("configuredSquelchLevelOff", false);
            tryVerify(function () { return metrics.configuredSquelchLevelOff === false });
            findChild(sheet, "radioSquelchMode").selected(0);
            compare(testContext.restoreSquelchCalls(), 3);
            compare(testContext.squelchCalls(), 0, "never sent as 0 dB, which means off");
            compare(findChild(sheet, "radioSquelchValue").text, "0 dB");
        }

        function test_auto_starts_from_the_margin_a_level_kept() {
            // auto+6, then dB: the level setting keeps the margin, and Auto starts from it again.
            setAuto(false, false, 6, "");
            findChild(sheet, "radioSquelchMode").selected(1);
            compare(testContext.lastSquelchMarginDb(), 6);
        }

        function test_row_auto_reads_first_and_names_a_level_default() {
            testContext.setMetric("configuredSquelchDb", -80.0);
            testContext.setMetric("squelchReadout", "auto +6 dB (floor -81.0 dB; row; default -80.0 dB)");
            testContext.setMetric("configuredSquelchAuto", false);
            testContext.setMetric("effectiveSquelchAuto", true);
            testContext.setMetric("effectiveSquelchMarginDb", 6);
            testContext.setMetric("squelchAutoStatus", "floor -81.0 dB");
            testContext.setMetric("squelchRowOverride", true);
            tryVerify(function () { return metrics.squelchRowOverride === true });
            compare(findChild(sheet, "radioSquelchValue").text, "auto +6 dB");
            compare(findChild(sheet, "radioSquelchAutoStatus").text, "floor -81.0 dB");
            compare(findChild(sheet, "radioSquelchDefault").text, "default -80 dB");
            // The mode and the buttons edit the default beneath the row, a level.
            compare(findChild(sheet, "radioSquelchMode").currentIndex, 0);
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.lastSquelchDb(), -75);
        }

        function test_this_channel_auto_on_an_analog_row() {
            onAir(true, squelchField | widthField);
            testContext.setMetric("effectiveSquelchDb", -55.0);
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowSquelch);
            var mode = findChild(sheet, "radioSquelchMode");
            verify(mode.visible, "an nfm row takes the auto squelch");
            mode.selected(1);
            var edit = testContext.lastScanRowEdit();
            compare(edit.field, squelchField);
            compare(edit.value.squelchMode, 1);
            compare(edit.value.squelchMarginDb, 10);
            compare(testContext.squelchAutoCalls(), 0, "the default was not touched");
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.lastScanRowEdit().value.squelchMarginDb, 11);
            // Back to dB: the row's level, within its whole-dB range, and the margin just asked for (no snapshot has
            // brought it back yet), which the row keeps for a switch back to Auto.
            mode.selected(0);
            edit = testContext.lastScanRowEdit();
            compare(edit.value.squelchDb, -55);
            compare(edit.value.squelchMarginDb, 11);
            verify(edit.value.squelchMode === undefined, "a level edit names no mode");
            // A legacy full-scale default beneath it is not off for the row either: -1 dB, the row's nearest.
            sheet.forgetRequests();
            mode.selected(1);
            testContext.setMetric("effectiveSquelchDb", 0.0);
            tryVerify(function () { return metrics.effectiveSquelchDb === 0.0 });
            mode.selected(0);
            compare(testContext.lastScanRowEdit().value.squelchDb, -1);
        }

        // The channel-map review and the target preview name a row's own auto squelch by its margin.
        function test_preview_summary_names_an_auto_squelch() {
            compare(Util.squelchSummary(0, 6), "Squelch: auto +6 dB")
            compare(Util.squelchSummary(-60, undefined), "Squelch: -60 dB")
            compare(Util.squelchSummary(0, undefined), "Squelch: off")
            compare(Util.squelchSummary(undefined, undefined), "Squelch: inherit")
        }

        function test_choosing_noise_starts_at_the_default_margin() {
            var mode = findChild(sheet, "radioSquelchMode");
            compare(mode.model.length, 3, "dB, Auto and Noise");
            mode.selected(2);
            compare(testContext.squelchNoiseCalls(), 1);
            compare(testContext.lastSquelchMarginDb(), 10);
            compare(testContext.squelchAutoCalls(), 0);
            compare(mode.currentIndex, 2);
            compare(findChild(sheet, "radioSquelchValue").text, "noise +10 dB");
            mode.selected(2);
            compare(testContext.squelchNoiseCalls(), 1, "choosing what is chosen sends nothing");
        }

        function test_noise_reads_its_quieting_and_steps_its_margin() {
            setNoise(true, true, 12, "quieting 23 dB");
            compare(findChild(sheet, "radioSquelchMode").currentIndex, 2);
            compare(findChild(sheet, "radioSquelchValue").text, "noise +12 dB");
            compare(findChild(sheet, "radioSquelchAutoStatus").text, "quieting 23 dB");
            compare(findChild(sheet, "radioSquelchValue").Accessible.name, "noise +12 dB (quieting 23 dB)");
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.squelchNoiseCalls(), 1);
            compare(testContext.lastSquelchMarginDb(), 13);
            compare(testContext.squelchAutoCalls(), 0, "the margin steps the noise squelch, not auto");
            compare(findChild(sheet, "radioSquelchValue").text, "noise +13 dB");
            // From Noise to Auto at the same margin, and dB back to the level kept beneath.
            sheet.forgetRequests();
            findChild(sheet, "radioSquelchMode").selected(1);
            compare(testContext.squelchAutoCalls(), 1);
            compare(testContext.lastSquelchMarginDb(), 12);
            sheet.forgetRequests();
            testContext.setMetric("configuredSquelchDb", -70.0);
            findChild(sheet, "radioSquelchMode").selected(0);
            compare(testContext.restoreSquelchCalls(), 1);
        }

        function test_no_noise_on_the_am_monitor() {
            testContext.setMetric("squelchNoiseOffered", false);
            tryVerify(function () { return metrics.squelchNoiseOffered === false });
            var mode = findChild(sheet, "radioSquelchMode");
            compare(mode.model.length, 2, "dB and Auto only");
            sheet.chooseSquelchMode(2);
            compare(testContext.squelchNoiseCalls(), 0);
        }

        function test_this_channel_noise_on_an_nfm_row_only() {
            onAir(true, squelchField | widthField | commands.scanRowFieldTone);
            testContext.setMetric("effectiveSquelchDb", -55.0);
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowNfm);
            var mode = findChild(sheet, "radioSquelchMode");
            compare(mode.model.length, 3, "an nfm row takes the noise squelch");
            mode.selected(2);
            var edit = testContext.lastScanRowEdit();
            compare(edit.field, squelchField);
            compare(edit.value.squelchMode, commands.squelchModeNoise);
            compare(edit.value.squelchMarginDb, 10);
            compare(testContext.squelchNoiseCalls(), 0, "the default was not touched");
            findChild(sheet, "radioScope").selected(0);
            sheet.forgetRequests();
            // An am row: Auto, no Noise.
            onAir(true, squelchField | widthField);
            findChild(sheet, "radioScope").selected(1);
            verify(!sheet.rowNfm);
            compare(findChild(sheet, "radioSquelchMode").model.length, 2, "an am row has no noise squelch");
        }

        // A noise setting that runs as auto -- the default on the AM monitor on
        // its own, an am row that inherits it -- shows and steps as Auto.
        function test_noise_where_it_runs_as_auto_steps_as_auto() {
            testContext.setMetric("squelchNoiseOffered", false);
            setNoise(true, true, 12, "as auto: floor -78.3 dB");
            var mode = findChild(sheet, "radioSquelchMode");
            compare(mode.model.length, 2);
            compare(mode.currentIndex, 1, "the AM monitor runs it as auto");
            compare(findChild(sheet, "radioSquelchValue").text, "auto +12 dB");
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.squelchAutoCalls(), 1);
            compare(testContext.squelchNoiseCalls(), 0, "never the refused noise squelch");
            compare(testContext.lastSquelchMarginDb(), 13);
            sheet.forgetRequests();
            testContext.setMetric("squelchNoiseOffered", true);
            // An am row inheriting a noise default.
            onAir(true, squelchField | widthField);
            findChild(sheet, "radioScope").selected(1);
            verify(!sheet.rowNfm);
            compare(findChild(sheet, "radioSquelchMode").currentIndex, 1);
            findChild(sheet, "radioSquelchUp").clicked();
            var edit = testContext.lastScanRowEdit();
            compare(edit.value.squelchMode, commands.squelchModeAuto);
            compare(edit.value.squelchMarginDb, 13);
        }

        function test_preview_summary_names_a_noise_squelch() {
            compare(Util.squelchSummary(0, 9, true), "Squelch: noise +9 dB")
            compare(Util.squelchSummary(0, 9, false), "Squelch: auto +9 dB")
        }

        function test_this_channel_on_a_digital_row_is_a_level() {
            // A digital row follows an auto default, which is off there, and takes only a level of its own.
            setAuto(true, true, 10, "off on digital");
            testContext.setMetric("effectiveSquelchOff", true);
            onAir(true, squelchField);
            findChild(sheet, "radioScope").selected(1);
            verify(!findChild(sheet, "radioSquelchMode").visible, "no Auto on a row that refuses it");
            compare(findChild(sheet, "radioSquelchValue").text, "off");
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.lastScanRowEdit().value.squelchDb, -100, "up from off is the row's floor");
        }
    }
}
