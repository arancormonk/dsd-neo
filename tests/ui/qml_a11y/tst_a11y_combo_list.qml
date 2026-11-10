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
    // Never opened before its case, with a current choice not the first:
    // the state a screen reader meets on a list's first open.
    Ui.PlexComboBox {
        id: fresh
        objectName: "fresh"
        x: 20
        y: 320
        width: 300
        model: ["Analog only", "Auto", "P25", "DMR"]
        currentIndex: 1
    }
    // Rows named through a text role, as ScanListScreen's pickers are.
    Ui.PlexComboBox {
        id: named
        objectName: "named"
        x: 20
        y: 600
        width: 300
        textRole: "name"
        model: [{name: "Talkgroups.csv", path: "/a"}, {name: "Channels.csv", path: "/b"}]
    }

    TestCase {
        name: "A11yComboList"
        when: windowShown

        function cleanup() {
            choice.popup.close();
            named.popup.close();
            choice.currentIndex = 0;
            named.currentIndex = 0;
        }
        function openList(combo) {
            combo = combo || choice;
            combo.popup.open();
            tryCompare(combo.popup, "opened", true);
            return combo.popup.contentItem;
        }
        function row(list, index) {
            var item = list.itemAtIndex(index);
            verify(item !== null, "row " + index);
            return item;
        }
        // The row's node: a child of its own, as the delegate (a button)
        // writes its own uncheckable state over any set on it. Without one,
        // the delegate, so the state checks below say what is wrong.
        function rowNode(list, index) {
            var item = row(list, index);
            return findChild(item, "plexComboRow") || item;
        }

        // The bridge makes a node clickable with a press or toggle action and
        // focusable from its state; a radio button, checkable and checked
        // while current, is how TalkBack says which choice holds.
        function test_rows_are_nodes_a_screen_reader_can_pick_data() {
            return [{tag: "strings", combo: "choice"}, {tag: "text role", combo: "named"}];
        }
        function test_rows_are_nodes_a_screen_reader_can_pick(data) {
            var combo = data.combo === "named" ? named : choice;
            combo.currentIndex = 1;
            var list = openList(combo);
            for (var i = 0; i < combo.count; ++i) {
                var node = testContext.accessibleNode(rowNode(list, i));
                verify(node.exists && node.inTree, "row " + i + " is in the tree");
                compare(node.name, combo.textAt(i), "row " + i + " is named by its choice");
                compare(node.role, Accessible.RadioButton, "row " + i + " is a radio button");
                verify(node.checkable, "row " + i + " is checkable");
                compare(node.checked, i === combo.currentIndex, "row " + i + " says whether it is the current one");
                verify(node.actions.indexOf("Press") >= 0, "row " + i + " has a press action: " + node.actions);
                verify(node.focusable, "row " + i + " is focusable");
                // Explore by touch lands on that row's node, not a neighbour's.
                var r = row(list, i);
                var touched = testContext.accessibleNodeAt(r, r.width / 2, r.height / 2);
                verify(touched.exists, "a touch on row " + i + " finds a node");
                compare(touched.name, combo.textAt(i), "a touch on row " + i + " finds that row");
                compare(touched.role, Accessible.RadioButton);
            }
        }

        // On Android every row read as not checkable, and on a list's first
        // open the current row as not checked: the button's own state stood
        // in for the row's. TalkBack speaks the state of a checkable item only.
        function test_a_first_open_reads_the_current_row_as_checked() {
            var list = openList(fresh);
            for (var i = 0; i < fresh.count; ++i) {
                var node = testContext.accessibleNode(rowNode(list, i));
                verify(node.checkable, "row " + i + " is checkable on the first open");
                compare(node.checked, i === 1, "row " + i + ": only Auto, the current one, is checked");
            }
            fresh.popup.close();
        }

        function test_a_screen_readers_double_tap_picks_that_row() {
            var list = openList();
            var activated = -1;
            var noteActivated = function (index) {
                activated = index;
            };
            choice.activated.connect(noteActivated);
            verify(testContext.accessibleClick(rowNode(list, 3)), "the bridge has an action to perform");
            choice.activated.disconnect(noteActivated);
            compare(choice.currentIndex, 3, "DMR is chosen, not another row");
            compare(activated, 3, "as a tap on it would report");
            tryCompare(choice.popup, "visible", false);
        }

        // A list keeps its rows built once it closes, and Android can keep its
        // copy of a row's node: a press on it then chooses nothing.
        function test_a_closed_lists_row_takes_no_press() {
            var list = openList();
            var stale = rowNode(list, 2);
            choice.popup.close();
            tryCompare(choice.popup, "visible", false);
            var activated = -1;
            var noteActivated = function (index) {
                activated = index;
            };
            choice.activated.connect(noteActivated);
            testContext.accessibleClick(stale);
            choice.activated.disconnect(noteActivated);
            compare(choice.currentIndex, 0, "the closed list's row did not choose");
            compare(activated, -1, "nothing was reported");
            verify(!testContext.accessibleNode(stale).focusable, "the closed list's row is no swipe stop");
        }
    }
}
