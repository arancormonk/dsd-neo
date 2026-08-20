// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The decode chip row after something other than the user picks the flag.
//
// DECODE_MODES is the user-pickable catalog; a RadioReference import chooses
// composite flags that are deliberately not in it ("-mq -^" for simulcast P25,
// "-fs -Y" for a conventional scan list), and an older saved system can carry
// one too. The chip row matches on the whole flag string, so those flags used
// to select nothing at all and blank the hint underneath — a screen that reads
// "no decode mode chosen" while the session is in fact correctly configured,
// and one tap away from silently dropping the "-^"/"-Y" the import added.
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

        name: "WizardDecodeChip"
        when: windowShown

        readonly property var wizard: wizardLoader.item

        function init() {
            verify(tc.wizard !== null, "WizardScreen.qml failed to load")
            tc.wizard.openForAdd(false)
        }

        // Chips are addressed by their LABEL, the text actually rendered.
        // The catalog's "P25 Simulcast" entry has the short name "P25 LSM",
        // which is also the composite "-mq -^" label, so short names collide
        // and labels do not.
        readonly property var everyLabel: [
            "Auto — P25/DMR/YSF", "P25", "P25 Simulcast", "DMR", "NXDN48",
            "NXDN96", "D-STAR", "YSF", "M17",
            "P25 LSM", "DMR Scan", "P25 Scan", "P25 LSM Scan",
            "NXDN48 Scan", "NXDN96 Scan", "EDACS", "EDACS EA"]

        function chipFor(label) {
            return findChild(tc.wizard, "wizardDecode_" + label)
        }

        function selectedLabels() {
            var out = []
            for (var i = 0; i < tc.everyLabel.length; i++) {
                var chip = tc.chipFor(tc.everyLabel[i])
                if (chip !== null && chip.selected)
                    out.push(tc.everyLabel[i])
            }
            return out
        }

        // Baseline: an ordinary catalog flag still selects exactly its own chip
        // and nothing else, and no extra chip is conjured up for it.
        function test_01_a_catalog_flag_selects_its_own_chip() {
            tc.wizard.pickDecodeFlag("-ft")
            compare(tc.selectedLabels(), ["P25"])
            compare(tc.chipFor("P25 Scan"), null,
                    "a composite chip must not appear for a catalog flag")
            verify(tc.chipFor("P25 Simulcast") !== null,
                   "the catalog must be intact for an ordinary flag")
        }

        // The simulcast import. "-mq -^" is what dsd_rr_decode_flag() answers
        // for a simulcast P25 system; the row must say so rather than going
        // blank.
        function test_02_an_imported_composite_flag_is_shown_selected() {
            tc.wizard.decodeFlag = "-mq -^"
            var chip = tc.chipFor("P25 LSM")
            verify(chip !== null, "no chip was offered for the imported flag")
            verify(chip.selected, "the imported flag's chip is not selected")
            compare(tc.selectedLabels(), ["P25 LSM"],
                    "exactly one chip may be selected")
            // Swapped in, not added alongside: the catalog's own simulcast chip
            // carries the bare "-mq" and would read as a near-duplicate.
            compare(tc.chipFor("P25 Simulcast"), null,
                    "the composite must replace the entry it refines")
        }

        // The conventional import. "-fs -Y" carries the scan list; collapsing
        // it onto the plain DMR chip would drop the "-Y", so it gets its own.
        function test_03_a_scan_list_flag_does_not_collapse_onto_its_base() {
            tc.wizard.decodeFlag = "-fs -Y"
            compare(tc.selectedLabels(), ["DMR Scan"])
            compare(tc.chipFor("DMR"), null,
                    "the scan-list chip replaces the plain DMR entry")
            // The chip carries the WHOLE flag, so tapping it cannot drop "-Y".
            compare(tc.chipFor("DMR Scan").modelData.flag, "-fs -Y")
        }

        // EDACS is only ever importer-chosen — it is kept out of the catalog on
        // purpose — so it is the case with no base chip to fall back on at all.
        function test_04_an_edacs_import_is_shown_selected() {
            tc.wizard.decodeFlag = "-fh"
            compare(tc.selectedLabels(), ["EDACS"])
            // Nothing in the catalog to refine, so it is appended and every
            // ordinary chip survives.
            verify(tc.chipFor("DMR") !== null)
            verify(tc.chipFor("P25 Simulcast") !== null)
        }

        // The hint under the row is what tells the user the mode was chosen for
        // them and can be overridden; it went empty for exactly these flags.
        function test_05_an_imported_flag_explains_itself() {
            tc.wizard.decodeFlag = "-mq -^"
            var hint = findChild(tc.wizard, "wizardDecodeHint").text
            verify(hint.length > 0, "the hint line is blank for an imported flag")
        }

        // Overriding must be a clean replacement: the composite chip goes away
        // and the flag becomes exactly what the tapped chip carries.
        function test_06_tapping_a_catalog_chip_replaces_the_imported_flag() {
            tc.wizard.decodeFlag = "-mq -^"
            verify(tc.chipFor("P25 LSM") !== null)

            tc.wizard.pickDecodeFlag("-fs")
            compare(tc.wizard.decodeFlag, "-fs")
            compare(tc.selectedLabels(), ["DMR"])
            compare(tc.chipFor("P25 LSM"), null,
                    "the imported chip must not outlive the flag it named")
            verify(tc.chipFor("P25 Simulcast") !== null,
                   "the catalog entry it replaced must come back")
        }

        // A flag nobody has a name for must not invent a chip; the row falls
        // back to showing nothing selected rather than a mystery label.
        function test_07_an_unknown_flag_adds_no_chip() {
            tc.wizard.decodeFlag = "-fh344"
            compare(tc.selectedLabels(), [])
            verify(tc.chipFor("P25 Simulcast") !== null,
                   "the catalog is untouched by a flag it does not know")
        }
    }
}
