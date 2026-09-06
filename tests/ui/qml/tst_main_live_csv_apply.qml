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
            wizard.openForEdit(savedRow)
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
    }
}
