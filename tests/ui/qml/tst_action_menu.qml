// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: testRoot
    width: 420
    height: 900

    Ui.OutlineButton {
        id: behind
        anchors.fill: parent
        text: "Underlying action"
        enabled: !menu.visible
    }
    Ui.ActionMenu {
        id: menu
        title: "County radio"
        actions: [
            {text: "Edit", description: "Change this system"},
            {text: "Remove…", destructive: true},
            {text: "Unavailable", enabled: false}
        ]
    }
    SignalSpy { id: triggered; target: menu; signalName: "triggered" }
    SignalSpy { id: cancelled; target: menu; signalName: "cancelled" }
    SignalSpy { id: underlying; target: behind; signalName: "clicked" }

    TestCase {
        name: "ActionMenu"
        when: windowShown

        function init() {
            menu.open();
            triggered.clear();
            cancelled.clear();
            underlying.clear();
            waitForRendering(menu);
        }
        function cleanup() {
            menu.visible = false;
            testRoot.height = 900;
        }
        function row(index) {
            var button = findChild(menu, "actionMenuAction" + index);
            verify(button !== null);
            return button;
        }
        function test_rows_and_tokens() {
            compare(menu.accessibleName, "County radio");
            compare(row(0).text, "Edit");
            compare(row(1).text, "Remove…");
            compare(row(1).textColor, Ui.Theme.magenta);
            compare(row(0).textColor, Ui.Theme.buttonSecondaryText);
            compare(findChild(menu, "actionMenuDescription0").text, "Change this system");
            verify(!row(2).enabled);
            mouseClick(row(2));
            row(2).clicked();
            menu.activate(-1);
            menu.activate(3);
            compare(triggered.count, 0);
            verify(menu.visible);
            compare(findChild(menu, "actionMenuCancel").text, "Cancel");
        }
        function test_activation_once() {
            var button = row(1);
            mouseClick(button);
            compare(triggered.count, 1);
            compare(triggered.signalArguments[0][0], 1);
            verify(!menu.visible);
            button.clicked();
            menu.activate(1);
            menu.cancel();
            compare(triggered.count, 1);
            compare(cancelled.count, 0);
            compare(underlying.count, 0);
        }
        function test_drag_from_action_row() {
            testRoot.height = 240;
            verify(waitForPolish(menu));
            var scroll = findChild(menu, "modalSheetScroll");
            verify(scroll.contentHeight > scroll.height);
            compare(scroll.contentY, 0);
            var button = row(0);
            mouseDrag(button, button.width / 2, button.height / 2, 0, -70, Qt.LeftButton);
            tryVerify(function () { return scroll.contentY > 0; });
            tryCompare(scroll, "moving", false);
            verify(menu.visible);
            compare(triggered.count, 0);
            compare(underlying.count, 0);
        }
        function test_cancel_data() {
            return [{tag: "Cancel"}, {tag: "Back"}, {tag: "Escape"}, {tag: "scrim"}];
        }
        function test_cancel(data) {
            if (data.tag === "Cancel")
                mouseClick(findChild(menu, "actionMenuCancel"));
            else if (data.tag === "Back")
                keyClick(Qt.Key_Back);
            else if (data.tag === "Escape")
                keyClick(Qt.Key_Escape);
            else
                mouseClick(menu, 2, 2);
            tryCompare(menu, "visible", false);
            compare(triggered.count, 0);
            compare(cancelled.count, 1);
            compare(underlying.count, 0);
            menu.cancel();
            compare(cancelled.count, 1);
        }
    }
}
