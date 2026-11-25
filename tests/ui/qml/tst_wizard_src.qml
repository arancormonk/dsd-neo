// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The wizard's radio ID list pick. It is the fifth trunking-data field beside
// the channel map, talkgroups, keys and P25 band plan, and it has to travel the same road: a
// new system starts without one, the picker seam assigns it, commit() writes it
// into the saved system, and an edit brings it back — a field that only lived
// in the wizard's own properties would be dropped by the first save and the
// session would start without its --src-csv.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: wizardLoader

        anchors.fill: parent
        source: uiDir + "/WizardScreen.qml"
    }

    TestCase {
        id: tc

        name: "WizardSrc"
        when: windowShown

        readonly property var wizard: wizardLoader.item
        // savedSystems is the production model on shared test storage: a row a
        // case adds has to go again whatever happens (see tst_wizard_trunking).
        property int savedRow: -1

        function init() {
            verify(tc.wizard !== null, "WizardScreen.qml failed to load")
            tc.wizard.openForAdd(false)
        }

        function cleanup() {
            if (tc.savedRow >= 0) {
                savedSystems.remove(tc.savedRow)
                tc.savedRow = -1
            }
        }

        function test_01_a_new_system_has_no_source_list() {
            compare(tc.wizard.srcCsvPath, "", "openForAdd() must start without a radio ID list")
            tc.wizard.openForFound(null, "851.375")
            compare(tc.wizard.srcCsvPath, "", "openForFound() must start without a radio ID list")
        }

        function test_02_the_picker_seam_assigns_it() {
            tc.wizard.assignCsvPath("src", "/data/imports/radio ID list.csv", false)
            compare(tc.wizard.srcCsvPath, "/data/imports/radio ID list.csv")
            // Its own field: the channel map row must not have been written.
            compare(tc.wizard.chanCsvPath, "", "the radio ID list pick landed in the channel map field")
            tc.wizard.assignCsvPath("src", "", false)
            compare(tc.wizard.srcCsvPath, "", "\"None\" must clear the field")
        }

        function test_03_it_round_trips_through_the_saved_system() {
            tc.wizard.nameText = "Radio ID system"
            tc.wizard.assignCsvPath("src", "/data/imports/radio ID list.csv", false)
            tc.wizard.commit()
            tc.savedRow = savedSystems.count - 1
            compare(savedSystems.get(tc.savedRow).srcCsvPath, "/data/imports/radio ID list.csv",
                    "commit() dropped the radio ID list from the saved system")

            tc.wizard.openForAdd(false)
            compare(tc.wizard.srcCsvPath, "", "a fresh add must not inherit the last system's radio ID list")

            tc.wizard.openForEdit(tc.savedRow)
            compare(tc.wizard.srcCsvPath, "/data/imports/radio ID list.csv",
                    "openForEdit() must restore the saved radio ID list")
        }

        // Issue #903: the editor's Remember heard talkgroups switch, on for a new system, saved with it, brought back
        // by an edit and an unsaved change of it asks before it goes.
        function test_04_remember_heard_talkgroups_switch() {
            var row = findChild(tc.wizard, "wizardRememberTalkgroups")
            verify(row !== null, "the editor has the switch")
            compare(row.title, "Remember heard talkgroups")
            verify(row.subtitle.length > 0, "it has a line of help")
            verify(row.height >= 48)
            verify(row.checked, "a new system remembers")
            verify(!tc.wizard.hasUnsavedDraft())
            row.toggled(false)
            verify(!row.checked)
            verify(tc.wizard.hasUnsavedDraft(), "turning it off is an edit")
            tc.wizard.nameText = "Remember test"
            tc.wizard.commit()
            tc.savedRow = savedSystems.count - 1
            compare(savedSystems.get(tc.savedRow).rememberTalkgroups, false)

            tc.wizard.openForAdd(false)
            verify(findChild(tc.wizard, "wizardRememberTalkgroups").checked, "a fresh add remembers again")

            tc.wizard.openForEdit(tc.savedRow)
            row = findChild(tc.wizard, "wizardRememberTalkgroups")
            verify(!row.checked, "an edit brings back the saved answer")
            row.toggled(true)
            tc.wizard.commit()
            compare(savedSystems.get(tc.savedRow).rememberTalkgroups, true)
        }
    }
}
