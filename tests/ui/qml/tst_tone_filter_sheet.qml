// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The live tone-filter editor (#527), reached from the monitor's Tone filter row: there
// with no policy too wherever detection runs, it opens on the configured policy, checks
// the list inline with the decoder's own parser, and sends the mode and list as one edit.
// Under a scan row with its own policy it says the edit goes to the default.
Item {
    id: root
    width: 411
    height: 700
    Ui.MonitorScreen { id: monitor; anchors.fill: parent }
    TestCase {
        name: "ToneFilterSheet"
        when: windowShown
        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function init() {
            testContext.resetCommands();
        }
        function cleanup() {
            item("toneFilterSheet").visible = false;
            testContext.setMetric("rxToneConfiguredText", "off");
            testContext.setMetric("toneFilterVisible", false);
            testContext.setMetric("toneFilterEditable", false);
            testContext.setMetric("toneFilterRowOverride", false);
            testContext.setMetric("toneFilterConfiguredMode", 0);
            testContext.setMetric("toneFilterConfiguredList", "");
            testContext.setHostRunning(false);
            Ui.Theme.resetFontScale();
            root.width = 411;
            root.height = 700;
        }
        // The analog FM monitor with no policy: the row and its editor are there, reading off.
        function showEditable() {
            testContext.setHostRunning(true);
            testContext.setMetric("toneFilterEditable", true);
        }
        function openEditor() {
            var edit = item("monitorToneFilterEdit");
            tryCompare(edit, "visible", true);
            // The row lays the button out when it is polished, after it comes on screen.
            waitForItemPolished(item("monitorToneFilter"));
            mouseClick(edit);
            var sheet = item("toneFilterSheet");
            tryCompare(sheet, "visible", true);
            return sheet;
        }
        function insideBody(reading) {
            var body = item("monitorBody");
            var left = reading.mapToItem(body, 0, 0).x;
            var right = reading.mapToItem(body, reading.width, 0).x;
            return left >= -0.5 && right <= body.width + 0.5;
        }

        // The Edit button is a full touch target whose label is never cut short.
        function editIsWhole() {
            var edit = item("monitorToneFilterEdit");
            verify(edit.height >= Ui.Theme.minimumTouchSize - 0.5,
                   "Edit is " + edit.height + " px tall");
            verify(edit.width >= edit.implicitWidth - 0.5 && edit.height >= edit.implicitHeight - 0.5,
                   "Edit is " + edit.width + "x" + edit.height + ", its label needs "
                   + edit.implicitWidth + "x" + edit.implicitHeight);
        }

        function test_row_is_reachable_with_no_policy() {
            showEditable();
            tryCompare(item("monitorToneFilter"), "visible", true);
            compare(item("monitorToneFilterValue").text, "off");
            verify(item("monitorToneFilterEdit").enabled);
            verify(!item("monitorToneFilterStatus").visible);
            editIsWhole();
        }
        function test_row_stays_away_where_nothing_detects() {
            testContext.setHostRunning(true);
            verify(!item("monitorToneFilter").visible);
            // A policy shown in force keeps the row even so (the view decides both).
            testContext.setMetric("toneFilterVisible", true);
            tryCompare(item("monitorToneFilter"), "visible", true);
        }
        function test_editing_needs_a_running_session() {
            showEditable();
            tryCompare(item("monitorToneFilter"), "visible", true);
            testContext.setHostRunning(false);
            tryCompare(item("monitorToneFilterEdit"), "enabled", false);
        }
        function test_opens_on_the_configured_policy() {
            showEditable();
            testContext.setMetric("toneFilterConfiguredMode", 2);
            testContext.setMetric("toneFilterConfiguredList", "100.0/D023I");
            var sheet = openEditor();
            compare(item("toneFilterMode").currentIndex, 2);
            compare(item("toneFilterList").text, "100.0/D023I");
            compare(item("toneFilterList").error, "");
            verify(item("toneFilterApply").enabled);
            verify(!item("toneFilterRowNote").visible);
            compare(testContext.toneFilterCalls(), 0);
        }
        // The inline message is the decoder's own refusal, and a list it would refuse is
        // not sent.
        function test_inline_validation_blocks_apply() {
            showEditable();
            openEditor();
            item("toneFilterMode").selected(1);
            var list = item("toneFilterList");
            list.text = "100,67";
            compare(list.error, "use / between entries, not commas");
            verify(!item("toneFilterApply").enabled);
            item("toneFilterApply").activate();
            compare(testContext.toneFilterCalls(), 0);
            list.text = "100/xyzzy";
            compare(list.error, "entry 2 is not a standard CTCSS tone or DCS code");
            list.text = "";
            compare(list.error, "allow needs a list of CTCSS tones or DCS codes, e.g. 67.0/100.0/D023N");
            verify(!item("toneFilterApply").enabled);
            // Off needs no list.
            item("toneFilterMode").selected(0);
            compare(list.error, "");
            verify(item("toneFilterApply").enabled);
        }
        function test_apply_sends_the_mode_and_list() {
            showEditable();
            var sheet = openEditor();
            item("toneFilterMode").selected(1);
            compare(item("toneFilterMode").currentIndex, 1);
            item("toneFilterList").text = "67.0/D023";
            mouseClick(item("toneFilterApply"));
            compare(testContext.toneFilterCalls(), 1);
            compare(testContext.lastToneFilterMode(), 1);
            compare(testContext.lastToneFilterList(), "67.0/D023");
            tryCompare(sheet, "visible", false);
        }
        // Off keeps the list in the field, and so in the edit: turning the filter off does
        // not throw the list away.
        function test_off_keeps_the_list() {
            showEditable();
            testContext.setMetric("toneFilterConfiguredMode", 1);
            testContext.setMetric("toneFilterConfiguredList", "D754N");
            openEditor();
            item("toneFilterMode").selected(0);
            mouseClick(item("toneFilterApply"));
            compare(testContext.lastToneFilterMode(), 0);
            compare(testContext.lastToneFilterList(), "D754N");
        }
        function test_close_sends_nothing() {
            showEditable();
            var sheet = openEditor();
            item("toneFilterList").text = "82.5";
            mouseClick(item("toneFilterClose"));
            tryCompare(sheet, "visible", false);
            compare(testContext.toneFilterCalls(), 0);
            // Reopened, it reads the configured policy again, not the abandoned edit.
            openEditor();
            compare(item("toneFilterList").text, "");
        }
        function test_row_note_under_a_scan_row() {
            showEditable();
            testContext.setMetric("toneFilterVisible", true);
            testContext.setMetric("rxToneConfiguredText", "block 67.0 Hz (row; default allow 100.0 Hz)");
            testContext.setMetric("toneFilterRowOverride", true);
            testContext.setMetric("toneFilterConfiguredMode", 1);
            testContext.setMetric("toneFilterConfiguredList", "100.0");
            openEditor();
            verify(item("toneFilterRowNote").visible);
            // The editor opens on the configured policy, never the row's.
            compare(item("toneFilterMode").currentIndex, 1);
            compare(item("toneFilterList").text, "100.0");
        }
        // A long policy, its verdict and the edit button on a narrow phone with large text:
        // nothing runs off the body, and the button stays whole.
        function test_row_fits_a_compact_monitor() {
            root.width = 320;
            root.height = 360;
            Ui.Theme.fontScale = 1.6;
            showEditable();
            testContext.setMetric("toneFilterVisible", true);
            testContext.setMetric("rxToneConfiguredText",
                                  "allow 67.0 Hz/71.9 Hz/74.4 Hz/77.0 Hz/…+6"
                                  + " (row; default block 100.0 Hz/…+3)");
            testContext.setMetric("toneFilterGate", 1);
            testContext.setMetric("toneFilterStatusText", "muted: checking tone");
            var row = item("monitorToneFilter");
            tryCompare(row, "visible", true);
            waitForItemPolished(row);
            verify(insideBody(item("monitorToneFilterValue")));
            verify(insideBody(item("monitorToneFilterEdit")));
            editIsWhole();
            testContext.setMetric("toneFilterGate", 0);
            testContext.setMetric("toneFilterStatusText", "");
        }
    }
}
