// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #518: while a scan row is on air and takes "this channel" edits, the Radio
// sheet's gain, squelch and width controls and the Tone filter editor can edit that
// row's own setting for the session. The scope is a choice; the row is captured when
// it is made, the controls then step what the row runs, and "Use default" / "List
// value" are offered when the list sets the field / the row runs an edit.
Item {
    width: 420
    height: 1200

    Loader {
        id: radio
        width: parent.width
        height: parent.height
        source: uiDir + "/RadioSheet.qml"
    }

    Loader {
        id: tone
        width: parent.width
        height: parent.height
        source: uiDir + "/ToneFilterSheet.qml"
    }

    TestCase {
        name: "ScanRowEdits"
        when: windowShown

        readonly property var sheet: radio.item
        readonly property var toneSheet: tone.item
        readonly property int squelchField: 1
        readonly property int widthField: 2
        readonly property int toneField: 4
        readonly property int gainField: 8

        function init() {
            verify(sheet !== null && toneSheet !== null, "both sheets must load");
            testContext.setHostRunning(true);
            testContext.resetCommands();
            testContext.setMetric("radioInput", true);
            testContext.setMetric("tunerGainDb", 12);
            testContext.setMetric("configuredTunerGainDb", 30);
            testContext.setMetric("effectiveSquelchDb", -55);
            testContext.setMetric("effectiveSquelchOff", false);
            testContext.setMetric("configuredSquelchDb", -80);
            testContext.setMetric("squelchRowOverride", true);
            sheet.open();
        }

        function cleanup() {
            testContext.setMetric("scanRowSynced", true);
            testContext.setMetric("effectiveSquelchOff", false);
            onAir(false, 0, 0, 0);
            testContext.setMetric("tunerGainDb", 12);
            testContext.setMetric("effectiveSquelchDb", -55);
            testContext.setMetric("squelchRowOverride", false);
            testContext.setMetric("tunerGainDb", 30);
            if (sheet) {
                sheet.forgetRequests();
                sheet.visible = false;
            }
            if (toneSheet)
                toneSheet.visible = false;
            testContext.setHostRunning(false);
        }

        // A trunk target "fire" on air, taking @p editable, setting @p listed, running edits of @p edited, its tone
        // policy allow 100.0.
        function onAir(active, editable, listed, edited) {
            testContext.setScanRowContext(active ? {"active": true, "scanner": 1, "session": 7, "row": 2, "mode": 9,
                                                    "target": "fire", "label": "fire", "key": "1:7:2",
                                                    "toneMode": 1, "toneList": "100.0"}
                                                 : {"active": false});
            testContext.setMetric("scanRowLabel", active ? "fire" : "");
            testContext.setMetric("scanRowKey", active ? "1:7:2" : "");
            testContext.setMetric("scanRowEditable", editable);
            testContext.setMetric("scanRowListed", listed);
            testContext.setMetric("scanRowEdited", edited);
            testContext.setMetric("scanRowActive", active);
            tryVerify(function () { return metrics.scanRowActive === active });
        }

        function test_no_row_no_scope() {
            verify(!findChild(sheet, "radioScope").visible, "no row on air, no scope choice");
            findChild(sheet, "radioGainUp").clicked();
            compare(testContext.lastGainDb(), 13, "the gain command, stepped from the gain in force, as before");
            compare(testContext.scanRowEditCalls(), 0);
        }

        function test_default_scope_is_unchanged() {
            onAir(true, squelchField | gainField, squelchField, 0);
            verify(findChild(sheet, "radioScope").visible);
            compare(findChild(sheet, "radioScope").currentIndex, 0, "the default is what the sheet opens on");
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.lastSquelchDb(), -75, "the default steps from its own -80");
            compare(testContext.scanRowEditCalls(), 0);
        }

        function test_this_channel_steps_what_the_row_runs() {
            onAir(true, squelchField | gainField, squelchField, squelchField);
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowScope);
            verify(findChild(sheet, "radioScopeNote").visible);
            verify(findChild(sheet, "radioScopeNote").text.indexOf("fire") >= 0);
            verify(!findChild(sheet, "radioSquelchRowNote").visible, "no default note while editing the row");

            findChild(sheet, "radioSquelchDown").clicked();
            compare(testContext.scanRowEditCalls(), 1);
            var edit = testContext.lastScanRowEdit();
            compare(edit.field, squelchField);
            compare(edit.action, 1);
            compare(edit.value.squelchDb, -60, "stepped from the row's -55");
            compare(edit.context.target, "fire");
            compare(edit.context.session, 7);
            compare(testContext.lastSquelchDb(), 0, "the default was not touched");

            findChild(sheet, "radioGainUp").clicked();
            edit = testContext.lastScanRowEdit();
            compare(edit.field, gainField);
            compare(edit.value.gainDb, 13, "stepped from the gain in force, 12");
            compare(testContext.gainCalls(), 0, "the configured gain was not touched");
        }

        function test_row_actions_follow_listed_and_edited() {
            onAir(true, squelchField | gainField, squelchField, squelchField);
            findChild(sheet, "radioScope").selected(1);
            verify(findChild(sheet, "radioSquelchRowActions").visible);
            verify(findChild(sheet, "radioSquelchRowInherit").visible, "the list sets a squelch");
            verify(findChild(sheet, "radioSquelchRowReset").visible, "the row runs an edit");
            verify(!findChild(sheet, "radioGainRowActions").visible, "neither for the gain");
            findChild(sheet, "radioSquelchRowInherit").clicked();
            compare(testContext.lastScanRowEdit().action, 2);
            findChild(sheet, "radioSquelchRowReset").clicked();
            compare(testContext.lastScanRowEdit().action, 3);
        }

        function test_another_row_on_air_goes_back_to_the_default() {
            onAir(true, squelchField, squelchField, 0);
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowScope);
            testContext.setMetric("scanRowKey", "1:7:3");
            tryVerify(function () { return !sheet.rowScope });
            compare(findChild(sheet, "radioScope").currentIndex, 0);
        }

        // Two rows can share a name: the row's identity, not its label, says another one came on air.
        function test_a_row_of_the_same_name_is_another_row() {
            onAir(true, squelchField, squelchField, 0);
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowScope);
            testContext.setMetric("scanRowKey", "2:9:4");
            tryVerify(function () { return !sheet.rowScope });
        }

        // A row that follows a default under the row range's -100 dB floor: a step up lands on the floor, never
        // on off, which only a step down reaches.
        function test_a_step_up_from_below_the_row_floor_lands_on_it() {
            var defaults = [-110, -120];
            for (var i = 0; i < defaults.length; i++) {
                onAir(true, squelchField, squelchField, 0);
                testContext.setMetric("effectiveSquelchDb", defaults[i]);
                tryVerify(function () { return metrics.effectiveSquelchDb === defaults[i] });
                findChild(sheet, "radioScope").selected(1);
                verify(sheet.rowScope);
                findChild(sheet, "radioSquelchUp").clicked();
                compare(testContext.lastScanRowEdit().value.squelchDb, -100, "up from " + defaults[i]);
                sheet.forgetRequests();
                findChild(sheet, "radioSquelchDown").clicked();
                compare(testContext.lastScanRowEdit().value.squelchDb, 0, "down from " + defaults[i] + " is off");
                sheet.forgetRequests();
                findChild(sheet, "radioScope").selected(0);
            }
            testContext.setMetric("effectiveSquelchDb", -55);
        }

        // A width step on "this channel" starts from the width setting the row runs in the same snapshots as its
        // identity, never from the live front-end reading, which can already be the next row's: row A runs 12.5 kHz
        // in coherent snapshots while the front end already reads row B's 20 kHz.
        function test_a_width_step_starts_from_the_rows_setting() {
            testContext.setMetric("analogBandwidthRowActive", true);
            testContext.setMetric("analogBandwidthSettingHz", 12500);
            testContext.setMetric("analogBandwidthHz", 20000);
            testContext.setMetric("analogBandwidthMaxHz", 25000);
            onAir(true, squelchField | widthField, squelchField | widthField, 0);
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowWidth);
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(testContext.lastScanRowEdit().field, widthField);
            compare(testContext.lastScanRowEdit().value.widthHz, 16000, "one step up from the row's 12.5 kHz");
            testContext.setMetric("analogBandwidthRowActive", false);
            testContext.setMetric("analogBandwidthSettingHz", 0);
            testContext.setMetric("analogBandwidthHz", 0);
            testContext.setMetric("analogBandwidthMaxHz", 0);
        }

        // The readings beside the row are another row's for a moment (the metrics read the options and the state
        // between the decoder's two publishes): "This channel" waits, and once chosen its steppers hold.
        function test_readings_of_another_scope_hold_the_row_controls() {
            onAir(true, squelchField | gainField, squelchField, 0);
            testContext.setMetric("scanRowSynced", false);
            tryVerify(function () { return metrics.scanRowSynced === false });
            findChild(sheet, "radioScope").selected(1);
            verify(!sheet.rowScope, "the choice waits for readings of the row");
            testContext.setMetric("scanRowSynced", true);
            tryVerify(function () { return metrics.scanRowSynced === true });
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowScope);
            testContext.setMetric("scanRowSynced", false);
            tryVerify(function () { return metrics.scanRowSynced === false });
            verify(sheet.rowScope, "the choice stands");
            verify(!findChild(sheet, "radioGainUp").enabled && !findChild(sheet, "radioSquelchUp").enabled);
            sheet.stepGain(1);
            sheet.stepSquelch(5);
            compare(testContext.scanRowEditCalls(), 0, "nothing stepped from readings not the row's");
            compare(testContext.gainCalls(), 0);
            testContext.setMetric("scanRowSynced", true);
            tryVerify(function () { return metrics.scanRowSynced === true });
            findChild(sheet, "radioGainUp").clicked();
            compare(testContext.scanRowEditCalls(), 1);
        }

        // The editor's context comes from a newer snapshot than the metrics the controls show: target B is on air
        // while the metrics still show target A's values. The choice waits rather than step B from A's.
        function test_a_context_ahead_of_the_metrics_waits() {
            onAir(true, squelchField | gainField, squelchField, 0);
            testContext.setScanRowContext({"active": true, "scanner": 1, "session": 7, "row": 3, "mode": 9,
                                           "target": "air", "label": "air", "key": "1:7:3",
                                           "toneMode": 0, "toneList": ""});
            findChild(sheet, "radioScope").selected(1);
            verify(!sheet.rowScope, "the metrics still show another row");
            compare(findChild(sheet, "radioScope").currentIndex, 0);
            findChild(sheet, "radioGainUp").clicked();
            compare(testContext.scanRowEditCalls(), 0, "no edit stepped from another row's values");
            // The metrics catch up: the choice is taken.
            testContext.setMetric("scanRowKey", "1:7:3");
            tryVerify(function () { return metrics.scanRowKey === "1:7:3" });
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowScope);
            findChild(sheet, "radioSquelchDown").clicked();
            compare(testContext.lastScanRowEdit().context.key, "1:7:3");
        }

        // In "This channel" a control the row takes no edit of is held: a -Y row runs no gain of its own, and
        // the gain buttons must not fall through to the configured gain the scope note says is left alone.
        function test_this_channel_holds_what_the_row_does_not_take() {
            onAir(true, squelchField, squelchField, 0);
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowScope);
            verify(!findChild(sheet, "radioGainUp").enabled);
            verify(!findChild(sheet, "radioGainDown").enabled);
            sheet.stepGain(1);
            compare(testContext.gainCalls(), 0, "the configured gain was not touched");
            compare(testContext.scanRowEditCalls(), 0);
            verify(!sheet.analogWidthCanWiden && !sheet.analogWidthCanNarrow, "no width edit for this row either");
            verify(findChild(sheet, "radioSquelchUp").enabled);
            findChild(sheet, "radioScope").selected(0);
            verify(findChild(sheet, "radioGainUp").enabled, "the default scope steps the gain again");
        }

        // An Airspy runs no gain per row, and its panel sets the device for every channel: held in "This channel".
        function test_this_channel_holds_the_airspy_panel() {
            testContext.setMetric("airspy", {"gain_mode": "sensitivity", "sensitivity_gain": 10});
            tryVerify(function () { return sheet.airspyActive });
            onAir(true, squelchField | widthField, squelchField, 0);
            findChild(sheet, "radioScope").selected(1);
            verify(sheet.rowScope);
            var panel = findChild(sheet, "radioAirspy");
            verify(!panel.enabled);
            panel.edited("sensitivity_gain", "12");
            compare(testContext.airspyCalls(), 0, "the device's gain was not touched");
            findChild(sheet, "radioScope").selected(0);
            verify(panel.enabled);
            panel.edited("sensitivity_gain", "12");
            compare(testContext.airspyCalls(), 1, "the default scope sets the device again");
            testContext.setMetric("airspy", {});
        }

        // "Use default" and "List value" drop a step still outstanding for that field: the next step starts from
        // what the row runs, not from the step before the action.
        function test_row_actions_drop_pending_steps() {
            onAir(true, squelchField | gainField, squelchField | gainField, gainField);
            findChild(sheet, "radioScope").selected(1);
            findChild(sheet, "radioGainUp").clicked();
            compare(testContext.lastScanRowEdit().value.gainDb, 13);
            findChild(sheet, "radioGainRowReset").clicked();
            compare(testContext.lastScanRowEdit().action, 3);
            testContext.setMetric("tunerGainDb", 10);
            tryVerify(function () { return metrics.tunerGainDb === 10 });
            findChild(sheet, "radioGainUp").clicked();
            compare(testContext.lastScanRowEdit().value.gainDb, 11, "stepped from the row's 10, not the pending 13");
            findChild(sheet, "radioSquelchDown").clicked();
            compare(testContext.lastScanRowEdit().value.squelchDb, -60);
            findChild(sheet, "radioSquelchRowInherit").clicked();
            testContext.setMetric("effectiveSquelchDb", -80);
            tryVerify(function () { return metrics.effectiveSquelchDb === -80 });
            findChild(sheet, "radioSquelchDown").clicked();
            compare(testContext.lastScanRowEdit().value.squelchDb, -85, "stepped from the default it now follows");
        }

        function test_tone_editor_this_channel() {
            onAir(true, squelchField | toneField, toneField, 0);
            toneSheet.openEditor();
            verify(findChild(toneSheet, "toneFilterScope").visible);
            findChild(toneSheet, "toneFilterScope").selected(1);
            verify(toneSheet.rowScope);
            // The editor opens on the row's own policy, not the configured one (off).
            compare(toneSheet.mode, 1);
            compare(findChild(toneSheet, "toneFilterList").text, "100.0");
            verify(findChild(toneSheet, "toneFilterRowInherit").visible);
            verify(!findChild(toneSheet, "toneFilterRowReset").visible);
            findChild(toneSheet, "toneFilterMode").selected(1);
            findChild(toneSheet, "toneFilterList").text = "100.0/D023N";
            findChild(toneSheet, "toneFilterApply").clicked();
            var edit = testContext.lastScanRowEdit();
            compare(edit.field, toneField);
            compare(edit.action, 1);
            compare(edit.value.toneMode, 1);
            compare(edit.value.toneList, "100.0/D023N");
            compare(testContext.toneFilterCalls(), 0, "the configured policy was not touched");
        }

        // The scan moves on to a row that takes no tone edit: Apply still goes to the row chosen, never to the
        // configured policy.
        function test_tone_editor_keeps_its_row_across_a_rotation() {
            onAir(true, squelchField | toneField, toneField, 0);
            toneSheet.openEditor();
            findChild(toneSheet, "toneFilterScope").selected(1);
            testContext.setMetric("scanRowEditable", squelchField);
            testContext.setMetric("scanRowKey", "1:7:3");
            tryVerify(function () { return metrics.scanRowKey === "1:7:3" });
            verify(toneSheet.rowScope, "the chosen row stays the destination");
            verify(findChild(toneSheet, "toneFilterScope").visible);
            verify(!findChild(toneSheet, "toneFilterRowActions").visible, "the row on air is another one");
            findChild(toneSheet, "toneFilterApply").clicked();
            compare(testContext.lastScanRowEdit().context.key, "1:7:2");
            compare(testContext.toneFilterCalls(), 0, "the configured policy was not touched");
        }

        function test_tone_editor_without_a_tone_row() {
            onAir(true, squelchField, squelchField, 0);
            toneSheet.openEditor();
            verify(!findChild(toneSheet, "toneFilterScope").visible, "an am or digital row takes no tone edit");
        }
    }
}
