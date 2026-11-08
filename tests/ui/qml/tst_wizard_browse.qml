// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #829: the wizard's Replay file Browse. It asks the app's document
// picker (the decoderHost map plays the host), copies the picked document
// through importContentUri() and keeps the previous file when the copy fails,
// saying so under the row (B8). A pick still out ends with the wizard session
// it was asked in.
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

        name: "WizardBrowse"
        when: windowShown

        readonly property var wizard: wizardLoader.item
        // B1: a content reference with an upper-case authority and an escaped
        // space, which a QML url would rewrite.
        readonly property string reference: "content://com.Example.Provider/document/primary%3ADownload%2Fa%20b.wav"
        property int savedRow: -1

        function init() {
            verify(tc.wizard !== null, "WizardScreen.qml failed to load")
            testContext.setDocumentPickerSupported(true)
            testContext.resetHostPlayback()
            wizardLoader.visible = true
            tc.wizard.openForAdd(false)
            tc.wizard.sourceType = "file"
        }

        function cleanup() {
            root.width = 420
            wizardLoader.visible = true
            testContext.resetHostPlayback()
            if (tc.savedRow >= 0) {
                savedSystems.remove(tc.savedRow)
                tc.savedRow = -1
            }
        }

        function item(name) {
            var found = findChild(tc.wizard, name)
            verify(found !== null, name)
            return found
        }
        // Tap Browse; the request the wizard now waits on.
        function browse() {
            var button = item("wizardBrowse")
            verify(button.visible && button.enabled)
            button.clicked()
            var request = testContext.lastDocumentRequest()
            compare(request.kind, "open")
            verify(item("wizardSourcePicker").visible)
            return request.id
        }
        function copies() {
            return testContext.hostCalls().filter(function (call) { return call.name === "importContentUri" })
        }
        function notice() {
            return item("wizardSourceNotice")
        }

        function test_browse_copies_the_pick() {
            tc.wizard.pickerTarget = "chan"
            var id = browse()
            compare(tc.wizard.pickerTarget, "chan", "Browse leaves the picker sheet's field alone")
            testContext.setImportContentResult("/data/imports/a b.wav")
            testContext.answerDocumentPicked(id, tc.reference, "a b.wav", false)
            compare(copies().length, 1)
            compare(copies()[0].reference, tc.reference, "the reference reaches the copy byte for byte")
            compare(copies()[0].fileName, "a b.wav", "named by the picker's display name")
            compare(tc.wizard.fileText, "/data/imports/a b.wav")
            compare(tc.wizard.sourceNotice, "")
            verify(!notice().visible)
            verify(!item("wizardSourcePicker").visible)
        }

        // B8: a copy that fails keeps what was there and says so, naming the
        // previous file only when there was one.
        function test_copy_failure_keeps_the_file_data() {
            return [
                {tag: "no-previous", previous: "", text: "Could not copy that file."},
                {tag: "previous", previous: "/data/imports/old.wav", text: "Could not copy that file. The previous file is still selected."}
            ]
        }
        function test_copy_failure_keeps_the_file(data) {
            tc.wizard.fileText = data.previous
            var said = tc.wizard.announcements
            var id = browse()
            testContext.setImportContentResult("")
            testContext.answerDocumentPicked(id, tc.reference, "a b.wav", false)
            compare(copies().length, 1)
            compare(tc.wizard.fileText, data.previous)
            compare(tc.wizard.sourceNotice, data.text)
            verify(notice().visible)
            compare(notice().text, data.text)
            compare(notice().Accessible.role, Accessible.AlertMessage)
            compare(tc.wizard.announcements, said + 1, "read out once")
            compare(tc.wizard.announced, data.text)

            // The next Browse clears it; the same failure is read out again.
            id = browse()
            compare(tc.wizard.sourceNotice, "", "a new Browse starts with no notice")
            verify(!notice().visible)
            testContext.answerDocumentPicked(id, tc.reference, "a b.wav", false)
            compare(tc.wizard.sourceNotice, data.text)
            compare(tc.wizard.announcements, said + 2, "a repeat is read out again")
        }

        // A narrow window wraps the longer sentence instead of cutting it.
        function test_notice_wraps() {
            tc.wizard.fileText = "/data/imports/old.wav"
            root.width = 320
            var id = browse()
            testContext.setImportContentResult("")
            testContext.answerDocumentPicked(id, tc.reference, "a b.wav", false)
            var line = notice()
            verify(line.width <= 320)
            verify(line.lineCount > 1, "the sentence wraps")
            verify(line.contentWidth <= line.width + 1, "and stays inside its line")
        }

        // A failed copy's notice stays while the next pick is out and goes
        // once a copy lands.
        function test_successful_copy_clears_the_notice() {
            tc.wizard.fileText = "/data/imports/old.wav"
            testContext.setImportContentResult("")
            testContext.answerDocumentPicked(browse(), tc.reference, "a b.wav", false)
            compare(tc.wizard.sourceNotice, "Could not copy that file. The previous file is still selected.")
            // The next Browse fails to open: its reason replaces the notice.
            testContext.answerDocumentUnavailable(browse(), "noPicker")
            compare(tc.wizard.sourceNotice, "No compatible file picker on this device. Install a file manager app, then try again.")
            var id = browse()
            testContext.setImportContentResult("/data/imports/new.wav")
            testContext.answerDocumentPicked(id, tc.reference, "new.wav", false)
            compare(tc.wizard.fileText, "/data/imports/new.wav")
            compare(tc.wizard.sourceNotice, "", "the copy that landed cleared it")
            verify(!notice().visible)
        }

        function test_picker_unavailable_data() {
            return [
                {tag: "noPicker", text: "No compatible file picker on this device. Install a file manager app, then try again."},
                {tag: "unreadable", text: "That app gave DSD-neo a file it can't open. Pick the file from another app."},
                {tag: "failed", text: "The file picker could not open. Try again."}
            ]
        }
        function test_picker_unavailable(data) {
            tc.wizard.fileText = "/data/imports/old.wav"
            var said = tc.wizard.announcements
            var id = browse()
            testContext.answerDocumentUnavailable(id, data.tag)
            compare(copies().length, 0)
            compare(tc.wizard.fileText, "/data/imports/old.wav")
            compare(tc.wizard.sourceNotice, data.text)
            verify(notice().visible)
            compare(notice().Accessible.role, Accessible.AlertMessage)
            compare(tc.wizard.announcements, said + 1)
            compare(tc.wizard.announced, data.text)
        }

        function test_backing_out_changes_nothing() {
            tc.wizard.fileText = "/data/imports/old.wav"
            var said = tc.wizard.announcements
            var id = browse()
            testContext.answerDocumentCancelled(id)
            compare(copies().length, 0)
            compare(tc.wizard.fileText, "/data/imports/old.wav")
            compare(tc.wizard.sourceNotice, "")
            compare(tc.wizard.announcements, said)
            verify(!item("wizardSourcePicker").visible)
        }

        // A pick still out, and the notice, end with the session: reopening
        // the wizard for another system or closing it (Save clears wizardOpen
        // in Main directly) cancels the request, and its late answer copies
        // nothing.
        function test_browse_ends_with_its_session_data() {
            return [{tag: "openForAdd"}, {tag: "openForFound"}, {tag: "openForEdit"}, {tag: "hidden"}]
        }
        function test_browse_ends_with_its_session(data) {
            if (data.tag === "openForEdit") {
                tc.wizard.nameText = "Browse owner"
                tc.wizard.fileText = "/data/imports/saved.wav"
                var before = savedSystems.count
                tc.wizard.commit()
                compare(savedSystems.count, before + 1, "the system was saved")
                tc.savedRow = savedSystems.count - 1
                tc.wizard.openForAdd(false)
                tc.wizard.sourceType = "file"
            }
            var failed = browse()
            testContext.answerDocumentUnavailable(failed, "noPicker")
            verify(tc.wizard.sourceNotice.length > 0)
            var id = browse()
            if (data.tag === "openForAdd")
                tc.wizard.openForAdd(false)
            else if (data.tag === "openForFound")
                tc.wizard.openForFound(null, "851.375")
            else if (data.tag === "openForEdit")
                tc.wizard.openForEdit(tc.savedRow)
            else
                wizardLoader.visible = false
            verify(!item("wizardSourcePicker").visible, "the pick ended with its session")
            verify(testContext.documentCalls().some(function (call) {
                return call.name === "cancelDocumentRequest" && call.id === id
            }), "the host request was cancelled")
            compare(tc.wizard.sourceNotice, "", "the notice went with its session")
            var fileText = tc.wizard.fileText
            testContext.answerDocumentPicked(id, tc.reference, "late.wav", false)
            compare(copies().length, 0, "the late pick was never copied")
            compare(tc.wizard.fileText, fileText)
        }

        // Browse is not a trunking-data field: a CSV pick out for one keeps
        // its field and still lands there.
        function test_browse_never_redirects_a_csv_pick() {
            tc.wizard.pickerTarget = "chan"
            var sheet = item("wizardCsvSheet")
            sheet.visible = true
            item("wizardCsvImportNew").clicked()
            var csvId = testContext.lastDocumentRequest().id
            var flow = item("wizardCsvImport")
            verify(flow.active)
            tc.wizard.step = 0
            tc.wizard.sourceType = "file"
            var browseId = browse()
            verify(browseId !== csvId)
            compare(tc.wizard.pickerTarget, "chan")
            testContext.answerDocumentCancelled(browseId)
            verify(flow.active, "Browse leaves the CSV pick out")
            flow.cancel()
        }
    }
}
