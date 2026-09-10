import "../../../src/ui/qt/qml" as Ui
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900

    Ui.ScanListScreen {
        id: screen

        anchors.fill: parent
    }

    TestCase {
        function init() {
            while (scanLists.count)scanLists.remove(0)
            screen.openFor(-1);
        }

        function cleanup() {
            while (scanLists.count)scanLists.remove(0)
        }

        function test_edit_entries_and_persist() {
            screen.draft.name = "Local";
            screen.addFrequency("Simplex", "p25", "851.5");
            compare(screen.entries.length, 1);
            screen.save();
            compare(scanLists.count, 1);
            var uid = scanLists.get(0).uid;
            screen.openFor(0);
            compare(screen.entries[0].freqMhz, "851.5");
            screen.setEntry(0, "enabled", false);
            screen.save();
            compare(scanLists.getByUid(uid).entries[0].enabled, false);
        }

        function visualChild(item, name) {
            if (item.objectName === name)
                return item;

            var kids = item.contentItem ? [item.contentItem] : item.children || [];
            for (var i = 0; i < kids.length; ++i) {
                var found = visualChild(kids[i], name);
                if (found)
                    return found;

            }
            return null;
        }

        function test_edit_keeps_row_and_focus() {
            screen.addFrequency("First", "dmr", "461");
            wait(20);
            var row = visualChild(screen, "scanEntryRow");
            verify(row !== null);
            row.forceActiveFocus();
            screen.setEntry(0, "name", "Second");
            wait(20);
            compare(visualChild(screen, "scanEntryRow"), row);
            verify(row.activeFocus);
        }

        function test_typing_keeps_cursor_and_persists_each_edit() {
            screen.draft.name = "Typing";
            screen.addFrequency("First", "dmr", "461");
            var field = null;
            tryVerify(function() { field = visualChild(screen, "scanFrequencyName"); return field !== null; });
            field.forceActiveFocus();
            field.cursorPosition = 2;
            keyClick(Qt.Key_X, Qt.ShiftModifier);
            compare(screen.entries[0].name, "Fixrst");
            compare(field.cursorPosition, 3);
            verify(field.activeFocus);
            keyClick(Qt.Key_Y, Qt.ShiftModifier);
            compare(screen.entries[0].name, "Fixyrst");
            compare(field.cursorPosition, 4);
            verify(field.activeFocus);
            compare(visualChild(screen, "scanFrequencyName"), field);
            screen.save();
            compare(scanLists.get(0).entries[0].name, "Fixyrst");
        }

        function test_reorder_remove() {
            screen.addFrequency("First", "dmr", "461");
            screen.addFrequency("Second", "nxdn", "462");
            screen.moveEntry(1, -1);
            compare(screen.entries[0].name, "Second");
            screen.removeEntry(0);
            compare(screen.entries.length, 1);
        }

        name: "ScanListScreen"
        when: windowShown
    }

}
