// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #800 (device finding F1): a screen pushed over the running session (Map, Diagnostics, Call recordings, the
// session's History) covers the Monitor, which stays drawn underneath. Nothing of the Monitor may stay reachable to a
// screen reader while it is covered: no node of it in the tree, and a touch on an empty part of the screen on top
// (the map's canvas, which is itself out of the tree) must not land on a Monitor row. The recent-calls rows are the
// hard case: a ListView writes each delegate's Accessible.ignored as it lays the delegate out, with no change signal
// (QQuickItemViewFxItem::setVisible), so a row laid out while covered (a call landing under the map) came back into
// the tree even though its own gate said it was covered. IDs and names are made up.
Item {
    width: 420
    height: 900
    Loader {
        id: loader
        source: uiDir + "/Main.qml"
    }
    TestCase {
        name: "A11yCoveredScreens"
        when: windowShown
        readonly property var app: loader.item

        function monitor() {
            return findChild(app, "monitorScreen");
        }
        // The roles a screen reader treats as controls (QAccessible::Role).
        readonly property var controlRoles: [Accessible.Button, Accessible.CheckBox, Accessible.RadioButton]
        // The nodes under @p root a screen reader can land on now: in the tree and with something to say or do: a
        // name, focus, a control's role, or any action (a press or toggle makes the Android node clickable, which
        // TalkBack stops on with no text at all).
        function reachableUnder(root) {
            var roles = controlRoles;
            return testContext.accessibleNodesUnder(root).filter(function (node) {
                if (!node.inTree)
                    return false;
                var actions = testContext.accessibleNode(node.item).actions || [];
                var acts = actions.filter(function (action) {
                    return action === "Press" || action === "Toggle";
                });
                return node.name.length > 0 || node.focusable || roles.indexOf(node.role) >= 0 || acts.length > 0;
            }).map(function (node) {
                return (node.objectName || node.file) + " \"" + node.name + "\"";
            });
        }
        // @p count calls land, and the Monitor's list lays them out (inserting moves every row).
        function land(count) {
            var list = findChild(monitor(), "recentCallsList");
            var before = callHistory.count;
            for (var i = 0; i < count; ++i)
                callHistory.pushWithIds(4100 + i, 7100 + i);
            tryCompare(callHistory, "count", before + count);
            if (list.model)
                tryVerify(function () { return list.count >= Math.min(before + count, monitorView.count); });
            waitForRendering(list);
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is in the tree");
        }
        function init() {
            verify(app !== null);
            testContext.useLifecycleHost(true);
            testContext.setLifecyclePhase(2);
            prefs.onboardingDone = true;
            app.currentTab = 0;
            app.requestActivate();
            tryCompare(app, "active", true);
            verify(app.monitorMode, "the session runs on the monitor");
            monitorView.historySession = 0;
            callHistory.clearAll();
            land(6);
            var list = findChild(monitor(), "recentCallsList");
            tryVerify(function () { return list.count >= 6 && list.itemAtIndex(1) !== null; });
            verify(reachableUnder(monitor()).length > 0, "uncovered, the Monitor is reachable");
        }
        function cleanup() {
            app.closeMap(false);
            app.diagnosticsOpen = false;
            app.callRecordingsOpen = false;
            app.sessionDestination = "";
            callHistory.clearAll();
            testContext.setLifecyclePhase(0);
            testContext.useLifecycleHost(false);
        }

        function open(which) {
            if (which === "map")
                app.openMap();
            else if (which === "diagnostics")
                app.diagnosticsOpen = true;
            else if (which === "callRecordings")
                app.callRecordingsOpen = true;
            else
                app.sessionDestination = which;
        }
        function test_covered_monitor_is_out_of_reach_data() {
            return [{tag: "map", which: "map"}, {tag: "diagnostics", which: "diagnostics"},
                    {tag: "callRecordings", which: "callRecordings"}, {tag: "history", which: "history"}];
        }

        // The session's History under the map (its Map button): neither History's rows nor the Monitor's reachable.
        function test_map_over_session_history() {
            app.sessionDestination = "history";
            var history = findChild(app, "sessionHistoryScreen");
            var log = findChild(history, "callLogList");
            tryVerify(function () { return log.itemAtIndex(1) !== null; });
            var opener = findChild(history, "historyMapButton");
            opener.forceActiveFocus();
            opener.activate();
            verify(app.mapOpen);
            // P9: the screen reader's focus moves into the map at once, onto a node there (Back), off the covered
            // button that opened it.
            var back = findChild(findChild(app, "mapScreen"), "mapBack");
            tryVerify(function () { return back.activeFocus; }, 1000, "the focus moves to the map's Back");
            verify(!opener.activeFocus);
            verify(testContext.accessibleNode(back).inTree);
            land(5);
            waitForRendering(log);
            var leaked = reachableUnder(findChild(app, "sessionTools")).concat(reachableUnder(monitor()));
            compare(leaked.length, 0, "reachable under the map: " + JSON.stringify(leaked));
        }
        // Covered, nothing of the Monitor is in the tree, not even rows laid out while it is covered (calls landing
        // under the screen on top); uncovered again, its rows are back.
        function test_covered_monitor_is_out_of_reach(data) {
            open(data.which);
            land(5);
            // Everything a pushed screen covers: the Monitor, and the session's own tools under a screen over them.
            var leaked = reachableUnder(monitor());
            if (data.which !== "history")
                leaked = leaked.concat(reachableUnder(findChild(app, "sessionTools")));
            compare(leaked.length, 0, "reachable under " + data.tag + ": " + JSON.stringify(leaked));
            if (data.which === "map")
                app.closeMap(false);
            else if (data.which === "diagnostics")
                app.diagnosticsOpen = false;
            else if (data.which === "callRecordings")
                app.callRecordingsOpen = false;
            else
                app.sessionDestination = "";
            var list = findChild(monitor(), "recentCallsList");
            tryVerify(function () { return list.itemAtIndex(1) !== null; });
            tryVerify(function () {
                return testContext.accessibleNode(list.itemAtIndex(1).node).inTree;
            }, 2000, "uncovered, a recent call is a node again");
        }

        // The device's case: a touch on an empty part of the map's canvas (itself out of the tree), with radios on
        // the map, lands on nothing of the Monitor under it that a screen reader can stop on.
        function test_touch_on_the_canvas_does_not_reach_what_it_covers() {
            testContext.resetPositions();
            testContext.pushPosition({source: 5001, label: "Unit 1", lat: 10.0, lon: 20.0});
            testContext.pushPosition({source: 5002, label: "Unit 2", lat: 10.01, lon: 20.01});
            app.openMap();
            var screen = findChild(app, "mapScreen");
            var canvas = findChild(screen, "mapCanvas");
            screen.tilesPromptDismissed = true;
            tryVerify(function () { return canvas.visible && canvas.markerCount === 2; });
            land(5);
            var hits = 0;
            for (var y = 60; y < canvas.height - 60; y += 40) {
                for (var x of [canvas.width / 4, canvas.width / 2]) {
                    if (canvas.trackAt(x, y) !== 0)
                        continue;
                    var hit = testContext.accessibleNodeAt(canvas, x, y);
                    if (!hit.exists)
                        continue;
                    hits++;
                    // A row root the view put back is a bare node with nothing to say or do, which TalkBack
                    // does not stop on; what a touch must never find under the map is a node that offers one.
                    if (Ui.Navigation.contains(monitor(), hit.object)) {
                        var node = testContext.accessibleNode(hit.object);
                        verify(node.name === "" && !node.focusable && (node.actions || []).length === 0
                               && controlRoles.indexOf(node.role) < 0,
                               "a touch at (" + x + ", " + y + ") on the canvas reached the Monitor: " + JSON.stringify(node));
                    }
                }
            }
            verify(hits > 0, "the walk reached the canvas's area");
            testContext.resetPositions();
        }

        // The Map's own List under the position sheet: fixes keep landing and re-lay its rows; none comes back into
        // reach under the sheet.
        function test_map_list_under_the_position_sheet() {
            testContext.resetPositions();
            for (var i = 0; i < 4; ++i)
                testContext.pushPosition({source: 6000 + i, label: "Unit " + i, lat: 10.0 + 0.01 * i, lon: 20.0});
            app.openMap();
            var screen = findChild(app, "mapScreen");
            screen.mapShown = false;
            var rows = findChild(screen, "mapList");
            tryVerify(function () { return rows.itemAtIndex(2) !== null; });
            screen.openTrack(rows.itemAtIndex(0).rowTrack, true);
            var sheet = findChild(screen, "mapPositionSheet");
            verify(sheet.visible);
            for (var r = 0; r < 4; ++r) {
                testContext.pushPosition({source: 6100 + r, label: "New " + r, lat: 10.1, lon: 20.0 + 0.01 * r});
                testContext.pushPosition({source: 6000 + r, label: "Unit " + r, lat: 10.2, lon: 20.0});
            }
            tryCompare(rows, "count", positionView.count);
            waitForRendering(rows);
            var leaked = reachableUnder(rows);
            compare(leaked.length, 0, "List rows reachable under the sheet: " + JSON.stringify(leaked));
            sheet.visible = false;
            tryVerify(function () { return testContext.accessibleNode(rows.itemAtIndex(0).node).inTree; }, 2000,
                      "the sheet closed, the rows are nodes again");
            screen.mapShown = true;
            testContext.resetPositions();
        }
    }
}
