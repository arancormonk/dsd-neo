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
            testContext.setDocumentPickerSupported(true)
            testContext.resetHostPlayback()
            wizardLoader.visible = true
            tc.wizard.openForAdd(false)
        }

        function cleanup() {
            wizardLoader.visible = true
            if (tc.savedRow >= 0) {
                savedSystems.remove(tc.savedRow)
                tc.savedRow = -1
            }
            while (importedFiles.count)
                importedFiles.remove(0)
        }

        // Issue #829: the pick the wizard is waiting on, answered by the host.
        function requestId() {
            var request = testContext.lastDocumentRequest()
            compare(request.kind, "open")
            return request.id
        }
        function flow() {
            var found = findChild(tc.wizard, "wizardCsvImport")
            verify(found !== null)
            return found
        }
        function mapFile(name) {
            var path = testContext.writeFixtureCsv(name, "channel,frequency\n1,851012500\n")
            verify(path.length > 0)
            return "file://" + path
        }
        // "Import new file" on the picker sheet for @p target.
        function importFor(target) {
            tc.wizard.pickerTarget = target
            var sheet = findChild(tc.wizard, "wizardCsvSheet")
            sheet.visible = true
            findChild(sheet, "wizardCsvImportNew").clicked()
            verify(!sheet.visible)
            verify(flow().active)
            return requestId()
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
                importFor("chan");
                testContext.answerDocumentCancelled(requestId());
            } else {
                var source = testContext.writeFixtureCsv("cancel-consumer.csv",
                    "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,missing.csv\n");
                tc.wizard.pickerTarget = "chan";
                flow.begin(source, "Map.csv", "chan", tc.wizard.csvContext());
                var sheet = findChild(flow, "csvCompanionSheet");
                // #832: the copy answers later; the companions it asks for come
                // with that answer.
                tryVerify(function () { return flow.requiredFiles.length > 0; }, 5000, "the companions are asked for");
                sheet.requestDismiss();
            }
            tryCompare(flow, "active", false);
            compare(tc.wizard.chanCsvPath, "keep.csv");
            compare(tc.wizard.csvNotice, "");
            verify(!tc.wizard.csvNoticeIsProblem);
            // Issue #829: the field travels with the pick; pickerTarget is the
            // sheet's, and a finished import leaves it alone.
            compare(tc.wizard.pickerTarget, "chan");
        }

        // B4: the field comes from the pick, so a later sheet for another
        // field, or Browse (which sets pickerTarget to "source"), cannot
        // redirect it.
        function test_a_pick_fills_the_field_it_was_asked_for_data() {
            return [{tag: "browse", later: "source"}, {tag: "other-sheet", later: "group"}];
        }
        function test_a_pick_fills_the_field_it_was_asked_for(data) {
            var id = importFor("chan")
            tc.wizard.pickerTarget = data.later
            testContext.answerDocumentPicked(id, mapFile("b4-map.csv"), "B4 map.csv", false)
            // #832: the copy answers later, from the import worker.
            tryCompare(flow(), "active", false)
            verify(tc.wizard.chanCsvPath.length > 0, "the map landed in its own field")
            // The provider's own name for the document: a local file's name.
            compare(importedFiles.get(importedFiles.rowForPath(tc.wizard.chanCsvPath)).name, "b4-map.csv")
            compare(tc.wizard.groupCsvPath, "")
            compare(tc.wizard.pickerTarget, data.later)
        }

        // The key format is the one chosen when the pick was asked for.
        function test_a_key_pick_keeps_its_format() {
            tc.wizard.pickerKeyHex = false
            var id = importFor("keys")
            tc.wizard.pickerKeyHex = true
            var keys = testContext.writeFixtureCsv("wizard-keys.csv", "id,value\n1,12345\n")
            testContext.answerDocumentPicked(id, "file://" + keys, "wizard-keys.csv", false)
            tryCompare(flow(), "active", false)
            verify(tc.wizard.keyCsvPath.length > 0)
            verify(!tc.wizard.keyCsvHex, "a decimal pick stays decimal")
            compare(importedFiles.get(importedFiles.rowForPath(tc.wizard.keyCsvPath)).type, "keysDec")
        }

        // B4: a pick still out when the wizard is reused or closed is ended
        // with that session; its late answer imports and assigns nothing.
        function test_a_pick_ends_with_its_session_data() {
            return [{tag: "openForAdd"}, {tag: "openForFound"}, {tag: "openForEdit"}, {tag: "hidden"}];
        }
        function test_a_pick_ends_with_its_session(data) {
            if (data.tag === "openForEdit") {
                tc.wizard.nameText = "Session owner"
                tc.wizard.commit()
                tc.savedRow = savedSystems.count - 1
                tc.wizard.openForAdd(false)
            }
            var id = importFor("chan")
            if (data.tag === "openForAdd")
                tc.wizard.openForAdd(false)
            else if (data.tag === "openForFound")
                tc.wizard.openForFound(null, "851.375")
            else if (data.tag === "openForEdit")
                tc.wizard.openForEdit(tc.savedRow)
            else
                wizardLoader.visible = false
            verify(!flow().active, "the pick ended with its session")
            verify(testContext.documentCalls().some(function (call) {
                return call.name === "cancelDocumentRequest" && call.id === id
            }), "the host request was cancelled")
            testContext.answerDocumentPicked(id, mapFile("late-session-map.csv"), "late.csv", false)
            compare(tc.wizard.chanCsvPath, "")
            compare(importedFiles.count, 0, "nothing was imported")
            compare(tc.wizard.csvNotice, "")
        }

        // A choice made for the field while the picker is out wins: the late
        // pick is dropped without a notice and without an import.
        function test_newer_choice_survives_a_late_pick_data() {
            return [{tag: "library", target: "chan"}, {tag: "none", target: "chan"},
                    {tag: "radioreference", target: "chan"}, {tag: "decryption-profile", target: "keys"}];
        }
        function test_newer_choice_survives_a_late_pick(data) {
            var id = importFor(data.target)
            if (data.tag === "library")
                tc.wizard.assignCsvPath("chan", "/data/imports/library.csv", false)
            else if (data.tag === "none")
                tc.wizard.assignCsvPath("chan", "", false)
            else if (data.tag === "radioreference")
                tc.wizard.applyRadioReference({chanCsvPath: "/data/imports/rr.csv", groupCsvPath: "", decodeFlag: "", trunking: true})
            else
                // A decryption profile takes the place of the key file.
                findChild(tc.wizard, "wizardDecryptionProfile").selected("profile-uid")
            var path = function () { return data.target === "keys" ? tc.wizard.keyCsvPath : tc.wizard.chanCsvPath }
            var chosen = path()
            var notice = tc.wizard.csvNotice
            var late = data.target === "keys" ? "file://" + testContext.writeFixtureCsv("late-keys.csv", "id,value\n1,12345\n") : mapFile("late-map.csv")
            testContext.answerDocumentPicked(id, late, "late.csv", false)
            tryCompare(flow(), "active", false)
            compare(path(), chosen)
            compare(tc.wizard.csvNotice, notice, "the newer choice's notice stays")
            compare(importedFiles.count, 0, "the dropped pick imported nothing")
        }
        // Another field's choice is not newer for this one.
        function test_another_fields_choice_keeps_the_pick() {
            var id = importFor("chan")
            tc.wizard.assignCsvPath("group", "/data/imports/groups.csv", false)
            testContext.answerDocumentPicked(id, mapFile("kept-map.csv"), "kept.csv", false)
            tryCompare(flow(), "active", false)
            verify(tc.wizard.chanCsvPath.length > 0)
            compare(tc.wizard.groupCsvPath, "/data/imports/groups.csv")
        }

        // A picker failure shows its reason, as an alert read out once; the
        // same failure again is read out again.
        function test_picker_failure_is_shown_and_read_out() {
            var said = tc.wizard.announcements
            testContext.answerDocumentUnavailable(importFor("chan"), "noPicker")
            var message = "No compatible file picker on this device. Install a file manager app, then try again."
            compare(tc.wizard.csvNotice, message)
            verify(tc.wizard.csvNoticeIsProblem)
            compare(tc.wizard.announcements, said + 1)
            compare(tc.wizard.announced, message)
            compare(findChild(tc.wizard, "wizardCsvNotice").Accessible.role, Accessible.AlertMessage)
            testContext.answerDocumentUnavailable(importFor("chan"), "noPicker")
            compare(tc.wizard.announcements, said + 2)
            compare(tc.wizard.chanCsvPath, "")
            // A file that cannot be read says so, naming it.
            testContext.answerDocumentPicked(importFor("chan"), "file:///missing/map.csv", "map.csv", false)
            tryCompare(flow(), "active", false)
            compare(tc.wizard.csvNotice, "Could not read map.csv. Check that it is still available, then try again.")
            compare(tc.wizard.announcements, said + 3)
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
            // The import stores the file under this name, and Windows file
            // names cannot hold "<" or ">", so no imported name carries markup
            // there.
            if (Qt.platform.os === "windows")
                skip("Windows file names cannot hold markup")
            var source = testContext.writeFixtureCsv("<b>Map &amp; plan.csv", "channel,frequency\n1,851012500\n")
            var result = testContext.importNow(source, "chan")
            verify(result.ok, "the map was not imported: " + result.error)
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
