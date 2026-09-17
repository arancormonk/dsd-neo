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
            while (scanLists.count)
                scanLists.remove(0);
            screen.openFor(-1);
        }

        function cleanup() {
            while (scanLists.count)
                scanLists.remove(0);
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
            screen.saveDraft();
            compare(scanLists.getByUid(uid).entries[0].enabled, false);
            verify(scanLists.getByUid(uid).isDraft);
        }

        function test_imported_target_preview_and_save() {
            var source = testContext.writeFixtureCsv("scan-targets.csv",
                "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\nCSV ID,p25-conventional,851500000,,,,,--scan-max-visit-ms 20000\n");
            var result = importedFiles.importFile(source, "Targets.csv", "trunkTargets");
            verify(result.ok, result.detail || "Import failed");
            try {
                screen.draft.name = "Imported";
                screen.selectTargets(result.path);
                verify(screen.csvMode);
                compare(screen.entries.length, 0);
                compare(screen.targetRows.length, 1);
                compare(screen.targetRows[0].id, "CSV ID");
                compare(screen.targetRows[0].dwellMs, -1);
                verify(screen.validate(), screen.validationText);
                screen.save();
                compare(scanLists.get(0).targetSource, "csv");
                compare(scanLists.get(0).targetsCsvPath, result.path);
                screen.openFor(0);
                compare(screen.targetRows.length, 1);
                var preview = visualChild(screen, "scanTargetPreview");
                verify(preview !== null);
                tryVerify(function() { return preview.height > 0; });
            } finally {
                importedFiles.remove(importedFiles.rowForPath(result.path));
            }
        }

        function test_reselect_manual_targets_keeps_entries() {
            screen.addFrequency("First unsaved target", "dmr", "461");
            screen.addFrequency("Second unsaved target", "nxdn", "462");
            var entries = JSON.stringify(screen.entries);
            var source = visualChild(screen, function (item) {
                return item.Accessible.name === "Scan targets";
            });
            verify(source !== null);
            compare(source.currentIndex, 0);
            compare(screen.entries.length, 2);
            compare(screen.draft.targetSource, "entries");

            source.activated(0);

            compare(screen.entries.length, 2);
            compare(JSON.stringify(screen.entries), entries);
            compare(screen.draft.targetSource, "entries");
        }

        function test_reselect_csv_targets_keeps_preview() {
            var fixture = testContext.writeFixtureCsv("scan-targets.csv",
                "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\nCSV ID,p25-conventional,851500000,,,,,--scan-max-visit-ms 20000\n");
            var result = importedFiles.importFile(fixture, "Targets.csv", "trunkTargets");
            verify(result.ok, result.detail || "Import failed");
            try {
                screen.selectTargets(result.path);
                compare(screen.targetRows.length, 1);
                compare(screen.targetRows[0].id, "CSV ID");
                var rows = JSON.stringify(screen.targetRows);
                var source = visualChild(screen, function (item) {
                    return item.Accessible.name === "Scan targets";
                });
                verify(source !== null);
                compare(source.currentIndex, 1);
                compare(screen.draft.targetsCsvPath, result.path);

                source.activated(1);

                compare(screen.draft.targetSource, "csv");
                compare(screen.draft.targetsCsvPath, result.path);
                compare(JSON.stringify(screen.targetRows), rows);

                source.activated(0);
                compare(screen.draft.targetSource, "entries");
                compare(screen.draft.targetsCsvPath, "");
                compare(screen.targetRows.length, 0);
                compare(screen.entries.length, 0);
            } finally {
                importedFiles.remove(importedFiles.rowForPath(result.path));
            }
        }

        function test_switch_to_csv_clears_manual_entries() {
            screen.addFrequency("First unsaved target", "dmr", "461");
            screen.addFrequency("Second unsaved target", "nxdn", "462");
            var source = visualChild(screen, function (item) {
                return item.Accessible.name === "Scan targets";
            });
            verify(source !== null);
            compare(source.currentIndex, 0);
            compare(screen.entries.length, 2);

            source.activated(1);

            compare(screen.draft.targetSource, "csv");
            compare(screen.entries.length, 0);
            compare(screen.draft.targetsCsvPath, "");
            compare(screen.targetRows.length, 0);
        }

        function visualChild(item, name) {
            if (typeof name === "function" ? name(item) : item.objectName === name)
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
            tryVerify(function () {
                field = visualChild(screen, "scanFrequencyName");
                return field !== null;
            });
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

        function test_validate_never_persists() {
            screen.draft.name = "Unsaved";
            screen.addFrequency("Simplex", "p25", "851.5");
            verify(screen.validate(), screen.validationText);
            compare(scanLists.count, 0);
            screen.closed();
            compare(scanLists.count, 0);
        }

        function test_invalid_requires_explicit_draft() {
            screen.draft.name = "Incomplete";
            screen.save();
            compare(scanLists.count, 0);
            verify(screen.validationText.length > 0);
            screen.saveDraft();
            compare(scanLists.count, 1);
            verify(scanLists.get(0).isDraft);
            verify(!scanListStarter.start(scanLists.get(0), null).ok);
        }

        name: "ScanListScreen"
        when: windowShown
    }
}
