// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #708: Settings → Manage call recordings. The store is the `recordings` map (qml_test_context.h): a case sets
// its readings and reads back the calls the screen made; the decoderHost map runs the export.
Item {
    width: 420
    height: 900
    Loader { id: loader; anchors.fill: parent; source: uiDir + "/CallRecordingsScreen.qml" }

    TestCase {
        name: "CallRecordingsScreen"
        when: windowShown

        readonly property var county: ({uid: "u1", kind: "saved", name: "County P25", count: 842, bytes: 210000000, first: 1761300000, last: 1761600000, available: true})
        readonly property var metro: ({uid: "u2", kind: "scan", name: "Metro scan", count: 1, bytes: 96000, first: 1761500000, last: 1761500000, available: true})
        readonly property var away: ({uid: "explore", kind: "explore", name: "Explore", count: 22, bytes: 6000000, first: 1761550000, last: 1761550000, available: false})

        function screen() {
            return loader.item;
        }
        function item(name) {
            return findChild(loader.item, name);
        }
        function lastStoreCall() {
            var calls = recordings.calls();
            return calls.length > 0 ? calls[calls.length - 1] : ({});
        }
        function lastHostCall() {
            var calls = testContext.hostCalls();
            return calls.length > 0 ? calls[calls.length - 1] : ({});
        }
        function hostCallsNamed(name) {
            return testContext.hostCalls().filter(function (call) { return call.name === name; });
        }
        // Issue #829: the save request the export waits on, answered by the host.
        function exportRequest() {
            var request = testContext.lastDocumentRequest();
            compare(request.kind, "save");
            compare(request.mime, "application/zip");
            verify(item("callRecordingsExportPicker").visible, "the picker waits");
            return request.id;
        }
        function pick(reference, inDownloads) {
            testContext.answerDocumentPicked(exportRequest(), reference, reference.split("/").pop(), inDownloads === true);
        }
        function fill() {
            recordings.folders = [county, metro, {uid: "empty", kind: "saved", name: "Quiet", count: 0, bytes: 0, first: 0, last: 0, available: true}];
            recordings.recordingCount = 843;
            recordings.usedBytes = 210096000;
        }
        function menuAction(name) {
            var menu = item("callRecordingsFolderMenu");
            verify(menu.visible, "the folder menu is open");
            for (var i = 0; i < menu.actions.length; i++) {
                if (menu.actions[i].objectName === name) {
                    menu.activate(i);
                    return;
                }
            }
            fail("no action " + name + " in " + JSON.stringify(menu.actions));
        }

        function init() {
            verify(screen() !== null);
            recordings.reset();
            testContext.resetHostPlayback();
            testContext.setHostReading("recordingExportSupported", true);
            testContext.setPrefs("recordCalls", false);
            screen().notice = "";
            screen().exportSelection = 0;
            screen().removedShown = 0;
            screen().failuresShown = 0;
        }
        function cleanup() {
            for (var name of ["callRecordingsFolderMenu", "callRecordingsRange", "callRecordingsDeleteConfirm"])
                item(name).visible = false;
            item("callRecordingsExportPicker").cancel();
            screen().visible = true;
            recordings.reset();
            testContext.resetHostPlayback();
            testContext.setHostReading("recordingExportSupported", false);
            testContext.setPrefs("recordCalls", false);
        }

        function test_empty_state() {
            verify(item("callRecordingsEmpty").visible);
            verify(!item("callRecordingsFolders").visible);
            verify(!item("callRecordingsUsage").visible);
            compare(item("callRecordingsEmptyDetail").text, "Turn on Record calls in Settings, and every call you hear is saved here to play from History.");
            testContext.setPrefs("recordCalls", true);
            compare(item("callRecordingsEmptyDetail").text, "Calls you hear are saved here. Open one in History to play it.");
            recordings.indexed = false;
            verify(!item("callRecordingsEmpty").visible, "not empty before the store has counted");
            compare(item("callRecordingsUsage").text, "Counting recordings…");
        }

        function test_folders() {
            fill();
            verify(!item("callRecordingsEmpty").visible);
            compare(item("callRecordingsUsage").text, "210 MB of 1 GB · 843 recordings");
            compare(item("callRecordingsFolder0").title, "County P25");
            var now = new Date(savedSystems.realtimeNowMs());
            var format = function (stamp) {
                var date = new Date(stamp * 1000);
                return Qt.formatDate(date, date.getFullYear() === now.getFullYear() ? "d MMM" : "d MMM yyyy");
            };
            compare(item("callRecordingsFolder0").subtitle, "842 recordings · 210 MB · " + format(county.first) + " – " + format(county.last));
            compare(item("callRecordingsFolder1").subtitle, "1 recording · 96 kB · " + format(metro.first), "one day reads as one date");
            compare(item("callRecordingsFolder2"), null, "a folder with nothing in it is not listed");
            verify(!item("callRecordingsUnfinished").visible);
            recordings.unfinishedCount = 2;
            recordings.unfinishedBytes = 4100000;
            verify(item("callRecordingsUnfinished").visible);
            compare(item("callRecordingsUnfinished").subtitle, "2 recordings · 4.1 MB · left when listening stopped unexpectedly");
        }

        function test_storage_away() {
            recordings.folders = [away];
            recordings.recordingCount = 22;
            compare(item("callRecordingsFolder0").subtitle, "22 recordings · 6 MB · storage not available");
            verify(!item("callRecordingsAway").visible);
            recordings.available = false;
            verify(item("callRecordingsAway").visible);
            verify(!item("callRecordingsExportAll").enabled, "nothing to export from storage that is away");
            item("callRecordingsFolder0").activate();
            var menu = item("callRecordingsFolderMenu");
            verify(menu.actions.every(function (action) { return action.enabled === false; }), "its row's actions wait for it");
        }

        function test_export_folder() {
            fill();
            item("callRecordingsFolder0").activate();
            compare(item("callRecordingsFolderMenu").title, "County P25");
            menuAction("folderActionExport");
            var range = item("callRecordingsRange");
            verify(range.visible);
            compare(range.accessibleName, "Export County P25");
            compare(range.choices, ["Today", "Yesterday", "Last 7 days", "Last 30 days", "All"]);

            recordings.setSelection(0, 0, []);
            findChild(range, "callRecordingsRange0").activate();
            compare(lastStoreCall().name, "selectForExport");
            compare(lastStoreCall().folderUid, "u1");
            compare(lastStoreCall().preset, "today");
            compare(item("callRecordingsNotice").text, "No recordings in that range.");
            compare(screen().exportSelection, 0, "no picker for an empty range");

            recordings.setSelection(12, 40000000, ["/r/a.wav"]);
            item("callRecordingsFolder0").activate();
            menuAction("folderActionExport");
            findChild(range, "callRecordingsRange2").activate();
            compare(lastStoreCall().preset, "last7");
            var selection = screen().exportSelection;
            verify(selection > 0);
            // B1: the reference reaches the export as the host sent it.
            var reference = "content://com.Example.Provider/document/primary%3ADownload%2Fmy%20calls.zip";
            pick(reference);
            compare(lastHostCall().name, "exportRecordings");
            compare(lastHostCall().reference, reference);
            compare(lastHostCall().selection.selection, selection);

            // Running: progress and a way out.
            verify(item("callRecordingsExportProgress").visible);
            verify(!item("callRecordingsExportAll").enabled, "one export at a time");
            testContext.setHostReading("recordingTransferBytes", 18000000);
            testContext.setHostReading("recordingTransferTotalBytes", 40000000);
            compare(item("callRecordingsExportText").text, "Exporting… 18 MB of 40 MB");
            // A folder's export waits for this one, as Export all does, and says why if reached.
            item("callRecordingsFolder1").activate();
            var actions = item("callRecordingsFolderMenu").actions;
            var exportAction = actions.filter(function (action) { return action.objectName === "folderActionExport"; })[0];
            verify(exportAction.enabled === false, "a folder's Export as zip waits for the running export");
            item("callRecordingsFolderMenu").visible = false;
            var selects = recordings.calls().filter(function (call) { return call.name === "selectForExport"; }).length;
            screen().beginExport(metro, "today");
            compare(item("callRecordingsNotice").text, "An export is running. Export again when it finishes.");
            compare(recordings.calls().filter(function (call) { return call.name === "selectForExport"; }).length, selects, "nothing new is selected");
            compare(screen().exportSelection, selection);
            item("callRecordingsCancelExport").activate();
            compare(lastHostCall().name, "cancelRecordingTransfer");

            testContext.finishRecordingTransfer(selection, 0, 2);
            verify(!item("callRecordingsExportProgress").visible);
            compare(item("callRecordingsNotice").text, "Exported 10 recordings. 2 were gone and left out.");
        }

        function test_export_results_data() {
            return [
                {tag: "done", result: 0, missing: 0, text: "Exported 12 recordings.", problem: false},
                {tag: "one-gone", result: 0, missing: 1, text: "Exported 11 recordings. 1 was gone and left out.", problem: false},
                {tag: "failed", result: 1, missing: 0, text: "The export couldn't be written. Try another location.", problem: true},
                {tag: "cancelled", result: 2, missing: 0, text: "Export cancelled.", problem: false},
                {tag: "missing", result: 3, missing: 12, text: "Those recordings are gone.", problem: true},
                {tag: "busy", result: 4, missing: 0, text: "Another export is running. Try again when it finishes.", problem: true},
                {tag: "unsupported", result: 5, missing: 0, text: "This device can't export call recordings.", problem: true}
            ];
        }
        function test_export_results(data) {
            fill();
            recordings.setSelection(12, 40000000, ["/r/a.wav"]);
            item("callRecordingsExportAll").activate();
            var range = item("callRecordingsRange");
            compare(range.accessibleName, "Export all");
            findChild(range, "callRecordingsRange4").activate();
            compare(lastStoreCall().folderUid, "");
            compare(lastStoreCall().preset, "all");
            var selection = screen().exportSelection;
            pick("content://picked/all.zip");
            testContext.finishRecordingTransfer(selection + 50, 1, 0);
            compare(item("callRecordingsNotice").text, "", "a save from a detail sheet is not this screen's");
            testContext.finishRecordingTransfer(selection, data.result, data.missing);
            compare(item("callRecordingsNotice").text, data.text);
            compare(screen().noticeProblem, data.problem);
            compare(item("callRecordingsNotice").Accessible.role, data.problem ? Accessible.AlertMessage : Accessible.StaticText);
            compare(screen().exportSelection, 0);
        }

        function test_export_picker_dismissed() {
            fill();
            recordings.setSelection(12, 40000000, ["/r/a.wav"]);
            screen().beginExport(county, "last30");
            var selection = screen().exportSelection;
            verify(selection > 0);
            var id = exportRequest();
            testContext.answerDocumentCancelled(id);
            compare(lastStoreCall().name, "releaseSelection");
            compare(lastStoreCall().selection, selection, "a dismissed picker lets the files go");
            compare(screen().exportSelection, 0);
            compare(testContext.hostCalls().map(function (call) { return call.name; }), ["requestSaveDocument"], "and nothing was written");
            compare(screen().notice, "", "backing out says nothing");
        }

        // Issue #829: no picker could ask. The files go again at once, the
        // reason shows as a problem and is read out, and Export works again.
        function test_export_picker_unavailable() {
            fill();
            recordings.setSelection(12, 40000000, ["/r/a.wav"]);
            var said = screen().announcements;
            screen().beginExport(county, "last30");
            var selection = screen().exportSelection;
            var id = exportRequest();
            testContext.answerDocumentUnavailable(id, "noSavePicker");
            compare(lastStoreCall().name, "releaseSelection");
            compare(lastStoreCall().selection, selection, "the hold is let go");
            compare(screen().exportSelection, 0);
            compare(hostCallsNamed("exportRecordings").length, 0);
            var message = "This device has no save picker, and DSD-neo could not save to the Download folder.";
            compare(item("callRecordingsNotice").text, message);
            verify(screen().noticeProblem);
            compare(item("callRecordingsNotice").Accessible.role, Accessible.AlertMessage);
            compare(screen().announcements, said + 1, "read out once");
            compare(screen().announced, message);
            testContext.answerDocumentPicked(id, "content://late/doc.zip", "doc.zip", false);
            compare(hostCallsNamed("exportRecordings").length, 0, "a late answer writes nothing");
            verify(item("callRecordingsExportAll").enabled, "Export works again");
            screen().beginExport(county, "last30");
            testContext.answerDocumentUnavailable(exportRequest(), "noSavePicker");
            compare(screen().announcements, said + 2, "a repeat is read out again");
        }

        // B5: while the picker is still asking nothing is exported yet. Export
        // all waits like a folder's Export does, and "An export is running"
        // is said only once a transfer runs.
        function test_export_all_waits_for_the_picker_without_saying_running() {
            fill();
            recordings.setSelection(12, 40000000, ["/r/a.wav"]);
            screen().beginExport(county, "last30");
            var selection = screen().exportSelection;
            verify(selection > 0);
            verify(!decoderHost.recordingTransferActive);
            verify(!item("callRecordingsExportAll").enabled, "Export all waits while the picker asks");
            var selects = recordings.calls().filter(function (call) { return call.name === "selectForExport"; }).length;
            screen().beginExport(metro, "today");
            compare(item("callRecordingsNotice").text, "", "no export is running yet");
            compare(recordings.calls().filter(function (call) { return call.name === "selectForExport"; }).length, selects, "nothing new is selected");
            compare(screen().exportSelection, selection);
            pick("content://picked/doc.zip");
            verify(!item("callRecordingsExportAll").enabled);
            screen().beginExport(metro, "today");
            compare(item("callRecordingsNotice").text, "An export is running. Export again when it finishes.");
            testContext.finishRecordingTransfer(selection, 0, 0);
            verify(item("callRecordingsExportAll").enabled);
        }

        // Issue #829: with no save picker the export went to Download/DSD-neo;
        // the result counts it and names the file there, followed by what was
        // left out, and is read out, as it is the only place that says where.
        function test_export_saved_to_downloads_data() {
            return [
                {tag: "named", count: 12, name: "Call recordings County P25 2026-11-08 (1).zip", missing: 2, readOut: true, text: "Exported 10 recordings to Download/DSD-neo as Call recordings County P25 2026-11-08 (1).zip. 2 were gone and left out."},
                {tag: "named-all-there", count: 12, name: "Call recordings 2026-11-08.zip", missing: 0, readOut: true, text: "Exported 12 recordings to Download/DSD-neo as Call recordings 2026-11-08.zip."},
                {tag: "named-one", count: 1, name: "Call recordings 2026-11-08 (2).zip", missing: 0, readOut: true, text: "Exported 1 recording to Download/DSD-neo as Call recordings 2026-11-08 (2).zip."},
                {tag: "named-with-marker", count: 12, name: "Calls %1 100%.zip", missing: 0, readOut: true, text: "Exported 12 recordings to Download/DSD-neo as Calls %1 100%.zip."},
                // A system name holding markup is shown as saved, not drawn as markup.
                {tag: "named-with-markup", count: 12, name: "Call recordings Fire &lt;East&gt; <b>2</b>.zip", missing: 0, readOut: true, text: "Exported 12 recordings to Download/DSD-neo as Call recordings Fire &lt;East&gt; <b>2</b>.zip."},
                {tag: "no-name", count: 12, name: "", missing: 1, readOut: false, text: "Exported 11 recordings. 1 was gone and left out."}
            ];
        }
        function test_export_saved_to_downloads(data) {
            fill();
            recordings.setSelection(data.count, 40000000, ["/r/a.wav"]);
            screen().beginExport(county, "all");
            var selection = screen().exportSelection;
            var reference = "content://media/external/downloads/" + data.tag;
            if (data.name.length > 0)
                testContext.setSavedDocumentName(reference, data.name);
            pick(reference, true);
            compare(lastHostCall().name, "exportRecordings");
            compare(lastHostCall().reference, reference);
            var said = screen().announcements;
            testContext.finishRecordingTransfer(selection, 0, data.missing);
            compare(item("callRecordingsNotice").text, data.text);
            compare(item("callRecordingsNotice").textFormat, Text.PlainText);
            verify(!screen().noticeProblem);
            compare(item("callRecordingsNotice").Accessible.role, Accessible.StaticText);
            compare(screen().announcements, said + (data.readOut ? 1 : 0), "read out once only when it names the file");
            if (data.readOut)
                compare(screen().announced, data.text);

            // A failed write never claims the Download folder; the next export
            // through a picker reads as before.
            screen().beginExport(county, "all");
            selection = screen().exportSelection;
            pick(reference, true);
            testContext.finishRecordingTransfer(selection, 1, 0);
            compare(item("callRecordingsNotice").text, "The export couldn't be written. Try another location.");
            screen().beginExport(county, "all");
            selection = screen().exportSelection;
            pick("content://picked/other.zip", false);
            said = screen().announcements;
            testContext.finishRecordingTransfer(selection, 0, 0);
            compare(item("callRecordingsNotice").text, data.count === 1 ? "Exported 1 recording." : "Exported 12 recordings.");
            compare(screen().announcements, said, "an ordinary result is not read out");
        }

        // Issue #829: leaving the screen (Back clears callRecordingsOpen in
        // Main) while the picker asks lets the files go, and its late answer
        // writes nothing; an export already writing runs on.
        function test_closing_ends_the_picker_and_lets_the_files_go() {
            fill();
            recordings.setSelection(12, 40000000, ["/r/a.wav"]);
            screen().beginExport(county, "all");
            var selection = screen().exportSelection;
            var id = exportRequest();
            screen().visible = false;
            verify(testContext.documentCalls().some(function (call) {
                return call.name === "cancelDocumentRequest" && call.id === id;
            }), "the host request was cancelled");
            compare(lastStoreCall().name, "releaseSelection");
            compare(lastStoreCall().selection, selection);
            compare(screen().exportSelection, 0);
            testContext.answerDocumentPicked(id, "content://late/doc.zip", "doc.zip", false);
            compare(hostCallsNamed("exportRecordings").length, 0);
            screen().visible = true;

            screen().beginExport(county, "all");
            selection = screen().exportSelection;
            pick("content://picked/doc.zip");
            var calls = recordings.calls().length;
            screen().visible = false;
            compare(recordings.calls().length, calls, "a running export keeps its files");
            compare(hostCallsNamed("cancelRecordingTransfer").length, 0, "and runs on");
            compare(screen().exportSelection, selection);
            screen().visible = true;
            testContext.finishRecordingTransfer(selection, 0, 0);
        }

        function test_export_unsupported() {
            fill();
            testContext.setHostReading("recordingExportSupported", false);
            verify(!item("callRecordingsExportAll").visible);
            compare(item("callRecordingsDeleteAll").width, item("callRecordingsFolders").width);
            item("callRecordingsFolder0").activate();
            var menu = item("callRecordingsFolderMenu");
            compare(menu.actions.length, 1);
            compare(menu.actions[0].objectName, "folderActionDelete");
        }

        function test_delete_data() {
            return [
                {tag: "system", folder: 0, action: "Delete all for this system", title: "Delete recordings of County P25?", message: "Only the audio of 842 recordings is deleted. History and detections are unchanged.", call: "deleteFolder", uid: "u1"},
                {tag: "scan", folder: 1, action: "Delete all for this scan list", title: "Delete recordings of Metro scan?", message: "Only the audio of 1 recording is deleted. History and detections are unchanged.", call: "deleteFolder", uid: "u2"}
            ];
        }
        // Issue #809 B3: a bulk delete takes only the files. A recording with no
        // History row (never linked, or trimmed away) is deleted too, so no
        // confirmation promises that the calls stay in History.
        function test_delete_says_only_the_audio_goes() {
            fill();
            item("callRecordingsFolder0").activate();
            menuAction("folderActionDelete");
            var message = findChild(item("callRecordingsDeleteConfirm"), "confirmMessage").text;
            verify(message.indexOf("stay in History") < 0, message);
            verify(message.indexOf("History and detections are unchanged.") > 0, message);
            findChild(item("callRecordingsDeleteConfirm"), "confirmCancelButton").activate();
        }
        function test_delete(data) {
            fill();
            item("callRecordingsFolder" + data.folder).activate();
            var menu = item("callRecordingsFolderMenu");
            compare(menu.actions[menu.actions.length - 1].text, data.action);
            verify(menu.actions[menu.actions.length - 1].destructive);
            menuAction("folderActionDelete");
            var confirm = item("callRecordingsDeleteConfirm");
            verify(confirm.visible);
            compare(confirm.title, data.title);
            compare(findChild(confirm, "confirmMessage").text, data.message);
            compare(findChild(confirm, "confirmActionButton").text, "Delete recordings");
            findChild(confirm, "confirmActionButton").activate();
            compare(lastStoreCall().name, data.call);
            compare(lastStoreCall().uid, data.uid);
        }

        function test_delete_explore_unfinished_and_all() {
            recordings.folders = [{uid: "explore", kind: "explore", name: "Explore", count: 3, bytes: 300000, first: 1761550000, last: 1761550000, available: true}];
            recordings.recordingCount = 3;
            recordings.unfinishedCount = 2;
            item("callRecordingsFolder0").activate();
            var menu = item("callRecordingsFolderMenu");
            compare(menu.actions[menu.actions.length - 1].text, "Delete all from Explore");
            menu.cancel();

            // Like a folder row, it opens its actions rather than a confirmation.
            item("callRecordingsUnfinished").activate();
            compare(menu.title, "Unfinished recordings");
            compare(menu.actions.length, 1);
            compare(menu.actions[0].text, "Delete unfinished");
            verify(menu.actions[0].destructive);
            menuAction("unfinishedActionDelete");
            var confirm = item("callRecordingsDeleteConfirm");
            compare(confirm.title, "Delete unfinished recordings?");
            compare(findChild(confirm, "confirmMessage").text, "They were left when listening stopped unexpectedly, and may be cut short.");
            compare(findChild(confirm, "confirmActionButton").text, "Delete unfinished");
            findChild(confirm, "confirmActionButton").activate();
            compare(lastStoreCall().name, "deleteUnfinished");

            item("callRecordingsDeleteAll").activate();
            compare(confirm.title, "Delete all call recordings?");
            compare(findChild(confirm, "confirmActionButton").text, "Delete all");
            compare(findChild(confirm, "confirmMessage").text, "Only the audio of 5 recordings is deleted. History and detections are unchanged.");
            findChild(confirm, "confirmCancelButton").activate();
            compare(lastStoreCall().name, "deleteUnfinished", "a cancelled confirmation deletes nothing");
            item("callRecordingsDeleteAll").activate();
            findChild(confirm, "confirmActionButton").activate();
            compare(lastStoreCall().name, "deleteAll");
        }

        // The store's failed deletes and the limit's removals show here and are acknowledged as they show, so Settings
        // stops repeating them; each visit starts from what is new.
        function test_delete_failures_and_limit_notices() {
            fill();
            verify(!item("callRecordingsDeleteFailures").visible);
            verify(!item("callRecordingsRemovedByLimit").visible);
            recordings.deleteFailures = 1;
            recordings.lastDeleteError = "Permission denied";
            tryCompare(recordings, "deleteFailures", 0);
            compare(lastStoreCall().name, "acknowledgeNotices");
            verify(item("callRecordingsDeleteFailures").visible);
            compare(item("callRecordingsDeleteFailures").text, "1 recording couldn't be deleted. The phone said: Permission denied");
            recordings.deleteFailures = 3;
            recordings.lastDeleteError = "Read-only file system";
            tryCompare(recordings, "deleteFailures", 0);
            compare(item("callRecordingsDeleteFailures").text, "4 recordings couldn't be deleted. The phone said: Read-only file system");
            recordings.removedByLimit = 2;
            tryCompare(recordings, "removedByLimit", 0);
            compare(item("callRecordingsRemovedByLimit").text, "The storage limit removed the 2 oldest recordings.");

            // Away, nothing is taken; the next visit shows only what came since.
            screen().visible = false;
            recordings.removedByLimit = 1;
            wait(20);
            compare(recordings.removedByLimit, 1, "not acknowledged unseen");
            screen().visible = true;
            compare(recordings.removedByLimit, 0);
            compare(item("callRecordingsRemovedByLimit").text, "The storage limit removed the oldest recording.");
            verify(!item("callRecordingsDeleteFailures").visible);
        }

        function test_deletes_waiting() {
            fill();
            verify(!item("callRecordingsDeletesWaiting").visible);
            recordings.deletesWaiting = 1;
            compare(item("callRecordingsDeletesWaiting").text, "1 recording is deleted once it stops playing, uploading or exporting.");
            recordings.deletesWaiting = 3;
            compare(item("callRecordingsDeletesWaiting").text, "3 recordings are deleted once they stop playing, uploading or exporting.");
        }

        function test_export_file_name_is_safe() {
            recordings.folders = [{uid: "u9", kind: "saved", name: "Fire/EMS: \"North\" <1>", count: 2, bytes: 2000, first: 1761300000, last: 1761300000, available: true}];
            recordings.recordingCount = 2;
            recordings.setSelection(2, 2000, ["/r/a.wav"]);
            screen().beginExport(recordings.folders[0], "all");
            var name = findChild(screen(), "callRecordingsExportPicker").suggestedName;
            verify(name.indexOf("Fire_EMS_ _North_ _1_") >= 0, name);
            compare(testContext.lastDocumentRequest().name, name, "the host is asked with that name");
            testContext.answerDocumentCancelled(exportRequest());
        }

        function test_storage_away_wording_matches_settings() {
            recordings.available = false;
            compare(item("callRecordingsAway").text, "Storage for call recordings isn't available right now. Saved recordings are kept.");
        }

        function test_back() {
            var closed = 0;
            screen().closed.connect(function () { closed++; });
            item("callRecordingsBack").activate();
            compare(closed, 1);
        }
    }
}
