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
            "NXDN96", "D-STAR", "YSF", "M17", "NFM — analog FM", "AM",
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

        // "-f1" is a legacy ALIAS rather than a refinement: it carries the
        // catalog's own "P25" label but is not "-ft "-prefixed, so appending it
        // would leave two chips reading "P25" side by side - and the delegate is
        // named after its label, so findChild() could not tell them apart either.
        function test_08_a_legacy_alias_replaces_the_chip_it_shadows() {
            tc.wizard.decodeFlag = "-f1"
            compare(tc.selectedLabels(), ["P25"],
                    "the legacy alias must select the one P25 chip")
            compare(tc.chipFor("P25").modelData.flag, "-f1",
                    "the surviving P25 chip must carry the saved flag")
            verify(tc.chipFor("P25 Simulcast") !== null,
                   "the rest of the catalog is untouched")
        }

        // Issue #525: the NFM chip (-fA, the analog monitor) comes with the
        // shared catalog, so the wizard offers it too. It selects on its own
        // flag, and it names a system type that is not trunked: picked on the
        // 800 MHz prefill, it must not suggest call-following, which an analog
        // monitor cannot do.
        function test_09_the_nfm_chip_is_offered_and_never_suggests_trunking() {
            var chip = tc.chipFor("NFM — analog FM")
            verify(chip !== null, "the wizard offers no NFM chip")
            compare(chip.modelData.flag, "-fA")
            compare(tc.wizard.trunking, true, "the 800 MHz prefill suggests trunking before the pick")
            tc.wizard.pickDecodeFlag("-fA")
            compare(tc.wizard.decodeFlag, "-fA")
            compare(tc.selectedLabels(), ["NFM — analog FM"])
            compare(tc.wizard.trunking, false, "picking NFM suggested call-following")
        }

        // Issue #524: the AM chip (-fM) comes with the shared catalog. It is
        // offered for radio sources only (network and file audio arrives already
        // demodulated), with a note saying why it is greyed out elsewhere,
        // selects on its own flag, and names a system type that is not trunked,
        // so picking it on the 800 MHz prefill suggests no call-following.
        function test_10_the_am_chip_needs_a_radio_source_and_never_suggests_trunking() {
            var chip = tc.chipFor("AM")
            var note = findChild(tc.wizard, "wizardDecodeIqNote")
            verify(chip !== null, "the wizard offers no AM chip")
            verify(note !== null, "the wizard has no note for a greyed-out AM chip")
            compare(chip.modelData.flag, "-fM")
            verify(tc.wizard.radioSource, "the wizard opens on a radio source")
            verify(chip.enabled, "the AM chip is disabled on a radio source")
            tc.wizard.step = 1
            verify(!note.visible, "the I/Q note shows on a radio source")
            compare(tc.wizard.trunking, true, "the 800 MHz prefill suggests trunking before the pick")
            tc.wizard.pickDecodeFlag("-fM")
            compare(tc.wizard.decodeFlag, "-fM")
            compare(tc.selectedLabels(), ["AM"])
            compare(tc.wizard.trunking, false, "picking AM suggested call-following")
            tc.wizard.sourceType = "tcp"
            verify(!chip.enabled, "the AM chip is offered for TCP audio")
            verify(note.visible, "a greyed-out AM chip says nothing about why")
            verify(note.text.indexOf("AM needs a radio source") === 0, "the note does not name the reason")
            tc.wizard.sourceType = "file"
            verify(note.visible, "a file source leaves the AM chip unexplained")
            tc.wizard.sourceType = "usb"
            verify(chip.enabled)
            verify(!note.visible)
            tc.wizard.step = 0
        }

        // Issue #524: the engine refuses an AM channel the radio's DSP bandwidth
        // cannot filter, and at 4 or 6 kHz none fits. Step 1 waits for a wider
        // bandwidth, with the reason and the fix under the chips; 8 kHz takes
        // the 6 kHz default, an empty field follows the 48 kHz app default, a
        // digital pick is not held, and neither is Airspy, whose device sets
        // the rate the engine checks at start.
        function test_12_am_waits_for_a_bandwidth_that_fits() {
            var note = findChild(tc.wizard, "wizardDecodeAmBandwidthNote")
            verify(note !== null, "the wizard has no AM bandwidth note")
            tc.wizard.step = 1
            tc.wizard.freqText = "118.1"
            tc.wizard.pickDecodeFlag("-fM")
            verify(tc.wizard.stepValid(), "AM at the app default bandwidth is refused")
            verify(!note.visible)

            for (var i = 0; i < 2; i++) {
                tc.wizard.sourceType = i === 0 ? "usb" : "rtltcp"
                tc.wizard.bwText = "6"
                verify(!tc.wizard.stepValid(), "AM at a 6 kHz bandwidth passes step 1")
                verify(note.visible, "the refusal is not shown")
                verify(note.text.indexOf("Set the bandwidth to 8, 12, 16, 24 or 48 kHz") > 0, "the note names no fix")
                tc.wizard.bwText = "4"
                verify(!tc.wizard.stepValid(), "AM at a 4 kHz bandwidth passes step 1")
                tc.wizard.bwText = "8"
                verify(tc.wizard.stepValid(), "AM at 8 kHz is refused")
                verify(!note.visible)
            }

            tc.wizard.bwText = "6"
            tc.wizard.pickDecodeFlag("-fs")
            verify(tc.wizard.stepValid(), "a digital mode at 6 kHz is held to the AM channel")
            verify(!note.visible)
            tc.wizard.pickDecodeFlag("-fM")
            tc.wizard.sourceType = "airspy"
            verify(tc.wizard.stepValid(), "Airspy is held here rather than where its device sets the rate")
            tc.wizard.sourceType = "usb"
            tc.wizard.bwText = ""
            verify(tc.wizard.stepValid(), "an empty bandwidth does not follow the 48 kHz app default")
            tc.wizard.step = 0
        }

        // Issue #524: the AM chip greyed out is not enough. Picking a network or
        // file source after AM drops the flag back to Auto, so the wizard cannot
        // save a system the engine refuses to start; staying on a radio source
        // keeps it. A system that reaches step 1 with AM on such a source (an
        // edit, set here directly) cannot continue until another chip is picked.
        function test_11_leaving_the_radio_drops_the_am_flag() {
            tc.wizard.pickDecodeFlag("-fM")
            findChild(tc.wizard, "wizardSource_rtltcp").clicked()
            compare(tc.wizard.sourceType, "rtltcp")
            compare(tc.wizard.decodeFlag, "-fM", "a radio source kept AM")
            findChild(tc.wizard, "wizardSource_tcp").clicked()
            compare(tc.wizard.sourceType, "tcp")
            compare(tc.wizard.decodeFlag, "", "TCP audio kept the AM flag")
            compare(tc.selectedLabels(), ["Auto — P25/DMR/YSF"])

            findChild(tc.wizard, "wizardSource_usb").clicked()
            tc.wizard.pickDecodeFlag("-fM")
            findChild(tc.wizard, "wizardSource_file").clicked()
            compare(tc.wizard.decodeFlag, "", "a file source kept the AM flag")

            tc.wizard.decodeFlag = "-fM"
            tc.wizard.step = 1
            verify(!tc.wizard.stepValid(), "AM on a file source passes step 1")
            tc.wizard.pickDecodeFlag("-fs")
            verify(tc.wizard.stepValid(), "another chip clears the refusal")
            tc.wizard.step = 0
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
