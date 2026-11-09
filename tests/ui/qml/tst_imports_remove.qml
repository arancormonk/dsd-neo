// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #801: removing a library file lets go of it in order (saved systems, then scan lists, then the file and its
// row), and stops at the first step that fails with a notice that says what happened. Nothing reports "Removed"
// until every step is done, and removing again finishes what is left.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: screenLoader

        anchors.fill: parent
        source: uiDir + "/ImportsScreen.qml"
    }

    TestCase {
        id: tc

        name: "ImportsRemove"
        when: windowShown

        property var screen: null

        function initTestCase() {
            tc.screen = screenLoader.item;
            verify(tc.screen !== null, "ImportsScreen.qml failed to load");
        }

        function cleanup() {
            for (var i = savedSystems.count - 1; i >= 0; i--) {
                if (savedSystems.get(i).name === "Order system")
                    savedSystems.remove(i);
            }
            for (var j = scanLists.count - 1; j >= 0; j--) {
                if (scanLists.get(j).name === "Order list" || scanLists.get(j).name === "Fallback list")
                    scanLists.remove(j);
            }
            // Bounded: a removal that fails (a path left blocked) must fail the case, not spin forever.
            for (var k = importedFiles.count; k > 0 && importedFiles.count > 0; k--)
                importedFiles.remove(0);
            compare(importedFiles.count, 0, "the library did not empty");
        }

        // Opens the confirmation for @p path's row and confirms it, as a tap on Remove then Remove file does.
        function removeFile(path) {
            tc.screen.actionRow = importedFiles.rowForPath(path);
            var sheet = findChild(tc.screen, "importsRemoveSheet");
            verify(sheet !== null, "the remove confirmation is missing");
            sheet.visible = true;
            var button = findChild(sheet, "importsRemoveFileButton");
            verify(button !== null && button.enabled, "Remove file is missing or disabled");
            button.clicked();
            verify(!sheet.visible);
        }

        function block(path, blocked) {
            verify(testContext.blockPath(path, blocked), (blocked ? "could not block " : "could not unblock ") + path);
        }

        function test_import_remove_order() {
            var source = testContext.writeFixtureCsv("order.csv", "id,mode,name\n1001,A,Dispatch\n");
            var imported = testContext.importNow(source, "group");
            verify(imported.ok, "the fixture did not import");
            var path = imported.path;
            verify(savedSystems.add({name: "Order system", sourceType: "usb", groupCsvPath: path}));
            verify(scanLists.add({name: "Order list", targetSource: "csv", targetsCsvPath: path}));

            // The systems cannot be saved: nothing changes.
            var systemsStore = testContext.storePath("saved_systems.json");
            block(systemsStore, true);
            removeFile(path);
            block(systemsStore, false);
            compare(tc.screen.notice, "Could not save the systems that use this file, so nothing was removed. Try again.");
            verify(tc.screen.noticeIsProblem);
            compare(savedSystems.systemsReferencingPath(path), ["Order system"]);
            compare(scanLists.listsReferencingPath(path), ["Order list"]);
            verify(importedFiles.rowForPath(path) >= 0 && testContext.fileExists(path), "the file must stay");

            // The lists cannot be saved: the systems let go, the file stays.
            var listsStore = testContext.storePath("scan_lists.json");
            block(listsStore, true);
            removeFile(path);
            block(listsStore, false);
            compare(tc.screen.notice, "Could not save the scan lists that use this file, so the file was kept. Remove it again to finish.");
            verify(tc.screen.noticeIsProblem);
            compare(savedSystems.systemsReferencingPath(path).length, 0);
            compare(scanLists.listsReferencingPath(path), ["Order list"]);
            verify(importedFiles.rowForPath(path) >= 0 && testContext.fileExists(path), "the file must stay");

            // The file will not delete: its row stays, so it can be removed again.
            block(path, true);
            removeFile(path);
            block(path, false);
            compare(tc.screen.notice, "The file was not fully removed. Remove it again to finish.");
            verify(tc.screen.noticeIsProblem);
            compare(scanLists.listsReferencingPath(path).length, 0);
            verify(importedFiles.rowForPath(path) >= 0 && testContext.fileExists(path), "the row must stay");

            // Removing again finishes it.
            removeFile(path);
            compare(tc.screen.notice, "Removed");
            verify(!tc.screen.noticeIsProblem);
            compare(tc.screen.actionRow, -1);
            compare(importedFiles.rowForPath(path), -1);
            verify(!testContext.fileExists(path), "the file must be deleted");
        }

        // A list that uses the file for its fallback talkgroups is named before the removal, and keeps running
        // without the file afterwards: the field empties and the list is no draft.
        function test_remove_names_and_clears_fallback_lists() {
            var source = testContext.writeFixtureCsv("fallback.csv", "id,mode,name\n1001,A,Dispatch\n");
            var imported = testContext.importNow(source, "group");
            verify(imported.ok, "the fixture did not import");
            var path = imported.path;
            verify(scanLists.add({name: "Fallback list", groupCsvPath: path}));
            tc.screen.actionRow = importedFiles.rowForPath(path);
            var sheet = findChild(tc.screen, "importsRemoveSheet");
            sheet.visible = true;
            var line = findChild(sheet, "importsRemoveFallbackLists");
            verify(line !== null, "the remove confirmation does not name fallback lists");
            verify(line.visible);
            compare(line.text, "Used by scan lists for talkgroups or radio IDs: Fallback list. Removing clears it from those lists.");
            sheet.visible = false;
            removeFile(path);
            compare(tc.screen.notice, "Removed");
            var list = scanLists.get(scanLists.count - 1);
            compare(list.name, "Fallback list");
            compare(list.groupCsvPath, "");
            verify(!list.isDraft);
        }
    }
}
