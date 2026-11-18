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
        // The nodes under @p root a screen reader can land on now: in the tree and with something to say or do (a
        // name, focus, or a button's press). A nameless, unfocusable text node in the tree offers nothing: that is
        // the #830 rule's floor for a delegate the view put back in the tree.
        function reachableUnder(root) {
            return testContext.accessibleNodesUnder(root).filter(function (node) {
                return node.inTree && (node.name.length > 0 || node.focusable || (node.role >= 43 && node.role <= 45));
            }).map(function (node) {
                return (node.objectName || node.file) + " \"" + node.name + "\"";
            });
        }
        function land(count) {
            for (var i = 0; i < count; ++i)
                callHistory.pushWithIds(4100 + i, 7100 + i);
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
            findChild(history, "historyMapButton").activate();
            verify(app.mapOpen);
            land(5);
            wait(50);
            var leaked = reachableUnder(findChild(app, "sessionTools")).concat(reachableUnder(monitor()));
            compare(leaked.length, 0, "reachable under the map: " + JSON.stringify(leaked));
        }
        // Covered, nothing of the Monitor is in the tree, not even rows laid out while it is covered (calls landing
        // under the screen on top); uncovered again, its rows are back.
        function test_covered_monitor_is_out_of_reach(data) {
            open(data.which);
            wait(20);
            land(5);
            wait(50);
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
                return testContext.accessibleNode(list.itemAtIndex(1)).inTree;
            }, 2000, "uncovered, a recent call is a node again");
        }

        // The device's case: a touch on the map's empty canvas (out of the tree itself) lands on nothing of the
        // Monitor under it.
        function test_touch_on_the_canvas_does_not_reach_the_monitor() {
            app.openMap();
            land(5);
            var screen = findChild(app, "mapScreen");
            var canvas = findChild(screen, "mapCanvas");
            for (var y = 40; y < canvas.height; y += 60) {
                var hit = testContext.accessibleNodeAt(canvas, canvas.width / 3, y);
                if (!hit.exists)
                    continue;
                verify(!Ui.Navigation.contains(monitor(), hit.object),
                       "a touch at y " + y + " on the canvas reached the Monitor: " + hit.name);
            }
        }
    }
}
