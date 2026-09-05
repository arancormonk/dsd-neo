// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// A ListView positions its own delegates: for a vertical list it writes the
// item's x on every layout pass, which silently overwrites an `x` the delegate
// declared. A card that subtracts the screen padding from its width but loses
// the matching x still measures right — it just sits flush against the left
// edge with the whole inset piled up on the right. Nothing in a C++ model test
// sees that, and on a phone it reads as the whole library being off-centre.
//
// So the assertion is on the rendered geometry: each card's gap to the left
// edge equals its gap to the right, and both equal the inset the Import file
// button below the list already uses.
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

        name: "ImportsScreenLayout"
        when: windowShown

        property var screen: null
        property var list: null
        property var importButton: null

        function initTestCase() {
            tc.screen = screenLoader.item
            verify(tc.screen !== null, "ImportsScreen.qml failed to load")
            tc.list = findChild(tc.screen, "importedFilesList")
            verify(tc.list !== null, "the imported-files list is missing")
            tc.importButton = findChild(tc.screen, "importFileButton")
            verify(tc.importButton !== null, "the import button is missing")

            // Two rows, because one card centred by accident on an empty model
            // would prove nothing; the library copies and parses a real file, so
            // the fixture has to be one.
            var groups = testContext.writeFixtureCsv(
                "groups.csv", "id,mode,name\n1001,A,Dispatch\n1002,A,Fireground\n")
            verify(groups.length > 0, "could not write the talkgroup fixture")
            var accepted = importedFiles.importFile(groups, "groups.csv", "group")
            verify(accepted.ok, "the talkgroup fixture did not import")

            var more = testContext.writeFixtureCsv(
                "extra.csv", "id,mode,name\n2001,A,Public Works\n")
            verify(more.length > 0, "could not write the second fixture")
            verify(importedFiles.importFile(more, "extra.csv", "group").ok,
                   "the second fixture did not import")

            tryCompare(tc.list, "count", 2)
            tc.waitForRendering(tc.list)
        }

        function cleanupTestCase() {
            // The library is one model shared by every case in this suite and it
            // persists; leaving rows behind would hand the next file a fixture
            // it never asked for.
            while (importedFiles.count > 0) {
                importedFiles.remove(0)
            }
        }

        function test_the_cards_sit_centred_between_the_screen_edges() {
            var inset = tc.importButton.mapToItem(tc.screen, 0, 0).x
            verify(inset > 0, "the import button is not inset from the edge")

            for (var i = 0; i < tc.list.count; i++) {
                var card = tc.list.itemAtIndex(i)
                verify(card !== null, "row " + i + " has no delegate")

                var left = card.mapToItem(tc.screen, 0, 0).x
                var right = tc.screen.width - (left + card.width)
                compare(left, inset, "row " + i + " does not start where the import button does")
                compare(right, left, "row " + i + " is not centred between the screen edges")
            }
        }

        // The empty-state sentence sits one step further in than the cards, and
        // it measures itself against the view rather than the screen — so it
        // moved with the inset and had to give up a step of its own.
        function test_the_empty_message_stays_a_step_in_from_the_cards() {
            var message = findChild(tc.screen, "importsEmptyMessage")
            verify(message !== null, "the empty-state message is missing")

            var inset = tc.importButton.mapToItem(tc.screen, 0, 0).x
            var card = tc.list.itemAtIndex(0)
            verify(card !== null, "the first row has no delegate")
            compare(message.width, card.width - 2 * inset)
        }
    }

    // Kind selection needs no imported fixture; keep its geometry check independent
    // of the library-card cases and their persistent test data.
    TestCase {
        id: kindTc
        name: "ImportsKindPickerLayout"
        when: windowShown
        readonly property var screen: screenLoader.item
        readonly property var importButton: findChild(screen, "importFileButton")

        function test_kind_labels_fit_at_phone_width() {
            root.width = 360
            kindTc.importButton.clicked()
            var picker = findChild(kindTc.screen, "importKindPicker")
            verify(picker !== null)
            tryVerify(function () { return picker.visible })
            wait(100)
            var kinds = ["chan", "group", "keys", "p25Bandplan", "src"]
            var labels = ["Channel map", "Talkgroups", "Keys", "P25 band plan", "Radio IDs"]
            for (var i = 0; i < kinds.length; i++) {
                var pill = findChild(picker, "importKind_" + kinds[i])
                verify(pill !== null && pill.visible)
                compare(pill.text, labels[i])
                verify(!pill.caret)
                var label = pill.children[0].children[0]
                compare(label.text, labels[i])
                verify(!label.truncated && label.width >= label.contentWidth)
                var inPill = label.mapToItem(pill, 0, 0)
                verify(inPill.x >= 0 && inPill.x + label.width <= pill.width)
                verify(inPill.y >= 0 && inPill.y + label.height <= pill.height)
                var onScreen = pill.mapToItem(kindTc.screen, 0, 0)
                verify(onScreen.x >= 0 && onScreen.x + pill.width <= kindTc.screen.width)
                verify(onScreen.y >= 0 && onScreen.y + pill.height <= kindTc.screen.height)
                pill.clicked()
                compare(kindTc.screen.pendingType, kinds[i])
            }
            root.width = 420
        }

    }
}
