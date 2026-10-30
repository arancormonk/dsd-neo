// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #782: the Monitor's toggles as TalkBack presses and reads them.
//
// Hold scan, Hold TG and Mute read back a command the decoder applies later: `checked` follows the engine's state,
// which reaches the screen on a later refresh. Qt's Android bridge builds the click event TalkBack reads out as soon as
// the press returns, so each toggle reports the state asked for from the press on, until the engine's state arrives or
// a few seconds pass without it.
Item {
    id: root
    width: 420
    height: 900
    Loader {
        id: monitorLoader
        anchors.fill: parent
        source: uiDir + "/MonitorScreen.qml"
    }
    TestCase {
        name: "A11yMonitorControls"
        when: windowShown
        readonly property var monitor: monitorLoader.item
        property var restore: ({})

        function set(values) {
            for (var key in values) {
                if (!(key in restore))
                    restore[key] = metrics[key];
                testContext.setMetric(key, values[key]);
            }
        }
        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            verify(monitor !== null);
        }
        function init() {
            testContext.setHostRunning(true);
            set({scanTargetCount: 3, scanTargetOrdinal: 1, scanTargetId: "target-1", scanRotationActive: true,
                scanHold: false});
            monitor.visible = true;
            waitForRendering(root);
            wait(20);
        }
        function cleanup() {
            for (var key in restore)
                testContext.setMetric(key, restore[key]);
            restore = {};
            testContext.setHostRunning(false);
        }

        function test_hold_scan_reports_the_state_asked_for() {
            var hold = findChild(monitor, "scanHoldButton");
            verify(hold !== null);
            compare(testContext.accessibleNode(hold).checked, false);
            var calls = testContext.scanHoldCalls();
            verify(testContext.pressAccessible(hold), "TalkBack's press");
            compare(testContext.scanHoldCalls(), calls + 1, "the hold is asked for");
            compare(testContext.accessibleNode(hold).checked, true, "and read as held from the press on");
            // The engine holds: it stays held.
            set({scanHold: true});
            wait(20);
            compare(testContext.accessibleNode(hold).checked, true);
            verify(testContext.pressAccessible(hold));
            compare(testContext.accessibleNode(hold).checked, false, "released from the press on");
            set({scanHold: false});
            wait(20);
            compare(testContext.accessibleNode(hold).checked, false);
            // A press the engine never applies goes back to the engine's state.
            verify(testContext.pressAccessible(hold));
            compare(testContext.accessibleNode(hold).checked, true);
            tryCompare(hold, "shownChecked", false, 3000, "back to the engine's state when the hold never comes");
            compare(testContext.accessibleNode(hold).checked, false);
        }
    }
}
