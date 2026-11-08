// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #708: a call's recordings in its History detail sheet. The decoderHost
// map plays the host (qml_test_context.h): a play opens Preparing, pause and
// resume flip the state, a stop clears the path, and a case publishes the rest
// (Playing, the end of a part, a failure) as a platform host would.
Item {
    width: 420
    height: 900

    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/HistoryDetailSheet.qml"
    }

    TestCase {
        name: "CallRecordingPlayer"
        when: windowShown
        // Issue #782: moving readings name themselves at the screen reader's pace (SpeechPacer); a short period and
        // no step floor keep these checks of what is read quick. The app's pace is restored for the next file in the
        // run.
        property int appSpeechPeriod: 0
        function initTestCase() {
            appSpeechPeriod = Ui.Navigation.speechPeriodMs;
            Ui.Navigation.speechPeriodMs = 50;
            Ui.Navigation.speechStepFloorMs = 1;
        }
        function cleanupTestCase() {
            Ui.Navigation.speechPeriodMs = appSpeechPeriod;
            Ui.Navigation.speechStepFloorMs = 1000;
        }

        readonly property string pathA: "/rec/County (12345678)/A.wav"
        readonly property string pathB: "/rec/County (12345678)/B.wav"
        readonly property string pathC: "/rec/County (12345678)/C.wav"

        function rec(id, path, ms, extra) {
            var map = {id: id, path: path, fileName: path.split("/").pop(), folderUid: "u1", start: 1700000000, durationMs: ms, rate: 8000, bytes: 96000, failed: false, available: true};
            for (var key in (extra || {}))
                map[key] = extra[key];
            return map;
        }
        function sheet() {
            return loader.item;
        }
        function item(name) {
            return findChild(loader.item, name);
        }
        function openWith(list) {
            var uid = callHistory.pushWithRecordings(list);
            sheet().open({uid: uid, name: "Dispatch", tg: 1001, when: 1700000000, systemUid: "test-system"});
            waitForRendering(sheet());
            return uid;
        }
        function lastHostCall() {
            var calls = testContext.hostCalls();
            return calls.length > 0 ? calls[calls.length - 1] : ({});
        }
        function hostCallNames() {
            return testContext.hostCalls().map(function (call) { return call.name; });
        }
        function hostCallsNamed(name) {
            return testContext.hostCalls().filter(function (call) { return call.name === name; });
        }
        function lastStoreCall() {
            var calls = recordings.calls();
            return calls.length > 0 ? calls[calls.length - 1] : ({});
        }
        // Issue #829: the save request Save to file waits on, answered by the host.
        function saveRequest() {
            var request = testContext.lastDocumentRequest();
            compare(request.kind, "save");
            compare(request.mime, "audio/x-wav");
            verify(findChild(item("historyRecording"), "recordingSavePicker").visible, "the picker waits");
            return request.id;
        }
        // Tap Save to file on the shown part; the request it waits on.
        function startSave() {
            tap("recordingSave");
            verify(item("historyRecording").saveSelection > 0);
            return saveRequest();
        }
        function tap(name) {
            var target = item(name);
            verify(target !== null, name);
            verify(target.visible && target.enabled, name + " can be tapped");
            mouseClick(target, target.width / 2, target.height / 2);
        }

        // A status line with nothing to say: its line kept in the layout, empty
        // and not read.
        function silent(status) {
            return status.text === "" && status.Accessible.ignored && status.height > 0;
        }

        function init() {
            verify(sheet() !== null);
            testContext.resetHostPlayback();
            testContext.setHostReading("shareSupported", false);
            recordings.reset();
            callHistory.clearAll();
        }
        function cleanup() {
            findChild(item("historyRecording"), "recordingSavePicker").cancel();
            sheet().visible = false;
            testContext.resetHostPlayback();
            testContext.setHostReading("shareSupported", false);
            recordings.reset();
            callHistory.clearAll();
        }

        function test_no_recording_no_block() {
            sheet().open({name: "Dispatch", tg: 1001});
            verify(!item("historyRecording").visible, "a row without a uid shows no recording");
            sheet().visible = false;
            openWith([]);
            verify(!item("historyRecording").visible, "a call without a recording shows no block");
            verify(item("historyHoldButton").visible, "the rest of the sheet is unchanged");
        }

        function test_play_pause_resume() {
            openWith([rec("t-1", pathA, 12000)]);
            var block = item("historyRecording");
            verify(block.visible);
            compare(item("recordingHeader").text, "Call recording");
            compare(item("recordingTime").text, "0:00 / 0:12");
            verify(silent(item("recordingStatus")), "a recording at rest says nothing more");
            verify(!item("recordingProblem").visible);
            // The bar sits with the time it shows, not apart under the row.
            var time = item("recordingTime").mapToItem(sheet(), 0, 0);
            var bar = item("recordingSeekBar");
            var track = bar.mapToItem(sheet(), 0, bar.height / 2);
            verify(track.y - (time.y + item("recordingTime").height) < 16, "the track is just under the time");
            verify(bar.height >= 48, "and keeps a full touch area");
            verify(item("recordingHeader").mapToItem(sheet(), 0, 0).y < item("historyHoldButton").mapToItem(sheet(), 0, 0).y, "the block sits above Hold");
            compare(item("recordingPlayButton").accessibleName, "Play recording");

            tap("recordingPlayButton");
            compare(lastHostCall().name, "playRecording");
            compare(lastHostCall().path, pathA);
            compare(item("recordingStatus").text, "Opening…");
            verify(item("recordingPlayButton").showPause, "a play on its way shows pause");

            testContext.publishPlayback(pathA, 2, 3000, 12000, "");
            compare(item("recordingTime").text, "0:03 / 0:12");
            verify(silent(item("recordingStatus")));
            compare(item("recordingPlayButton").accessibleName, "Pause recording");

            tap("recordingPlayButton");
            compare(lastHostCall().name, "pausePlayback");
            compare(item("recordingStatus").text, "Paused");
            verify(!item("recordingPlayButton").showPause);
            compare(item("recordingPlayButton").accessibleName, "Resume recording");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "resumePlayback");
        }

        // The player is as tall stopped, opening, playing, paused and stopped
        // again: its status line keeps its place empty, so a sheet centred on
        // its content never moves under the finger at a tap.
        function test_height_steady_across_states() {
            openWith([rec("t-1", pathA, 12000)]);
            var block = item("historyRecording");
            var status = item("recordingStatus");
            waitForRendering(sheet());
            var height = block.height;
            verify(silent(status), "stopped");
            tap("recordingPlayButton");
            compare(status.text, "Opening…");
            verify(!status.Accessible.ignored, "a status is read");
            waitForRendering(sheet());
            compare(block.height, height, "opening");
            testContext.publishPlayback(pathA, 2, 3000, 12000, "");
            verify(silent(status));
            waitForRendering(sheet());
            compare(block.height, height, "playing");
            tap("recordingPlayButton");
            compare(status.text, "Paused");
            waitForRendering(sheet());
            compare(block.height, height, "paused");
            testContext.publishPlayback(pathA, 0, 0, 12000, "");
            verify(silent(status));
            waitForRendering(sheet());
            compare(block.height, height, "stopped again");
        }

        // D10: the host pauses a recording when the app leaves the foreground; the block says so.
        function test_background_pause_reads_paused() {
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 4000, 12000, "");
            testContext.publishPlayback(pathA, 3, 4000, 12000, "");
            compare(item("recordingStatus").text, "Paused");
            compare(item("recordingTime").text, "0:04 / 0:12");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "resumePlayback", "coming back resumes where it paused");
        }

        function test_another_recording_playing_reads_stopped_here() {
            openWith([rec("t-1", pathA, 12000)]);
            testContext.publishPlayback("/rec/other.wav", 2, 5000, 30000, "");
            compare(item("recordingTime").text, "0:00 / 0:12");
            verify(!item("recordingPlayButton").showPause);
            verify(!item("recordingSeekBar").enabled);
            tap("recordingPlayButton");
            compare(lastHostCall().name, "playRecording", "a play replaces the other one");
        }

        function test_seek() {
            openWith([rec("t-1", pathA, 10000)]);
            var bar = item("recordingSeekBar");
            verify(!bar.enabled, "nothing to seek before it plays");
            tap("recordingPlayButton");
            verify(!bar.enabled, "nor while it opens");
            testContext.publishPlayback(pathA, 2, 1000, 10000, "");
            verify(bar.enabled);
            mouseClick(bar, bar.width / 2, bar.height / 2);
            compare(lastHostCall().name, "seekPlayback");
            verify(Math.abs(lastHostCall().ms - 5000) <= 100, "a tap halfway seeks halfway: " + lastHostCall().ms);
            bar.nudge(5000);
            compare(lastHostCall().ms, 10000, "a nudge stops at the end");
            testContext.publishPlayback(pathA, 3, 2000, 10000, "");
            bar.nudge(-5000);
            compare(lastHostCall().ms, 0, "and at the start");
        }

        // Keyboard focus on the bar frames the bar, not the time above it (the touch area overhangs the time).
        function test_seek_focus_frame_clears_the_time() {
            openWith([rec("t-1", pathA, 10000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 10000, "");
            var bar = item("recordingSeekBar");
            bar.Window.window.requestActivate();
            tryVerify(function () { return bar.Window.window.active; });
            bar.forceActiveFocus();
            tryVerify(function () { return bar.activeFocus; });
            var frame = item("recordingSeekFocus");
            verify(frame.visible);
            var time = item("recordingTime");
            var timeBottom = time.mapToItem(sheet(), 0, time.height).y;
            var frameTop = frame.mapToItem(sheet(), 0, 0).y;
            verify(frameTop >= timeBottom, "frame top " + frameTop + " under the time's bottom " + timeBottom);
            verify(frame.width >= bar.width, "and spans the bar");
        }

        function test_cant_play_data() {
            return [
                {tag: "unavailable", error: "unavailable", text: "Can't play right now. Another app has the audio, such as a phone call."},
                {tag: "failed", error: "failed", text: "Can't play this recording. The file may be damaged."},
                {tag: "unsupported", error: "unsupported", text: "Recordings can't be played on this device."}
            ];
        }
        function test_cant_play(data) {
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 4, 0, 0, data.error);
            verify(item("historyRecording").visible);
            compare(item("recordingProblem").text, data.text, "a sentence, under the player");
            verify(silent(item("recordingStatus")), "nothing crammed beside the time");
            compare(item("recordingProblem").width, item("historyRecording").width);
            tap("recordingPlayButton");
            compare(lastHostCall().name, "playRecording", "a play that failed can be tried again");
        }

        function test_missing_recording_leaves() {
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 4, 0, 0, "missing");
            verify(!item("historyRecording").visible, "a recording found gone leaves the sheet");
            sheet().visible = false;
            testContext.resetHostPlayback();

            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPart1");
            testContext.publishPlayback(pathB, 4, 0, 0, "missing");
            verify(item("historyRecording").visible, "the other part stays");
            compare(item("recordingHeader").text, "Call recording");
            compare(item("recordingPart0"), null, "one part left, no part list");
        }

        // A recording found missing because its storage went: once the store
        // lists it as away it shows again, and plays when the storage is back.
        function test_storage_returns_after_missing() {
            var uid = openWith([rec("t-1", pathA, 12000)]);
            testContext.setHostReading("shareSupported", true);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 4, 0, 0, "missing");
            verify(!item("historyRecording").visible, "found gone, it leaves at once");
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000)]));
            verify(!item("historyRecording").visible, "the row read again, unchanged, keeps it out");
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000, {available: false})]));
            verify(item("historyRecording").visible, "listed as away, it shows again");
            compare(item("recordingProblem").text, "The storage this recording is on isn't available right now.");
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000, {available: true})]));
            verify(item("historyRecording").visible, "the storage back, the recording is usable again");
            verify(!item("recordingProblem").visible, "the failure from before is not read as a damaged file");
            verify(item("recordingShare").enabled && item("recordingSave").enabled && item("recordingDelete").enabled);
            compare(item("recordingPlayButton").accessibleName, "Play recording");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "playRecording");
            compare(lastHostCall().path, pathA);
            testContext.publishPlayback(pathA, 4, 0, 0, "missing");
            verify(!item("historyRecording").visible, "found gone again, it leaves again");
        }

        function test_storage_away() {
            openWith([rec("t-1", pathA, 12000, {available: false})]);
            testContext.setHostReading("shareSupported", true);
            compare(item("recordingProblem").text, "The storage this recording is on isn't available right now.");
            verify(!item("recordingPlayButton").enabled);
            verify(!item("recordingShare").enabled && !item("recordingSave").enabled && !item("recordingDelete").enabled);
        }

        function test_cut_short() {
            openWith([rec("t-1", pathA, 12000, {failed: true})]);
            compare(item("recordingProblem").text, "Saving stopped early, so the end may be missing.");
        }

        function test_parts_and_play_all() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000), rec("t-3", pathC, 4000)]);
            compare(item("recordingHeader").text, "Call recording · 3 parts");
            compare(item("recordingPart0").text, "Part 1 · 0:12");
            compare(item("recordingPart2").text, "Part 3 · 0:04");
            verify(item("recordingPart0").checked);
            compare(item("recordingDelete").text, "Delete part 1");

            var player = item("historyRecording");
            verify(!item("recordingPlayAll").checkable, "Play all is an action, not a fourth part");
            compare(item("recordingPlayAll").text, "Play all parts");
            verify(item("recordingPlayAll").mapToItem(sheet(), 0, 0).y > item("recordingPart2").mapToItem(sheet(), 0, 0).y, "under the parts");
            tap("recordingPlayAll");
            compare(lastHostCall().path, pathA);
            verify(player.playAll);
            testContext.publishPlayback(pathA, 2, 6000, 12000, "");
            testContext.publishPlayback(pathA, 0, 12000, 12000, "");
            compare(lastHostCall().name, "playRecording", "the end of a part plays the next one");
            compare(lastHostCall().path, pathB);
            verify(item("recordingPart1").checked);
            compare(item("recordingDelete").text, "Delete part 2");

            testContext.publishPlayback(pathB, 2, 8000, 8000, "");
            testContext.publishPlayback(pathB, 0, 8000, 8000, "");
            compare(lastHostCall().path, pathC);
            testContext.publishPlayback(pathC, 0, 4000, 4000, "");
            var played = hostCallNames().filter(function (name) { return name === "playRecording"; }).length;
            compare(played, 3, "the last part ends Play all");
            verify(!player.playAll);
            var checked = [0, 1, 2].filter(function (i) { return item("recordingPart" + i).checked; });
            compare(checked, [2], "one chip shows the part");
        }

        // A length rounds, and never reads 0:00 for a real recording; the
        // position floors, and reads the length once it reaches the end.
        function test_sub_second_and_rounded_lengths() {
            openWith([rec("t-1", pathA, 240), rec("t-2", pathB, 2600)]);
            compare(item("recordingPart0").text, "Part 1 · 0:01");
            compare(item("recordingPart1").text, "Part 2 · 0:03");
            compare(item("recordingTime").text, "0:00 / 0:01");
            testContext.publishPlayback(pathA, 0, 240, 240, "");
            compare(item("recordingTime").text, "0:01 / 0:01", "a finished part reads its length");
            tryCompare(item("recordingTime").Accessible, "name", "0:01 of 0:01");
            tap("recordingPart1");
            testContext.publishPlayback(pathB, 2, 1500, 2600, "");
            compare(item("recordingTime").text, "0:01 / 0:03");
            testContext.publishPlayback(pathB, 0, 2600, 2600, "");
            compare(item("recordingTime").text, "0:03 / 0:03");
            tryCompare(item("recordingSeekBar").Accessible, "description", "0:03 of 0:03");
        }

        function test_duration_line_counts_seconds() {
            sheet().open({name: "Dispatch", tg: 1001, durationSecs: 1});
            verify(sheet().detailLines.indexOf("Duration: 1 second") >= 0, "one second is singular");
            sheet().visible = false;
            sheet().open({name: "Dispatch", tg: 1001, durationSecs: 3});
            verify(sheet().detailLines.indexOf("Duration: 3 seconds") >= 0);
        }

        // Play all starts from the first part, whichever part was shown last.
        function test_play_all_after_a_later_part() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPart1");
            compare(lastHostCall().path, pathB);
            testContext.publishPlayback(pathB, 0, 8000, 8000, "");
            verify(item("recordingPart1").checked);
            tap("recordingPlayAll");
            compare(lastHostCall().name, "playRecording");
            compare(lastHostCall().path, pathA, "Play all plays part 1 first");
            verify(item("recordingPart0").checked);
            testContext.publishPlayback(pathA, 2, 6000, 12000, "");
            testContext.publishPlayback(pathA, 0, 12000, 12000, "");
            compare(lastHostCall().path, pathB, "then part 2");
        }

        function test_stop_ends_play_all() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPlayAll");
            var player = item("historyRecording");
            testContext.publishPlayback(pathA, 2, 2000, 12000, "");
            decoderHost.stopPlayback();
            verify(!player.playAll, "a stop ends Play all");
            var played = hostCallNames().filter(function (name) { return name === "playRecording"; }).length;
            compare(played, 1, "nothing plays after it");

            // A part chosen by hand plays alone.
            tap("recordingPart1");
            compare(lastHostCall().path, pathB);
            verify(!player.playAll);
            testContext.publishPlayback(pathB, 0, 8000, 8000, "");
            compare(lastHostCall().name, "playRecording");
            compare(lastHostCall().path, pathB, "and stops at its end");
        }

        // The part shown stays on its recording when another one leaves, and every label names it by where it is now.
        function test_part_selection_follows_its_recording() {
            var uid = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000), rec("t-3", pathC, 4000)]);
            tap("recordingPart1");
            compare(lastHostCall().path, pathB);
            // The first part leaves (deleted elsewhere): the one shown is still B, part 1 now.
            verify(callHistory.setRowRecordings(uid, [rec("t-2", pathB, 8000), rec("t-3", pathC, 4000)]));
            verify(item("recordingPart0").checked && !item("recordingPart1").checked, "the selection stays on its recording");
            compare(item("recordingPart0").text, "Part 1 · 0:08");
            compare(item("recordingDelete").text, "Delete part 1");
            verify(item("historyRecording").loaded, "and it is still the one open");
            tap("recordingDelete");
            var confirm = findChild(sheet(), "recordingDeleteConfirm");
            compare(findChild(confirm, "confirmMessage").text, "Part 1 is deleted from this phone. The other parts and the call stay in History.");
            findChild(confirm, "confirmActionButton").activate();
            compare(recordings.calls()[recordings.calls().length - 1].id, "t-2", "and it is the one deleted");
        }
        function test_part_selection_when_its_recording_goes_missing() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000), rec("t-3", pathC, 4000)]);
            tap("recordingPart2");
            testContext.publishPlayback(pathC, 4, 0, 0, "missing");
            compare(item("recordingHeader").text, "Call recording · 2 parts");
            var checked = [0, 1].filter(function (i) { return item("recordingPart" + i).checked; });
            compare(checked, [1], "the last part left is shown");
            compare(item("recordingDelete").text, "Delete part 2", "no label names a part that is gone");
            compare(item("historyRecording").part.path, pathB);
        }

        // A recording journaled, or a fragment folded into the call, shows while the sheet is open.
        function test_live_update_through_a_fold() {
            var uid = openWith([rec("t-1", pathA, 12000)]);
            callHistory.pushWithRecordings([]);
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]));
            compare(item("recordingHeader").text, "Call recording · 2 parts");
            verify(item("recordingPart1").visible);
            verify(callHistory.setRowRecordings(uid, []));
            verify(!item("historyRecording").visible, "a recording deleted elsewhere leaves");
        }

        function test_share() {
            testContext.setHostReading("shareSupported", true);
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingShare");
            compare(lastHostCall().name, "shareRecording");
            compare(lastHostCall().path, pathA);
            compare(lastHostCall().title, "Dispatch · " + Qt.formatDateTime(new Date(1700000000 * 1000), "yyyy-MM-dd hh:mm"));
            verify(!item("recordingNotice").visible);

            testContext.setShareAccepted(false);
            tap("recordingShare");
            compare(item("recordingNotice").text, "Couldn't share this recording. Try again, or use Save to file.");

            testContext.setShareAccepted(true);
            tap("recordingShare");
            verify(!item("recordingNotice").visible);
            testContext.failRecordingShare("/rec/someone-else.wav");
            verify(!item("recordingNotice").visible, "another recording's failure is not this one's");
            testContext.failRecordingShare(pathA);
            compare(item("recordingNotice").text, "Couldn't share this recording. Try again, or use Save to file.");
        }

        function test_share_hidden_without_support() {
            openWith([rec("t-1", pathA, 12000)]);
            verify(!item("recordingShare").visible);
            compare(item("recordingSave").width, item("historyRecording").width);
        }

        function test_save_data() {
            return [
                {tag: "done", result: 0, text: "Saved."},
                {tag: "failed", result: 1, text: "Couldn't save the recording. Try another location."},
                {tag: "missing", result: 3, text: "This recording is gone."},
                {tag: "busy", result: 4, text: "An export is running. Save again when it finishes."},
                {tag: "cancelled", result: 2, text: ""}
            ];
        }
        function test_save(data) {
            openWith([rec("t-1", pathA, 12000)]);
            recordings.setSelection(1, 96000, [pathA]);
            var player = item("historyRecording");
            tap("recordingSave");
            var calls = recordings.calls();
            compare(calls[calls.length - 1].name, "selectRecordings");
            compare(calls[calls.length - 1].ids, ["t-1"]);
            verify(player.saveSelection > 0);
            verify(!item("recordingSave").enabled, "one save at a time");
            var selection = player.saveSelection;
            var id = saveRequest();
            compare(testContext.lastDocumentRequest().name, "A.wav", "offered under the part's own name");
            // B1: the reference reaches the save as the host sent it.
            var reference = "content://com.Example.Provider/document/primary%3ADownload%2Fmy%20call.wav";
            testContext.answerDocumentPicked(id, reference, "my call.wav", false);
            compare(lastHostCall().name, "saveRecording");
            compare(lastHostCall().reference, reference);
            compare(lastHostCall().selection.selection, selection);

            testContext.finishRecordingTransfer(selection + 100, 1, 0);
            compare(player.saveSelection, selection, "another transfer's end is not this save's");
            testContext.finishRecordingTransfer(selection, data.result, 0);
            compare(player.saveSelection, 0);
            compare(item("recordingNotice").text, data.text);
            verify(item("recordingSave").enabled);
        }

        function test_save_picker_dismissed_or_nothing_to_save() {
            openWith([rec("t-1", pathA, 12000)]);
            var player = item("historyRecording");
            tap("recordingSave");
            compare(item("recordingNotice").text, "This recording is gone.", "a selection that matched nothing says so");
            verify(player.saveSelection === 0);

            recordings.setSelection(1, 96000, [pathA]);
            tap("recordingSave");
            var selection = player.saveSelection;
            testContext.answerDocumentCancelled(saveRequest());
            var calls = recordings.calls();
            compare(calls[calls.length - 1].name, "releaseSelection");
            compare(calls[calls.length - 1].selection, selection, "a dismissed picker lets the file go");
            compare(player.saveSelection, 0);
            compare(hostCallsNamed("saveRecording").length, 0);
            verify(!item("recordingNotice").visible, "backing out says nothing");
        }

        // Issue #829: no picker could ask. The recording goes again at once,
        // the reason shows as a problem and is read out, and Save works again.
        function test_save_picker_unavailable() {
            openWith([rec("t-1", pathA, 12000)]);
            recordings.setSelection(1, 96000, [pathA]);
            var player = item("historyRecording");
            var said = player.announcements;
            var id = startSave();
            var selection = player.saveSelection;
            testContext.answerDocumentUnavailable(id, "noSavePicker");
            compare(lastStoreCall().name, "releaseSelection");
            compare(lastStoreCall().selection, selection, "the hold is let go");
            compare(player.saveSelection, 0);
            var message = "This device has no save picker, and DSD-neo could not save to the Download folder.";
            compare(item("recordingNotice").text, message);
            compare(item("recordingNotice").Accessible.role, Accessible.AlertMessage);
            compare(player.announcements, said + 1, "read out once");
            compare(player.announced, message);
            testContext.answerDocumentPicked(id, "content://late/A.wav", "A.wav", false);
            compare(hostCallsNamed("saveRecording").length, 0, "a late answer writes nothing");
            verify(item("recordingSave").enabled, "Save works again");
            testContext.answerDocumentUnavailable(startSave(), "noSavePicker");
            compare(player.announcements, said + 2, "a repeat is read out again");
        }

        // Issue #829: with no save picker the recording went to
        // Download/DSD-neo; the result names the file there.
        function test_save_to_downloads_data() {
            return [
                {tag: "named", name: "A (1).wav", text: "Saved to Download/DSD-neo as A (1).wav."},
                // A name holding markup is shown as saved, not drawn as markup.
                {tag: "named-with-markup", name: "Fire &lt;East&gt; <b>A</b>.wav", text: "Saved to Download/DSD-neo as Fire &lt;East&gt; <b>A</b>.wav."},
                {tag: "no-name", name: "", text: "Saved."}
            ];
        }
        function test_save_to_downloads(data) {
            openWith([rec("t-1", pathA, 12000)]);
            recordings.setSelection(1, 96000, [pathA]);
            var player = item("historyRecording");
            var id = startSave();
            var selection = player.saveSelection;
            var reference = "content://media/external/downloads/" + data.tag;
            if (data.name.length > 0)
                testContext.setSavedDocumentName(reference, data.name);
            testContext.answerDocumentPicked(id, reference, "A.wav", true);
            var said = player.announcements;
            testContext.finishRecordingTransfer(selection, 0, 0);
            compare(item("recordingNotice").text, data.text);
            compare(item("recordingNotice").textFormat, Text.PlainText);
            verify(!player.noticeProblem);
            // Where it went is said only here, so a named save is read out, once.
            compare(player.announcements, said + (data.name.length > 0 ? 1 : 0));
            if (data.name.length > 0)
                compare(player.announced, data.text);

            // The next save through a picker reads as before, unannounced.
            id = startSave();
            selection = player.saveSelection;
            testContext.answerDocumentPicked(id, "content://picked/A.wav", "A.wav", false);
            said = player.announcements;
            testContext.finishRecordingTransfer(selection, 0, 0);
            compare(item("recordingNotice").text, "Saved.");
            compare(player.announcements, said);
        }

        // r1 #5: a save whose sheet moved to another row ends there, success or
        // failure: the hold is gone, Save works on the new row, and nothing is
        // said about another call's file, here or back on its own row.
        function test_save_ends_after_switching_rows_data() {
            return [{tag: "downloads-done", result: 0}, {tag: "failed", result: 1}];
        }
        function test_save_ends_after_switching_rows(data) {
            openWith([rec("t-1", pathA, 12000)]);
            recordings.setSelection(1, 96000, [pathA]);
            var player = item("historyRecording");
            var id = startSave();
            var selection = player.saveSelection;
            var reference = "content://media/external/downloads/switch";
            testContext.setSavedDocumentName(reference, "A (2).wav");
            testContext.answerDocumentPicked(id, reference, "A.wav", true);
            compare(hostCallsNamed("cancelDocumentRequest").length, 0);
            openWith([rec("t-2", pathB, 12000)]);
            compare(hostCallsNamed("cancelDocumentRequest").length, 0, "an accepted save is not a pick to end");
            testContext.finishRecordingTransfer(selection, data.result, 0);
            compare(player.saveSelection, 0, "the save is over");
            compare(player.saveReference, "", "and forgotten");
            compare(player.savePath, "");
            verify(!item("recordingNotice").visible, "nothing about another call's file");
            verify(item("recordingSave").enabled, "Save works on this row");

            // The new row's own save names its own file.
            recordings.setSelection(1, 96000, [pathB]);
            id = startSave();
            selection = player.saveSelection;
            testContext.answerDocumentPicked(id, "content://picked/B.wav", "B.wav", false);
            testContext.finishRecordingTransfer(selection, 0, 0);
            compare(item("recordingNotice").text, "Saved.");
        }

        // Issue #829: a picker still out ends with what it was asked for. The
        // sheet closing (Back), another row, the row losing its recording, or
        // another part shown cancels it and lets the recording go; its late
        // answer writes nothing.
        function test_save_picker_ends_with_its_recording_data() {
            return [{tag: "sheet-closed"}, {tag: "other-row"}, {tag: "row-trimmed"}, {tag: "other-part"}];
        }
        function test_save_picker_ends_with_its_recording(data) {
            var uid = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            recordings.setSelection(1, 96000, [pathA]);
            var player = item("historyRecording");
            var id = startSave();
            var selection = player.saveSelection;
            if (data.tag === "sheet-closed")
                sheet().visible = false;
            else if (data.tag === "other-row")
                openWith([rec("t-3", pathC, 12000)]);
            else if (data.tag === "row-trimmed")
                verify(callHistory.setRowRecordings(uid, []));
            else
                player.select(1);
            verify(!findChild(player, "recordingSavePicker").visible, "the pick ended");
            verify(testContext.documentCalls().some(function (call) {
                return call.name === "cancelDocumentRequest" && call.id === id;
            }), "the host request was cancelled");
            compare(lastStoreCall().name, "releaseSelection");
            compare(lastStoreCall().selection, selection, "the recording is let go");
            compare(player.saveSelection, 0);
            testContext.answerDocumentPicked(id, "content://late/A.wav", "A.wav", false);
            compare(hostCallsNamed("saveRecording").length, 0, "the late pick writes nothing");
        }

        // The recording's list read again (a fold, another part added) keeps
        // a pick for the part still shown.
        function test_save_picker_survives_a_reread() {
            var uid = openWith([rec("t-1", pathA, 12000)]);
            recordings.setSelection(1, 96000, [pathA]);
            var player = item("historyRecording");
            var id = startSave();
            var selection = player.saveSelection;
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]));
            verify(findChild(player, "recordingSavePicker").visible, "the pick is still out");
            compare(hostCallsNamed("cancelDocumentRequest").length, 0);
            testContext.answerDocumentPicked(id, "content://picked/A.wav", "A.wav", false);
            compare(hostCallsNamed("saveRecording").length, 1);
            testContext.finishRecordingTransfer(selection, 0, 0);
            compare(item("recordingNotice").text, "Saved.");
        }

        function test_delete() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPart1");
            testContext.publishPlayback(pathB, 2, 1000, 8000, "");
            tap("recordingDelete");
            var confirm = findChild(sheet(), "recordingDeleteConfirm");
            verify(confirm.visible);
            compare(confirm.title, "Delete part 2?");
            compare(findChild(confirm, "confirmMessage").text, "Part 2 is deleted from this phone. The other parts and the call stay in History.");
            compare(findChild(confirm, "confirmActionButton").text, "Delete part 2", "the button names what it deletes");
            findChild(confirm, "confirmActionButton").activate();
            verify(!confirm.visible);
            var names = hostCallNames();
            compare(names[names.length - 1], "stopPlayback", "the part playing stops first");
            var calls = recordings.calls();
            compare(calls[calls.length - 1].name, "deleteRecording");
            compare(calls[calls.length - 1].id, "t-2");
        }

        function test_delete_cancelled() {
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingDelete");
            var confirm = findChild(sheet(), "recordingDeleteConfirm");
            compare(confirm.title, "Delete this recording?");
            compare(findChild(confirm, "confirmMessage").text, "Only the audio is deleted. History and detections are unchanged.");
            compare(findChild(confirm, "confirmActionButton").text, "Delete recording");
            findChild(confirm, "confirmCancelButton").activate();
            compare(recordings.calls().length, 0, "nothing is deleted");
        }

        // Issue #809 B3: a single recording's delete takes only its file, whether
        // or not History has a row for it (here none: never linked), and no
        // confirmation promises the call stays in History. A host can say it its
        // own way. A Signal search detection's sheet lists recordings with no row,
        // and ones whose row History trimmed (tst_signal_search.qml).
        function test_delete_wording_without_a_row() {
            var component = Qt.createComponent(uiDir + "/CallRecordingPlayer.qml");
            compare(component.status, Component.Ready, component.errorString());
            var host = Qt.createQmlObject("import QtQuick; Item { anchors.fill: parent }", loader);
            var player = component.createObject(host, {width: 400, dialogParent: host, recordingList: [rec("t-9", pathC, 4000)]});
            verify(player !== null);
            tryVerify(function () { return player.visible; });
            player.askDelete();
            var confirm = findChild(player, "recordingDeleteConfirm");
            verify(confirm.visible);
            compare(findChild(confirm, "confirmMessage").text, "Only the audio is deleted. History and detections are unchanged.");
            findChild(confirm, "confirmCancelButton").activate();
            player.deleteMessage = "Only this recording's audio is deleted.";
            player.askDelete();
            compare(findChild(confirm, "confirmMessage").text, "Only this recording's audio is deleted.");
            findChild(confirm, "confirmCancelButton").activate();
            player.destroy();
            host.destroy();
            wait(0);
        }
        function test_closing_stops_its_own_playback() {
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            sheet().visible = false;
            compare(lastHostCall().name, "stopPlayback");

            testContext.resetHostPlayback();
            openWith([rec("t-2", pathB, 12000)]);
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            sheet().visible = false;
            compare(testContext.hostCalls().length, 0, "another recording plays on");
        }

        // What this block started stops with it even once the row no longer
        // names that recording (History trimmed or changed the row).
        function test_closing_stops_what_it_started_after_the_row_changed() {
            var uid = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            verify(callHistory.setRowRecordings(uid, [rec("t-2", pathB, 8000)]));
            sheet().visible = false;
            compare(lastHostCall().name, "stopPlayback");
        }
        function test_row_trimmed_away_stops_its_playback() {
            var uid = openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            verify(callHistory.setRowRecordings(uid, []));
            verify(!item("historyRecording").visible);
            compare(lastHostCall().name, "stopPlayback", "no control is left for it, so it stops");
        }

        // The playing part of a call of several gone from the list (its file
        // deleted from a computer): the play stops, as no control is left for it.
        function test_playing_part_gone_stops_its_playback() {
            var uid = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            verify(callHistory.setRowRecordings(uid, [rec("t-2", pathB, 12000)]));
            verify(item("historyRecording").visible, "the other part is still shown");
            compare(lastHostCall().name, "stopPlayback", "the part that played is gone, so its play stops");
        }

        // A save still running when its sheet closes ends quietly on another row's sheet.
        function test_save_ends_quietly_on_another_rows_sheet() {
            openWith([rec("t-1", pathA, 12000)]);
            recordings.setSelection(1, 96000, [pathA]);
            tap("recordingSave");
            var player = item("historyRecording");
            var selection = player.saveSelection;
            verify(selection > 0);
            testContext.answerDocumentPicked(saveRequest(), "file:///tmp/A.wav", "A.wav", false);
            sheet().visible = false;
            openWith([rec("t-2", pathB, 12000)]);
            testContext.finishRecordingTransfer(selection, 0, 0);
            compare(player.saveSelection, 0, "the save is over");
            verify(!item("recordingNotice").visible, "and says nothing about another call's file");
            verify(item("recordingSave").enabled);
        }
    }
}
