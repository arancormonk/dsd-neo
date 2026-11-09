// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

Item {
    id: root
    width: 420
    height: 900

    Loader {
        id: appLoader
        anchors.fill: parent
        source: uiDir + "/Main.qml"
    }

    TestCase {
        id: tc
        name: "MainLiveCsvApply"
        when: windowShown
        property int savedRow: -1

        function cleanup() {
            testContext.setHostRunning(false)
            appLoader.item.wizardOpen = false
            appLoader.item.sessionRow = -1
            appLoader.item.sessionSystem = null
            if (savedRow >= 0) {
                savedSystems.remove(savedRow)
                savedRow = -1
            }
            testContext.resetCommands()
        }

        function test_src_import_clear_and_name_only_save() {
            var mainRoot = appLoader.item
            verify(mainRoot !== null, "Main.qml failed to load")
            var wizard = findChild(mainRoot, "wizardScreen")
            verify(wizard !== null)
            wizard.openForAdd(false)
            wizard.nameText = "Live radio IDs"
            wizard.commit()
            savedRow = savedSystems.count - 1
            mainRoot.sessionRow = savedRow
            mainRoot.sessionSystem = savedSystems.get(savedRow)
            testContext.setHostRunning(true)
            testContext.resetCommands()

            var path = "/data/imports/radio IDs.csv"
            // Header long-press was removed; the visible menu route still edits
            // the running saved system and applies its CSV changes live.
            var monitor = findChild(mainRoot, "monitorScreen")
            var title = findChild(monitor, "monitorHeaderTitle")
            mousePress(title, title.width / 2, title.height / 2)
            wait(1000)
            mouseRelease(title, title.width / 2, title.height / 2)
            verify(!mainRoot.wizardOpen)
            compare(mainRoot.sessionRow, savedRow)
            monitor.openSessionMenu()
            verify(findChild(mainRoot, "sessionMenu").visible)
            findChild(findChild(mainRoot, "sessionMenu"), "sessionMenuEdit").activate()
            verify(mainRoot.wizardOpen)
            wizard.assignCsvPath("src", path, false)
            wizard.commit()
            compare(commands.srcImportCalls(), 1)
            compare(commands.lastSrcPath(), path)
            compare(commands.srcClearCalls(), 0)
            compare(mainRoot.sessionSystem.srcCsvPath, path)

            testContext.resetCommands()
            wizard.openForEdit(savedRow)
            wizard.assignCsvPath("src", "", false)
            wizard.commit()
            compare(commands.srcImportCalls(), 0)
            compare(commands.srcClearCalls(), 1)
            compare(mainRoot.sessionSystem.srcCsvPath, "")

            testContext.resetCommands()
            wizard.openForEdit(savedRow)
            wizard.nameText = "Renamed live system"
            wizard.commit()
            compare(commands.srcImportCalls(), 0)
            compare(commands.srcClearCalls(), 0)
            compare(mainRoot.sessionSystem.name, "Renamed live system")
        }

        // Issue #748: the import refuses a talkgroup list whose rows all fail to load, so switching the running
        // system to one posted an import that left the previous list live, and the previous file then took this
        // system's edits. Such a list clears the session's list instead. A list with no rows at all imports: the
        // decoder takes it as the empty list it is, saved to that file. What the file holds is read from the file
        // now, not from its import: the decoder rewrites a list in place, and a file can be updated.
        function test_group_list_switch_data() {
            return [
                {tag: "rows", imported: "id,mode,name\n1001,A,Dispatch\n", now: "", want: "importGroupList"},
                {tag: "header only", imported: "id,mode,name\n", now: "", want: "importGroupList"},
                {tag: "emptied since its import", imported: "id,mode,name\n1001,A,Dispatch\n",
                 now: "id,mode,name\n", want: "importGroupList"},
                {tag: "no usable rows", imported: "id;mode;name\n1001;A;Dispatch\n", now: "", want: "clearGroupList"},
                {tag: "made unusable since its import", imported: "id,mode,name\n1001,A,Dispatch\n",
                 now: "id;mode;name\n1001;A;Dispatch\n", want: "clearGroupList"},
                {tag: "fixed since its import", imported: "id;mode;name\n1001;A;Dispatch\n",
                 now: "id,mode,name\n1001,A,Dispatch\n", want: "importGroupList"}
            ]
        }
        function test_group_list_switch(data) {
            var mainRoot = appLoader.item
            var wizard = findChild(mainRoot, "wizardScreen")
            var source = testContext.writeFixtureCsv("switch-" + data.tag.replace(/ /g, "-") + ".csv", data.imported)
            var imported = importedFiles.importFile(source, "switch.csv", "group")
            verify(imported.ok, "the talkgroup fixture did not import")
            var path = imported.path
            try {
                if (data.now.length > 0)
                    verify(testContext.rewriteFile(path, data.now), "could not rewrite the library file")
                wizard.openForAdd(false)
                wizard.nameText = "Live talkgroups"
                wizard.commit()
                savedRow = savedSystems.count - 1
                mainRoot.sessionRow = savedRow
                mainRoot.sessionSystem = savedSystems.get(savedRow)
                testContext.setHostRunning(true)
                testContext.resetCommands()

                wizard.openForEdit(savedRow)
                wizard.assignCsvPath("group", path, false)
                wizard.commit()
                compare(commands.listCommands(), [data.want === "clearGroupList" ? data.want : data.want + " " + path])
                compare(mainRoot.sessionSystem.groupCsvPath, path)

                // Back to None clears, whatever the file held.
                testContext.resetCommands()
                wizard.openForEdit(savedRow)
                wizard.assignCsvPath("group", "", false)
                wizard.commit()
                compare(commands.listCommands(), ["clearGroupList"])
            } finally {
                importedFiles.remove(importedFiles.rowForPath(path))
            }
        }

        // A path the library does not hold cannot be read here; the import goes ahead and the decoder says why it
        // refuses, if it does.
        function test_group_list_switch_to_a_file_outside_the_library() {
            var mainRoot = appLoader.item
            var wizard = findChild(mainRoot, "wizardScreen")
            wizard.openForAdd(false)
            wizard.nameText = "Outside talkgroups"
            wizard.commit()
            savedRow = savedSystems.count - 1
            mainRoot.sessionRow = savedRow
            mainRoot.sessionSystem = savedSystems.get(savedRow)
            testContext.setHostRunning(true)
            testContext.resetCommands()

            var path = "/data/imports/not in the library.csv"
            wizard.openForEdit(savedRow)
            wizard.assignCsvPath("group", path, false)
            wizard.commit()
            compare(commands.listCommands(), ["importGroupList " + path])
        }
    }
}
