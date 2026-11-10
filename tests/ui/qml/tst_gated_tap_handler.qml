// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #831: a tap acts only if its press began, and it ends, while its
// control could take input. A press on a sheet's button also reaches the tap
// handlers beneath it, and a button that closes the sheet on release would
// otherwise hand the same release to the control underneath.
Item {
    id: root
    width: 420
    height: 600

    property int taps: 0
    property int longPresses: 0
    property int clicks: 0

    // A bare handler over the whole window, as the Spectrum's waterfall is.
    Item {
        id: below
        anchors.fill: parent
        Ui.GatedTapHandler {
            id: belowTap
            longPressThreshold: 0.2
            onAllowedTap: root.taps++
            onAllowedLongPress: root.longPresses++
        }
    }
    // A real control, placed under the sheet's Close by the cases.
    Ui.OutlineButton {
        id: underButton
        objectName: "underButton"
        x: 20
        y: 20
        width: 200
        text: "Under"
        onClicked: root.clicks++
    }

    Ui.ModalSheet {
        id: sheet
        objectName: "sheet"
        Text {
            width: parent.width
            text: "Details"
        }
        Ui.OutlineButton {
            id: closeButton
            objectName: "closeButton"
            width: parent.width
            text: "Close"
            onClicked: sheet.visible = false
        }
    }

    TestCase {
        name: "GatedTapHandler"
        when: windowShown

        function init() {
            root.taps = 0;
            root.longPresses = 0;
            root.clicks = 0;
            sheet.visible = false;
            underButton.x = 20;
            underButton.y = 20;
            underButton.width = 200;
        }
        function cleanup() {
            sheet.visible = false;
        }
        // Open the sheet and put the real control exactly under its Close.
        function openOverButton() {
            sheet.open();
            waitForRendering(sheet);
            var at = closeButton.mapToItem(root, 0, 0);
            underButton.x = at.x;
            underButton.y = at.y;
            underButton.width = closeButton.width;
            waitForRendering(root);
            var p = underButton.mapToItem(closeButton, underButton.width / 2, underButton.height / 2);
            verify(closeButton.contains(p), "the control lies under Close");
            return p;
        }

        function test_a_tap_with_nothing_over_it_acts() {
            mouseClick(root, 300, 500);
            compare(root.taps, 1);
            mouseClick(underButton, underButton.width / 2, underButton.height / 2);
            compare(root.clicks, 1);
        }

        function test_a_sheet_closed_on_release_hands_nothing_on() {
            var p = openOverButton();
            mousePress(closeButton, p.x, p.y);
            mouseRelease(closeButton, p.x, p.y);
            compare(sheet.visible, false, "Close closed the sheet");
            wait(30);
            compare(root.taps, 0, "the bare handler beneath did not act on the release");
            compare(root.clicks, 0, "the control beneath did not act on the release");
            // With the sheet gone, a new tap there acts.
            mouseClick(underButton, underButton.width / 2, underButton.height / 2);
            compare(root.clicks, 1);
        }

        function test_a_sheet_closed_while_the_press_is_held_hands_nothing_on() {
            var p = openOverButton();
            mousePress(closeButton, p.x, p.y);
            sheet.visible = false;
            mouseRelease(underButton, underButton.width / 2, underButton.height / 2);
            wait(30);
            compare(root.taps, 0);
            compare(root.clicks, 0);
        }

        function test_a_sheet_opened_while_the_press_is_held_stops_the_tap() {
            mousePress(root, 300, 500);
            sheet.open();
            mouseRelease(root, 300, 500);
            wait(30);
            compare(root.taps, 0, "a tap that ends under a sheet does not act");
        }

        function test_a_long_press_under_a_sheet_is_refused() {
            var p = openOverButton();
            mousePress(closeButton, p.x, p.y);
            wait(400);
            compare(root.longPresses, 0, "a long press under the sheet did not act");
            sheet.visible = false;
            mouseRelease(root, 300, 500);
            mousePress(root, 300, 500);
            wait(400);
            mouseRelease(root, 300, 500);
            compare(root.longPresses, 1, "a long press with nothing over it acts");
        }

        // A press refused under a sheet leaves no state behind for the keyboard
        // or a screen reader, which reach the control's own guarded activation.
        function test_keyboard_and_accessibility_still_activate() {
            var p = openOverButton();
            mousePress(closeButton, p.x, p.y);
            mouseRelease(closeButton, p.x, p.y);
            compare(root.clicks, 0);
            underButton.forceActiveFocus();
            keyClick(Qt.Key_Space);
            compare(root.clicks, 1, "Space activates");
            keyClick(Qt.Key_Return);
            compare(root.clicks, 2, "Return activates");
            underButton.Accessible.pressAction();
            compare(root.clicks, 3, "the accessibility press activates");
        }
    }
}
