// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

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
        function tap(name) {
            var target = item(name);
            verify(target !== null, name);
            verify(target.visible && target.enabled, name + " can be tapped");
            mouseClick(target, target.width / 2, target.height / 2);
        }

        function init() {
            verify(sheet() !== null);
            testContext.resetHostPlayback();
            testContext.setHostReading("shareSupported", false);
            recordings.reset();
            callHistory.clearAll();
        }
        function cleanup() {
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
            verify(!item("recordingStatus").visible, "a recording at rest says nothing more");
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
            verify(!item("recordingStatus").visible);
            compare(item("recordingPlayButton").accessibleName, "Pause recording");

            tap("recordingPlayButton");
            compare(lastHostCall().name, "pausePlayback");
            compare(item("recordingStatus").text, "Paused");
            verify(!item("recordingPlayButton").showPause);
            compare(item("recordingPlayButton").accessibleName, "Resume recording");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "resumePlayback");
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
            verify(!item("recordingStatus").visible, "nothing crammed beside the time");
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
            player.savePicked("file:///tmp/A.wav");
            compare(lastHostCall().name, "saveRecording");
            compare(lastHostCall().reference, "file:///tmp/A.wav");
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
            player.saveCancelled();
            var calls = recordings.calls();
            compare(calls[calls.length - 1].name, "releaseSelection");
            compare(calls[calls.length - 1].selection, selection, "a dismissed picker lets the file go");
            compare(player.saveSelection, 0);
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
            compare(findChild(confirm, "confirmMessage").text, "The recording is deleted from this phone. The call stays in History.");
            compare(findChild(confirm, "confirmActionButton").text, "Delete recording");
            findChild(confirm, "confirmCancelButton").activate();
            compare(recordings.calls().length, 0, "nothing is deleted");
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
    }
}
