// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #782: TalkBack swiping from a control onto a container (a sheet's panel, a scrolling pane) and back.
//
// Qt's Android bridge carries out TalkBack's focus action with the node's set-focus action (a keyboard focus) when the
// node offers one, and tells TalkBack the focus landed only from the focus change that follows; on a node without one it
// lands TalkBack's focus itself and leaves the keyboard focus where it was. A container that offered none left the
// control holding the keyboard focus, so TalkBack's focus back onto that control changed nothing, raised nothing, and
// TalkBack stuck there until a touch: from a sheet's panel ("Dialog") onto its Done button, from the Monitor's recent
// calls pane onto Stop listening, from Home's scrolling pane onto its Listen heading. Each container now takes the
// keyboard focus as TalkBack lands on it, so the way back is a real change and is heard.
Item {
    id: root
    width: 420
    height: 900
    Loader {
        id: homeLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/HomeScreen.qml"
    }
    Loader {
        id: monitorLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/MonitorScreen.qml"
    }
    QtObject {
        id: fakeNet
        property bool active: false
        property var neighbours: []
        property var patches: []
        property var affiliations: []
        property var radios: []
    }
    Loader {
        id: networkLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/NetworkSheet.qml"
        onLoaded: item.network = fakeNet
    }
    TestCase {
        name: "A11yContainerFocus"
        when: windowShown

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
        }
        function cleanup() {
            Ui.Navigation.modals = [];
            homeLoader.active = false;
            monitorLoader.active = false;
            networkLoader.active = false;
            testContext.setHostRunning(false);
        }
        function item(scope, name) {
            var found = findChild(scope, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        // TalkBack's focus on @p node: its set-focus action when it offers one, else the bridge lands the focus itself
        // and Qt is not asked (QtAccessibilityDelegate.performActionForVirtualViewId). Returns the Focus events @p heard
        // raised.
        function talkBackFocus(node, heard) {
            testContext.startAccessibilityCount();
            testContext.focusAccessible(node);
            wait(20);
            var focus = testContext.accessibilityCountWithin(heard, "Focus");
            testContext.stopAccessibilityCount();
            return focus;
        }
        // TalkBack on @p control, then onto @p container, then back: the way back is heard.
        function roundTrip(control, container, what) {
            verify(testContext.accessibleNode(control).inTree, what + ": the control is in the tree");
            verify(testContext.accessibleNode(container).inTree, what + ": the container is in the tree");
            if (!control.activeFocus)
                compare(talkBackFocus(control, control), 1, what + ": TalkBack's first focus lands");
            talkBackFocus(container, container);
            verify(!control.activeFocus, what + ": the container took the keyboard focus from the control");
            compare(talkBackFocus(control, control), 1, what + ": TalkBack's focus back onto the control lands");
        }

        function test_home_heading_and_its_pane() {
            homeLoader.active = true;
            waitForRendering(homeLoader.item);
            wait(20);
            // The Listen heading: pinned above the body with a brokered receiver (listenHeaderTitle, as on the phone),
            // else the body's own first line, the same heading.
            var title = homeLoader.item.pinnedHeader ? item(homeLoader.item, "listenHeaderTitle")
                                                     : item(homeLoader.item, "listenTitle");
            roundTrip(title, item(homeLoader.item, "dongleScrollBody"), "Home");
        }

        function test_monitor_stop_and_recent_calls() {
            testContext.setHostRunning(true);
            monitorLoader.active = true;
            waitForRendering(monitorLoader.item);
            wait(20);
            roundTrip(item(monitorLoader.item, "stopListeningButton"), item(monitorLoader.item, "recentCallsList"),
                      "Monitor");
        }

        function test_sheet_done_and_its_panel() {
            networkLoader.active = true;
            var sheet = networkLoader.item;
            sheet.open();
            waitForRendering(sheet);
            wait(20);
            compare(Ui.Navigation.topModal, sheet);
            var done = item(sheet, "networkDone");
            roundTrip(done, sheet, "Network sheet panel");
            roundTrip(done, item(sheet, "modalSheetScroll"), "Network sheet pane");
            sheet.visible = false;
        }
    }
}
