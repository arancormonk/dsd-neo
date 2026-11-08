// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The wizard's P25 band plan pick. It is the fourth trunking-data field beside
// the channel map, talkgroups and keys, and it has to travel the same road: a
// new system starts without one, the picker seam assigns it, commit() writes it
// into the saved system, and an edit brings it back — a field that only lived
// in the wizard's own properties would be dropped by the first save and the
// session would start without its --p25-bandplan.
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

        name: "WizardBandplan"
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

        function test_cancel_import_cleanup_data() {
            return [{tag: "primary"}, {tag: "sheet"}];
        }
        function test_cancel_import_cleanup(data) {
            tc.wizard.pickerTarget = "chan";
            tc.wizard.chanCsvPath = "keep.csv";
            tc.wizard.csvNotice = "Old notice";
            tc.wizard.csvNoticeIsProblem = true;
            var flow = findChild(tc.wizard, "wizardCsvImport");
            verify(flow !== null);
            if (data.tag === "primary") {
                flow.pick("chan");
                findChild(flow, "csvPrimaryPicker").reject();
            } else {
                var source = testContext.writeFixtureCsv("cancel-consumer.csv",
                    "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,missing.csv\n");
                flow.begin(source, "Map.csv", "chan");
                findChild(flow, "csvCompanionSheet").requestDismiss();
            }
            compare(tc.wizard.chanCsvPath, "keep.csv");
            compare(tc.wizard.csvNotice, "");
            verify(!tc.wizard.csvNoticeIsProblem);
            compare(tc.wizard.pickerTarget, "");
        }

        function test_01_a_new_system_has_no_band_plan() {
            compare(tc.wizard.p25BandplanCsvPath, "", "openForAdd() must start without a band plan")
            tc.wizard.openForFound(null, "851.375")
            compare(tc.wizard.p25BandplanCsvPath, "", "openForFound() must start without a band plan")
        }

        // Issue #802 B12: an imported file's name in the picker is drawn and
        // measured as written, never as markup (a "/" in it would start a
        // path, so the tag is left open).
        function textShowing(item, value) {
            if (item.textFormat !== undefined && item.text === value && item.visible)
                return item
            var kids = item.children || []
            for (var i = 0; i < kids.length; ++i) {
                var found = textShowing(kids[i], value)
                if (found)
                    return found
            }
            return null
        }
        function test_01b_an_imported_file_name_draws_as_written() {
            var source = testContext.writeFixtureCsv("markup-map.csv", "channel,frequency\n1,851012500\n")
            var result = importedFiles.importFile(source, "<b>Map &amp; plan.csv", "chan")
            verify(result.ok, "the map was not imported")
            var sheet = findChild(tc.wizard, "wizardCsvSheet")
            try {
                tc.wizard.pickerTarget = "chan"
                tc.wizard.chanCsvPath = result.path
                sheet.visible = true
                var text = null
                tryVerify(function () { text = textShowing(sheet, "Selected: <b>Map &amp; plan.csv"); return text !== null },
                          2000, "the imported map is not listed")
                var mirror = Qt.createQmlObject("import QtQuick; Text { visible: false; textFormat: Text.PlainText }", tc.wizard)
                mirror.font = text.font
                mirror.text = text.text
                var plainWidth = mirror.implicitWidth
                mirror.destroy()
                verify(Math.abs(text.implicitWidth - plainWidth) <= 0.5, "the file name draws as written: " + text.implicitWidth + " vs " + plainWidth)
                compare(text.textFormat, Text.PlainText, "the file name is plain text")
            } finally {
                sheet.visible = false
                tc.wizard.pickerTarget = ""
                tc.wizard.chanCsvPath = ""
                importedFiles.remove(importedFiles.rowForPath(result.path))
            }
        }

        function test_02_the_picker_seam_assigns_it() {
            tc.wizard.assignCsvPath("p25Bandplan", "/data/imports/band plan.csv", false)
            compare(tc.wizard.p25BandplanCsvPath, "/data/imports/band plan.csv")
            // Its own field: the channel map row must not have been written.
            compare(tc.wizard.chanCsvPath, "", "the band plan pick landed in the channel map field")
            tc.wizard.assignCsvPath("p25Bandplan", "", false)
            compare(tc.wizard.p25BandplanCsvPath, "", "\"None\" must clear the field")
        }

        function test_03_it_round_trips_through_the_saved_system() {
            tc.wizard.nameText = "Band plan system"
            tc.wizard.assignCsvPath("p25Bandplan", "/data/imports/band plan.csv", false)
            tc.wizard.commit()
            tc.savedRow = savedSystems.count - 1
            compare(savedSystems.get(tc.savedRow).p25BandplanCsvPath, "/data/imports/band plan.csv",
                    "commit() dropped the band plan from the saved system")

            tc.wizard.openForAdd(false)
            compare(tc.wizard.p25BandplanCsvPath, "", "a fresh add must not inherit the last system's band plan")

            tc.wizard.openForEdit(tc.savedRow)
            compare(tc.wizard.p25BandplanCsvPath, "/data/imports/band plan.csv",
                    "openForEdit() must restore the saved band plan")
        }
    }
}
