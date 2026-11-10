// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// #832: the wizard's imports run on the import worker and answer later.
//
// A replay picked with Browse is copied through `replayFiles` (scripted here by
// the case): the Replay file row shows the copy's progress with a way out,
// Continue and Save wait for it, a failure says why and keeps the file that was
// there, and a wizard that closes or opens again drops the copy.
//
// A CSV import remembers the field it fills when it starts, so nothing that
// moves the picker meanwhile redirects it, and a wizard opened again since
// ignores what an earlier import answers.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: wizardLoader

        anchors.fill: parent
        source: uiDir + "/WizardScreen.qml"
    }

    // A reseed that lands after a commit ran and before its answer is in.
    Connections {
        id: reseedOnCommit

        target: importedFiles
        enabled: false
        function onEntriesChanged() {
            reseedOnCommit.enabled = false;
            wizardLoader.item.openForAdd(false);
        }
    }

    TestCase {
        id: tc

        name: "WizardReplayImport"
        when: windowShown

        readonly property var wizard: wizardLoader.item
        property int savedRow: -1

        function init() {
            verify(tc.wizard !== null, "WizardScreen.qml failed to load");
            tc.wizard.visible = true;
            tc.wizard.openForAdd(false);
            tc.wizard.sourceType = "file";
            testContext.resetReplayFiles();
        }
        function cleanup() {
            if (Ui.FileBrowser.request)
                Ui.FileBrowser.dismiss(Ui.FileBrowser.request.token);
            testContext.setFileBrowserSupported(false);
            testContext.setPrefs("chooseFilesInApp", false);
            tc.wizard.visible = true;
            tc.wizard.openForAdd(false);
            testContext.resetReplayFiles();
            if (tc.savedRow >= 0) {
                savedSystems.remove(tc.savedRow);
                tc.savedRow = -1;
            }
        }
        function item(name) {
            var found = findChild(tc.wizard, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        // A provider's document, as Browse hands it on (tst_wizard_browse
        // drives Browse and the picker themselves).
        function pickReplay(reference) {
            tc.wizard.beginReplay(reference, reference.substring(reference.lastIndexOf('/') + 1));
        }

        // A local file (the desktop's pick) is used where it is: the store
        // answers with its path on the next turn and copies nothing, so the
        // row never shows a copy, not even for that turn, and Continue stays.
        function test_browse_uses_a_local_file_without_a_copy() {
            var path = testContext.writeFixtureCsv("browse-capture.wav", "RIFF");
            tc.wizard.fileText = "/data/before.wav";
            var progress = item("wizardReplayProgress");
            var watch = Qt.createQmlObject("import QtTest; SignalSpy { signalName: \"visibleChanged\" }", root);
            watch.target = progress;
            item("wizardReplayBrowse").clicked();
            testContext.answerDocumentPicked(testContext.lastDocumentRequest().id, "file://" + path, "browse-capture.wav", false);
            compare(testContext.replayCalls().begins, 1);
            compare(testContext.replayCalls().reference, "file://" + path);
            verify(!progress.visible, "no copy shows for a local file");
            verify(item("wizardReplayBrowse").enabled);
            verify(item("wizardContinue").inputValid, "Continue does not flicker off");
            tryCompare(tc.wizard, "fileText", path);
            compare(watch.count, 0, "the copy row never showed");
            verify(!tc.wizard.replayPending);
            watch.destroy();
        }

        // #830: with DSD-neo's file browser chosen in Settings, Browse asks
        // it for a replay file. The browsed file's reference reaches the
        // store as it came and is copied (a local pick is copied where the
        // host has the browser), shown by the name the browser gave; a wizard
        // that closes takes the browser with it.
        function test_replay_browse() {
            testContext.setFileBrowserSupported(true);
            testContext.setPrefs("chooseFilesInApp", true);
            var reference = "file:///storage/emulated/0/Download/a%20b%231%25.csv";
            var hostCalls = testContext.documentCalls().length;
            item("wizardReplayBrowse").clicked();
            var request = Ui.FileBrowser.request;
            verify(request !== null, "the browser is asked");
            compare(request.kind, "replay");
            compare(request.title, "Choose a replay file");
            compare(testContext.documentCalls().length, hostCalls, "Android's picker is not asked");
            Ui.FileBrowser.finish(request.token, reference, "a b#1%.csv");
            compare(testContext.replayCalls().begins, 1);
            compare(testContext.replayCalls().reference, reference, "the reference reaches the store as it came");
            verify(tc.wizard.replayCopying, "the local pick copies");
            compare(item("wizardReplayProgressText").text, "Copying a b#1%.csv…");
            verify(testContext.finishReplay({ok: true, path: "/data/replays/a b#1%.csv"}));
            compare(tc.wizard.fileText, "/data/replays/a b#1%.csv");

            item("wizardReplayBrowse").clicked();
            verify(Ui.FileBrowser.request !== null);
            tc.wizard.visible = false;
            compare(Ui.FileBrowser.request, null, "a hidden wizard ends its browse");
            compare(testContext.replayCalls().begins, 1, "and nothing is picked");
        }

        // #830: the store is handed the host's name as given; only a pick
        // that came with no name is named from its reference, decoded once.
        function test_replay_pick_names_once() {
            tc.wizard.replayFilePicked("content://provider/document/100%25%20capture.wav", "100%25 capture.wav");
            compare(item("wizardReplayProgressText").text, "Copying 100%25 capture.wav…");
            tc.wizard.replayFilePicked("content://provider/document/cap%20one.wav", "");
            compare(item("wizardReplayProgressText").text, "Copying cap one.wav…");
        }

        // The copy belongs to the File source: picking another source cancels
        // it, so Continue does not wait on a copy nothing shows.
        function test_leaving_the_file_source_cancels_the_copy() {
            tc.wizard.fileText = "/data/before.wav";
            pickReplay("content://provider/document/cap.wav");
            verify(!item("wizardContinue").inputValid, "Continue waits for the copy");
            tc.wizard.sourceType = "usb";
            compare(testContext.replayCalls().cancels, 1);
            verify(!tc.wizard.replayPending);
            verify(item("wizardContinue").inputValid, "Continue is back for the USB source");
            verify(!testContext.finishReplay({ok: true, path: "/data/replays/late.wav"}), "the copy was answered at its cancel");
            wait(20); // the dropped copy's "cancelled", queued
            compare(tc.wizard.fileText, "/data/before.wav", "the dropped copy's answer is ignored");
            tc.wizard.sourceType = "file";
            verify(!item("wizardReplayProgress").visible);
            compare(tc.wizard.replayNotice, "");
        }

        function test_sized_copy_shows_its_progress_then_the_file() {
            tc.wizard.fileText = "/data/before.wav";
            pickReplay("content://provider/document/cap.wav");
            compare(testContext.replayCalls().reference, "content://provider/document/cap.wav");
            verify(tc.wizard.replayCopying);
            verify(item("wizardReplayProgress").visible, "the copy shows under the row");
            verify(!item("wizardReplayBrowse").enabled, "Browse waits for the copy");
            testContext.replayProgress(1.2e9, 3.4e9);
            compare(item("wizardReplayProgressText").text, "Copying cap.wav…");
            compare(item("wizardReplayProgressAmount").text, "1.2 GB of 3.4 GB");
            verify(item("wizardReplayProgressBar").visible, "a known size has a bar");
            compare(tc.wizard.fileText, "/data/before.wav", "the earlier file stays while the copy runs");
            verify(testContext.finishReplay({ok: true, path: "/data/replays/cap.wav"}));
            compare(tc.wizard.fileText, "/data/replays/cap.wav");
            verify(!item("wizardReplayProgress").visible);
            verify(item("wizardReplayBrowse").enabled);
            compare(tc.wizard.replayNotice, "");
        }

        // A screen reader reads the bar's value after its name: a whole
        // percent, never the byte count, with the amount as its description.
        function test_progress_bar_reads_a_percent() {
            pickReplay("content://provider/document/cap.wav");
            testContext.replayProgress(1.2e9, 3.4e9);
            var bar = item("wizardReplayProgressBar");
            var node = testContext.accessibleNode(bar);
            compare(node.name, "Copy progress");
            compare(node.currentValue, 35, "1.2 GB of 3.4 GB reads as 35 percent");
            compare(String(node.currentValue), "35");
            compare(node.description, item("wizardReplayProgressAmount").text, "the amount describes it");
            compare(bar.minimumValue, 0);
            compare(bar.maximumValue, 100);
            testContext.replayProgress(3.4e9, 3.4e9);
            compare(testContext.accessibleNode(bar).currentValue, 100);
        }

        function test_unsized_copy_shows_bytes_without_a_bar() {
            pickReplay("content://provider/document/cap.wav");
            testContext.replayProgress(1.2e9, -1);
            compare(item("wizardReplayProgressText").text, "Copying cap.wav…");
            compare(item("wizardReplayProgressAmount").text, "1.2 GB copied");
            verify(!item("wizardReplayProgressBar").visible, "no bar without a size to measure against");
        }

        // I4: a failed copy says why, under the row, and the earlier file stays.
        function test_failure_keeps_the_earlier_file_data() {
            return [
                {tag: "read", result: {ok: false, error: "read"}, text: "Could not read cap.wav. Check that it is still available, then try again."},
                {tag: "too large", result: {ok: false, error: "too_large", neededBytes: 3.4e9 + 536870912, freeBytes: 1.2e9, reserveBytes: 536870912}, text: "There is not enough free space to copy cap.wav (3.4\u00a0GB). This device has 1.2\u00a0GB free, and DSD-neo keeps about 0.5\u00a0GB spare."},
                {tag: "no space", result: {ok: false, error: "local", noSpace: true}, text: "Could not save a copy of cap.wav on this device. Free up some storage, then try again."}
            ];
        }
        function test_failure_keeps_the_earlier_file(data) {
            tc.wizard.fileText = "/data/before.wav";
            pickReplay("content://provider/document/cap.wav");
            verify(testContext.finishReplay(data.result));
            compare(tc.wizard.fileText, "/data/before.wav");
            compare(item("wizardReplayNotice").text, data.text);
            verify(item("wizardReplayNotice").visible);
            // The next pick starts clean.
            pickReplay("content://provider/document/cap.wav");
            verify(!item("wizardReplayNotice").visible);
        }

        function test_cancel_keeps_the_earlier_file() {
            tc.wizard.fileText = "/data/before.wav";
            pickReplay("content://provider/document/cap.wav");
            var cancel = item("wizardReplayCancel");
            compare(cancel.text, "Cancel copy");
            cancel.clicked();
            compare(testContext.replayCalls().cancels, 1);
            // The store stops the copy at once and answers on the next turn,
            // whether or not the provider lets go of the copy.
            verify(!item("wizardReplayProgress").visible, "the copy row goes at once");
            tryVerify(function () { return !tc.wizard.replayPending; }, 1000, "the cancel is answered at once");
            verify(!testContext.finishReplay({ok: true, path: "/data/replays/cap.wav"}), "no copy is left to answer");
            compare(tc.wizard.fileText, "/data/before.wav");
            verify(item("wizardReplayBrowse").enabled, "Browse is back");
            verify(!item("wizardReplayNotice").visible, "a cancel the user asked for is not a failure");
            verify(!item("wizardReplayProgress").visible);
        }

        // Continue and Save wait for the copy: saving now would keep the earlier file.
        function test_save_waits_for_the_copy() {
            tc.wizard.fileText = "/data/before.wav";
            tc.wizard.decodeFlag = "-fa";
            tc.wizard.nameText = "Replay system";
            tc.wizard.step = 2;
            var save = item("wizardContinue");
            verify(save.inputValid, "the system can be saved before the pick");
            pickReplay("content://provider/document/cap.wav");
            verify(!save.inputValid, "Save waits for the copy");
            verify(!save.enabled);
            var count = savedSystems.count;
            tc.wizard.commit();
            compare(savedSystems.count, count, "no save while the copy runs");
            verify(testContext.finishReplay({ok: true, path: "/data/replays/cap.wav"}));
            verify(save.inputValid);
            tc.wizard.commit();
            compare(savedSystems.count, count + 1);
            tc.savedRow = savedSystems.count - 1;
            compare(savedSystems.get(tc.savedRow).filePath, "/data/replays/cap.wav");
        }

        function test_closing_cancels_the_copy() {
            tc.wizard.fileText = "/data/before.wav";
            pickReplay("content://provider/document/cap.wav");
            tc.wizard.visible = false;
            compare(testContext.replayCalls().cancels, 1);
            verify(!tc.wizard.replayCopying);
            verify(!testContext.finishReplay({ok: true, path: "/data/replays/late.wav"}), "the copy was answered at its cancel");
            wait(20); // the copy's "cancelled", queued
            compare(tc.wizard.fileText, "/data/before.wav", "a closed wizard takes no late file");
        }

        function test_reopening_cancels_the_copy() {
            pickReplay("content://provider/document/cap.wav");
            tc.wizard.openForAdd(false);
            compare(testContext.replayCalls().cancels, 1);
            verify(!testContext.finishReplay({ok: false, error: "read"}), "the copy was answered at its cancel");
            wait(20); // the copy's "cancelled", queued
            compare(tc.wizard.replayNotice, "", "the earlier opening's answer is not this one's");
            compare(tc.wizard.fileText, "");
        }

        // Gate r1: an Edit draft opened while the startup migration copied
        // its cached replay follows the copy when the migration lands, so a
        // save keeps it; an untouched draft stays untouched, and a draft
        // naming another file keeps it.
        function test_open_draft_follows_the_replay_migration() {
            var cached = "/cache/legacy.wav";
            var copy = "/data/replays/legacy.wav";
            verify(savedSystems.add({name: "Cached replay", sourceType: "file", filePath: cached, decodeFlag: "-fa"}));
            tc.savedRow = savedSystems.count - 1;
            var uid = savedSystems.get(tc.savedRow).uid;
            tc.wizard.openForEdit(tc.savedRow);
            compare(tc.wizard.fileText, cached);
            testContext.migrateReplay(uid, cached, copy);
            compare(tc.wizard.fileText, copy, "the draft names the copy");
            compare(tc.wizard.initialDraft, tc.wizard.draftFingerprint(), "the untouched draft stays untouched");

            tc.wizard.openForEdit(tc.savedRow);
            tc.wizard.nameText = "Renamed";
            testContext.migrateReplay(uid, cached, copy);
            compare(tc.wizard.fileText, copy);
            verify(tc.wizard.initialDraft !== tc.wizard.draftFingerprint(), "a changed draft stays changed");
            tc.wizard.nameText = "Cached replay";
            compare(tc.wizard.initialDraft, tc.wizard.draftFingerprint(), "and reads clean once the change is undone");

            tc.wizard.openForEdit(tc.savedRow);
            tc.wizard.fileText = "/data/other.wav";
            testContext.migrateReplay(uid, cached, copy);
            compare(tc.wizard.fileText, "/data/other.wav", "a draft naming another file keeps it");
        }

        // The Cancel copy button has one label: the cancel answers at once.
        function test_cancel_copy_has_one_label() {
            pickReplay("content://provider/document/cap.wav");
            var cancel = item("wizardReplayCancel");
            compare(cancel.text, "Cancel copy");
            verify(cancel.enabled);
            tc.wizard.cancelReplay();
            tc.wizard.cancelReplay();
            compare(testContext.replayCalls().cancels, 1, "a second tap asks nothing more");
            compare(cancel.text, "Cancel copy");
            verify(!item("wizardReplayProgress").visible);
        }

        // A second pick replaces the first copy; only its own answer counts.
        function test_a_second_pick_supersedes_the_first() {
            pickReplay("content://provider/document/first.wav");
            pickReplay("content://provider/document/second.wav");
            compare(testContext.replayCalls().begins, 2);
            wait(20); // the first copy's "cancelled", queued
            verify(tc.wizard.replayCopying, "the second copy still runs");
            verify(testContext.finishReplay({ok: true, path: "/data/replays/second.wav"}));
            compare(tc.wizard.fileText, "/data/replays/second.wav");
        }
    }

    TestCase {
        id: csv

        name: "WizardCsvImport"
        when: windowShown

        readonly property var wizard: wizardLoader.item
        readonly property var flow: findChild(wizardLoader.item, "wizardCsvImport")

        function init() {
            csv.wizard.visible = true;
            csv.wizard.openForAdd(false);
        }
        function cleanup() {
            reseedOnCommit.enabled = false;
            testContext.holdImports(false);
            csv.wizard.visible = true;
            csv.wizard.openForAdd(false);
            tryCompare(csv.flow, "active", false);
            findChild(csv.wizard, "wizardCsvSheet").visible = false;
            while (importedFiles.count)
                importedFiles.remove(0);
        }
        function channelMap() {
            return testContext.writeFixtureCsv("wizard-map.csv", "channel,frequency\n1,851012500\n");
        }

        // The field is the one picked from, whatever the picker serves when
        // the copy answers: Browse, or the sheet opened for another field.
        function test_a_copy_keeps_the_field_it_was_started_for() {
            testContext.holdImports(true);
            csv.wizard.pickerTarget = "chan";
            verify(csv.flow.begin(channelMap(), "wizard-map.csv", "chan", csv.wizard.csvContext()));
            findChild(csv.wizard, "wizardReplayBrowse").clicked();
            findChild(csv.wizard, "wizardReplayPicker").close();
            csv.wizard.pickerTarget = "group";
            testContext.holdImports(false);
            tryCompare(csv.flow, "active", false);
            var row = importedFiles.rowForPath(csv.wizard.chanCsvPath);
            verify(row >= 0, "the channel map field took the import");
            compare(importedFiles.get(row).type, "chan");
            compare(csv.wizard.groupCsvPath, "");
        }

        // A wizard opened again since the import began ignores its answer, even
        // a successful one.
        function test_a_reopened_wizard_ignores_a_late_answer() {
            csv.wizard.pickerTarget = "chan";
            reseedOnCommit.enabled = true;
            verify(csv.flow.begin(channelMap(), "wizard-map.csv", "chan", csv.wizard.csvContext()));
            tryCompare(csv.flow, "active", false);
            verify(!reseedOnCommit.enabled, "the wizard was opened again before the answer");
            compare(importedFiles.count, 1, "the file is in the library");
            compare(csv.wizard.chanCsvPath, "", "but not in the system opened since");
            compare(csv.wizard.csvNotice, "");
        }

        // Closing the wizard cancels the copy; nothing lands anywhere.
        function test_closing_cancels_a_csv_copy() {
            testContext.holdImports(true);
            csv.wizard.pickerTarget = "chan";
            verify(csv.flow.begin(channelMap(), "wizard-map.csv", "chan", csv.wizard.csvContext()));
            verify(csv.flow.copying);
            csv.wizard.visible = false;
            tryCompare(csv.flow, "active", false);
            compare(importedFiles.count, 0, "the cancelled copy added nothing");
            compare(csv.wizard.chanCsvPath, "");
            compare(csv.wizard.csvNotice, "");
        }

        // Each failure says what went wrong, not one catch-all line.
        function test_failures_say_why_data() {
            return [
                {tag: "read", size: -1, text: "Could not read wizard-gone.csv. Check that it is still available, then try again."},
                {tag: "too large", size: 32 * 1024 * 1024 + 1, text: "wizard-big.csv is too large to import. A CSV file can be up to 32\u00a0MiB."}
            ];
        }
        function test_failures_say_why(data) {
            var path = data.size > 0 ? testContext.writeSizedFixture("wizard-big.csv", data.size) : testContext.storePath("fixtures/wizard-gone.csv");
            verify(path.length > 0);
            csv.wizard.pickerTarget = "group";
            verify(csv.flow.begin(path, path.substring(path.lastIndexOf('/') + 1), "group", csv.wizard.csvContext()));
            tryCompare(csv.flow, "active", false);
            compare(csv.wizard.csvNotice, data.text);
            verify(csv.wizard.csvNoticeIsProblem);
            compare(csv.wizard.groupCsvPath, "");
        }

        // B7: the picker's counts follow a file replaced in place, which keeps
        // the library's size.
        function test_picker_counts_follow_a_replaced_file() {
            var first = testContext.importNow(testContext.writeFixtureCsv("wizard-groups.csv", "id,mode,name\n1001,A,Dispatch\n"), "group");
            verify(first.ok);
            var sheet = findChild(csv.wizard, "wizardCsvSheet");
            csv.wizard.pickerTarget = "group";
            sheet.visible = true;
            compare(sheet.entries.length, 1);
            compare(sheet.entries[0].accepted, 1);
            var spy = Qt.createQmlObject("import QtTest; SignalSpy { signalName: \"importFinished\" }", root);
            spy.target = importedFiles;
            importedFiles.beginImport(testContext.writeFixtureCsv("wizard-groups-2.csv", "id,mode,name\n1001,A,Dispatch\n1002,A,Fireground\n"), "wizard-groups-2.csv", "group", {}, first.path);
            tryCompare(spy, "count", 1);
            verify(spy.signalArguments[0][1].ok, "the replace committed");
            compare(importedFiles.count, 1);
            compare(sheet.entries[0].accepted, 2, "the sheet lists the replaced file's new count");
            spy.destroy();
        }
    }
}
