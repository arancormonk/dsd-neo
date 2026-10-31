// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #782: TalkBack coming back to a control after a system dialog.
//
// Qt's Android bridge carries out TalkBack's "focus this" as a keyboard focus on the control (the set-focus action) and
// sends TalkBack its "focused" event only from the focus change that follows. A system dialog takes the window's focus
// and gives it back, and Qt gives the keyboard focus back to the same control; TalkBack then asks to focus a control
// that has it already, and no event follows. Here the window loses its focus and gets it back, and
// the set-focus action is asked of the control again: with ScreenReaderFocusRelease enabled (on Android with TalkBack
// on), the control raises Focus; without it (the control case), it raises nothing.
Item {
    id: root
    width: 420
    height: 400
    Ui.OutlineButton {
        id: connect
        objectName: "connectButton"
        width: 200
        text: "Connect RTL-SDR"
    }
    Ui.PlexTextField {
        id: field
        y: 100
        width: 200
        label: "Name"
    }
    TestCase {
        name: "A11yFocusReturn"
        when: windowShown
        property var release: null

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            var component = Qt.createComponent(uiDir + "/ScreenReaderFocusRelease.qml");
            compare(component.status, Component.Ready, component.errorString());
            release = component.createObject(root, {window: root.Window.window});
            verify(release !== null);
        }
        // The window loses its focus to a system dialog, and gets it back.
        function awayAndBack() {
            testContext.sendWindowFocus(root, false);
            compare(root.Window.activeFocusItem, null, "the window's focus is given up");
            testContext.sendWindowFocus(root, true);
            wait(20);
        }
        // Focus events @p item raises as TalkBack asks to focus it.
        function focusEvents(item) {
            testContext.startAccessibilityCount();
            verify(testContext.focusAccessible(item), "the control offers the set-focus action");
            wait(20);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            return testContext.accessibilityCountWithin(item, "Focus");
        }

        function test_a_without_release_focus_never_comes_back() {
            release.enabled = false;
            compare(focusEvents(connect), 1, "TalkBack's first focus lands");
            awayAndBack();
            verify(connect.activeFocus, "Qt gives the control its keyboard focus back");
            compare(focusEvents(connect), 0, "so TalkBack's focus on it changes nothing, and no event follows");
        }

        function test_b_released_focus_comes_back() {
            release.enabled = true;
            connect.focus = false;
            compare(focusEvents(connect), 1, "TalkBack's first focus lands");
            awayAndBack();
            verify(!connect.activeFocus, "the control let go of the focus while the window was away");
            compare(focusEvents(connect), 1, "so TalkBack's focus on it lands again");
        }

        function test_c_a_text_field_keeps_its_focus() {
            release.enabled = true;
            field.input.forceActiveFocus();
            verify(field.input.activeFocus);
            awayAndBack();
            verify(field.input.activeFocus, "a text field keeps its focus, for the keyboard");
        }
    }
}
