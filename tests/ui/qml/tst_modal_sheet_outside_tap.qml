// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #718: a sheet closes on a real tap outside its panel only (press and
// release both outside, no drag), and a form sheet ignores outside taps
// altogether and leaves through its own controls, Back or Escape.
Item {
    id: root
    width: 420
    height: 900

    // A control under the gutter: nothing a sheet's scrim receives may reach it.
    MouseArea {
        id: behind
        objectName: "behindControl"
        anchors.fill: parent
        property int presses: 0
        property int releases: 0
        property int clicks: 0
        onPressed: presses++
        onReleased: releases++
        onClicked: clicks++
    }

    Ui.ModalSheet {
        id: plainSheet
        objectName: "plainSheet"
        Text {
            width: parent.width
            text: "Details"
        }
        Ui.OutlineButton {
            width: parent.width
            text: "Close"
            onClicked: plainSheet.visible = false
        }
    }

    Ui.ModalSheet {
        id: formSheet
        objectName: "formSheet"
        closesOnOutsideTap: false
        property int dismissRequests: 0
        dismissHandler: function () {
            formSheet.dismissRequests++;
        }
        Ui.DiscardDialog {
            id: formDiscard
            parent: formSheet
        }
        Ui.PlexTextField {
            id: formField
            objectName: "formField"
            width: parent.width
            label: "Name"
        }
        Ui.PlexComboBox {
            id: formCombo
            objectName: "formCombo"
            width: parent.width
            model: ["One", "Two"]
        }
        Ui.OutlineButton {
            width: parent.width
            text: "Cancel"
            onClicked: formSheet.visible = false
        }
    }

    TestCase {
        name: "ModalSheetOutsideTap"
        when: windowShown

        function init() {
            root.width = 420;
            root.height = 900;
            behind.presses = 0;
            behind.releases = 0;
            behind.clicks = 0;
            formSheet.dismissRequests = 0;
        }
        function cleanup() {
            formDiscard.visible = false;
            formCombo.popup.close();
            plainSheet.visible = false;
            formSheet.visible = false;
        }
        function panel(sheet) {
            return findChild(sheet, "modalSheetScroll").parent;
        }
        function gutterX(sheet) {
            var x = panel(sheet).x / 2;
            verify(panel(sheet).x >= 8, "the panel leaves a gutter beside it");
            verify(!sheet.hitsPanel(x, sheet.height / 2), "the gutter lies outside the panel");
            return x;
        }
        function show(sheet) {
            sheet.open();
            waitForRendering(sheet);
            verify(sheet.visible);
        }

        function test_gutter_tap_dismisses_a_default_sheet() {
            show(plainSheet);
            mouseClick(plainSheet, gutterX(plainSheet), plainSheet.height / 2);
            tryCompare(plainSheet, "visible", false);
            compare(behind.presses, 0, "the tap never reaches the control behind");
        }
        function test_gutter_tap_keeps_a_form_sheet() {
            show(formSheet);
            mouseClick(formSheet, gutterX(formSheet), formSheet.height / 2);
            wait(50);
            verify(formSheet.visible, "a form sheet ignores an outside tap");
            compare(formSheet.dismissRequests, 0, "and does not ask to close");
            verify(!formDiscard.visible);
            compare(behind.presses, 0);
        }
        function test_press_on_panel_released_in_gutter_keeps_sheet() {
            show(plainSheet);
            var p = panel(plainSheet);
            // The panel's padding ring: on the panel, outside every control.
            var startX = p.x + 4, startY = p.y + p.height / 2;
            verify(plainSheet.hitsPanel(startX, startY));
            mousePress(plainSheet, startX, startY);
            mouseMove(plainSheet, gutterX(plainSheet), startY);
            mouseRelease(plainSheet, gutterX(plainSheet), startY);
            wait(50);
            verify(plainSheet.visible, "a press that began on the panel is not an outside tap");
        }
        function test_gutter_drag_keeps_sheet() {
            show(plainSheet);
            var x = gutterX(plainSheet), y = plainSheet.height / 2;
            var far = y + 4 * Qt.styleHints.startDragDistance;
            mousePress(plainSheet, x, y);
            mouseMove(plainSheet, x, y + Qt.styleHints.startDragDistance + 1);
            mouseMove(plainSheet, x, far);
            mouseRelease(plainSheet, x, far);
            wait(50);
            verify(plainSheet.visible, "a drag beside the panel is not a tap");
            compare(behind.presses, 0);
            compare(behind.releases, 0);
            compare(behind.clicks, 0, "a drag released over the screen delivers nothing to it");
        }
        function test_small_jitter_still_taps() {
            show(plainSheet);
            var x = gutterX(plainSheet), y = plainSheet.height / 2;
            mousePress(plainSheet, x, y);
            mouseMove(plainSheet, x, y + 1);
            mouseRelease(plainSheet, x, y + 1);
            tryCompare(plainSheet, "visible", false);
        }
        function test_landscape_gutter() {
            root.width = 800;
            root.height = 360;
            show(plainSheet);
            mouseClick(plainSheet, gutterX(plainSheet), plainSheet.height / 2);
            tryCompare(plainSheet, "visible", false);
            show(formSheet);
            mouseClick(formSheet, gutterX(formSheet), formSheet.height / 2);
            wait(50);
            verify(formSheet.visible);
            compare(formSheet.dismissRequests, 0);
        }
        function test_back_and_escape_reach_the_dismiss_handler() {
            show(formSheet);
            verify(Ui.Navigation.back(formSheet.Window.window));
            compare(formSheet.dismissRequests, 1, "Back asks the form to close");
            formSheet.forceActiveFocus();
            keyClick(Qt.Key_Escape);
            compare(formSheet.dismissRequests, 2, "Escape asks the form to close");
            verify(formSheet.visible, "the form decides; nothing closes it behind its back");
        }
        function test_outside_tap_on_a_nested_prompt_keeps_editing() {
            show(formSheet);
            var discarded = false;
            formDiscard.ask(function () {
                discarded = true;
            });
            waitForRendering(formDiscard);
            verify(formDiscard.visible);
            mouseClick(formDiscard, gutterX(formDiscard), formDiscard.height / 2);
            tryCompare(formDiscard, "visible", false);
            verify(formSheet.visible, "an outside tap only closes the question");
            verify(!discarded, "and means keep editing");
            compare(formSheet.dismissRequests, 0);
        }
        function test_back_closes_a_combo_popup_first() {
            show(formSheet);
            formCombo.forceActiveFocus();
            formCombo.popup.open();
            tryCompare(formCombo.popup, "opened", true);
            verify(Ui.Navigation.back(formSheet.Window.window));
            tryCompare(formCombo.popup, "visible", false);
            verify(formSheet.visible);
            compare(formSheet.dismissRequests, 0, "the first Back only closes the popup");
            tryVerify(function () {
                return formCombo.activeFocus;
            }, 1000, "focus returns to the selector");
        }
    }
}
