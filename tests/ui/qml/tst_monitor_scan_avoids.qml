// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #672: the Monitor's SCAN AVOIDS row and the sheet its Show opens. The
// row's buttons keep a touch target and a one-line label at every text scale
// (B-Qt1) and say what they act on (B-Qt2); the sheet lists the avoided
// channels by key from the real list model, sends Put back by key, never
// removes a row before the decoder does, and builds rows only while open and
// only for the rows in view.
Item {
    id: root
    width: 360
    height: 690
    // The row lives in the Monitor's "Signal & site" disclosure (#718), open here.
    Ui.MonitorScreen { id: monitor; anchors.fill: parent; detailsExpanded: true }
    TestCase {
        name: "MonitorScanAvoids"
        when: windowShown

        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function entries(n, first) {
            var list = [];
            for (var i = first || 0; i < n; ++i)
                list.push({key: "k" + i, label: "Row " + (i + 1) + "  " + (851.0125 + i * 0.0125).toFixed(6) + " MHz"});
            return list;
        }
        // The readings MetricsModel publishes beside a list of @p n entries.
        function publish(list, available) {
            var whole = available !== false;
            testContext.setScanAvoids(whole ? list : []);
            testContext.setMetrics({scanAvoidsManageable: true, scanAvoidsAvailable: whole,
                scanAvoidsTotalKnown: whole, scanAvoidCount: whole ? list.length : 0});
        }
        // The label inside an OutlineButton.
        function labelOf(button) {
            for (var i = 0; i < button.children.length; ++i) {
                if (button.children[i].text === button.text)
                    return button.children[i];
            }
            return null;
        }
        // The delegates the list has made, by the key each shows.
        function delegates(list) {
            var made = {};
            var kids = list.contentItem.children;
            for (var i = 0; i < kids.length; ++i) {
                if (kids[i].key !== undefined)
                    made[kids[i].key] = kids[i];
            }
            return made;
        }
        function putBackOf(row) {
            for (var i = 0; i < row.children.length; ++i) {
                if (row.children[i].objectName === "scanAvoidPutBack")
                    return row.children[i];
            }
            return null;
        }

        function init() {
            testContext.resetCommands();
            testContext.setHostRunning(true);
        }
        function cleanup() {
            item("scanAvoidSheet").visible = false;
            testContext.setScanAvoids([]);
            testContext.setMetrics({scanAvoidsManageable: false, scanAvoidsAvailable: true, scanAvoidsTotalKnown: true,
                scanAvoidCount: 0, scanRotationActive: false});
            Ui.Theme.fontScale = 1;
            root.width = 360;
            testContext.setHostRunning(false);
        }

        // B-Qt1: Show and Clear keep the default touch height and the width of
        // their label at any text scale, one line each, inside the row, 8 px
        // apart when side by side. Clear was a 28 px, 72 px wide target whose
        // label broke in two from 1.3.
        function test_row_buttons_keep_a_touch_target_data() {
            return [
                {tag: "360@1", width: 360, scale: 1},
                {tag: "360@1.3", width: 360, scale: 1.3},
                {tag: "360@1.6", width: 360, scale: 1.6},
                {tag: "320@1", width: 320, scale: 1},
                {tag: "320@1.3", width: 320, scale: 1.3},
                {tag: "320@1.6", width: 320, scale: 1.6}
            ];
        }
        function test_row_buttons_keep_a_touch_target(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            publish(entries(2));
            var row = item("scanAvoidRow");
            tryVerify(function () { return row.visible });
            waitForRendering(monitor);
            var show = item("scanAvoidShowButton");
            var clear = item("scanAvoidClearButton");
            for (var button of [show, clear]) {
                verify(button.height >= Ui.Theme.minimumTouchSize, button.objectName + " is under a touch target");
                var label = labelOf(button);
                verify(label !== null, button.objectName + " label");
                compare(label.lineCount, 1, button.objectName + " breaks its label");
                verify(!label.truncated, button.objectName + " cuts its label");
                verify(button.x >= 0 && button.x + button.width <= row.width + 0.5, button.objectName + " leaves the row");
            }
            if (Math.abs(show.y - clear.y) < 0.5)
                verify(clear.x - (show.x + show.width) >= 8, "the buttons are under 8 px apart");
        }

        // B-Qt2: each button says what it acts on, starting with its own word.
        function test_row_buttons_are_named_for_the_avoids() {
            compare(item("scanAvoidShowButton").accessibleName, "Show avoided scan channels");
            compare(item("scanAvoidClearButton").accessibleName, "Clear scan avoids");
        }

        // B1, D3: avoids a stopped or paused scan keeps are shown with Show and
        // Clear; a running rotation with none shows no 0; an unknown count reads "?".
        function test_row_follows_the_avoids_not_the_rotation() {
            var row = item("scanAvoidRow");
            var value = item("scanAvoidValue");
            publish(entries(2));
            tryVerify(function () { return row.visible });
            compare(value.text, "2");
            verify(item("scanAvoidShowButton").enabled && item("scanAvoidClearButton").enabled);
            publish([]);
            testContext.setMetric("scanRotationActive", true);
            tryVerify(function () { return !row.visible });
            publish([], false);
            tryVerify(function () { return row.visible });
            compare(value.text, "?");
        }

        // Show opens the list as it is now; each row's Put back sends its key,
        // named for the entry; nothing leaves before the decoder says so, and a
        // row whose entry stays keeps its delegate when another goes.
        function test_put_back_sends_the_key_and_waits_for_the_decoder() {
            publish(entries(3));
            var sheet = item("scanAvoidSheet");
            var list = item("scanAvoidList");
            item("scanAvoidShowButton").clicked();
            tryVerify(function () { return sheet.visible });
            compare(item("scanAvoidTitle").text, "Avoided channels (3)");
            verify(item("scanAvoidHint").visible);
            // Issue #697: a put back can let a scan leave a channel it owes a departure, whichever kind it is.
            compare(item("scanAvoidHint").text, "Put back returns a channel to the rotation without retuning. If the scan is waiting to leave a channel, it may move on at once.");
            verify(!item("scanAvoidStatus").visible);
            tryCompare(list, "count", 3);
            waitForRendering(list);
            var made = delegates(list);
            var second = putBackOf(made["k1"]);
            verify(second !== null);
            compare(second.text, "Put back");
            compare(second.accessibleName, "Put back Row 2  851.025000 MHz");
            verify(second.enabled);
            second.clicked();
            compare(testContext.restoredScanAvoids(), ["k1"]);
            compare(list.count, 3, "a row left before the decoder put it back");
            var kept = made["k2"];
            publish([entries(3)[0], entries(3)[2]]);
            tryCompare(list, "count", 2);
            waitForRendering(list);
            compare(delegates(list)["k2"], kept, "a kept row lost its delegate");
            compare(item("scanAvoidTitle").text, "Avoided channels (2)");
            item("scanAvoidDone").clicked();
            verify(!sheet.visible);
        }

        // An empty list says so and has nothing to put back; one the decoder
        // could not copy whole lists nothing to act on, and Put all back
        // (Clear) still works.
        function test_empty_and_unavailable() {
            var sheet = item("scanAvoidSheet");
            var putAll = item("scanAvoidPutAllBack");
            var status = item("scanAvoidStatus");
            publish([]);
            testContext.setMetric("scanRotationActive", true);
            sheet.open();
            verify(status.visible && !item("scanAvoidList").visible && !item("scanAvoidHint").visible);
            compare(status.text, "No avoided channels.");
            compare(item("scanAvoidTitle").text, "Avoided channels", "the title gave a 0 the status line says");
            verify(!putAll.enabled, "Put all back with nothing to put back");
            publish(entries(2), false);
            tryCompare(status, "text", "List not ready yet. Put all back still works.");
            compare(item("scanAvoidTitle").text, "Avoided channels");
            verify(!item("scanAvoidList").visible);
            verify(putAll.enabled);
            putAll.clicked();
            compare(testContext.scanAvoidClearCalls(), 1);
            publish(entries(2));
            tryVerify(function () { return item("scanAvoidList").visible });
            verify(putAll.enabled);
        }

        // The sheet closes when nothing is left to manage, when the session
        // stops, and with the screen.
        function test_sheet_closes_with_its_reason() {
            var sheet = item("scanAvoidSheet");
            publish(entries(1));
            sheet.open();
            testContext.setMetric("scanAvoidsManageable", false);
            tryVerify(function () { return !sheet.visible });
            publish(entries(1));
            sheet.open();
            testContext.setHostRunning(false);
            tryVerify(function () { return !sheet.visible });
            testContext.setHostRunning(true);
            sheet.open();
            monitor.visible = false;
            verify(!sheet.visible);
            monitor.visible = true;
        }

        // A closed sheet hands its list no model, so it makes no rows; opened
        // on 10,000 entries it makes only the rows in view, inside a list of
        // bounded height, with Put all back and Done still on the panel. The
        // first put back removes one row and leaves the rest as they were.
        function test_closed_builds_nothing_and_long_lists_stay_bounded() {
            var sheet = item("scanAvoidSheet");
            var list = item("scanAvoidList");
            testContext.setManyScanAvoids(10000);
            testContext.setMetrics({scanAvoidsManageable: true, scanAvoidsAvailable: true, scanAvoidsTotalKnown: true,
                scanAvoidCount: 10000});
            compare(list.count, 0, "a closed sheet made rows");
            compare(Object.keys(delegates(list)).length, 0);
            sheet.open();
            tryCompare(list, "count", 10000);
            waitForRendering(list);
            var made = Object.keys(delegates(list)).length;
            verify(made > 0 && made < 40, made + " rows made for 10,000 entries");
            verify(list.height <= list.maximumHeight + 0.5 && list.height < sheet.height);
            var done = item("scanAvoidDone");
            var bottom = done.mapToItem(sheet, 0, done.height).y;
            verify(bottom <= sheet.height, "Done fell off the screen");
            var rest = [];
            for (var i = 1; i < 10000; ++i)
                rest.push({key: "k" + i, label: "#" + (i + 1) + "  target-" + (i + 1) + "  851.012500 MHz"});
            var second = delegates(list)["k1"];
            testContext.setScanAvoids(rest);
            tryCompare(list, "count", 9999);
            waitForRendering(list);
            compare(delegates(list)["k1"], second, "a kept row lost its delegate");
            verify(Object.keys(delegates(list)).length < 40);
            sheet.visible = false;
            tryCompare(list, "count", 0);
        }
    }
}
