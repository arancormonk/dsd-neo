// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #829: the picker every screen uses. The host path runs on the fixture's
// fake picker, which answers only when a case says (testContext.answerDocument*);
// the desktop path drives the FileDialog inside by its objectName.
Item {
    id: root
    width: 420
    height: 600

    Component {
        id: pickerComponent
        Ui.DocumentPicker {}
    }
    Component {
        id: spyComponent
        SignalSpy {}
    }

    TestCase {
        id: testCase
        name: "DocumentPicker"
        when: windowShown

        // A picked document's reference as a platform provider spells it: the
        // authority's case and the escapes must reach the screen as they are (B1).
        readonly property string mixedCaseReference: "content://com.Example.Provider/document/primary%3ADownload%2Fa%20b.csv"

        property var spyRecords: []

        function init() {
            spyRecords = [];
            testContext.useLifecycleHost(false);
            testContext.setDocumentPickerSupported(true);
            testContext.setFileBrowserSupported(false);
            testContext.setPrefs("chooseFilesInApp", false);
            testContext.resetHostPlayback();
            compare(Ui.FileBrowser.request, null, "no browse request is left from an earlier case");
        }
        function cleanup() {
            testContext.setDocumentPickerSupported(true);
            testContext.setFileBrowserSupported(false);
            testContext.setPrefs("chooseFilesInApp", false);
            testContext.useLifecycleHost(false);
            testContext.resetHostPlayback();
        }

        function makePicker(properties) {
            var picker = createTemporaryObject(pickerComponent, root, properties || ({}));
            verify(picker, "a picker");
            spyRecords.push({
                picker: picker,
                accepted: createTemporaryObject(spyComponent, testCase, {target: picker, signalName: "accepted"}),
                rejected: createTemporaryObject(spyComponent, testCase, {target: picker, signalName: "rejected"}),
                unavailable: createTemporaryObject(spyComponent, testCase, {target: picker, signalName: "unavailable"})
            });
            return picker;
        }
        // The signal spies makePicker() gave @p picker.
        function spiesOf(picker) {
            for (var record of spyRecords) {
                if (record.picker === picker)
                    return record;
            }
            verify(false, "a picker made by makePicker()");
            return null;
        }
        function answers(picker) {
            var spies = spiesOf(picker);
            return spies.accepted.count + spies.rejected.count + spies.unavailable.count;
        }
        function callNames() {
            return testContext.documentCalls().map(function (call) { return call.name; });
        }
        function callsNamed(name) {
            return testContext.documentCalls().filter(function (call) { return call.name === name; });
        }
        function cancelledIds() {
            return callsNamed("cancelDocumentRequest").map(function (call) { return call.id; });
        }
        // A picker the file browser can serve (issue #830): it names what it
        // opens, on a host with the browser, and the toggle says browse first.
        function makeBrowsingPicker(properties, browseFirst) {
            testContext.setFileBrowserSupported(true);
            testContext.setPrefs("chooseFilesInApp", browseFirst !== false);
            var settings = {browseKind: "csv", title: "Choose a CSV file"};
            for (var key in (properties || {}))
                settings[key] = properties[key];
            return makePicker(settings);
        }
        // The live browse request, which must be there.
        function browseToken() {
            verify(Ui.FileBrowser.request !== null, "a browse request is live");
            return Ui.FileBrowser.request.token;
        }
        function dialogOf(picker) {
            var dialog = findChild(picker, "documentPickerDialog");
            verify(dialog, "the desktop picker has its FileDialog");
            return dialog;
        }

        // ---- host path ----

        function test_open_request() {
            var picker = makePicker();
            verify(!picker.visible);
            picker.open();
            var request = testContext.lastDocumentRequest();
            compare(request.kind, "open");
            verify(request.id > 0);
            verify(picker.visible, "a request is waiting");
            compare(callNames(), ["requestOpenDocument"]);
            compare(answers(picker), 0, "nothing answers before the host does");
            verify(!findChild(picker, "documentPickerDialog"), "no FileDialog where the host picks");
        }

        function test_save_request() {
            var picker = makePicker({mode: "save", suggestedName: "dsd-neo-backup 2026-11-08.dsdbak", mimeType: "application/octet-stream"});
            picker.open();
            var request = testContext.lastDocumentRequest();
            compare(request.kind, "save");
            compare(request.name, "dsd-neo-backup 2026-11-08.dsdbak");
            compare(request.mime, "application/octet-stream");
            compare(callNames(), ["requestSaveDocument"]);
            verify(picker.visible);
        }

        // B1: the reference is handed on byte for byte, never through a url.
        function test_accepted_reference_is_unchanged() {
            var picker = makePicker();
            picker.open();
            var id = testContext.lastDocumentRequest().id;
            testContext.answerDocumentPicked(id, mixedCaseReference, "a b.csv", false);
            compare(spiesOf(picker).accepted.count, 1);
            compare(spiesOf(picker).accepted.signalArguments[0][0], mixedCaseReference);
            compare(spiesOf(picker).accepted.signalArguments[0][1], "a b.csv");
            verify(!picker.visible);
            verify(!picker.savedToDownloads);
            compare(answers(picker), 1);
            testContext.answerDocumentPicked(id, mixedCaseReference, "a b.csv", false);
            compare(spiesOf(picker).accepted.count, 1, "an answered request is answered once");
        }

        function test_saved_to_downloads() {
            var picker = makePicker({mode: "save", suggestedName: "Call recordings 2026-11-08.zip", mimeType: "application/zip"});
            picker.open();
            testContext.answerDocumentPicked(testContext.lastDocumentRequest().id, "content://media/external/downloads/42", "Call recordings 2026-11-08.zip", true);
            compare(spiesOf(picker).accepted.count, 1);
            verify(picker.savedToDownloads, "the save went to the Download folder");
            compare(callsNamed("discardPendingDownload").length, 0, "the live save is kept");
            picker.open();
            testContext.answerDocumentPicked(testContext.lastDocumentRequest().id, mixedCaseReference, "a b.csv", false);
            compare(spiesOf(picker).accepted.count, 2);
            verify(!picker.savedToDownloads, "the next save went where the user chose");
        }

        function test_stale_ids_are_ignored() {
            var picker = makePicker();
            picker.open();
            var id = testContext.lastDocumentRequest().id;
            testContext.answerDocumentPicked(id + 100, mixedCaseReference, "a b.csv", false);
            testContext.answerDocumentCancelled(id + 100);
            testContext.answerDocumentUnavailable(id + 100, "failed");
            testContext.answerDocumentPicked(0, mixedCaseReference, "a b.csv", false);
            compare(answers(picker), 0, "only the live id is answered");
            verify(picker.visible, "still waiting");
            testContext.answerDocumentCancelled(id);
            compare(spiesOf(picker).rejected.count, 1);
            verify(!picker.visible);
        }

        function test_reopen_cancels_the_previous_request() {
            var picker = makePicker();
            picker.open();
            var first = testContext.lastDocumentRequest().id;
            picker.open();
            var second = testContext.lastDocumentRequest().id;
            verify(second !== first);
            compare(callNames(), ["requestOpenDocument", "cancelDocumentRequest", "requestOpenDocument"]);
            compare(cancelledIds(), [first]);
            compare(answers(picker), 0, "reopening is silent");
            // The host answers a superseded request "cancelled"; that is not the user's answer.
            testContext.answerDocumentCancelled(first);
            testContext.answerDocumentPicked(first, mixedCaseReference, "a b.csv", false);
            compare(answers(picker), 0);
            verify(picker.visible);
            testContext.answerDocumentPicked(second, mixedCaseReference, "a b.csv", false);
            compare(spiesOf(picker).accepted.count, 1);
            compare(callsNamed("discardPendingDownload").length, 0, "nothing in the Download folder to throw away");
        }

        function test_close_never_emits() {
            var picker = makePicker();
            picker.close();
            compare(callNames(), [], "nothing to cancel");
            picker.open();
            var id = testContext.lastDocumentRequest().id;
            picker.close();
            verify(!picker.visible);
            compare(cancelledIds(), [id]);
            testContext.answerDocumentPicked(id, mixedCaseReference, "a b.csv", false);
            testContext.answerDocumentCancelled(id);
            compare(answers(picker), 0, "close() ends the request silently, and a late answer stays unheard");
        }

        function test_cancel_emits_rejected_once_while_waiting() {
            var picker = makePicker();
            picker.cancel();
            compare(spiesOf(picker).rejected.count, 0, "nothing was waiting");
            compare(callNames(), []);
            picker.open();
            var id = testContext.lastDocumentRequest().id;
            picker.cancel();
            compare(spiesOf(picker).rejected.count, 1);
            verify(!picker.visible);
            compare(cancelledIds(), [id]);
            picker.cancel();
            testContext.answerDocumentCancelled(id);
            compare(spiesOf(picker).rejected.count, 1, "once");
            compare(answers(picker), 1);
            // After an answer there is nothing left to cancel.
            picker.open();
            testContext.answerDocumentPicked(testContext.lastDocumentRequest().id, mixedCaseReference, "a b.csv", false);
            picker.cancel();
            compare(spiesOf(picker).rejected.count, 1);
            compare(spiesOf(picker).accepted.count, 1);
        }

        function test_unavailable_messages_data() {
            return [
                {tag: "noPicker", message: "No compatible file picker on this device. Install a file manager app, then try again."},
                {tag: "noSavePicker", message: "This device has no save picker, and DSD-neo could not save to the Download folder."},
                {tag: "unreadable", message: "That app gave DSD-neo a file it can't open. Pick the file from another app."},
                {tag: "noResult", message: "No file came back from the picker. Try again."},
                {tag: "failed", message: "The file picker could not open. Try again."},
                {tag: "unsupported", message: "The file picker could not open. Try again."}
            ];
        }
        function test_unavailable_messages(data) {
            var picker = makePicker();
            compare(picker.messageFor(data.tag), data.message);
            picker.open();
            testContext.answerDocumentUnavailable(testContext.lastDocumentRequest().id, data.tag);
            compare(spiesOf(picker).unavailable.count, 1);
            compare(spiesOf(picker).unavailable.signalArguments[0][0], data.tag);
            compare(spiesOf(picker).unavailable.signalArguments[0][1], data.message);
            compare(answers(picker), 1);
            verify(!picker.visible);
        }

        // A Download-folder save that lands after the picker gave its request up
        // must not stay behind as a pending row: the picker throws it away.
        function test_stale_download_pick_is_discarded() {
            var picker = makePicker({mode: "save", suggestedName: "a.wav", mimeType: "audio/x-wav"});
            picker.open();
            var first = testContext.lastDocumentRequest().id;
            picker.open();
            var second = testContext.lastDocumentRequest().id;
            testContext.answerDocumentPicked(first, "content://media/external/downloads/7", "a.wav", true);
            compare(answers(picker), 0);
            compare(callsNamed("discardPendingDownload").map(function (call) { return call.reference; }), ["content://media/external/downloads/7"]);
            testContext.answerDocumentPicked(first, "content://media/external/downloads/7", "a.wav", true);
            compare(callsNamed("discardPendingDownload").length, 1, "one answer per request");

            picker.close();
            testContext.answerDocumentPicked(second, "content://media/external/downloads/8", "a (1).wav", true);
            compare(answers(picker), 0);
            compare(callsNamed("discardPendingDownload").length, 2, "a closed request's save is thrown away too");
            verify(!picker.savedToDownloads);
        }

        // One picker never throws away a save another picker is waiting for.
        function test_another_pickers_download_is_kept() {
            var owner = makePicker({mode: "save", suggestedName: "a.wav", mimeType: "audio/x-wav"});
            var bystander = makePicker({mode: "save", suggestedName: "b.wav", mimeType: "audio/x-wav"});
            bystander.open();
            var gone = testContext.lastDocumentRequest().id;
            bystander.close();
            owner.open();
            var id = testContext.lastDocumentRequest().id;
            testContext.answerDocumentPicked(id, "content://media/external/downloads/9", "a.wav", true);
            compare(spiesOf(owner).accepted.count, 1);
            verify(owner.savedToDownloads);
            compare(answers(bystander), 0);
            compare(callsNamed("discardPendingDownload").length, 0, "the owner's save stays");
            testContext.answerDocumentPicked(gone, "content://media/external/downloads/10", "b.wav", true);
            compare(callsNamed("discardPendingDownload").map(function (call) { return call.reference; }), ["content://media/external/downloads/10"]);
            compare(spiesOf(owner).accepted.count, 1);
        }

        function test_destruction_cancels() {
            var picker = pickerComponent.createObject(root);
            picker.open();
            var id = testContext.lastDocumentRequest().id;
            picker.destroy();
            tryVerify(function () { return cancelledIds().indexOf(id) >= 0; }, 2000, "the request is cancelled with its picker");
            var idle = pickerComponent.createObject(root);
            idle.open();
            idle.close();
            testContext.resetHostPlayback();
            idle.destroy();
            wait(0);
            compare(callNames(), [], "a picker with nothing waiting cancels nothing");
        }

        // The same answers through a real DecoderHost's signals.
        function test_lifecycle_host() {
            testContext.useLifecycleHost(true);
            var picker = makePicker({mode: "save", suggestedName: "x.dsdbak", mimeType: "application/octet-stream"});
            picker.open();
            var request = testContext.lastDocumentRequest();
            compare(request.kind, "save");
            compare(request.name, "x.dsdbak");
            testContext.answerDocumentPicked(request.id, mixedCaseReference, "a b.csv", true);
            compare(spiesOf(picker).accepted.signalArguments[0][0], mixedCaseReference);
            verify(picker.savedToDownloads);
            picker.open();
            var next = testContext.lastDocumentRequest().id;
            testContext.answerDocumentUnavailable(next, "noSavePicker");
            compare(spiesOf(picker).unavailable.signalArguments[0][1], picker.messageFor("noSavePicker"));
            compare(callNames(), ["requestSaveDocument", "requestSaveDocument"]);
        }

        // ---- in-app file browser (issue #830) ----

        function test_browser_first_when_toggled() {
            var picker = makeBrowsingPicker();
            picker.open();
            compare(callNames(), [], "Android's picker is not asked");
            var request = Ui.FileBrowser.request;
            verify(request !== null);
            verify(request.token > 0);
            compare(request.kind, "csv");
            compare(request.title, "Choose a CSV file");
            compare(request.allowOtherApp, true, "another app's picker is offered");
            verify(picker.visible, "a browse request is waiting");
            compare(answers(picker), 0);
        }

        function test_toggle_off_uses_host() {
            var picker = makeBrowsingPicker({}, false);
            picker.open();
            compare(callNames(), ["requestOpenDocument"]);
            compare(Ui.FileBrowser.request, null);
            verify(picker.visible);
        }

        // Each part of the rule alone keeps Android's picker: no browser on the
        // host, no kind named, a save.
        function test_browser_needs_every_condition_data() {
            return [
                {tag: "unsupported", supported: false, kind: "csv", mode: "open", call: "requestOpenDocument"},
                {tag: "no-kind", supported: true, kind: "", mode: "open", call: "requestOpenDocument"},
                {tag: "save", supported: true, kind: "backup", mode: "save", call: "requestSaveDocument"}
            ];
        }
        function test_browser_needs_every_condition(data) {
            var picker = makeBrowsingPicker({browseKind: data.kind, mode: data.mode, suggestedName: "a.dsdbak", mimeType: "application/octet-stream"});
            testContext.setFileBrowserSupported(data.supported);
            picker.open();
            compare(callNames(), [data.call]);
            compare(Ui.FileBrowser.request, null);
        }

        function test_no_picker_opens_browser() {
            var picker = makeBrowsingPicker({}, false);
            picker.open();
            var id = testContext.lastDocumentRequest().id;
            testContext.answerDocumentUnavailable(id, "noPicker");
            compare(spiesOf(picker).unavailable.count, 0, "the browser answers instead");
            var request = Ui.FileBrowser.request;
            verify(request !== null);
            compare(request.kind, "csv");
            compare(request.title, "Choose a CSV file");
            compare(request.allowOtherApp, false, "there is no other app to offer");
            verify(picker.visible);
            compare(answers(picker), 0);
            Ui.FileBrowser.finish(request.token, "file:///x/a.csv", "a.csv");
            compare(spiesOf(picker).accepted.count, 1);
            compare(answers(picker), 1, "one answer for the open");
        }

        function test_no_picker_without_browse_kind() {
            var picker = makeBrowsingPicker({browseKind: ""}, false);
            picker.open();
            testContext.answerDocumentUnavailable(testContext.lastDocumentRequest().id, "noPicker");
            compare(Ui.FileBrowser.request, null);
            compare(spiesOf(picker).unavailable.count, 1);
            compare(spiesOf(picker).unavailable.signalArguments[0][0], "noPicker");
            compare(spiesOf(picker).unavailable.signalArguments[0][1], picker.messageFor("noPicker"));
            verify(!picker.visible);
        }

        function test_no_picker_in_save_mode() {
            var picker = makeBrowsingPicker({mode: "save", browseKind: "backup", suggestedName: "a.dsdbak", mimeType: "application/octet-stream"});
            picker.open();
            compare(callNames(), ["requestSaveDocument"], "a save never browses");
            testContext.answerDocumentUnavailable(testContext.lastDocumentRequest().id, "noPicker");
            compare(Ui.FileBrowser.request, null);
            compare(spiesOf(picker).unavailable.count, 1);
            compare(spiesOf(picker).unavailable.signalArguments[0][0], "noPicker");
            compare(spiesOf(picker).unavailable.signalArguments[0][1], picker.messageFor("noPicker"));
        }

        // Only noPicker turns to the browser; any other reason is the owner's.
        function test_other_unavailable_reasons_stay() {
            var picker = makeBrowsingPicker({}, false);
            picker.open();
            testContext.answerDocumentUnavailable(testContext.lastDocumentRequest().id, "failed");
            compare(Ui.FileBrowser.request, null);
            compare(spiesOf(picker).unavailable.count, 1);
            compare(spiesOf(picker).unavailable.signalArguments[0][0], "failed");
        }

        function test_browser_pick_accepts_once() {
            var picker = makeBrowsingPicker();
            picker.open();
            var token = browseToken();
            Ui.FileBrowser.finish(token, "file:///x/a%20b.csv", "a b.csv");
            compare(Ui.FileBrowser.request, null);
            compare(spiesOf(picker).accepted.count, 1);
            compare(spiesOf(picker).accepted.signalArguments[0][0], "file:///x/a%20b.csv", "the reference as the browser gave it");
            compare(spiesOf(picker).accepted.signalArguments[0][1], "a b.csv", "the name as the browser gave it");
            verify(!picker.visible);
            verify(!picker.savedToDownloads);
            Ui.FileBrowser.finish(token, "file:///x/a%20b.csv", "a b.csv");
            Ui.FileBrowser.reject(token);
            compare(answers(picker), 1, "an answered request is answered once");
        }

        function test_close_is_silent() {
            var picker = makeBrowsingPicker();
            picker.open();
            var token = browseToken();
            picker.close();
            compare(Ui.FileBrowser.request, null, "the browser closes with the picker's request");
            verify(!picker.visible);
            Ui.FileBrowser.finish(token, "file:///x/a.csv", "a.csv");
            Ui.FileBrowser.reject(token);
            compare(answers(picker), 0);
            compare(callNames(), [], "nothing went to Android's picker");
        }

        function test_cancel_rejects_once() {
            var picker = makeBrowsingPicker();
            picker.open();
            var token = browseToken();
            picker.cancel();
            compare(Ui.FileBrowser.request, null);
            compare(spiesOf(picker).rejected.count, 1);
            verify(!picker.visible);
            picker.cancel();
            Ui.FileBrowser.reject(token);
            compare(spiesOf(picker).rejected.count, 1, "once");
            compare(answers(picker), 1);
        }

        function test_browser_back_rejects() {
            var picker = makeBrowsingPicker();
            picker.open();
            Ui.FileBrowser.reject(browseToken());
            compare(spiesOf(picker).rejected.count, 1);
            compare(answers(picker), 1);
            verify(!picker.visible);
        }

        function test_destruction_dismisses() {
            var picker = makeBrowsingPicker();
            // Not temporary: this case destroys it.
            var owned = pickerComponent.createObject(root, {browseKind: "replay", title: "Choose a replay file"});
            var spy = createTemporaryObject(spyComponent, testCase, {target: Ui.FileBrowser, signalName: "rejected"});
            var picked = createTemporaryObject(spyComponent, testCase, {target: Ui.FileBrowser, signalName: "picked"});
            owned.open();
            compare(Ui.FileBrowser.request.kind, "replay");
            owned.destroy();
            tryCompare(Ui.FileBrowser, "request", null, 2000, "the request goes with its picker");
            compare(spy.count, 0, "silently");
            compare(picked.count, 0);
            compare(answers(picker), 0, "a bystander hears nothing");
        }

        function test_same_picker_reopen() {
            var picker = makeBrowsingPicker();
            var spy = createTemporaryObject(spyComponent, testCase, {target: Ui.FileBrowser, signalName: "rejected"});
            picker.open();
            var first = browseToken();
            picker.open();
            var second = browseToken();
            verify(second !== first, "a new request");
            compare(spy.count, 0, "reopening is silent");
            compare(answers(picker), 0);
            verify(picker.visible);
            Ui.FileBrowser.finish(first, "file:///x/old.csv", "old.csv");
            compare(answers(picker), 0, "the first request is gone");
            Ui.FileBrowser.finish(second, "file:///x/new.csv", "new.csv");
            compare(spiesOf(picker).accepted.count, 1);
            compare(spiesOf(picker).accepted.signalArguments[0][1], "new.csv");
        }

        function test_cross_picker_supersede() {
            var first = makeBrowsingPicker();
            var second = makeBrowsingPicker({browseKind: "backup", title: "Choose a backup"});
            first.open();
            var firstToken = browseToken();
            var visibleWhenTold = [];
            var browserWhenTold = [];
            var record = function () {
                visibleWhenTold.push(first.visible);
                browserWhenTold.push(Ui.FileBrowser.request ? Ui.FileBrowser.request.kind : "");
            };
            first.rejected.connect(record);
            second.open();
            first.rejected.disconnect(record);
            compare(spiesOf(first).rejected.count, 1, "the first owner is told once");
            compare(visibleWhenTold, [false], "and no longer waits when it is told");
            compare(browserWhenTold, ["backup"], "the new request already shows");
            compare(answers(second), 0);
            verify(second.visible);
            Ui.FileBrowser.finish(firstToken, "file:///x/a.csv", "a.csv");
            compare(answers(first), 1, "its request is gone");
            Ui.FileBrowser.finish(browseToken(), "file:///x/a.dsdbak", "a.dsdbak");
            compare(spiesOf(second).accepted.count, 1);
            compare(answers(first), 1);
        }

        // The owner a browse supersedes may open its picker again while it is
        // told; the request that superseded it is then gone too, and its owner
        // hears rejected() once, after open() returns, rather than wait forever.
        function test_supersede_reopened_while_told() {
            var first = makeBrowsingPicker();
            var second = makeBrowsingPicker({browseKind: "backup", title: "Choose a backup"});
            first.open();
            var reopen = function () { first.open(); };
            first.rejected.connect(reopen);
            second.open();
            first.rejected.disconnect(reopen);
            compare(spiesOf(first).rejected.count, 1);
            verify(first.visible, "the first picker browses again");
            compare(Ui.FileBrowser.request.kind, "csv");
            compare(spiesOf(second).rejected.count, 0, "not before open() returns");
            verify(second.visible);
            tryCompare(spiesOf(second).rejected, "count", 1);
            verify(!second.visible);
            compare(answers(second), 1);
            Ui.FileBrowser.finish(browseToken(), "file:///x/a.csv", "a.csv");
            compare(spiesOf(first).accepted.count, 1);
            compare(answers(second), 1);

            // close() before the answer lands ends it silently, as ever.
            first.open();
            first.rejected.connect(reopen);
            second.open();
            first.rejected.disconnect(reopen);
            second.close();
            verify(!second.visible);
            wait(0);
            compare(answers(second), 1, "nothing after close()");
            first.close();
        }

        function test_other_app_loop() {
            var picker = makeBrowsingPicker();
            picker.open();
            Ui.FileBrowser.otherApp(browseToken());
            compare(Ui.FileBrowser.request, null);
            compare(callNames(), ["requestOpenDocument"], "Android's picker, though the toggle says browse first");
            verify(picker.visible);
            compare(answers(picker), 0);
            testContext.answerDocumentPicked(testContext.lastDocumentRequest().id, mixedCaseReference, "a b.csv", false);
            compare(spiesOf(picker).accepted.count, 1);
            compare(spiesOf(picker).accepted.signalArguments[0][0], mixedCaseReference);
            compare(answers(picker), 1);

            // Another app was asked for, and there is none: back to the
            // browser, without the way out that just failed.
            picker.open();
            Ui.FileBrowser.otherApp(browseToken());
            testContext.answerDocumentUnavailable(testContext.lastDocumentRequest().id, "noPicker");
            compare(spiesOf(picker).unavailable.count, 0);
            var request = Ui.FileBrowser.request;
            verify(request !== null);
            compare(request.allowOtherApp, false);
            compare(request.kind, "csv");
            Ui.FileBrowser.reject(request.token);
            compare(spiesOf(picker).rejected.count, 1);
            compare(answers(picker), 2, "one answer per open");
        }

        function test_host_request_unaffected_by_browser() {
            var host = makeBrowsingPicker({browseKind: ""});
            var browsing = makeBrowsingPicker();
            host.open();
            var id = testContext.lastDocumentRequest().id;
            browsing.open();
            verify(host.visible, "the host request still waits");
            verify(browsing.visible);
            compare(cancelledIds(), [], "nothing cancelled it");
            Ui.FileBrowser.reject(browseToken());
            compare(answers(host), 0);
            verify(host.visible);
            testContext.answerDocumentPicked(id, mixedCaseReference, "a b.csv", false);
            compare(spiesOf(host).accepted.count, 1);
            compare(answers(browsing), 1);
        }

        // A browsing picker that turns to Android's picker drops its browse
        // request first: one request per picker.
        function test_reopen_on_host_dismisses_browse() {
            var picker = makeBrowsingPicker();
            picker.open();
            var token = browseToken();
            testContext.setPrefs("chooseFilesInApp", false);
            picker.open();
            compare(Ui.FileBrowser.request, null);
            compare(callNames(), ["requestOpenDocument"]);
            Ui.FileBrowser.finish(token, "file:///x/a.csv", "a.csv");
            compare(answers(picker), 0);
            verify(picker.visible);
        }

        // ---- desktop path ----

        function test_desktop_open() {
            testContext.setDocumentPickerSupported(false);
            var picker = makePicker({title: "Choose a CSV"});
            var dialog = dialogOf(picker);
            compare(dialog.title, "Choose a CSV");
            picker.open();
            tryVerify(function () { return dialog.visible; }, 2000, "the FileDialog opens");
            verify(picker.visible);
            compare(dialog.fileMode, 0, "FileDialog.OpenFile");
            compare(callNames(), [], "the host is not asked");
            var path = testContext.writeFixtureCsv("Talk groups.csv", "tg,name\n1,Dispatch\n");
            dialog.selectedFile = "file://" + path;
            compare(dialog.selectedFiles.length, 1, "the fixture file is selected");
            var reference = dialog.selectedFile.toString();
            // dialog.accept() would take the platform dialog's own selection,
            // which a test cannot set: emit the answer with ours, then close as
            // the dialog does after an accept.
            dialog.accepted();
            dialog.close();
            compare(spiesOf(picker).accepted.count, 1);
            compare(spiesOf(picker).accepted.signalArguments[0][0], reference);
            compare(spiesOf(picker).accepted.signalArguments[0][1], "Talk groups.csv");
            verify(!picker.visible);
            verify(!picker.savedToDownloads);
            compare(answers(picker), 1, "one answer for one accept");

            // The second open starts with no selection, so an answer without a
            // new choice never hands back the first file.
            picker.open();
            tryVerify(function () { return dialog.visible; }, 2000);
            compare(dialog.selectedFile.toString(), "");
            compare(dialog.selectedFiles.length, 0);
            dialog.accept(); // the real path: nothing chosen in the dialog
            verify(!dialog.visible);
            compare(spiesOf(picker).accepted.count, 1, "the first file is not returned again");
            compare(spiesOf(picker).unavailable.count, 1);
            compare(spiesOf(picker).unavailable.signalArguments[0][0], "noResult");

            picker.open();
            tryVerify(function () { return dialog.visible; }, 2000);
            dialog.reject();
            compare(spiesOf(picker).rejected.count, 1);
            verify(!picker.visible);
        }

        function test_desktop_save() {
            testContext.setDocumentPickerSupported(false);
            var picker = makePicker({mode: "save", suggestedName: "dsd-neo-backup-2026-11-08.dsdbak", nameFilters: ["DSD-neo backup (*.dsdbak)"], defaultSuffix: "dsdbak"});
            var dialog = dialogOf(picker);
            picker.open();
            tryVerify(function () { return dialog.visible; }, 2000);
            compare(dialog.fileMode, 2, "FileDialog.SaveFile");
            compare(dialog.selectedFile.toString(), "dsd-neo-backup-2026-11-08.dsdbak");
            compare(dialog.nameFilters, ["DSD-neo backup (*.dsdbak)"]);
            compare(dialog.defaultSuffix, "dsdbak");
            dialog.accept();
            verify(!dialog.visible);
            compare(spiesOf(picker).accepted.count, 1);
            compare(spiesOf(picker).accepted.signalArguments[0][0], dialog.selectedFile.toString());
            compare(spiesOf(picker).accepted.signalArguments[0][1], "dsd-neo-backup-2026-11-08.dsdbak");
            compare(answers(picker), 1);
            verify(!picker.savedToDownloads, "the desktop never saves to the Download folder on its own");
        }

        function test_desktop_close_and_cancel() {
            testContext.setDocumentPickerSupported(false);
            var picker = makePicker();
            var dialog = dialogOf(picker);
            picker.open();
            tryVerify(function () { return dialog.visible; }, 2000);
            picker.close();
            verify(!dialog.visible);
            verify(!picker.visible);
            compare(answers(picker), 0, "close() is silent");
            picker.open();
            tryVerify(function () { return dialog.visible; }, 2000);
            picker.cancel();
            verify(!dialog.visible);
            compare(spiesOf(picker).rejected.count, 1);
            picker.cancel();
            compare(spiesOf(picker).rejected.count, 1, "once");
            compare(answers(picker), 1);
        }

        // open() while the dialog is up starts over silently: the dialog closed
        // on the way says nothing, and the reopened one answers once.
        function test_desktop_reopen_while_open() {
            testContext.setDocumentPickerSupported(false);
            var picker = makePicker();
            var dialog = dialogOf(picker);
            picker.open();
            tryVerify(function () { return dialog.visible; }, 2000);
            var path = testContext.writeFixtureCsv("first.csv", "tg,name\n1,Dispatch\n");
            dialog.selectedFile = "file://" + path;
            picker.open();
            tryVerify(function () { return dialog.visible; }, 2000, "open again");
            verify(picker.visible);
            compare(answers(picker), 0, "no rejected for the dialog closed on the way");
            compare(dialog.selectedFiles.length, 0, "the reopened dialog starts with no selection");
            dialog.accept();
            compare(spiesOf(picker).accepted.count, 0, "the file chosen before the reopen is gone");
            compare(spiesOf(picker).unavailable.count, 1);
            compare(answers(picker), 1, "one answer for the reopened request");
            verify(!picker.visible);
        }

        // A dialog that cannot open answers "failed" rather than leave its owner
        // waiting, and never before open() returns.
        function test_desktop_dialog_without_a_window_fails() {
            testContext.setDocumentPickerSupported(false);
            var picker = makePicker();
            // No parent, so no window to open in; temporary, so a failed case
            // does not leave it to later ones.
            var stranded = createTemporaryObject(pickerComponent, null);
            verify(stranded, "a picker without a parent");
            var spy = createTemporaryObject(spyComponent, testCase, {target: stranded, signalName: "unavailable"});
            stranded.open();
            compare(spy.count, 0, "not before open() returns");
            tryCompare(spy, "count", 1);
            compare(spy.signalArguments[0][0], "failed");
            compare(spy.signalArguments[0][1], "The file picker could not open. Try again.");
            verify(!stranded.visible);
            compare(answers(picker), 0);
        }
    }
}
