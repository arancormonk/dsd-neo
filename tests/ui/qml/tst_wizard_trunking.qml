// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The wizard's trunking answer. Until the user answers the switch themselves it
// is a suggestion, drawn from the decode chip when the chip names a system type
// — the P25 chips are almost always trunked systems — and from the frequency's
// band when it does not. The band matters because Auto IS the default chip: a
// user who accepts the wizard's own 851.375 prefill taps no chip at all, so a
// chip-only suggestion never runs and the shipped defaults produce a decoder
// locked to a control channel with call-following off, playing nothing.
//
// An explicit answer ends the guessing for good, and survives every later chip
// pick and frequency edit, whether it came from the user's own toggle, a
// RadioReference import, or a saved system being edited.
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
        // savedSystems is the production model on shared test storage, so a row
        // a case adds has to go again whatever happens — a compare() that fails
        // throws, and a row left behind would follow every later file in the
        // same binary. Recorded here, dropped in cleanup().
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

        // The shipped defaults have to decode. openForAdd() prefills 851.375 —
        // an 800 MHz P25 control channel, which is why it is the prefill — and
        // leaves Auto selected, so nothing else in the flow ever answers the
        // trunking question for a user who accepts them.
        function test_01_the_prefilled_control_channel_suggests_trunking() {
            compare(tc.wizard.trunking, true,
                    "the 800 MHz prefill plus the Auto chip produced a silent session")

            // Worse for a found system, not better: a carrier that stands out on
            // a 700/800 waterfall is most often a constant control channel.
            tc.wizard.openForFound(null, "851.375")
            compare(tc.wizard.trunking, true,
                    "a control channel found while exploring is presumed conventional")
        }

        // The suggestion is the band's, not a blanket default. VHF and UHF carry
        // conventional repeater pairs at least as often as trunked sites, so
        // there the switch stays off until something answers it.
        function test_02_only_the_trunked_bands_suggest_it() {
            tc.wizard.freqText = "154.0"
            compare(tc.wizard.trunking, false, "VHF must not be presumed trunked")
            tc.wizard.freqText = "453.0"
            compare(tc.wizard.trunking, false, "UHF must not be presumed trunked")
            tc.wizard.freqText = "770.0"
            compare(tc.wizard.trunking, true, "700 MHz is a trunked band")

            // An empty or half-typed field parses to NaN, which must read as
            // "no band, no suggestion" rather than as 0 Hz.
            tc.wizard.freqText = ""
            compare(tc.wizard.trunking, false, "an empty frequency suggested something")
            tc.wizard.freqText = "8."
            compare(tc.wizard.trunking, false, "a half-typed frequency suggested something")
        }

        // A chip that names a system type has said more than the band can, in
        // both directions.
        function test_03_the_chip_pick_outranks_the_band() {
            tc.wizard.pickDecodeFlag("-ft")
            compare(tc.wizard.trunking, true, "picking P25 must suggest trunking on")
            tc.wizard.pickDecodeFlag("-fs")
            compare(tc.wizard.trunking, false,
                    "an explicit DMR pick must outrank the 800 MHz band")
            tc.wizard.pickDecodeFlag("-mq")
            compare(tc.wizard.trunking, true, "picking P25 Simulcast must suggest trunking on")
            tc.wizard.pickDecodeFlag("")
            compare(tc.wizard.trunking, true,
                    "returning to Auto must fall back to the band, not to off")

            // And a P25 pick is trunked wherever it is tuned — the band only
            // speaks for the chip that names no system type.
            tc.wizard.freqText = "154.0"
            tc.wizard.pickDecodeFlag("-ft")
            compare(tc.wizard.trunking, true,
                    "an explicit P25 pick must hold outside the trunked bands")
        }

        function test_04_the_users_own_answer_survives_everything() {
            tc.wizard.answerTrunking(true)
            tc.wizard.pickDecodeFlag("-fs")
            compare(tc.wizard.trunking, true, "an explicit on must survive a DMR pick")

            tc.wizard.openForAdd(false)
            tc.wizard.pickDecodeFlag("-ft")
            tc.wizard.answerTrunking(false)
            tc.wizard.pickDecodeFlag("-mq")
            compare(tc.wizard.trunking, false, "an explicit off must survive a P25 pick")

            // Now that the field feeds the suggestion, an answer has to survive
            // retuning as well as re-chipping.
            tc.wizard.freqText = "851.375"
            compare(tc.wizard.trunking, false,
                    "an explicit off must survive retuning into a trunked band")
        }

        function test_05_a_radioreference_import_is_the_answer() {
            tc.wizard.applyRadioReference({
                                              "name": "Imported System",
                                              "freqMhz": "853.0625",
                                              "decodeFlag": "-fs",
                                              "trunking": true
                                          })
            compare(tc.wizard.trunking, true, "the import must carry the database's trunking answer")
            tc.wizard.pickDecodeFlag("-fy")
            compare(tc.wizard.trunking, true, "a chip pick must not second-guess the imported answer")

            // The ordering that makes this fragile: applyRadioReference() writes
            // the frequency field before it answers, so the write fires the band
            // suggestion. A conventional system in a trunked band is the case
            // where the two disagree, and the database has to win.
            tc.wizard.openForAdd(false)
            tc.wizard.applyRadioReference({
                                              "name": "Imported Conventional",
                                              "freqMhz": "853.0625",
                                              "decodeFlag": "-fs",
                                              "trunking": false
                                          })
            compare(tc.wizard.trunking, false,
                    "the 800 MHz band overrode a conventional system's own record")
        }

        function test_06_editing_keeps_the_saved_answer() {
            // Deliberately a conventional system on an 800 MHz frequency: the
            // band suggests trunking, the saved row says otherwise, and
            // openForEdit() writes the frequency field before it restores the
            // answer. Yesterday's card must not change what it does.
            savedSystems.add({
                                 "name": "Saved Conventional", "sourceType": "usb", "host": "", "port": 0,
                                 "freqMhz": "851.375", "decodeFlag": "-fs", "trunking": false,
                                 "gainDb": -1, "ppm": "", "bandwidthKhz": -1, "biasTee": -1,
                                 "extraArgs": "", "filePath": "", "chanCsvPath": "",
                                 "groupCsvPath": "", "keyCsvPath": "", "keyCsvHex": false
                             })
            tc.savedRow = savedSystems.count - 1
            tc.wizard.openForEdit(tc.savedRow)
            compare(tc.wizard.trunking, false, "the edit must show the saved answer, not the band's")
            tc.wizard.pickDecodeFlag("-ft")
            compare(tc.wizard.trunking, false, "a chip pick must not flip a saved system's answer")
        }
    }
}
