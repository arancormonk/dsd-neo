// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The wizard's trunking answer: a new system starts with it off, and until the
// user answers the switch themselves the decode chip pick suggests it — the
// P25 chips are almost always trunked systems, the others usually are not. An
// explicit answer survives every later chip change, whether it came from the
// user's own toggle, a RadioReference import, or a saved system being edited.
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

        name: "WizardTrunking"
        when: windowShown

        readonly property var wizard: wizardLoader.item

        function init() {
            verify(tc.wizard !== null, "WizardScreen.qml failed to load")
            tc.wizard.openForAdd(false)
        }

        function test_01_a_new_system_starts_with_trunking_off() {
            compare(tc.wizard.trunking, false, "openForAdd must not presume a trunked system")
            tc.wizard.openForFound(null, "851.375")
            compare(tc.wizard.trunking, false, "a system found while exploring is not presumed trunked either")
        }

        function test_02_the_chip_pick_suggests_the_answer() {
            tc.wizard.pickDecodeFlag("-ft")
            compare(tc.wizard.trunking, true, "picking P25 must suggest trunking on")
            tc.wizard.pickDecodeFlag("-fs")
            compare(tc.wizard.trunking, false, "picking DMR must drop the unanswered suggestion")
            tc.wizard.pickDecodeFlag("-mq")
            compare(tc.wizard.trunking, true, "picking P25 Simulcast must suggest trunking on")
            tc.wizard.pickDecodeFlag("")
            compare(tc.wizard.trunking, false, "the Auto chip is not an explicit P25 pick")
        }

        function test_03_the_users_own_answer_survives_chip_changes() {
            tc.wizard.answerTrunking(true)
            tc.wizard.pickDecodeFlag("-fs")
            compare(tc.wizard.trunking, true, "an explicit on must survive a DMR pick")

            tc.wizard.openForAdd(false)
            tc.wizard.pickDecodeFlag("-ft")
            tc.wizard.answerTrunking(false)
            tc.wizard.pickDecodeFlag("-mq")
            compare(tc.wizard.trunking, false, "an explicit off must survive a P25 pick")
        }

        function test_04_a_radioreference_import_is_the_answer() {
            tc.wizard.applyRadioReference({
                                              "name": "Imported System",
                                              "freqMhz": "853.0625",
                                              "decodeFlag": "-fs",
                                              "trunking": true
                                          })
            compare(tc.wizard.trunking, true, "the import must carry the database's trunking answer")
            tc.wizard.pickDecodeFlag("-fy")
            compare(tc.wizard.trunking, true, "a chip pick must not second-guess the imported answer")
        }

        function test_05_editing_keeps_the_saved_answer() {
            savedSystems.add({
                                 "name": "Saved Trunked", "sourceType": "usb", "host": "", "port": 0,
                                 "freqMhz": "851.375", "decodeFlag": "-fs", "trunking": true,
                                 "gainDb": -1, "ppm": "", "bandwidthKhz": -1, "biasTee": -1,
                                 "extraArgs": "", "filePath": "", "chanCsvPath": "",
                                 "groupCsvPath": "", "keyCsvPath": "", "keyCsvHex": false
                             })
            var row = savedSystems.count - 1
            tc.wizard.openForEdit(row)
            compare(tc.wizard.trunking, true, "the edit must show the saved answer")
            tc.wizard.pickDecodeFlag("-fy")
            compare(tc.wizard.trunking, true, "a chip pick must not flip a saved system's answer")
            savedSystems.remove(row)
        }
    }
}
