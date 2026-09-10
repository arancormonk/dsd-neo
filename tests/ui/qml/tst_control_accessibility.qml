// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    width: 360
    height: 700
    Ui.ToggleRow {
        id: toggle
        title: "Background listening"
        subtitle: "Keep listening while other apps are open. A persistent notification shows the session."
        checked: false
        onToggled: function (value) {
            checked = value;
        }
    }
    Ui.OutlineButton {
        id: button
        y: toggle.height + 20
        width: 160
        text: "Save draft"
    }
    Ui.DashedActionButton {
        id: addButton
        width: 300
        y: 350
        text: "Add a system"
    }
    Ui.PlexFlickable {
        id: scroll
        width: 300
        height: 100
        y: 500
        contentHeight: 500
    }
    Ui.PlexComboBox {
        id: choices
        width: 240
        y: 600
        model: ["Use received identifiers"]
    }
    TestCase {
        name: "ControlAccessibility"
        when: windowShown
        function cleanup() {
            Ui.Theme.resetFontScale();
            toggle.checked = false;
        }
        function test_row_label_and_accessible_action_toggle_once() {
            mouseClick(toggle, 25, toggle.height / 2);
            verify(toggle.checked);
            toggle.Accessible.pressAction();
            verify(!toggle.checked);
            compare(toggle.Accessible.name, toggle.title);
            verify(toggle.height >= 48);
        }
        function test_keyboard_activation_and_disabled_action() {
            var presses = 0;
            function count() {
                presses++;
            }
            button.clicked.connect(count);
            button.forceActiveFocus();
            keyClick(Qt.Key_Space);
            compare(presses, 1);
            button.enabled = false;
            button.Accessible.pressAction();
            compare(presses, 1);
            button.enabled = true;
            button.clicked.disconnect(count);
        }
        function test_accessible_scroll_actions() {
            scroll.contentY = 0;
            scroll.Accessible.scrollDownAction();
            verify(scroll.contentY > 0 && scroll.contentY <= 400);
            scroll.Accessible.scrollUpAction();
            compare(scroll.contentY, 0);
        }
        function test_add_action_is_accessible() {
            var count = 0;
            function added() {
                count++;
            }
            addButton.clicked.connect(added);
            addButton.Accessible.pressAction();
            compare(count, 1);
            compare(addButton.Accessible.name, "Add a system");
            addButton.clicked.disconnect(added);
        }
        function test_larger_text_grows_controls() {
            Ui.Theme.fontScale = 1;
            var original = toggle.height;
            Ui.Theme.fontScale = 2;
            tryVerify(function () {
                return toggle.height > original;
            });
            verify(button.height >= 48);
        }
        function test_selected_choice_wraps_at_large_text() {
            Ui.Theme.fontScale = 1;
            var original = choices.height;
            Ui.Theme.fontScale = 2;
            tryVerify(function () {
                return choices.height > original && choices.contentItem.lineCount > 1;
            });
            verify(choices.contentItem.paintedHeight <= choices.contentItem.height);
        }
    }
}
