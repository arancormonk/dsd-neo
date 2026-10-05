// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The squelch on audio input (#628): the monitor's Squelch row reads the setting
// as the terminal's SQL field spells it, and its editor sets a level or the
// noise squelch -- not auto, which needs a radio input -- on the default or,
// under a scan row that takes the edit, on that row.
Item {
    id: root
    width: 411
    height: 900
    Ui.MonitorScreen { id: monitor; anchors.fill: parent }
    TestCase {
        name: "MonitorSquelch"
        when: windowShown
        readonly property int squelchField: 1
        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function init() {
            testContext.resetCommands();
        }
        function cleanup() {
            var sheet = item("squelchSheet");
            sheet.forgetRequests();
            sheet.visible = false;
            onAir(false, 0);
            testContext.setMetric("squelchAudioInput", false);
            testContext.setMetric("squelchReadout", "-120.0 dB");
            testContext.setMetric("squelchDb", -120.0);
            testContext.setMetric("squelchOff", false);
            testContext.setMetric("configuredSquelchDb", -120.0);
            testContext.setMetric("configuredSquelchOff", false);
            testContext.setMetric("configuredSquelchLevelOff", false);
            testContext.setMetric("configuredSquelchNoise", false);
            testContext.setMetric("configuredSquelchAuto", false);
            testContext.setMetric("configuredSquelchMarginDb", 0);
            testContext.setMetric("effectiveSquelchNoise", false);
            testContext.setMetric("effectiveSquelchAuto", false);
            testContext.setMetric("effectiveSquelchMarginDb", 0);
            testContext.setMetric("effectiveSquelchDb", -120.0);
            testContext.setMetric("squelchNoiseOffered", true);
            testContext.setMetric("squelchAutoStatus", "");
            testContext.setHostRunning(false);
        }
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
        function showAudioInput(readout) {
            testContext.setHostRunning(true);
            testContext.setMetric("squelchReadout", readout);
            testContext.setMetric("squelchAudioInput", true);
            tryCompare(item("monitorSquelch"), "visible", true);
        }
        function setNoise(margin, status) {
            testContext.setMetric("configuredSquelchNoise", true);
            testContext.setMetric("effectiveSquelchNoise", true);
            testContext.setMetric("configuredSquelchMarginDb", margin);
            testContext.setMetric("effectiveSquelchMarginDb", margin);
            testContext.setMetric("squelchAutoStatus", status);
            tryVerify(function () {
                return metrics.configuredSquelchNoise === true && metrics.squelchAutoStatus === status;
            });
        }
        function openEditor() {
            var edit = item("monitorSquelchEdit");
            tryCompare(edit, "visible", true);
            waitForItemPolished(item("monitorSquelch"));
            mouseClick(edit);
            var sheet = item("squelchSheet");
            tryCompare(sheet, "visible", true);
            return sheet;
        }

        function test_row_only_on_audio_input() {
            testContext.setHostRunning(true);
            verify(!item("monitorSquelch").visible, "a radio input has the Radio sheet");
            showAudioInput("noise +10 dB (learning)");
            compare(item("monitorSquelchLabel").text, "SQUELCH");
            compare(item("monitorSquelchValue").text, "noise +10 dB (learning)");
            verify(item("monitorSquelchEdit").enabled);
            testContext.setHostRunning(false);
            tryCompare(item("monitorSquelchEdit"), "enabled", false);
        }

        function test_a_level_steps_in_db() {
            showAudioInput("-50.0 dB");
            testContext.setMetric("squelchDb", -50.0);
            testContext.setMetric("configuredSquelchDb", -50.0);
            tryVerify(function () { return metrics.squelchDb === -50.0 });
            openEditor();
            var mode = item("squelchMode");
            verify(mode.visible);
            compare(mode.model.length, 2, "dB and Noise, no Auto");
            compare(mode.currentIndex, 0);
            compare(item("squelchValue").text, "-50 dB");
            verify(!item("squelchStatus").visible, "a level has no status");
            mouseClick(item("squelchDown"));
            compare(testContext.squelchCalls(), 1);
            compare(testContext.lastSquelchDb(), -55);
            compare(item("squelchValue").text, "-55 dB");
        }

        function test_choosing_noise_starts_at_the_default_margin() {
            showAudioInput("-120.0 dB");
            openEditor();
            item("squelchMode").selected(1);
            compare(testContext.squelchNoiseCalls(), 1);
            compare(testContext.lastSquelchMarginDb(), 10);
            compare(testContext.squelchAutoCalls(), 0, "never auto on audio input");
            compare(item("squelchMode").currentIndex, 1);
            compare(item("squelchValue").text, "noise +10 dB");
        }

        function test_noise_reads_its_state_and_steps_its_margin() {
            showAudioInput("noise +12 dB (learning)");
            setNoise(12, "learning");
            openEditor();
            compare(item("squelchMode").currentIndex, 1);
            compare(item("squelchValue").text, "noise +12 dB");
            compare(item("squelchStatus").text, "learning");
            compare(item("squelchValue").Accessible.name, "noise +12 dB (learning)");
            mouseClick(item("squelchUp"));
            compare(testContext.squelchNoiseCalls(), 1);
            compare(testContext.lastSquelchMarginDb(), 13);
            item("squelchSheet").forgetRequests();
            testContext.setMetric("squelchAutoStatus", "off: no band above voice");
            tryCompare(item("squelchStatus"), "text", "off: no band above voice");
            // dB goes back to the level the setting kept beneath it.
            testContext.setMetric("configuredSquelchDb", -70.0);
            item("squelchMode").selected(0);
            compare(testContext.restoreSquelchCalls(), 1);
        }

        // An auto setting from the command line or a config does not run on audio
        // input: neither segment is chosen, the steppers wait, and dB or Noise
        // replaces it.
        function test_an_auto_setting_is_replaced_not_stepped() {
            showAudioInput("auto +10 dB (off: no radio input)");
            testContext.setMetric("configuredSquelchAuto", true);
            testContext.setMetric("effectiveSquelchAuto", true);
            testContext.setMetric("configuredSquelchMarginDb", 10);
            testContext.setMetric("squelchAutoStatus", "off: no radio input");
            tryVerify(function () { return metrics.configuredSquelchAuto === true });
            openEditor();
            compare(item("squelchMode").currentIndex, -1);
            compare(item("squelchValue").text, "auto +10 dB");
            compare(item("squelchStatus").text, "off: no radio input");
            verify(!item("squelchUp").enabled);
            verify(!item("squelchDown").enabled);
            item("squelchMode").selected(1);
            compare(testContext.squelchNoiseCalls(), 1);
            compare(testContext.lastSquelchMarginDb(), 10);
        }

        // The AM monitor on its own refuses the noise squelch: dB alone.
        function test_no_noise_on_the_am_monitor() {
            showAudioInput("-120.0 dB");
            testContext.setMetric("squelchNoiseOffered", false);
            tryVerify(function () { return metrics.squelchNoiseOffered === false });
            openEditor();
            verify(!item("squelchMode").visible, "a level is all the AM monitor takes here");
            item("squelchSheet").chooseSquelchMode(1);
            compare(testContext.squelchNoiseCalls(), 0);
        }

        function test_this_channel_noise_on_an_nfm_row() {
            showAudioInput("-120.0 dB");
            onAir(true, squelchField | commands.scanRowFieldTone);
            testContext.setMetric("effectiveSquelchDb", -55.0);
            openEditor();
            var scope = item("squelchScope");
            verify(scope.visible);
            scope.selected(1);
            var sheet = item("squelchSheet");
            verify(sheet.rowSquelch);
            verify(item("squelchScopeNote").visible);
            item("squelchMode").selected(1);
            var edit = testContext.lastScanRowEdit();
            compare(edit.field, squelchField);
            compare(edit.value.squelchMode, commands.squelchModeNoise);
            compare(edit.value.squelchMarginDb, 10);
            compare(testContext.squelchNoiseCalls(), 0, "the default was not touched");
            // An am row takes no noise squelch.
            scope.selected(0);
            onAir(true, squelchField);
            scope.selected(1);
            verify(!sheet.rowNfm);
            verify(!item("squelchMode").visible);
        }

        function test_stopping_the_session_closes_the_editor() {
            showAudioInput("-120.0 dB");
            var sheet = openEditor();
            testContext.setHostRunning(false);
            tryCompare(sheet, "visible", false);
        }
    }
}
