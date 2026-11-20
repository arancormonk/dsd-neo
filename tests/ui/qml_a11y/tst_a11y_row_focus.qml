// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #800 (P10a): a list row's accessible node is a child of the row (a list view writes Accessible.ignored on the
// row's root). The keyboard must land on that named node, not on the bare root: Tab onto a row, and a sheet the row
// opened handing the focus back on close, each put the focus on a node a screen reader reads (the row's name) and can
// press. Checked for History's call rows (CallRow, the same rows as the Monitor's and Home's) and the Map's List rows.
// The Imports cards and file browser entries are checked in their own suites. Names and places are made up.
Item {
    width: 420
    height: 900
    Loader {
        id: historyLoader
        anchors.fill: parent
        source: uiDir + "/HistoryScreen.qml"
    }
    Loader {
        id: mapLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/MapScreen.qml"
    }
    TestCase {
        name: "A11yRowFocus"
        when: windowShown

        function focusedNode(item) {
            return testContext.accessibleNode(item.Window.window.activeFocusItem);
        }
        // The focused item is a named stop with a press: what TalkBack reads and double-taps.
        function verifyNamedStop(scope, what) {
            var node = focusedNode(scope);
            verify(node.exists && node.name.length > 0, what + ": the focused node has no name: " + JSON.stringify(node));
            verify((node.actions || []).indexOf("Press") >= 0, what + ": the focused node has no press: " + JSON.stringify(node));
            return node;
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1");
        }

        function test_history_rows() {
            mapLoader.active = false;
            historyLoader.active = true;
            callHistory.clearAll();
            for (var i = 0; i < 4; ++i)
                callHistory.pushWithIds(1100 + i, 2100 + i);
            var screen = historyLoader.item;
            var list = findChild(screen, "callLogList");
            tryVerify(function () { return list.itemAtIndex(2) !== null; });
            // Tab from one row to the next.
            list.itemAtIndex(0).forceActiveFocus();
            verifyNamedStop(screen, "a row given the focus");
            keyClick(Qt.Key_Tab);
            var node = verifyNamedStop(screen, "Tab onto a row");
            compare(node.name, list.itemAtIndex(1).node.Accessible.name);
            // Return opens the row's sheet; closing it hands the focus back to the row's named node.
            keyClick(Qt.Key_Return);
            var sheet = findChild(screen, "historyDetailSheet");
            verify(sheet.visible, "Return opens the row's details");
            keyClick(Qt.Key_Escape);
            tryVerify(function () { return !sheet.visible; });
            tryVerify(function () {
                return screen.Window.window.activeFocusItem === list.itemAtIndex(1).node;
            }, 1000, "the focus comes back to the row's node");
            verifyNamedStop(screen, "the sheet's focus return");
            callHistory.clearAll();
        }

        function test_map_list_rows() {
            historyLoader.active = false;
            testContext.resetPositions();
            for (var i = 0; i < 3; ++i)
                testContext.pushPosition({source: 7000 + i, label: "Unit " + i, lat: 10.0 + 0.01 * i, lon: 20.0});
            mapLoader.active = true;
            var screen = mapLoader.item;
            screen.mapShown = false;
            var rows = findChild(screen, "mapList");
            tryVerify(function () { return rows.itemAtIndex(2) !== null; });
            rows.itemAtIndex(0).forceActiveFocus();
            verifyNamedStop(screen, "a List row given the focus");
            keyClick(Qt.Key_Tab);
            verifyNamedStop(screen, "Tab onto a List row");
            var row = rows.itemAtIndex(1);
            verify(row.node.activeFocus, "the second row's node has the focus");
            keyClick(Qt.Key_Return);
            var sheet = findChild(screen, "mapPositionSheet");
            verify(sheet.visible, "Return opens the radio's sheet");
            keyClick(Qt.Key_Escape);
            tryVerify(function () { return !sheet.visible; });
            tryVerify(function () { return row.node.activeFocus; }, 1000, "the focus comes back to the row's node");
            verifyNamedStop(screen, "the sheet's focus return");
            mapLoader.active = false;
            testContext.resetPositions();
        }
    }
}
