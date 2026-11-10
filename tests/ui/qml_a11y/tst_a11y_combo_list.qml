// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #831: each row of an open choice list is a node a screen reader can
// focus and pick. A Controls ItemDelegate is a ListItem, which carries no
// press action, so Qt's Android bridge built each row as a TextView that is
// neither focusable nor clickable: TalkBack could not settle on "DMR", and its
// double tap went to whatever row lay at the screen's centre.
Item {
    id: root
    width: 420
    height: 900

    Ui.PlexComboBox {
        id: choice
        objectName: "choice"
        x: 20
        y: 40
        width: 300
        model: ["Analog only", "Auto", "P25", "DMR"]
    }

    TestCase {
        name: "A11yComboList"
        when: windowShown

        function cleanup() {
            choice.popup.close();
            choice.currentIndex = 0;
        }
        function openList() {
            choice.popup.open();
            tryCompare(choice.popup, "opened", true);
            return choice.popup.contentItem;
        }
        function row(list, index) {
            var item = list.itemAtIndex(index);
            verify(item !== null, "row " + index);
            return item;
        }

        // The bridge's own test: clickable with a press or toggle action,
        // never for a static text or heading; focusable from the state.
        function test_rows_are_nodes_a_screen_reader_can_pick() {
            var list = openList();
            for (var i = 0; i < choice.count; ++i) {
                var node = testContext.accessibleNode(row(list, i));
                verify(node.exists && node.inTree, "row " + i + " is in the tree");
                compare(node.name, choice.textAt(i), "row " + i + " is named by its choice");
                verify(node.role !== Accessible.StaticText && node.role !== Accessible.Heading, "row " + i);
                verify(node.actions.indexOf("Press") >= 0, "row " + i + " has a press action: " + node.actions);
                verify(node.focusable, "row " + i + " is focusable");
                compare(node.checked, i === choice.currentIndex, "row " + i + " says whether it is the current one");
            }
        }

        function test_a_screen_readers_double_tap_picks_that_row() {
            var list = openList();
            var activated = -1;
            var noteActivated = function (index) {
                activated = index;
            };
            choice.activated.connect(noteActivated);
            verify(testContext.accessibleClick(row(list, 3)), "the bridge has an action to perform");
            choice.activated.disconnect(noteActivated);
            compare(choice.currentIndex, 3, "DMR is chosen, not another row");
            compare(activated, 3, "as a tap on it would report");
            tryCompare(choice.popup, "visible", false);
        }
    }
}
