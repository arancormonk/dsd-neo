// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The passband on audio input with a rigctl peer (#621): the monitor's Passband
// row reads the passband the peer is asked for, and its editor steps the NFM and
// AM widths with no DSP rate to bound them, -B standing in for an unset NFM
// width, on the default or, under a scan row that takes the edit, on that row.
Item {
    id: root
    width: 411
    height: 900
    Ui.MonitorScreen { id: monitor; anchors.fill: parent }
    TestCase {
        name: "MonitorPassband"
        when: windowShown
        readonly property int widthField: 2
        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function init() {
            testContext.resetCommands();
        }
        function cleanup() {
            var sheet = item("passbandSheet");
            sheet.forgetRequests();
            sheet.visible = false;
            onAir(false, 0, "");
            testContext.setMetric("scanRowSynced", true);
            testContext.setMetric("peerPassband", false);
            testContext.setMetric("passbandInForce", false);
            testContext.setMetric("nfmBandwidthOffered", false);
            testContext.setMetric("amBandwidthOffered", false);
            testContext.setMetric("analogBandwidthReading", "");
            testContext.setMetric("analogBandwidthAm", false);
            testContext.setMetric("analogBandwidthRowActive", false);
            testContext.setMetric("analogBandwidthRowOverride", false);
            testContext.setMetric("analogBandwidthSettingHz", 0);
            testContext.setMetric("analogBandwidthMaxHz", 0);
            testContext.setMetric("nfmBandwidthConfiguredHz", 0);
            testContext.setMetric("amBandwidthConfiguredHz", 0);
            testContext.setMetric("nfmBandwidthUnsetText", "default");
            testContext.setMetric("amBandwidthUnsetText", "default");
            testContext.setMetric("nfmBandwidthUnsetHz", 16000);
            testContext.setMetric("amBandwidthUnsetHz", 6000);
            testContext.setHostRunning(false);
        }
        // A trunk target "fire" on air under @p key, taking @p editable.
        function onAir(active, editable, key) {
            testContext.setScanRowContext(active ? {"active": true, "scanner": 1, "session": 7, "row": 2, "mode": 9,
                                                    "target": "fire", "label": "fire", "key": key}
                                                 : {"active": false});
            testContext.setMetric("scanRowLabel", active ? "fire" : "");
            testContext.setMetric("scanRowKey", active ? key : "");
            testContext.setMetric("scanRowEditable", editable);
            testContext.setMetric("scanRowListed", 0);
            testContext.setMetric("scanRowEdited", 0);
            testContext.setMetric("scanRowActive", active);
            tryVerify(function () { return metrics.scanRowActive === active && metrics.scanRowKey === (active ? key : "") });
        }
        // An nfm row on air (the width in force is its NFM passband).
        function nfmRowOnAir(key) {
            onAir(true, widthField, key);
            testContext.setMetric("analogBandwidthRowActive", true);
            testContext.setMetric("analogBandwidthAm", false);
            tryVerify(function () { return metrics.analogBandwidthRowActive === true });
        }
        // A running session on audio input with a rigctl peer, -B 12.5 kHz standing
        // in for the unset NFM width, the FM monitor asking the peer for @p reading.
        function showPeer(reading) {
            testContext.setHostRunning(true);
            testContext.setMetric("nfmBandwidthUnsetText", "-B 12.5 kHz");
            testContext.setMetric("nfmBandwidthUnsetHz", 12500);
            testContext.setMetric("analogBandwidthReading", reading);
            testContext.setMetric("passbandInForce", true);
            testContext.setMetric("nfmBandwidthOffered", true);
            testContext.setMetric("peerPassband", true);
            tryCompare(item("monitorPassband"), "visible", true);
        }
        // The sheet lays its sections out again when one shows or hides; a click
        // before that lands on the old layout, where the scroll clips it.
        function settle() {
            waitForItemPolished(item("passbandClose").parent);
        }
        function openEditor() {
            var edit = item("monitorPassbandEdit");
            tryCompare(edit, "visible", true);
            waitForItemPolished(item("monitorPassband"));
            mouseClick(edit);
            var sheet = item("passbandSheet");
            tryCompare(sheet, "visible", true);
            settle();
            return sheet;
        }
        function chooseThisChannel() {
            item("passbandScope").selected(1);
            settle();
        }

        function test_row_only_with_a_peer_and_an_offered_kind() {
            testContext.setHostRunning(true);
            testContext.setMetric("analogBandwidthReading", "12.5 kHz (-B)");
            testContext.setMetric("nfmBandwidthOffered", true);
            tryVerify(function () { return metrics.nfmBandwidthOffered === true });
            verify(!item("monitorPassband").visible, "without a peer the width filters nothing on audio input");
            testContext.setMetric("nfmBandwidthOffered", false);
            testContext.setMetric("peerPassband", true);
            tryVerify(function () { return metrics.peerPassband === true });
            verify(!item("monitorPassband").visible, "a peer with no width offered has nothing to set");
            testContext.setMetric("amBandwidthOffered", true);
            tryCompare(item("monitorPassband"), "visible", true);
            testContext.setMetric("amBandwidthOffered", false);
            showPeer("12.5 kHz (-B)");
            compare(item("monitorPassbandLabel").text, "PASSBAND");
            compare(item("monitorPassbandValue").text, "NFM 12.5 kHz (-B)");
            verify(item("monitorPassbandEdit").enabled);
            // Off the monitor (a digital preset with an explicit NFM default) the
            // reading is what the peer is asked for now, -B, never the setting.
            testContext.setMetric("passbandInForce", false);
            testContext.setMetric("nfmBandwidthConfiguredHz", 20000);
            testContext.setMetric("amBandwidthOffered", true);
            tryVerify(function () { return metrics.amBandwidthOffered === true });
            compare(item("monitorPassbandValue").text, "NFM 12.5 kHz (-B)");
            // With no reading the row claims no passband, and no setting
            // stands in for one.
            testContext.setMetric("analogBandwidthReading", "");
            tryCompare(item("monitorPassbandValue"), "text", "");
            testContext.setHostRunning(false);
            tryCompare(item("monitorPassbandEdit"), "enabled", false);
        }

        // A stale DSP bound from an earlier radio session bounds nothing here: the
        // peer's passband steps through the whole list.
        function test_nfm_steps_ignore_a_stale_dsp_bound() {
            showPeer("12.5 kHz (-B)");
            testContext.setMetric("analogBandwidthMaxHz", 12000);
            tryVerify(function () { return metrics.analogBandwidthMaxHz === 12000 });
            openEditor();
            verify(item("passbandNfm").visible);
            verify(!item("passbandAm").visible, "AM is not offered");
            compare(item("passbandNfmTitle").text, "NFM passband");
            compare(item("passbandNfmValue").text, "12.5 kHz (-B)");
            verify(!item("passbandNfmDefault").visible, "the width is unset already");
            var up = item("passbandNfmUp");
            mouseClick(up);
            compare(testContext.nfmBandwidthCalls(), 1);
            compare(testContext.lastNfmBandwidthHz(), 16000, "the unset width steps from -B");
            compare(item("passbandNfmValue").text, "16 kHz");
            mouseClick(up);
            compare(testContext.lastNfmBandwidthHz(), 20000);
            mouseClick(up);
            compare(testContext.lastNfmBandwidthHz(), 25000);
            verify(!up.enabled, "the top of the list");
            verify(item("passbandNfmDown").enabled);
            item("passbandSheet").stepWidth(false, 1);
            compare(testContext.nfmBandwidthCalls(), 3);
            compare(testContext.amBandwidthCalls(), 0);
        }

        // A typed digital row on air under the FM preset keeps the peer at -B: the
        // reading is not the configured width's, so the section shows the setting
        // a step moves, as the terminal's row does.
        function test_a_digital_row_leaves_the_setting_to_read() {
            showPeer("12.5 kHz (-B)");
            testContext.setMetric("passbandInForce", false);
            testContext.setMetric("nfmBandwidthConfiguredHz", 20000);
            tryVerify(function () { return metrics.nfmBandwidthConfiguredHz === 20000 && metrics.passbandInForce === false });
            compare(item("monitorPassbandValue").text, "NFM 12.5 kHz (-B)", "the row reads what the peer is asked for");
            openEditor();
            compare(item("passbandNfmValue").text, "20 kHz");
            mouseClick(item("passbandNfmUp"));
            compare(testContext.lastNfmBandwidthHz(), 25000);
            compare(item("passbandNfmValue").text, "25 kHz");
        }

        // Neither a width nor -B: the peer keeps its own passband, which has no
        // width to step from, so the steps start at the NFM default.
        function test_the_peers_own_passband_steps_from_the_default() {
            showPeer("peer's own");
            testContext.setMetric("nfmBandwidthUnsetText", "peer's own");
            testContext.setMetric("nfmBandwidthUnsetHz", 0);
            tryVerify(function () { return metrics.nfmBandwidthUnsetHz === 0 });
            openEditor();
            compare(item("passbandNfmValue").text, "peer's own");
            mouseClick(item("passbandNfmUp"));
            compare(testContext.lastNfmBandwidthHz(), 20000, "from the 16 kHz default");
            item("passbandSheet").forgetRequests();
            mouseClick(item("passbandNfmDown"));
            compare(testContext.lastNfmBandwidthHz(), 12500);
        }

        function test_use_the_default_width_sends_0() {
            showPeer("20 kHz");
            testContext.setMetric("nfmBandwidthConfiguredHz", 20000);
            tryVerify(function () { return metrics.nfmBandwidthConfiguredHz === 20000 });
            openEditor();
            compare(item("passbandNfmValue").text, "20 kHz");
            var reset = item("passbandNfmDefault");
            verify(reset.visible);
            mouseClick(reset);
            compare(testContext.nfmBandwidthCalls(), 1);
            compare(testContext.lastNfmBandwidthHz(), 0);
            compare(item("passbandNfmValue").text, "-B 12.5 kHz", "-B stands in for the unset width");
            verify(!reset.visible);
        }

        // An am row on air over the NFM session: the AM passband is in force, and
        // its unset default steps from 6 kHz.
        function test_am_under_an_am_row() {
            showPeer("6 kHz (default)");
            onAir(true, widthField, "1:7:2");
            testContext.setMetric("analogBandwidthRowActive", true);
            testContext.setMetric("analogBandwidthAm", true);
            testContext.setMetric("amBandwidthOffered", true);
            tryVerify(function () { return metrics.analogBandwidthAm === true });
            compare(item("monitorPassbandValue").text, "AM 6 kHz (default)");
            openEditor();
            verify(item("passbandAm").visible);
            verify(item("passbandNfm").visible, "the preset's NFM width stays offered");
            compare(item("passbandAmTitle").text, "AM passband");
            compare(item("passbandAmValue").text, "6 kHz (default)");
            compare(item("passbandNfmValue").text, "-B 12.5 kHz", "the reading is AM's, not NFM's");
            mouseClick(item("passbandAmDown"));
            compare(testContext.amBandwidthCalls(), 1);
            compare(testContext.lastAmBandwidthHz(), 5000);
            compare(testContext.nfmBandwidthCalls(), 0);
            compare(testContext.scanRowEditCalls(), 0, "All channels edits the default");
            verify(!item("passbandAmDown").enabled, "the bottom of the AM list");
        }

        function test_this_channel_edits_the_row() {
            showPeer("12.5 kHz (-B)");
            nfmRowOnAir("1:7:2");
            var sheet = openEditor();
            var scope = item("passbandScope");
            verify(scope.visible);
            compare(scope.currentIndex, 0, "the default is what the sheet opens on");
            chooseThisChannel();
            verify(sheet.rowWidth);
            compare(scope.currentIndex, 1);
            verify(item("passbandScopeNote").visible);
            verify(item("passbandScopeNote").text.indexOf("fire") >= 0);
            verify(!item("passbandAm").visible, "only the row's kind");
            verify(!item("passbandNfmDefault").visible, "a row has no unset width");
            compare(item("passbandNfmValue").text, "12.5 kHz (-B)");
            // The row inherits the unset width, which -B stands for.
            mouseClick(item("passbandNfmUp"));
            compare(testContext.scanRowEditCalls(), 1);
            var edit = testContext.lastScanRowEdit();
            compare(edit.field, widthField);
            compare(edit.action, commands.scanRowEditSet);
            compare(edit.value.widthHz, 16000);
            compare(edit.context.target, "fire");
            compare(testContext.nfmBandwidthCalls(), 0, "the default was not touched");
            compare(item("passbandNfmValue").text, "16 kHz");
        }

        function test_this_channel_row_actions() {
            showPeer("16 kHz");
            nfmRowOnAir("1:7:2");
            testContext.setMetric("analogBandwidthSettingHz", 16000);
            testContext.setMetric("scanRowListed", widthField);
            testContext.setMetric("scanRowEdited", widthField);
            tryVerify(function () { return metrics.scanRowEdited === widthField });
            openEditor();
            verify(!item("passbandRowActions").visible, "All channels has no row actions");
            chooseThisChannel();
            verify(item("passbandRowActions").visible);
            mouseClick(item("passbandNfmDown"));
            compare(item("passbandNfmValue").text, "12.5 kHz");
            mouseClick(item("passbandRowInherit"));
            compare(testContext.scanRowEditCalls(), 2);
            compare(testContext.lastScanRowEdit().field, widthField);
            compare(testContext.lastScanRowEdit().action, commands.scanRowEditInherit);
            compare(item("passbandNfmValue").text, "16 kHz", "the step no longer stands for the reading");
            mouseClick(item("passbandRowReset"));
            compare(testContext.scanRowEditCalls(), 3);
            compare(testContext.lastScanRowEdit().action, commands.scanRowEditReset);
            compare(testContext.nfmBandwidthCalls(), 0);
        }

        function test_another_row_on_air_resets_the_scope() {
            showPeer("12.5 kHz (-B)");
            nfmRowOnAir("1:7:2");
            var sheet = openEditor();
            chooseThisChannel();
            verify(sheet.rowWidth);
            nfmRowOnAir("1:7:3");
            tryVerify(function () { return !sheet.rowWidth });
            compare(item("passbandScope").currentIndex, 0, "back to All channels");
            verify(!item("passbandScopeNote").visible);
            settle();
            mouseClick(item("passbandNfmUp"));
            compare(testContext.scanRowEditCalls(), 0, "nothing reaches the earlier row");
            compare(testContext.nfmBandwidthCalls(), 1);
        }

        // A row that owns 8 kHz, NFM unset and -B 12.5 kHz: All channels edits the
        // default beneath the row, from -B; This channel steps the row's 8 kHz.
        function test_a_row_owning_its_width() {
            showPeer("8 kHz (row; default -B 12.5 kHz)");
            nfmRowOnAir("1:7:2");
            testContext.setMetric("analogBandwidthRowOverride", true);
            testContext.setMetric("analogBandwidthSettingHz", 8000);
            tryVerify(function () { return metrics.analogBandwidthSettingHz === 8000 });
            compare(item("monitorPassbandValue").text, "NFM 8 kHz (row; default -B 12.5 kHz)");
            var sheet = openEditor();
            var note = item("passbandRowNote");
            verify(note.visible);
            compare(note.text, "This channel sets its own passband; changes here apply to the default (-B 12.5 kHz).");
            compare(item("passbandNfmValue").text, "-B 12.5 kHz", "the default being edited, not the row's");
            mouseClick(item("passbandNfmUp"));
            compare(testContext.lastNfmBandwidthHz(), 16000, "from -B, never from the row's own width");
            compare(note.text, "This channel sets its own passband; changes here apply to the default (16 kHz).");
            chooseThisChannel();
            verify(sheet.rowWidth);
            verify(!note.visible);
            compare(item("passbandNfmValue").text, "8 kHz (row; default -B 12.5 kHz)");
            verify(!item("passbandNfmDown").enabled, "8 kHz is the bottom of the NFM list");
            verify(item("passbandNfmUp").enabled);
            mouseClick(item("passbandNfmUp"));
            compare(testContext.lastScanRowEdit().value.widthHz, 11250);
            compare(testContext.nfmBandwidthCalls(), 1, "only the earlier default step");
        }

        // Steps wait while the readings are not the row's.
        function test_this_channel_waits_for_the_rows_readings() {
            showPeer("12.5 kHz (-B)");
            nfmRowOnAir("1:7:2");
            var sheet = openEditor();
            chooseThisChannel();
            verify(sheet.rowWidth);
            testContext.setMetric("scanRowSynced", false);
            tryVerify(function () { return !item("passbandNfmUp").enabled });
            sheet.stepWidth(false, 1);
            compare(testContext.scanRowEditCalls(), 0);
        }

        function test_the_sheet_closes_with_the_session_or_the_peer() {
            showPeer("12.5 kHz (-B)");
            var sheet = openEditor();
            testContext.setHostRunning(false);
            tryCompare(sheet, "visible", false);
            showPeer("12.5 kHz (-B)");
            openEditor();
            testContext.setMetric("peerPassband", false);
            tryCompare(sheet, "visible", false);
            verify(!item("monitorPassband").visible);
            showPeer("12.5 kHz (-B)");
            openEditor();
            testContext.setMetric("nfmBandwidthOffered", false);
            tryCompare(sheet, "visible", false);
        }
    }
}
