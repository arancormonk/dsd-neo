// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #708: a call's recordings in its History detail sheet. The decoderHost
// map plays the host (qml_test_context.h): a play opens Preparing under a new
// generation, pause and resume flip the state, a stop clears the path, and a
// case publishes the rest (Playing, the next item of a queue, a failure) as a
// platform host would. Issue #909: the host has a player here
// (setPlaybackSupported), and a case that needs none says so.
Item {
    width: 420
    height: 900

    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/HistoryDetailSheet.qml"
    }
    // R69: History's now-playing bar and the playlist's status, made for the case that checks the bar stays silent.
    Component {
        id: playlistStatusComponent
        Ui.PlaylistStatus {}
    }
    Component {
        id: nowPlayingBarComponent
        Ui.NowPlayingBar {}
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

        // Issue #909: each with the identity the store indexed (its size here
        // the length in bytes at 8 kHz, so each differs).
        function rec(id, path, ms, extra) {
            var map = {id: id, path: path, fileName: path.split("/").pop(), folderUid: "u1", start: 1700000000, durationMs: ms, rate: 8000, bytes: 96000, failed: false, available: true,
                identitySize: 44 + ms * 16, mtimeSec: 1700000100, mtimeNanos: 250};
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
            testContext.setPlaybackSupported(true);
            testContext.resetHostPlayback();
            testContext.setHostReading("shareSupported", false);
            recordings.reset();
            callHistory.clearAll();
        }
        function cleanup() {
            item("recordingDeleteConfirm").visible = false;
            findChild(item("historyRecording"), "recordingSavePicker").cancel();
            sheet().visible = false;
            sheet().showPlayFromHere = false;
            recordings.setDeleteIdentityRefusal("");
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
            compare(lastHostCall().name, "playRecordingOwned");
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
            compare(lastHostCall().name, "playRecordingOwned", "a play replaces the other one");
        }

        function test_seek() {
            openWith([rec("t-1", pathA, 10000)]);
            var bar = item("recordingSeekBar");
            verify(!bar.enabled, "nothing to seek before it plays");
            tap("recordingPlayButton");
            verify(!bar.enabled, "nor while it opens");
            testContext.publishPlayback(pathA, 2, 1000, 10000, "");
            testContext.publishPlaybackReading({playbackToken: 11});
            verify(bar.enabled);
            mouseClick(bar, bar.width / 2, bar.height / 2);
            compare(lastHostCall().name, "seekPlaybackFor");
            compare(lastHostCall().token, 11, "for the recording open");
            verify(Math.abs(lastHostCall().ms - 5000) <= 100, "a tap halfway seeks halfway: " + lastHostCall().ms);
            bar.nudge(5000);
            compare(lastHostCall().ms, 10000, "a nudge stops at the end");
            testContext.publishPlayback(pathA, 3, 2000, 10000, "");
            bar.nudge(-5000);
            compare(lastHostCall().ms, 0, "and at the start");
            // Issue #909 (B7): the host's player says when a seek would move it; a recording it reads no length for
            // takes none, whatever the journal's length says.
            testContext.publishPlaybackReading({playbackCanSeek: false});
            verify(!bar.enabled, "the host says it cannot seek");
            var seeks = hostCallsNamed("seekPlaybackFor").length;
            bar.nudge(5000);
            compare(hostCallsNamed("seekPlaybackFor").length, seeks, "and asks none");
        }

        // Issue #831: a drag that begins on the delete confirmation takes the
        // grab for the bar's DragHandler beneath it, so the handler stands
        // down under a sheet (its enabled reads Navigation.allows()). Without
        // that the bar drags under the dialog: the thumb, the time and the
        // bar's accessible value follow the finger, though seekTo()'s own
        // gate still refuses the seek on release.
        // Issue #902 B9: under the delete confirmation the seek bar is nameless,
        // describes no position (the paced reading is empty, which would read
        // as 0:00), and a step a screen reader sends the node it holds seeks
        // nothing.
        function test_seek_bar_under_a_sheet_offers_nothing() {
            openWith([rec("t-1", pathA, 60000)]);
            var bar = item("recordingSeekBar");
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 31000, 60000, "");
            verify(bar.Accessible.description.length > 0, "shown, it says where it is");
            var cover = item("recordingDeleteConfirm");
            cover.visible = true;
            try {
                compare(bar.Accessible.name, "", "covered: nameless");
                compare(bar.Accessible.description, "", "covered: no position");
                var seeks = hostCallsNamed("seekPlaybackFor").length;
                testContext.accessibleDoAction(bar, "Increase");
                testContext.accessibleDoAction(bar, "Decrease");
                compare(hostCallsNamed("seekPlaybackFor").length, seeks, "covered: a step seeks nothing");
            } finally {
                cover.visible = false;
            }
            compare(bar.Accessible.name, "Position");
            // Hidden but not covered (nothing else refuses the seek then): a step seeks nothing; shown, the same
            // action seeks.
            bar.visible = false;
            try {
                verify(Ui.Navigation.allows(bar), "nothing covers it");
                var hiddenSeeks = hostCallsNamed("seekPlaybackFor").length;
                testContext.accessibleDoAction(bar, "Increase");
                compare(hostCallsNamed("seekPlaybackFor").length, hiddenSeeks, "hidden: a step seeks nothing");
            } finally {
                bar.visible = true;
            }
            var shownSeeks = hostCallsNamed("seekPlaybackFor").length;
            verify(testContext.accessibleDoAction(bar, "Increase"), "shown, it offers the step");
            compare(hostCallsNamed("seekPlaybackFor").length, shownSeeks + 1, "shown: the same step seeks");
        }

        function test_seek_drag_under_a_sheet_data() {
            return [{tag: "mouse", touch: false}, {tag: "touch", touch: true}];
        }
        function test_seek_drag_under_a_sheet(data) {
            openWith([rec("t-1", pathA, 10000)]);
            var bar = item("recordingSeekBar");
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 10000, "");
            verify(bar.enabled);
            var cover = item("recordingDeleteConfirm");
            cover.visible = true;
            waitForRendering(cover);
            var start = bar.mapToItem(cover, bar.width * 0.2, bar.height / 2);
            var seeks = hostCallsNamed("seekPlaybackFor").length;
            var dragged = false;
            var noteDrag = function () {
                if (bar.dragging)
                    dragged = true;
            };
            bar.draggingChanged.connect(noteDrag);
            if (data.touch) {
                var touch = touchEvent(cover);
                touch.press(0, cover, start.x, start.y).commit();
                for (var i = 1; i <= 8; ++i)
                    touch.move(0, cover, start.x + i * bar.width * 0.075, start.y).commit();
                touch.release(0, cover, start.x + bar.width * 0.6, start.y).commit();
            } else {
                mousePress(cover, start.x, start.y);
                for (var j = 1; j <= 8; ++j)
                    mouseMove(cover, start.x + j * bar.width * 0.075, start.y);
                mouseRelease(cover, start.x + bar.width * 0.6, start.y);
            }
            wait(30);
            bar.draggingChanged.disconnect(noteDrag);
            verify(!dragged, "the bar did not drag under the sheet");
            compare(hostCallsNamed("seekPlaybackFor").length, seeks, "a drag under the sheet did not seek");
            verify(cover.visible, "a drag does not dismiss the confirmation");
            cover.visible = false;
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
        // Issue #909 (B3): a failure is said on the sheet's notice line and read out, once; a play that fails again
        // is read out again.
        function test_cant_play(data) {
            openWith([rec("t-1", pathA, 12000)]);
            var player = item("historyRecording");
            var said = player.announcements;
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 4, 0, 0, data.error);
            verify(item("historyRecording").visible);
            compare(item("recordingNotice").text, data.text, "a sentence, on the sheet");
            compare(item("recordingNotice").Accessible.role, Accessible.AlertMessage);
            compare(player.announcements, said + 1, "read out");
            compare(player.announced, data.text);
            testContext.publishPlayback(pathA, 4, 0, 0, data.error);
            compare(player.announcements, said + 1, "once");
            verify(!item("recordingProblem").visible, "not said twice");
            verify(silent(item("recordingStatus")), "nothing crammed beside the time");
            compare(item("recordingNotice").width, item("historyRecording").width);
            tap("recordingPlayButton");
            compare(lastHostCall().name, "playRecordingOwned", "a play that failed can be tried again");
            verify(!item("recordingNotice").visible, "the try clears it");
            testContext.publishPlayback(pathA, 4, 0, 0, data.error);
            compare(item("recordingNotice").text, data.text);
            compare(player.announcements, said + 2, "a repeat is read out again");
        }

        // Issue #909 (B3): a sole recording found gone leaves, and the sheet says so, read out once.
        function test_missing_recording_leaves() {
            openWith([rec("t-1", pathA, 12000)]);
            var said = item("historyRecording").announcements;
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 4, 0, 0, "missing");
            verify(!item("historyRecording").visible, "a recording found gone leaves the sheet");
            verify(item("recordingNotice").visible, "the sheet says why");
            compare(item("recordingNotice").text, "This recording is gone.");
            compare(item("historyRecording").announcements, said + 1);
            compare(item("historyRecording").announced, "This recording is gone.");
            sheet().visible = false;
            testContext.resetHostPlayback();

            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            verify(!item("recordingNotice").visible, "opened again, the sheet has nothing to say");
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
            compare(lastHostCall().name, "playRecordingOwned");
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

        // Issue #909 (B2): Play all parts is one play of the host, its parts in order with their identities and the
        // call's numbering; the chip shown follows the part the host plays.
        function test_parts_and_play_all() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000), rec("t-3", pathC, 4000)]);
            compare(item("recordingHeader").text, "Call recording · 3 parts");
            compare(item("recordingPart0").text, "Part 1 · 0:12");
            compare(item("recordingPart2").text, "Part 3 · 0:04");
            verify(item("recordingPart0").checked);
            compare(item("recordingDelete").text, "Delete part 1");

            verify(!item("recordingPlayAll").checkable, "Play all is an action, not a fourth part");
            compare(item("recordingPlayAll").text, "Play all parts");
            verify(item("recordingPlayAll").mapToItem(sheet(), 0, 0).y > item("recordingPart2").mapToItem(sheet(), 0, 0).y, "under the parts");
            tap("recordingPlayAll");
            compare(lastHostCall().name, "playRecordings");
            compare(lastHostCall().mode, 1, "as the call's parts");
            var items = lastHostCall().items;
            compare(items.map(function (it) { return it.path; }), [pathA, pathB, pathC]);
            compare(items.map(function (it) { return it.partOrdinal; }), [1, 2, 3]);
            compare(items.map(function (it) { return it.partCount; }), [3, 3, 3]);
            compare(items.map(function (it) { return it.call; }), [0, 0, 0]);
            compare(items[1].size, 44 + 8000 * 16, "with the identity the store indexed");
            compare(items[1].mtimeSec, 1700000100);
            compare(items[1].mtimeNanos, 250);
            compare(items[0].title, "Dispatch · " + Qt.formatDateTime(new Date(1700000000 * 1000), "yyyy-MM-dd hh:mm"));
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 1, playbackPositionMs: 6000, playbackDurationMs: 12000, playbackCanSeek: true});
            compare(item("recordingTime").text, "0:06 / 0:12");
            testContext.publishPlaybackReading({item: 1, playbackToken: 2, playbackPositionMs: 0, playbackDurationMs: 8000});
            verify(item("recordingPart1").checked, "the host's next part shows");
            compare(item("recordingDelete").text, "Delete part 2");
            compare(hostCallsNamed("playRecordingOwned").length + hostCallsNamed("playRecording").length, 0, "the block plays nothing by itself");

            testContext.publishPlaybackReading({item: 2, playbackToken: 3, playbackPositionMs: 4000, playbackDurationMs: 4000});
            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished", playbackCanSeek: false});
            compare(hostCallsNamed("playRecordings").length, 1);
            var checked = [0, 1, 2].filter(function (i) { return item("recordingPart" + i).checked; });
            compare(checked, [2], "one chip shows the part");
            compare(item("recordingTime").text, "0:04 / 0:04");
        }

        // Issue #909 (B7): a part the host reads no length for advances all the same (the host's queue), takes no
        // seek, and the chip follows it.
        function test_play_all_part_without_a_length() {
            openWith([rec("t-1", pathA, 0), rec("t-2", pathB, 8000)]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 1, playbackDurationMs: 0, playbackCanSeek: false});
            verify(!item("recordingSeekBar").enabled, "no seek with no length");
            testContext.publishPlaybackReading({item: 1, playbackToken: 2, playbackDurationMs: 8000, playbackCanSeek: true});
            verify(item("recordingPart1").checked);
            verify(item("recordingSeekBar").enabled);
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
            compare(lastHostCall().name, "playRecordings");
            compare(lastHostCall().items[0].path, pathA, "Play all plays part 1 first");
            verify(item("recordingPart0").checked);
            testContext.publishPlaybackReading({item: 1, playbackState: 2, playbackToken: 4});
            verify(item("recordingPart1").checked, "then part 2");
        }

        // A part chosen by hand plays alone: one recording, which the host stops at its end.
        function test_part_chosen_by_hand_plays_alone() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPart1");
            compare(lastHostCall().name, "playRecordingOwned");
            compare(lastHostCall().path, pathB);
            compare(hostCallsNamed("playRecordings").length, 0);
            testContext.publishPlayback(pathB, 0, 8000, 8000, "");
            compare(lastHostCall().name, "playRecordingOwned", "nothing follows its end");
            verify(item("recordingPart1").checked);
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
            compare(lastStoreCall().name, "deleteRecordingChecked");
            compare(lastStoreCall().id, "t-2", "and it is the one deleted");
            compare(lastStoreCall().size, 44 + 8000 * 16, "with the identity shown");
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
                {tag: "changed", result: 6, text: "This recording changed. Save it again."},
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
            var generation = decoderHost.playbackGeneration;
            testContext.publishPlayback(pathB, 2, 1000, 8000, "");
            tap("recordingDelete");
            var confirm = findChild(sheet(), "recordingDeleteConfirm");
            verify(confirm.visible);
            compare(confirm.title, "Delete part 2?");
            compare(findChild(confirm, "confirmMessage").text, "Part 2 is deleted from this phone. The other parts and the call stay in History.");
            compare(findChild(confirm, "confirmActionButton").text, "Delete part 2", "the button names what it deletes");
            findChild(confirm, "confirmActionButton").activate();
            verify(!confirm.visible);
            compare(lastHostCall().name, "stopPlaybackGeneration", "the single recording playing stops");
            compare(lastHostCall().generation, generation);
            var calls = recordings.calls();
            compare(calls[calls.length - 1].name, "deleteRecordingChecked");
            compare(calls[calls.length - 1].id, "t-2");
            compare(calls[calls.length - 1].path, pathB, "R68: the path pressed goes with the delete");
            compare([calls[calls.length - 1].size, calls[calls.length - 1].mtimeSec, calls[calls.length - 1].mtimeNanos], [44 + 8000 * 16, 1700000100, 250]);
        }

        // Issue #909: the store lists every recording's identity, so a part
        // listed without one is malformed. Play all parts passes it as it is,
        // inventing none, and the host refuses the play: nothing plays, and the
        // sheet says it couldn't start. Delete deletes it by its id alone, as
        // before #909.
        function test_part_without_an_identity() {
            var bare = rec("t-2", pathB, 8000);
            delete bare.identitySize;
            delete bare.mtimeSec;
            delete bare.mtimeNanos;
            openWith([rec("t-1", pathA, 12000), bare]);
            compare(decoderHost.playbackGeneration, 0);
            tap("recordingPlayAll");
            compare(lastHostCall().name, "playRecordings");
            var items = lastHostCall().items;
            compare([items[0].size, items[0].mtimeSec, items[0].mtimeNanos], [44 + 12000 * 16, 1700000100, 250], "part 1's identity");
            compare([items[1].size, items[1].mtimeSec, items[1].mtimeNanos], [undefined, undefined, undefined], "none invented for part 2");
            compare(decoderHost.playbackGeneration, 0, "refused: nothing plays");
            compare(item("recordingNotice").text, "Couldn't start playing. Try again.");
            tap("recordingPart1");
            tap("recordingDelete");
            findChild(item("recordingDeleteConfirm"), "confirmActionButton").activate();
            compare(lastStoreCall().name, "deleteRecording", "by its id alone");
            compare(lastStoreCall().id, "t-2");
        }

        // Issue #909 (B11): the store refuses a delete whose file is another one under the id now, or whose id is
        // gone; nothing stops, and the sheet says which.
        function test_delete_refused_by_identity_data() {
            return [{tag: "changed", refusal: "changed", text: "This recording changed. Delete it again."},
                {tag: "gone", refusal: "gone", text: "This recording is gone."}];
        }
        function test_delete_refused_by_identity(data) {
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            recordings.setDeleteIdentityRefusal(data.refusal);
            var said = item("historyRecording").announcements;
            tap("recordingDelete");
            findChild(item("recordingDeleteConfirm"), "confirmActionButton").activate();
            compare(lastStoreCall().name, "deleteRecordingChecked");
            compare(hostCallsNamed("stopPlaybackGeneration").length + hostCallsNamed("stopPlayback").length, 0, "nothing stops");
            compare(item("recordingNotice").text, data.text);
            compare(item("historyRecording").announcements, said + 1);
        }

        // Issue #902: a save the store refuses (storage is read again) says so,
        // as an export does, and opens no picker.
        function test_save_refused_while_loading() {
            openWith([rec("t-1", pathA, 12000)]);
            recordings.setSelection(1, 96000, [pathA]);
            recordings.setLoading(true);
            var requests = testContext.lastDocumentRequest();
            tap("recordingSave");
            compare(lastStoreCall().name, "selectRecordings");
            compare(item("historyRecording").saveSelection, 0, "nothing is held");
            compare(item("recordingNotice").text, "Recordings are still loading. Try again in a moment.");
            verify(!findChild(item("historyRecording"), "recordingSavePicker").visible, "no picker opens");
            compare(testContext.lastDocumentRequest(), requests, "nothing is asked of the platform");
            recordings.setLoading(false);
        }

        // Issue #902: a delete the store refuses (storage that came back is read
        // again) says so and leaves the recording playing.
        function test_delete_refused_while_loading() {
            openWith([rec("t-1", pathA, 12000)]);
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            recordings.setLoading(true);
            var stops = hostCallsNamed("stopPlayback").length;
            tap("recordingDelete");
            var confirm = findChild(sheet(), "recordingDeleteConfirm");
            findChild(confirm, "confirmActionButton").activate();
            verify(!confirm.visible);
            var calls = recordings.calls();
            compare(calls[calls.length - 1].name, "deleteRecordingChecked");
            compare(hostCallsNamed("stopPlayback").length, stops, "nothing was deleted, so it plays on");
            compare(hostCallsNamed("stopPlaybackGeneration").length, 0);
            compare(item("recordingNotice").text, "Recordings are still loading. Try again in a moment.");
            recordings.setLoading(false);
        }

        // Issue #902: a delete of a recording whose storage is not connected is
        // refused at once; the player says so and plays on.
        function test_delete_refused_while_storage_away() {
            openWith([rec("t-1", pathA, 12000)]);
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            recordings.setStorageAway(true);
            var stops = hostCallsNamed("stopPlayback").length;
            tap("recordingDelete");
            var confirm = findChild(sheet(), "recordingDeleteConfirm");
            findChild(confirm, "confirmActionButton").activate();
            var calls = recordings.calls();
            compare(calls[calls.length - 1].name, "deleteRecordingChecked");
            compare(hostCallsNamed("stopPlayback").length, stops, "nothing was deleted, so it plays on");
            compare(hostCallsNamed("stopPlaybackGeneration").length, 0);
            compare(item("recordingNotice").text, "That storage isn't connected. Delete again once it's back.");
            recordings.setStorageAway(false);
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
            var generation = decoderHost.playbackGeneration;
            verify(generation > 0);
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            sheet().visible = false;
            compare(lastHostCall().name, "stopPlaybackGeneration");
            compare(lastHostCall().generation, generation, "by the generation it started");

            testContext.resetHostPlayback();
            openWith([rec("t-2", pathB, 12000)]);
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            sheet().visible = false;
            compare(testContext.hostCalls().length, 0, "another recording plays on");
        }

        // What this block started stops once the row no longer names that
        // recording (History trimmed or changed the row), and nothing more at
        // the close.
        function test_closing_stops_what_it_started_after_the_row_changed() {
            var uid = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            verify(callHistory.setRowRecordings(uid, [rec("t-2", pathB, 8000)]));
            compare(lastHostCall().name, "stopPlaybackGeneration");
            var calls = testContext.hostCalls().length;
            sheet().visible = false;
            compare(testContext.hostCalls().length, calls, "stopped already");
        }
        function test_row_trimmed_away_stops_its_playback() {
            var uid = openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            verify(callHistory.setRowRecordings(uid, []));
            verify(!item("historyRecording").visible);
            compare(lastHostCall().name, "stopPlaybackGeneration", "no control is left for it, so it stops");
        }

        // The playing part of a call of several gone from the list (its file
        // deleted from a computer): the play stops, as no control is left for it.
        function test_playing_part_gone_stops_its_playback() {
            var uid = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            verify(callHistory.setRowRecordings(uid, [rec("t-2", pathB, 12000)]));
            verify(item("historyRecording").visible, "the other part is still shown");
            compare(lastHostCall().name, "stopPlaybackGeneration", "the part that played is gone, so its play stops");
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

        // ---- Issue #909: the sheet beside a playlist, its own plays and Play from here ----

        // The row with @p uid opened, as History opens it.
        function openRow(uid) {
            sheet().open({uid: uid, name: "Dispatch", tg: 1001, when: 1700000000, systemUid: "test-system"});
            waitForRendering(sheet());
        }
        // A playlist item of @p recording, as History's playlist builds one.
        function itemOf(recording, call, ordinal, count, uid) {
            return {path: recording.path, size: recording.identitySize, mtimeSec: recording.mtimeSec, mtimeNanos: recording.mtimeNanos, call: call, partOrdinal: ordinal, partCount: count, title: "Dispatch", line: "", key: String(uid)};
        }
        // History's playlist playing @p items (2 is DecoderHost.PlaybackPlaylist); its generation.
        function playPlaylist(items) {
            var generation = decoderHost.playRecordings(items, 2, "history");
            verify(generation > 0);
            return generation;
        }
        // A button below the window's fold, pressed as a screen reader does.
        function press(name) {
            var target = item(name);
            verify(target !== null, name);
            verify(target.visible && target.enabled, name + " can be pressed");
            target.activate();
        }
        function stopCalls() {
            return hostCallsNamed("stopPlaybackGeneration").length + hostCallsNamed("stopPlayback").length;
        }
        function checkedParts(count) {
            var checked = [];
            for (var i = 0; i < count; i++) {
                if (item("recordingPart" + i).checked)
                    checked.push(i);
            }
            return checked;
        }

        // The sheet of the call a playlist plays opens on the part playing (a part before it away), follows it to
        // the next one, and comes back on it after a close (Show on map's return); closing it, or the row losing its
        // recordings, never stops the playlist.
        function test_follows_the_playlist_part() {
            var away = rec("t-1", pathA, 12000, {available: false});
            var b = rec("t-2", pathB, 8000);
            var c = rec("t-3", pathC, 4000);
            var uid = callHistory.pushWithRecordings([away, b, c]);
            playPlaylist([itemOf(b, 0, 2, 3, uid), itemOf(c, 0, 3, 3, uid)]);
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 7, playbackPositionMs: 2000, playbackDurationMs: 8000, playbackCanSeek: true});
            openRow(uid);
            compare(checkedParts(3), [1], "opened on the part that plays");
            compare(item("recordingTime").text, "0:02 / 0:08");
            verify(item("recordingPlayButton").showPause);
            verify(item("recordingSeekBar").enabled);
            testContext.publishPlaybackReading({item: 1, playbackToken: 8, playbackPositionMs: 0, playbackDurationMs: 4000});
            compare(checkedParts(3), [2], "and on to the next");
            // Moved off it (not by a chip, which would end following), a position tick leaves it there; the next
            // item brings it back.
            item("historyRecording").select(1);
            testContext.publishPlaybackReading({playbackPositionMs: 1000});
            testContext.publishPlaybackReading({playbackPositionMs: 1500, playbackDurationMs: 4000});
            compare(checkedParts(3), [1], "never on a position tick");
            testContext.publishPlaybackReading({item: 0, playbackToken: 9});
            compare(checkedParts(3), [1], "an item change follows");
            testContext.publishPlaybackReading({item: 1, playbackToken: 10});
            compare(checkedParts(3), [2]);
            sheet().visible = false;
            compare(stopCalls(), 0, "closing a sheet that follows a playlist stops nothing");
            openRow(uid);
            compare(checkedParts(3), [2], "opened again, it follows again");
            verify(callHistory.setRowRecordings(uid, []));
            verify(!item("historyRecording").visible);
            compare(stopCalls(), 0, "a row trimmed under it stops nothing");
            compare(hostCallsNamed("playRecordingOwned").length, 0, "and the sheet started nothing");
        }

        // A chip whose file is another one under the same path (the store read it again) is not the recording that
        // plays: no item controls, and the selection does not follow it.
        function test_follow_needs_the_same_file() {
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([rec("t-1", pathA, 12000), b]);
            var listed = itemOf(b, 0, 2, 2, uid);
            listed.size = b.identitySize + 1;
            playPlaylist([listed]);
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 9, playbackPositionMs: 3000, playbackDurationMs: 8000, playbackCanSeek: true});
            openRow(uid);
            compare(checkedParts(2), [0], "not followed");
            var player = item("historyRecording");
            player.select(1);
            verify(!player.loaded, "the chip is not the file that plays");
            compare(item("recordingTime").text, "0:00 / 0:08");
            verify(!item("recordingSeekBar").enabled);
            compare(item("recordingPlayButton").accessibleName, "Play recording");
        }

        // B1: a part whose storage is away is selected with its away line and not played; a chip the user picked
        // keeps its place while the playlist moves on.
        function test_picked_chip_keeps_its_place() {
            var away = rec("t-1", pathA, 12000, {available: false});
            var b = rec("t-2", pathB, 8000);
            var c = rec("t-3", pathC, 4000);
            var uid = callHistory.pushWithRecordings([away, b, c]);
            playPlaylist([itemOf(b, 0, 2, 3, uid), itemOf(c, 0, 3, 3, uid)]);
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 7});
            openRow(uid);
            compare(checkedParts(3), [1]);
            var calls = testContext.hostCalls().length;
            tap("recordingPart0");
            compare(checkedParts(3), [0], "the away part is selected");
            compare(testContext.hostCalls().length, calls, "and not played");
            compare(item("recordingProblem").text, "The storage this recording is on isn't available right now.");
            verify(!item("recordingPlayButton").enabled);
            testContext.publishPlaybackReading({item: 1, playbackToken: 8});
            compare(checkedParts(3), [0], "the user's pick stays");
        }

        // B1: Play all parts plays the parts whose storage is there, each with the call's own numbering (part 2 of
        // 3), and is offered while any is.
        function test_play_all_leaves_away_parts_out() {
            var uid = openWith([rec("t-1", pathA, 12000, {available: false}), rec("t-2", pathB, 8000), rec("t-3", pathC, 4000, {available: false})]);
            verify(item("recordingPlayAll").enabled);
            tap("recordingPlayAll");
            compare(lastHostCall().name, "playRecordings");
            var items = lastHostCall().items;
            compare(items.length, 1);
            compare([items[0].path, items[0].partOrdinal, items[0].partCount], [pathB, 2, 3], "part 2 of 3");
            compare(checkedParts(3), [1], "the part it plays first is shown");
            compare(item("recordingPart1").text, "Part 2 · 0:08");
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000, {available: false}), rec("t-2", pathB, 8000, {available: false})]));
            verify(!item("recordingPlayAll").enabled, "none there: nothing to play");
        }

        // B6: a host with no player offers no play: no play button, seek bar or Play all parts, and the block says
        // why; a chip only selects, and Save and Delete act on the part selected.
        function test_no_player_selects_only() {
            testContext.setPlaybackSupported(false);
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            verify(!item("recordingPlayButton").visible);
            verify(!item("recordingSeekBar").visible);
            verify(!item("recordingPlayAll").visible);
            compare(item("recordingTime").text, "0:12", "its length alone");
            compare(item("recordingProblem").text, "Recordings can't be played on this device.");
            tap("recordingPart1");
            compare(testContext.hostCalls().length, 0, "a chip plays nothing");
            compare(checkedParts(2), [1]);
            compare(item("recordingTime").text, "0:08");
            verify(item("recordingSave").enabled);
            compare(item("recordingDelete").text, "Delete part 2");
            tap("recordingDelete");
            findChild(item("recordingDeleteConfirm"), "confirmActionButton").activate();
            compare(lastStoreCall().name, "deleteRecordingChecked");
            compare(lastStoreCall().id, "t-2");
            item("historyRecording").toggle();
            item("historyRecording").startPlayAll();
            compare(testContext.hostCalls().length, 0, "nor do the block's own entry points");
            sheet().showPlayFromHere = true;
            verify(!item("historyPlayFromHere").visible, "and no Play from here");
        }

        // Issue #909 (R49): a start asked for and not yet made (5) reads as wanted: Pause, with no status line of its
        // own and its position held; a tap pauses it.
        function test_starting_reads_pause() {
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlaybackReading({playbackState: 5, playbackToken: 3, playbackDurationMs: 12000, playbackPositionMs: 2000, playbackCanSeek: true});
            verify(item("recordingPlayButton").showPause, "Pause while the start is pending");
            compare(item("recordingPlayButton").accessibleName, "Pause recording");
            compare(item("recordingStatus").text, "", "no line of its own");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "pausePlaybackFor");
            compare(lastHostCall().token, 3);
        }

        // B4: halted for another app's audio, it reads so, and its button is Pause, which cancels the carry-on;
        // headphones unplugged say so too.
        function test_interrupted_reads_pause() {
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 3, playbackDurationMs: 12000, playbackCanSeek: true});
            testContext.publishPlaybackReading({playbackState: 3, playbackInterrupted: true, playbackCanSeek: false});
            compare(item("recordingStatus").text, "Paused for another app's audio");
            verify(item("recordingPlayButton").showPause);
            compare(item("recordingPlayButton").accessibleName, "Pause recording");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "pausePlaybackFor");
            compare(lastHostCall().token, 3);
            testContext.publishPlaybackReading({playbackInterrupted: false, playbackPausedReason: "noisy"});
            compare(item("recordingStatus").text, "Paused: headphones unplugged");
            compare(item("recordingPlayButton").accessibleName, "Resume recording");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "resumePlaybackFor");
            compare(lastHostCall().token, 3);
        }

        // A playlist's part opening paused (Next while paused): the reading is Preparing with the reason it pauses for,
        // so the sheet reads Paused and offers Resume, which resumes it by its token.
        function test_opening_paused_reads_paused() {
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([b]);
            playPlaylist([itemOf(b, 0, 1, 1, uid)]);
            testContext.publishPlaybackReading({playbackState: 1, playbackToken: 4, playbackPausedReason: "user"});
            openRow(uid);
            compare(item("recordingStatus").text, "Paused");
            verify(!item("recordingPlayButton").showPause);
            compare(item("recordingPlayButton").accessibleName, "Resume recording");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "resumePlaybackFor");
            compare(lastHostCall().token, 4);
            testContext.publishPlaybackReading({playbackState: 1, playbackPausedReason: ""});
            compare(item("recordingStatus").text, "Opening…", "wanted again, it opens");
            verify(item("recordingPlayButton").showPause);
        }

        // A single play the host publishes before the platform answers (generation 0, Preparing) is opening, not idle.
        function test_single_play_starting_reads_opening() {
            openWith([rec("t-1", pathA, 12000)]);
            testContext.publishPlaybackReading({playbackPath: pathA, playbackState: 1, playbackGeneration: 0});
            compare(item("recordingStatus").text, "Opening…");
            verify(item("recordingPlayButton").showPause);
        }

        // A resume the platform refused the audio stays paused and says why, read out once, in any mode (here a
        // playlist's part the sheet follows); the sentence goes once it plays, and a later refusal is said again.
        function test_resume_refused_says_cant_play() {
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([b]);
            playPlaylist([itemOf(b, 0, 1, 1, uid)]);
            testContext.publishPlaybackReading({playbackState: 3, playbackToken: 4, playbackPausedReason: "user", playbackDurationMs: 8000, playbackCanSeek: true});
            openRow(uid);
            var player = item("historyRecording");
            var said = player.announcements;
            tap("recordingPlayButton");
            compare(lastHostCall().name, "resumePlaybackFor");
            compare(lastHostCall().token, 4);
            testContext.publishPlaybackReading({playbackState: 3, playbackPausedReason: "unavailable"});
            var text = "Can't play right now. Another app has the audio, such as a phone call.";
            compare(item("recordingNotice").text, text);
            compare(player.announcements, said + 1);
            compare(item("recordingStatus").text, "Paused");
            testContext.publishPlaybackReading({playbackPositionMs: 500});
            compare(player.announcements, said + 1, "once");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "resumePlaybackFor");
            compare(decoderHost.playbackState, 2, "the platform took it this time");
            verify(!item("recordingNotice").visible, "playing, it goes");
            testContext.publishPlaybackReading({playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 2});
            compare(item("recordingNotice").text, text);
            compare(player.announcements, said + 2, "a later refusal is said again");
        }

        // Pause, Resume and a seek carry the token of the recording open when the gesture began: a drag begun on one
        // part and released on the next is dropped by the host.
        function test_item_commands_carry_the_gesture_token() {
            openWith([rec("t-1", pathA, 10000), rec("t-2", pathB, 10000)]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 5, playbackPositionMs: 1000, playbackDurationMs: 10000, playbackCanSeek: true});
            var bar = item("recordingSeekBar");
            var y = bar.height / 2;
            mousePress(bar, bar.width * 0.2, y);
            for (var i = 1; i <= 8; ++i)
                mouseMove(bar, bar.width * (0.2 + i * 0.05), y);
            verify(bar.dragging, "a drag is under way");
            testContext.publishPlaybackReading({item: 1, playbackToken: 6, playbackPositionMs: 0});
            compare(checkedParts(2), [1]);
            mouseRelease(bar, bar.width * 0.6, y);
            compare(lastHostCall().name, "seekPlaybackFor");
            compare(lastHostCall().token, 5, "the token of the part the drag began on");
            compare(decoderHost.playbackPositionMs, 0, "which the host drops");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "pausePlaybackFor");
            compare(lastHostCall().token, 6);
        }

        // A tap acts on what its press began on: Pause pressed on call A's part, the playlist on to call B before the
        // release, sends nothing (not a play of A, which would replace the playlist); a tap on the seek bar pressed on
        // one part and released on the next seeks nothing, and a drag that starts after the move does not start.
        function test_a_press_acts_on_its_own_target() {
            var a = rec("t-1", pathA, 12000);
            var b = rec("t-2", pathB, 8000);
            var uidA = callHistory.pushWithRecordings([a]);
            var uidB = callHistory.pushWithRecordings([b]);
            playPlaylist([itemOf(a, 0, 1, 1, uidA), itemOf(b, 1, 1, 1, uidB)]);
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 3, playbackDurationMs: 12000, playbackCanSeek: true});
            openRow(uidA);
            var button = item("recordingPlayButton");
            verify(button.showPause, "A plays here");
            var calls = testContext.hostCalls().length;
            mousePress(button, button.width / 2, button.height / 2);
            testContext.publishPlaybackReading({item: 1, playbackToken: 4, playbackDurationMs: 8000});
            verify(!button.showPause, "A no longer plays");
            mouseRelease(button, button.width / 2, button.height / 2);
            compare(testContext.hostCalls().length, calls, "the press's target moved: nothing is sent");
            compare(hostCallsNamed("playRecordingOwned").length, 0);
            tap("recordingPlayButton");
            compare(lastHostCall().name, "playRecordingOwned", "a press begun now plays A");
            sheet().visible = false;

            uidA = openWith([rec("t-3", pathA, 10000), rec("t-4", pathB, 10000)]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 5, playbackPositionMs: 1000, playbackDurationMs: 10000, playbackCanSeek: true});
            var bar = item("recordingSeekBar");
            var y = bar.height / 2;
            var seeks = hostCallsNamed("seekPlaybackFor").length;
            mousePress(bar, bar.width / 2, y);
            testContext.publishPlaybackReading({item: 1, playbackToken: 6, playbackPositionMs: 0});
            mouseRelease(bar, bar.width / 2, y);
            compare(hostCallsNamed("seekPlaybackFor").length, seeks, "a tap pressed on part 1 seeks nothing on part 2");
            mousePress(bar, bar.width * 0.2, y);
            testContext.publishPlaybackReading({item: 0, playbackToken: 7, playbackPositionMs: 0});
            for (var i = 1; i <= 8; ++i)
                mouseMove(bar, bar.width * (0.2 + i * 0.05), y);
            verify(!bar.dragging, "a drag that starts after the move does not start");
            mouseRelease(bar, bar.width * 0.6, y);
            compare(hostCallsNamed("seekPlaybackFor").length, seeks, "and seeks nothing");
            mouseClick(bar, bar.width / 2, y);
            compare(hostCallsNamed("seekPlaybackFor").length, seeks + 1, "a tap begun on the part open seeks it");
            compare(lastHostCall().token, 7);
        }

        // Ownership by generation: a sheet whose own play a playlist replaced on the same path stops nothing at its
        // close; one whose Play all parts moved on to a later part stops it by its generation.
        function test_close_stops_only_its_own_generation() {
            var a = rec("t-1", pathA, 12000);
            var uid = openWith([a, rec("t-2", pathB, 8000)]);
            tap("recordingPlayButton");
            var own = decoderHost.playbackGeneration;
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            var other = playPlaylist([itemOf(a, 0, 1, 2, uid)]);
            verify(other !== own);
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 2});
            verify(item("historyRecording").loaded, "the same file plays");
            sheet().visible = false;
            compare(stopCalls(), 0, "the playlist plays on");

            openRow(uid);
            tap("recordingPlayAll");
            var parts = decoderHost.playbackGeneration;
            testContext.publishPlaybackReading({item: 1, playbackState: 2, playbackToken: 3});
            sheet().visible = false;
            compare(lastHostCall().name, "stopPlaybackGeneration");
            compare(lastHostCall().generation, parts, "its parts stop with it, on whichever part");
        }

        // A delete never stops a queue: a playlist's part, or the sheet's own Play all parts' part, plays to its end
        // and the queue goes on; the chip follows the next part.
        function test_delete_never_stops_a_queue() {
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([b]);
            playPlaylist([itemOf(b, 0, 1, 1, uid)]);
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 4});
            openRow(uid);
            tap("recordingDelete");
            findChild(item("recordingDeleteConfirm"), "confirmActionButton").activate();
            compare(lastStoreCall().name, "deleteRecordingChecked");
            compare(stopCalls(), 0, "the playlist plays on");
            sheet().visible = false;

            uid = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000), rec("t-3", pathC, 4000)]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 1, playbackState: 2, playbackToken: 5});
            compare(checkedParts(3), [1]);
            tap("recordingDelete");
            findChild(item("recordingDeleteConfirm"), "confirmActionButton").activate();
            compare(lastStoreCall().id, "t-2");
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000), rec("t-3", pathC, 4000)]));
            compare(stopCalls(), 0, "the parts play on");
            testContext.publishPlaybackReading({item: 2, playbackToken: 6});
            compare(checkedParts(2), [1], "on to the next part");
        }

        // D9 for Play all parts: the row's last listed part deleted while it plays (the others deleted before) leaves
        // the open part playing to its end, since the row is still there; the row itself trimmed stops the sheet's play.
        function test_last_part_deleted_keeps_play_all_parts() {
            var uid = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPlayAll");
            var parts = decoderHost.playbackGeneration;
            testContext.publishPlaybackReading({item: 1, playbackState: 2, playbackToken: 5});
            verify(callHistory.setRowRecordings(uid, [rec("t-2", pathB, 8000)]));
            compare(stopCalls(), 0, "an earlier part deleted: it plays on");
            verify(callHistory.setRowRecordings(uid, []));
            verify(!item("historyRecording").visible, "no recording is listed");
            compare(stopCalls(), 0, "the part playing deleted, the row still there: it plays to its end");
            verify(callHistory.trimRow(uid));
            compare(lastHostCall().name, "stopPlaybackGeneration", "the row gone: the sheet's play stops");
            compare(lastHostCall().generation, parts);
        }

        // B10: while a Delete confirmation is out the chip stays put and its words and target are the ones pressed;
        // following catches up once it is dismissed.
        function test_following_waits_for_a_delete_confirmation() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 1});
            tap("recordingDelete");
            var confirm = item("recordingDeleteConfirm");
            compare(confirm.title, "Delete part 1?");
            testContext.publishPlaybackReading({item: 1, playbackToken: 2});
            compare(checkedParts(2), [0], "the chip waits");
            compare(confirm.title, "Delete part 1?");
            compare(findChild(confirm, "confirmActionButton").text, "Delete part 1");
            findChild(confirm, "confirmCancelButton").activate();
            compare(checkedParts(2), [1], "dismissed, it follows");

            tap("recordingDelete");
            compare(confirm.title, "Delete part 2?");
            testContext.publishPlaybackReading({item: 0, playbackToken: 3});
            compare(confirm.title, "Delete part 2?", "its words stay");
            findChild(confirm, "confirmActionButton").activate();
            compare(lastStoreCall().name, "deleteRecordingChecked");
            compare(lastStoreCall().id, "t-2", "the part pressed on is the one deleted");
            compare(stopCalls(), 0);
            compare(checkedParts(2), [0]);
        }

        // B10: a Save picker out keeps its part shown while the parts play on, and is not cancelled by it; following
        // catches up once the picker has its answer.
        function test_following_waits_for_a_save_picker() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            recordings.setSelection(1, 96000, [pathA]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 1});
            var id = startSave();
            testContext.publishPlaybackReading({item: 1, playbackToken: 2});
            verify(findChild(item("historyRecording"), "recordingSavePicker").visible, "the pick is still out");
            compare(hostCallsNamed("cancelDocumentRequest").length, 0);
            compare(checkedParts(2), [0]);
            var selection = item("historyRecording").saveSelection;
            testContext.answerDocumentPicked(id, "content://picked/A.wav", "A.wav", false);
            compare(hostCallsNamed("saveRecording").length, 1, "the save goes ahead");
            compare(checkedParts(2), [1], "then the chip follows");
            testContext.finishRecordingTransfer(selection, 0, 0);
            compare(item("historyRecording").saveSelection, 0);
        }

        // Play from here: History's sheet only, while the host has a player and the row has a recording that can be
        // played and is among the rows History shows.
        function test_play_from_here_shown() {
            var uid = openWith([rec("t-1", pathA, 12000)]);
            verify(!item("historyPlayFromHere").visible, "not on a sheet that does not ask for it");
            sheet().showPlayFromHere = true;
            var button = item("historyPlayFromHere");
            verify(button.visible);
            waitForRendering(sheet());
            compare(button.text, "Play from here");
            compare(button.Accessible.description, "Plays this call, then each newer call History shows");
            verify(button.mapToItem(sheet(), 0, 0).y > item("historyRecording").mapToItem(sheet(), 0, 0).y, "under the recording");
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000, {available: false})]));
            verify(!button.visible, "nothing that can be played");
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000, {deleteQueued: true})]));
            verify(button.visible, "a delete waiting is the press's to answer");
            testContext.setHistoryPlaylistAnswer("rowNone", []);
            press("historyPlayFromHere");
            verify(sheet().visible);
            compare(item("recordingNotice").text, "This call's recording can't be played now.");
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000)]));
            verify(button.visible);
            historyView.filterText = "no-row-matches-this";
            try {
                verify(!button.visible, "the row not among those History shows");
            } finally {
                historyView.filterText = "";
            }
            verify(button.visible);
        }

        // Accepted: the search still being typed is handed over first, the sheet closes, and the playlist is the
        // host's play from then on (the sheet's own play it replaced does not stop it at the close).
        function test_play_from_here_accepted() {
            var uid = openWith([rec("t-1", pathA, 12000)]);
            sheet().showPlayFromHere = true;
            tap("recordingPlayButton");
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            var asked = -1;
            var note = function () {
                asked = testContext.historyPlaylistCalls().length;
            };
            sheet().playFromHereRequested.connect(note);
            try {
                press("historyPlayFromHere");
            } finally {
                sheet().playFromHereRequested.disconnect(note);
            }
            compare(asked, 0, "History is asked before the list is read");
            var calls = testContext.historyPlaylistCalls();
            compare(calls.length, 1);
            compare(calls[0].startUid, uid);
            verify(!sheet().visible, "the sheet closes");
            compare(decoderHost.playbackMode, 2, "the playlist plays");
            compare(stopCalls(), 0, "and nothing stops it");
            // A failure of the new playlist is its own (the bar's), not the closed sheet's.
            var said = item("historyRecording").announcements;
            testContext.publishPlaybackReading({playbackState: 4, playbackError: "failed", playbackEndedReason: "start"});
            compare(item("historyRecording").announcements, said);
            verify(!sheet().visible);
        }

        // Refused at the press: the sheet stays, says why on its notice line (read out), and what played plays on.
        function test_play_from_here_refused_data() {
            return [{tag: "rowNone", result: "rowNone", text: "This call's recording can't be played now."},
                {tag: "gone", result: "gone", text: "This call isn't in the list now."},
                {tag: "none", result: "none", text: "None of the shown calls has a recording that can be played now."},
                {tag: "refused", result: "refused", text: "Couldn't start playing. Try again."},
                {tag: "unsupported", result: "unsupported", text: "Recordings can't be played on this device."}];
        }
        function test_play_from_here_refused(data) {
            openWith([rec("t-1", pathA, 12000)]);
            sheet().showPlayFromHere = true;
            tap("recordingPlayButton");
            var own = decoderHost.playbackGeneration;
            testContext.publishPlayback(pathA, 2, 1000, 12000, "");
            testContext.setHistoryPlaylistAnswer(data.result, []);
            var player = item("historyRecording");
            var said = player.announcements;
            press("historyPlayFromHere");
            verify(sheet().visible, "the sheet stays");
            compare(item("recordingNotice").text, data.text);
            waitForRendering(sheet());
            verify(item("recordingNotice").mapToItem(sheet(), 0, 0).y < item("historyPlayFromHere").mapToItem(sheet(), 0, 0).y, "the reason right under the recording, above the button");
            compare(player.announcements, said + 1, "read out");
            compare(decoderHost.playbackGeneration, own, "what played plays on");
            compare(stopCalls(), 0);
        }

        // The playlist refused by the host itself (the fake's map refusing the items) reads as refused.
        function test_play_from_here_host_refused() {
            openWith([rec("t-1", pathA, 12000)]);
            sheet().showPlayFromHere = true;
            testContext.setPlaybackAccepted(false);
            press("historyPlayFromHere");
            verify(sheet().visible);
            compare(item("recordingNotice").text, "Couldn't start playing. Try again.");
        }

        // A seek the host drops (its token not the one open) leaves no landing to wait for; one it takes does.
        function test_dropped_seek_waits_for_no_landing() {
            openWith([rec("t-1", pathA, 60000)]);
            tap("recordingPlayButton");
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 5, playbackPositionMs: 1000, playbackDurationMs: 60000, playbackCanSeek: true});
            var player = item("historyRecording");
            var bar = item("recordingSeekBar");
            testContext.setSeekApplied(false);
            bar.seekTo(30000, 4);
            compare(lastHostCall().token, 4);
            compare(player.seekTargetMs, -1, "a stale token: nothing to wait for");
            bar.seekTo(30000, 5);
            compare(player.seekTargetMs, 30000, "the open one's: waited for");
            testContext.setSeekApplied(true);
        }

        // The Android host shows no position for a seek (the player may have moved on since the poll its token came
        // from): only a record that counts a landing since the seek (#909 R56), with a position near the target, lands
        // it; a position near the target read before the seek reached the player does not.
        function test_seek_lands_on_the_players_record() {
            openWith([rec("t-1", pathA, 60000)]);
            tap("recordingPlayButton");
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 5, playbackPositionMs: 1000, playbackDurationMs: 60000, playbackCanSeek: true});
            var player = item("historyRecording");
            var landed = player.seekLanded;
            testContext.setSeekApplied(false);
            item("recordingSeekBar").seekTo(30000, 5);
            testContext.setSeekApplied(true);
            compare(decoderHost.playbackPositionMs, 1000, "nothing shown at the seek");
            compare(player.seekTargetMs, 30000, "waited for");
            testContext.publishPlaybackReading({playbackPositionMs: 1250});
            compare(player.seekLanded, landed, "a record from before the seek is no landing");
            testContext.publishPlaybackReading({playbackPositionMs: 30100});
            compare(player.seekLanded, landed, "a position near the target with no landing counted is none either");
            testContext.publishPlaybackReading({playbackSeeksLanded: 1});
            compare(player.seekLanded, landed + 1, "the record that counts its landing, there, is");
            compare(player.seekTargetMs, -1);

            // A short seek: the confirmed position already near the target lands nothing until the player says so.
            testContext.setSeekApplied(false);
            item("recordingSeekBar").seekTo(30600, 5);
            testContext.setSeekApplied(true);
            testContext.publishPlaybackReading({playbackPositionMs: 30400});
            compare(player.seekTargetMs, 30600, "a stale confirmed position near the target is no landing");
            testContext.publishPlaybackReading({playbackPositionMs: 30600, playbackSeeksLanded: 2});
            compare(player.seekLanded, landed + 2, "its own landing is");
        }

        // R76: a seek asked again at the same target restarts its 2 s wait: the thumb stays on the target past 2 s from
        // the first request, and falls back only 2 s after the second.
        function test_repeated_seek_target_restarts_its_wait() {
            openWith([rec("t-1", pathA, 60000)]);
            tap("recordingPlayButton");
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 5, playbackPositionMs: 1000, playbackDurationMs: 60000, playbackCanSeek: true});
            var player = item("historyRecording");
            testContext.setSeekApplied(false);
            item("recordingSeekBar").seekTo(30000, 5);
            wait(1900);
            item("recordingSeekBar").seekTo(30000, 5);
            wait(400);
            compare(player.seekTargetMs, 30000, "2.3 s after the first request: still on the target");
            tryCompare(player, "seekTargetMs", -1, 2500, "it falls back 2 s after the second");
            testContext.setSeekApplied(true);
        }

        // A playlist's part still opening, with no token yet: the sheet offers no pause or resume that would reach
        // nothing, until the part has one.
        function test_no_control_before_the_token() {
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([b]);
            playPlaylist([itemOf(b, 0, 1, 1, uid)]);
            openRow(uid);
            verify(item("historyRecording").loaded);
            verify(!item("recordingPlayButton").enabled, "opening, no token");
            item("historyRecording").toggle();
            compare(hostCallsNamed("pausePlaybackFor").length + hostCallsNamed("pausePlayback").length, 0);
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 3});
            verify(item("recordingPlayButton").enabled);
            tap("recordingPlayButton");
            compare(lastHostCall().name, "pausePlaybackFor");
        }

        // A recording gone at the press: the host refuses the sheet's play before taking it, publishing nothing, and
        // the sheet says the recording is gone, read out once; it has nothing of its own to stop at the close.
        function test_gone_at_the_press() {
            openWith([rec("t-1", pathA, 12000)]);
            var player = item("historyRecording");
            var said = player.announcements;
            testContext.setPlaybackRefusal("missing");
            tap("recordingPlayButton");
            testContext.setPlaybackRefusal("");
            compare(lastHostCall().name, "playRecordingOwned");
            compare(decoderHost.playbackState, 0, "nothing published");
            compare(item("recordingNotice").text, "This recording is gone.");
            compare(player.announcements, said + 1, "read out");
            compare(player.announced, "This recording is gone.");
            var stops = stopCalls();
            sheet().visible = false;
            compare(stopCalls(), stops, "the close stops nothing");
        }

        // The sheet owns a play of part A; part B is gone at its press: the host refuses B before taking it and A
        // plays on, still the sheet's, so closing the sheet stops it.
        function test_gone_at_the_press_keeps_its_own_play() {
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            tap("recordingPlayButton");
            var own = decoderHost.playbackGeneration;
            verify(own > 0);
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 3, playbackDurationMs: 12000, playbackCanSeek: true});
            var player = item("historyRecording");
            compare(player.ownedGeneration, own);
            testContext.setPlaybackRefusal("missing");
            tap("recordingPart1");
            testContext.setPlaybackRefusal("");
            compare(lastHostCall().name, "playRecordingOwned");
            compare(lastHostCall().path, pathB);
            compare(item("recordingNotice").text, "This recording is gone.");
            compare(decoderHost.playbackGeneration, own, "A plays on");
            compare(player.ownedGeneration, own, "and is still the sheet's");
            sheet().visible = false;
            compare(lastHostCall().name, "stopPlaybackGeneration");
            compare(lastHostCall().generation, own, "closing the sheet stops it");
        }

        // A sheet's own start the host refused with a failed reading (generation 0, the platform refused it) is
        // cleared at the close, as no one owns it.
        function test_refused_start_cleared_at_close_data() {
            return [{tag: "failed", error: "failed"}];
        }
        function test_refused_start_cleared_at_close(data) {
            var uid = openWith([rec("t-1", pathA, 12000)]);
            testContext.setPlaybackRefusal(data.error);
            tap("recordingPlayButton");
            compare(decoderHost.playbackState, 4);
            compare(decoderHost.playbackGeneration, 0);
            verify(item("recordingNotice").visible, "said");
            testContext.setPlaybackRefusal("");
            var stops = hostCallsNamed("stopPlayback").length;
            sheet().visible = false;
            compare(hostCallsNamed("stopPlayback").length, stops + 1, "its reading is cleared");
            compare(decoderHost.playbackState, 0);
            openRow(uid);
            verify(item("historyRecording").visible, "the part shows again");
            // Another play's reading at the close is not this sheet's to clear.
            testContext.setPlaybackRefusal(data.error);
            tap("recordingPlayButton");
            testContext.setPlaybackRefusal("");
            playPlaylist([itemOf(rec("t-9", pathC, 4000), 0, 1, 1, 0)]);
            stops = stopCalls();
            sheet().visible = false;
            compare(stopCalls(), stops, "a playlist that replaced it plays on");
        }

        // A failure standing when the sheet opens (a resume refused) is said, and read out, as it opens.
        function test_standing_failure_said_on_open() {
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([b]);
            playPlaylist([itemOf(b, 0, 1, 1, uid)]);
            testContext.publishPlaybackReading({playbackState: 3, playbackToken: 4, playbackPausedReason: "unavailable"});
            var said = item("historyRecording").announcements;
            openRow(uid);
            compare(item("recordingNotice").text, "Can't play right now. Another app has the audio, such as a phone call.");
            compare(item("historyRecording").announcements, said + 1);
        }

        // R64: a store update that moves the sheet's selection reconciles its refusal, both ways. A refusal on part A
        // leaves the notice line once A is gone and the sheet shows B; and a reread that makes the refused file match
        // the sheet for the first time shows its refusal, read once through the shared key.
        function test_store_update_reconciles_the_refusal() {
            var text = "Can't play right now. Another app has the audio, such as a phone call.";
            var a = rec("t-1", pathA, 12000);
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([a, b]);
            playPlaylist([itemOf(a, 0, 1, 2, uid), itemOf(b, 0, 2, 2, uid)]);
            testContext.publishPlaybackReading({item: 0, playbackState: 3, playbackToken: 4, playbackPausedReason: "unavailable", playbackRefusals: 1, playbackDurationMs: 12000});
            openRow(uid);
            compare(item("recordingNotice").text, text, "the refusal on A is shown");
            verify(callHistory.setRowRecordings(uid, [b]));
            compare(item("historyRecording").currentPath, pathB, "A gone: the sheet shows B");
            tryCompare(item("recordingNotice"), "text", "", 1000, "A's refusal leaves the notice line");
            sheet().visible = false;
            testContext.resetHostPlayback();

            // The reverse: the sheet lists the file under another identity, then a reread lists the one refused.
            var c = rec("t-3", pathC, 4000);
            var other = rec("t-3", pathC, 4000, {mtimeSec: 1700000999});
            var row = callHistory.pushWithRecordings([other]);
            playPlaylist([itemOf(c, 0, 1, 1, row)]);
            testContext.publishPlaybackReading({item: 0, playbackState: 3, playbackToken: 6, playbackPausedReason: "unavailable", playbackRefusals: 1, playbackDurationMs: 4000});
            var player = item("historyRecording");
            var said = player.announcements;
            openRow(row);
            compare(item("recordingNotice").text, "", "not the file refused: no refusal shown");
            verify(callHistory.setRowRecordings(row, [c]));
            tryCompare(item("recordingNotice"), "text", text, 1000, "the reread file is the one refused: shown");
            compare(player.announcements, said + 1, "and read once");
            verify(callHistory.setRowRecordings(row, [c]));
            wait(20);
            compare(player.announcements, said + 1, "not again on another reread");
        }

        // R69: a dialog's dismissal that moves the selection reconciles the refusal. Delete (or the Save picker) open
        // on part 1, the playlist moves on to part 2, pauses, and a Resume is refused: the sheet still shows part 1, so
        // nothing is said. Dismissed, the sheet follows to part 2, shows the refusal and reads it once; closed, the
        // bar, shown then, stays silent.
        function test_dismissal_reconciles_the_refusal_data() {
            return [{tag: "delete"}, {tag: "save"}];
        }
        function test_dismissal_reconciles_the_refusal(data) {
            var text = "Can't play right now. Another app has the audio, such as a phone call.";
            var a = rec("t-1", pathA, 12000);
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([a, b]);
            var status = playlistStatusComponent.createObject(sheet().parent);
            var bar = nowPlayingBarComponent.createObject(sheet().parent, {status: status, visible: false, width: 360});
            var generation = playPlaylist([itemOf(a, 0, 1, 2, uid), itemOf(b, 0, 2, 2, uid)]);
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 4, playbackDurationMs: 12000});
            openRow(uid);
            var player = item("historyRecording");
            var said = player.announcements;
            if (data.tag === "delete") {
                tap("recordingDelete");
                verify(item("recordingDeleteConfirm").visible);
            } else {
                recordings.setSelection(1, 96000, [pathA]);
                startSave();
            }
            testContext.publishPlaybackReading({item: 1, playbackState: 3, playbackToken: 5, playbackDurationMs: 8000});
            testContext.publishPlaybackReading({playbackPausedReason: "unavailable", playbackRefusals: 1});
            compare(player.currentPath, pathA, "the dialog holds the sheet on part 1");
            compare(item("recordingNotice").text, "", "nothing said for a part not shown");
            if (data.tag === "delete")
                findChild(item("recordingDeleteConfirm"), "confirmCancelButton").activate();
            else
                findChild(player, "recordingSavePicker").cancel();
            compare(player.currentPath, pathB, "dismissed: the sheet follows to part 2");
            tryCompare(item("recordingNotice"), "text", text, 1000, "the refusal is shown");
            compare(player.announcements, said + 1, "and read once");
            compare(Ui.Navigation.refusalSaid, Ui.Navigation.refusalKey(generation, 1), "through the shared key");
            sheet().visible = false;
            bar.visible = true;
            tryVerify(function () { return status.eventHeard; }, 2000, "the bar takes the refusal as heard");
            compare(bar.announcements, 0, "the bar stays silent");
            bar.destroy();
            status.destroy();
        }

        // A refusal standing through a move inside the call (Previous from part 2 to part 1: opening, then prepared,
        // still "unavailable") is shown on, and read out once, as the bar has it.
        function test_standing_refusal_said_once_across_a_move() {
            var a = rec("t-1", pathA, 12000);
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([a, b]);
            playPlaylist([itemOf(a, 0, 1, 2, uid), itemOf(b, 0, 2, 2, uid)]);
            testContext.publishPlaybackReading({item: 1, playbackState: 3, playbackToken: 4, playbackPausedReason: "unavailable", playbackRefusals: 1, playbackDurationMs: 8000});
            var player = item("historyRecording");
            var said = player.announcements;
            openRow(uid);
            var text = "Can't play right now. Another app has the audio, such as a phone call.";
            compare(item("recordingNotice").text, text);
            compare(player.announcements, said + 1, "said as it opens");
            testContext.publishPlaybackReading({item: 0, playbackState: 1, playbackToken: 5});
            compare(checkedParts(2), [0], "on to part 1");
            testContext.publishPlaybackReading({playbackState: 3});
            compare(item("recordingNotice").text, text, "still shown");
            compare(player.announcements, said + 1, "not read again");
            testContext.publishPlaybackReading({playbackRefusals: 2});
            compare(player.announcements, said + 2, "a new refusal is");
        }

        // A Play pressed on this sheet while a playlist plays another call: the playlist moving on to its next part
        // during the press is not this play's target, so the play is sent.
        function test_play_press_survives_another_playlist_moving() {
            var c = rec("t-9", pathC, 4000);
            var other = callHistory.pushWithRecordings([c]);
            openWith([rec("t-1", pathA, 12000)]);
            playPlaylist([itemOf(c, 0, 1, 2, other), itemOf(rec("t-8", "/rec/County (12345678)/D.wav", 4000), 0, 2, 2, other)]);
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 3});
            var button = item("recordingPlayButton");
            verify(!button.showPause, "nothing plays here");
            mousePress(button, button.width / 2, button.height / 2);
            testContext.publishPlaybackReading({item: 1, playbackToken: 4});
            mouseRelease(button, button.width / 2, button.height / 2);
            compare(lastHostCall().name, "playRecordingOwned", "the sheet's play is sent");
            compare(lastHostCall().path, pathA);
        }

        // A Play tap acts on the part shown at the press: a store reread that put another recording (another size)
        // under the same id and path during the press is another part, so the tap is dropped; a press begun on it
        // plays it.
        function test_play_press_checks_the_shown_identity() {
            var uid = openWith([rec("t-1", pathA, 12000)]);
            var button = item("recordingPlayButton");
            mousePress(button, button.width / 2, button.height / 2);
            callHistory.setRowRecordings(uid, [rec("t-1", pathA, 13000)]);
            compare(item("historyRecording").part.identitySize, 44 + 13000 * 16, "the reread recording shows");
            mouseRelease(button, button.width / 2, button.height / 2);
            compare(hostCallsNamed("playRecordingOwned").length, 0, "the recording pressed on is gone: nothing played");
            tap("recordingPlayButton");
            compare(hostCallsNamed("playRecordingOwned").length, 1, "a press begun on the shown one plays it");
            compare(lastHostCall().path, pathA);
        }

        // Issue #909 (R42): a part chip's tap acts only on the recording, and the action, it was pressed on. A reread
        // that puts another recording at the pressed place (the count unchanged), or storage coming back that would
        // turn a select into a play, drops the release; an unchanged press acts as before.
        function test_part_chip_press_keeps_its_recording() {
            var uid = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            var chip = item("recordingPart1");
            mousePress(chip, chip.width / 2, chip.height / 2);
            verify(callHistory.setRowRecordings(uid, [rec("t-1", pathA, 12000), rec("t-3", pathC, 4000)]));
            compare(item("historyRecording").parts[1].path, pathC, "another recording at the pressed place");
            mouseRelease(chip, chip.width / 2, chip.height / 2);
            compare(hostCallsNamed("playRecordingOwned").length, 0, "the recording pressed on is gone: nothing plays");
            compare(checkedParts(2), [0], "nor is it shown");
            tap("recordingPart1");
            compare(hostCallsNamed("playRecordingOwned").length, 1, "a press begun on the one shown plays it");
            compare(lastHostCall().path, pathC);

            // Select-only at the press (its storage away), playable at the release: dropped.
            var away = openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000, {available: false})]);
            var plays = hostCallsNamed("playRecordingOwned").length;
            chip = item("recordingPart1");
            mousePress(chip, chip.width / 2, chip.height / 2);
            verify(callHistory.setRowRecordings(away, [rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]));
            mouseRelease(chip, chip.width / 2, chip.height / 2);
            compare(hostCallsNamed("playRecordingOwned").length, plays, "a select would have become a play: dropped");
            compare(checkedParts(2), [0]);
            mouseClick(chip, chip.width / 2, chip.height / 2);
            compare(hostCallsNamed("playRecordingOwned").length, plays + 1, "an unchanged press acts as before");
            compare(lastHostCall().path, pathB);
            compare(checkedParts(2), [1]);
        }

        // A keyboard or screen reader activation while a pointer holds the button acts on the state then, and
        // leaves the held press alone: Pause held, Pause activated, the paused reading in, then the release: it is
        // dropped (it began on Pause), never taken for a Resume.
        function test_activation_leaves_a_held_press() {
            openWith([rec("t-1", pathA, 12000)]);
            tap("recordingPlayButton");
            testContext.publishPlaybackReading({playbackState: 2, playbackToken: 3, playbackDurationMs: 12000});
            var button = item("recordingPlayButton");
            verify(button.showPause);
            mousePress(button, button.width / 2, button.height / 2);
            button.activate();
            compare(hostCallsNamed("pausePlaybackFor").length, 1, "the activation pauses");
            verify(!button.showPause, "paused");
            mouseRelease(button, button.width / 2, button.height / 2);
            compare(hostCallsNamed("resumePlaybackFor").length, 0, "the held Pause is not a Resume");
            compare(hostCallsNamed("pausePlaybackFor").length, 1, "nor a second Pause");
            tap("recordingPlayButton");
            compare(hostCallsNamed("resumePlaybackFor").length, 1, "a press begun now resumes");
        }

        // A part of the sheet's own Play all parts skipped (its prepare never answered) is said on the notice line once,
        // on the part that played after it, in the bar's words; a later skip says the new count. A playlist's skips
        // are the bar's to say, not a sheet's that follows it.
        function test_parts_skip_said_on_the_part_after() {
            var pathD = "/rec/County (12345678)/D.wav";
            openWith([rec("t-1", pathA, 4000), rec("t-2", pathB, 4000), rec("t-3", pathC, 4000), rec("t-4", pathD, 4000)]);
            var player = item("historyRecording");
            tap("recordingPlayAll");
            var said = player.announcements;
            testContext.publishPlaybackReading({item: 1, playbackState: 1, playbackToken: 5, playbackSkipped: 1});
            verify(!item("recordingNotice").visible, "nothing while part 2 opens");
            testContext.publishPlaybackReading({item: 1, playbackState: 2, playbackSkipNoticeCall: 0, playbackSkipNoticeCount: 1});
            compare(item("recordingNotice").text, "Skipped 1 recording that couldn't be played");
            compare(player.announcements, said + 1, "read out");
            testContext.publishPlaybackReading({playbackPositionMs: 1500});
            testContext.publishPlaybackReading({item: 2, playbackState: 1, playbackToken: 6, playbackPositionMs: 0, playbackSkipped: 2});
            testContext.publishPlaybackReading({playbackState: 3, playbackPausedReason: "user"});
            compare(player.announcements, said + 1, "said once, not on a tick, an open or a pause");
            testContext.publishPlaybackReading({item: 3, playbackState: 2, playbackToken: 7, playbackPausedReason: "", playbackSkipNoticeCount: 2});
            compare(item("recordingNotice").text, "Skipped 2 recordings that couldn't be played", "a later skip: the new count");
            compare(player.announcements, said + 2);
            sheet().visible = false;

            var c = rec("t-9", pathC, 4000);
            var uid = callHistory.pushWithRecordings([c]);
            playPlaylist([itemOf(c, 0, 1, 1, uid)]);
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 8, playbackSkipped: 1, playbackSkipNoticeCall: 0, playbackSkipNoticeCount: 1});
            openRow(uid);
            testContext.publishPlaybackReading({playbackPositionMs: 500});
            verify(!item("recordingNotice").visible, "a playlist's skips are the bar's");
        }

        // Skips no part played after are said when the sheet's Play all parts ends: the last part hangs and the play
        // finishes, so the end's skip count is said once; one already said is not said again. A play whose every part
        // was skipped says the skips as its failure.
        function test_parts_skip_said_at_the_end() {
            openWith([rec("t-1", pathA, 4000), rec("t-2", pathB, 4000), rec("t-3", pathC, 4000)]);
            var player = item("historyRecording");
            tap("recordingPlayAll");
            var said = player.announcements;
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 5});
            testContext.publishPlaybackReading({item: 1, playbackState: 2, playbackToken: 6});
            testContext.publishPlaybackReading({item: 2, playbackState: 1, playbackToken: 7});
            verify(!item("recordingNotice").visible, "nothing while part 3 opens");
            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished", playbackSkipped: 1});
            compare(item("recordingNotice").text, "Skipped 1 recording that couldn't be played", "part 3 hung: said at the end");
            compare(player.announcements, said + 1, "read out");
            testContext.publishPlaybackReading({playbackPositionMs: 4000});
            compare(player.announcements, said + 1, "once");
            sheet().visible = false;

            openWith([rec("t-4", pathA, 4000), rec("t-5", pathB, 4000), rec("t-6", pathC, 4000)]);
            player = item("historyRecording");
            tap("recordingPlayAll");
            said = player.announcements;
            testContext.publishPlaybackReading({item: 1, playbackState: 2, playbackToken: 8, playbackSkipped: 1, playbackSkipNoticeCall: 0, playbackSkipNoticeCount: 1});
            compare(player.announcements, said + 1, "part 1 skipped: said on part 2");
            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
            compare(player.announcements, said + 1, "the end has no skip not said yet: nothing more");
            sheet().visible = false;

            openWith([rec("t-7", pathA, 4000), rec("t-8", pathB, 4000)]);
            player = item("historyRecording");
            tap("recordingPlayAll");
            said = player.announcements;
            testContext.publishPlaybackReading({item: 1, playbackState: 4, playbackError: "failed", playbackEndedReason: "unplayable", playbackSkipped: 2});
            compare(item("recordingNotice").text, "Skipped 2 recordings that couldn't be played", "every part skipped");
            compare(player.announcements, said + 1, "said once, as its failure");
        }

        // Share, Save to file and Delete pressed on part 1 while the sheet follows the play to part 2 before the
        // release: the tap is dropped, never taken for part 2. A keyboard or screen reader activation acts on the part
        // shown then, and a press begun on part 2 acts on it.
        function test_file_action_press_keeps_its_part_data() {
            return [{tag: "share", name: "recordingShare"}, {tag: "save", name: "recordingSave"}, {tag: "delete", name: "recordingDelete"}];
        }
        function test_file_action_press_keeps_its_part(data) {
            testContext.setHostReading("shareSupported", true);
            openWith([rec("t-1", pathA, 12000), rec("t-2", pathB, 8000)]);
            recordings.setSelection(1, 96000, [pathB]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 1});
            compare(checkedParts(2), [0]);
            var player = item("historyRecording");
            var picker = findChild(player, "recordingSavePicker");
            var confirm = item("recordingDeleteConfirm");
            var acted = function () {
                return data.tag === "share" ? hostCallsNamed("shareRecording").length
                    : data.tag === "save" ? recordings.calls().filter(function (call) { return call.name === "selectRecordings"; }).length
                    : (confirm.visible ? 1 : 0);
            };
            var button = item(data.name);
            mousePress(button, button.width / 2, button.height / 2);
            testContext.publishPlaybackReading({item: 1, playbackToken: 2});
            compare(checkedParts(2), [1], "the sheet followed to part 2 while the press was held");
            mouseRelease(button, button.width / 2, button.height / 2);
            compare(acted(), 0, data.tag + ": the press began on part 1, so nothing is done to part 2");
            verify(!picker.visible && !confirm.visible && player.saveSelection === 0, "no picker, no confirmation");

            button.activate();
            compare(acted(), 1, data.tag + ": an activation acts on the part shown");
            if (data.tag === "share") {
                compare(lastHostCall().path, pathB);
            } else if (data.tag === "save") {
                compare(lastStoreCall().ids, ["t-2"]);
                picker.cancel();
                compare(player.saveSelection, 0);
            } else {
                compare(confirm.target.path, pathB);
                findChild(confirm, "confirmCancelButton").activate();
            }
            testContext.publishPlaybackReading({item: 0, playbackToken: 3});
            compare(checkedParts(2), [0], "back on part 1");
            mouseClick(button, button.width / 2, button.height / 2);
            // Delete counts its confirmation out; the others their calls.
            compare(acted(), data.tag === "delete" ? 1 : 2, data.tag + ": a press begun and ended on part 1 acts on it");
            if (data.tag === "share")
                compare(lastHostCall().path, pathA);
            else if (data.tag === "save")
                compare(lastStoreCall().ids, ["t-1"]);
            else
                compare(confirm.target.path, pathA);
        }

        // A drag's preview belongs to the part it began on: the play moving to the next part mid-drag shows that
        // part's own position, a keyboard or screen reader step seeks from it (never from the drag's preview), and the
        // release seeks nothing on the new part.
        function test_seek_preview_stays_on_its_part() {
            openWith([rec("t-1", pathA, 10000), rec("t-2", pathB, 10000)]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 5, playbackPositionMs: 1000, playbackDurationMs: 10000, playbackCanSeek: true});
            var player = item("historyRecording");
            var bar = item("recordingSeekBar");
            var y = bar.height / 2;
            mousePress(bar, bar.width * 0.2, y);
            for (var i = 1; i <= 8; ++i)
                mouseMove(bar, bar.width * (0.2 + i * 0.075), y);
            verify(bar.dragging, "a drag is under way");
            verify(player.positionMs > 7000, "it previews where it would land: " + player.positionMs);
            bar.nudge(5000);
            compare(lastHostCall().token, 5);
            compare(lastHostCall().ms, 6000, "a step during the drag is from the part's own position, not the preview");
            testContext.publishPlaybackReading({item: 1, playbackToken: 6, playbackPositionMs: 2000});
            compare(player.positionMs, 2000, "part 2 shows its own position, not part 1's preview");
            compare(item("recordingTime").text, "0:02 / 0:10");
            bar.nudge(5000);
            compare(lastHostCall().name, "seekPlaybackFor");
            compare(lastHostCall().token, 6, "the step is part 2's");
            compare(lastHostCall().ms, 7000, "from part 2's own position");
            mouseMove(bar, bar.width * 0.9, y);
            verify(Math.abs(player.positionMs - 7000) < 1, "a moving drag does not bring the preview back: " + player.positionMs);
            mouseRelease(bar, bar.width * 0.9, y);
            compare(lastHostCall().token, 5, "the release names part 1, which the host drops");
        }

        // A sheet's own Play all parts offers Pause and seek only once the host has published its first part's
        // token: before that a pause would be the queue's, and could reach the next part (the first already done).
        function test_parts_wait_for_a_token() {
            openWith([rec("t-1", pathA, 4000), rec("t-2", pathB, 4000)]);
            tap("recordingPlayAll");
            var player = item("historyRecording");
            verify(player.ownsPlay && player.wantsPlay, "its own parts, opening");
            testContext.publishPlaybackReading({playbackCanSeek: true, playbackDurationMs: 4000});
            verify(!item("recordingPlayButton").enabled, "no Pause before the token");
            verify(!item("recordingSeekBar").seekable, "no seek before the token");
            player.toggle();
            compare(hostCallsNamed("pausePlayback").length + hostCallsNamed("pausePlaybackFor").length, 0, "nothing sent");
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 5});
            verify(item("recordingPlayButton").enabled && item("recordingSeekBar").seekable, "offered with the token");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "pausePlaybackFor");
            compare(lastHostCall().token, 5);
        }

        // A sheet closed (or opened again for another call) with its delete confirmation out ends that confirmation
        // and forgets its file: it never comes back over another call's sheet, where Delete would take the first
        // call's recording.
        function test_delete_confirmation_ends_with_the_sheet() {
            openWith([rec("t-1", pathA, 12000)]);
            press("recordingDelete");
            var confirm = item("recordingDeleteConfirm");
            verify(confirm.visible && confirm.target && confirm.target.path === pathA, "asked for A");
            sheet().visible = false;
            openWith([rec("t-2", pathB, 8000)]);
            verify(!confirm.visible, "no confirmation over B");
            compare(confirm.target, null, "A's file forgotten");
            confirm.confirm();
            compare(recordings.calls().filter(function (call) { return call.name === "deleteRecordingChecked"; }).length, 0, "nothing deleted");
            press("recordingDelete");
            verify(confirm.visible && confirm.target.path === pathB, "Delete on B asks for B");
            sheet().open({uid: callHistory.pushWithRecordings([rec("t-3", pathC, 4000)]), name: "Dispatch", tg: 1001, when: 1700000000, systemUid: "test-system"});
            verify(!confirm.visible && confirm.target === null, "opened again for another call: ended too");
        }

        // Previous opens the part shown again under a new token (a player is never reused): the sheet's own Play all
        // parts stays its own on the same chip, a seek waiting for its landing is forgotten, a Pause pressed on the
        // pass before is dropped, and a fresh one carries the new token.
        function test_a_reopen_moves_the_token_on_the_same_part() {
            openWith([rec("t-1", pathA, 10000), rec("t-2", pathB, 10000)]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 5, playbackPositionMs: 4000, playbackDurationMs: 10000, playbackCanSeek: true});
            var player = item("historyRecording");
            var generation = player.ownedGeneration;
            testContext.setSeekApplied(false); // the player's record lands a seek later, as on Android
            item("recordingSeekBar").seekTo(8000, 5);
            testContext.setSeekApplied(true);
            compare(player.seekTargetMs, 8000, "a seek waits for its landing");
            var button = item("recordingPlayButton");
            mousePress(button, button.width / 2, button.height / 2);
            // The poll after the reopen finds the new pass already playing (it missed the opening reading).
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 6, playbackPositionMs: 0, playbackCanSeek: true});
            mouseRelease(button, button.width / 2, button.height / 2);
            compare(hostCallsNamed("pausePlaybackFor").length, 0, "the Pause pressed on the pass before is dropped");
            verify(player.ownsPlay && player.ownedGeneration === generation, "still this sheet's own play");
            compare(checkedParts(2), [0], "on the same chip");
            compare(player.seekTargetMs, -1, "the waiting seek is forgotten");
            compare(player.positionMs, 0, "the position is the new pass's");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "pausePlaybackFor");
            compare(lastHostCall().token, 6, "a fresh press carries the new token");
        }

        // While a seek waits for its landing, the bar shows where it goes (the Android host shows nothing until the
        // player's record); the record that lands it, a seek the host can no longer take, or another recording falls
        // back on the host's position.
        function test_seek_target_shown_until_it_lands() {
            openWith([rec("t-1", pathA, 10000), rec("t-2", pathB, 10000)]);
            tap("recordingPlayAll");
            testContext.publishPlaybackReading({item: 0, playbackState: 3, playbackToken: 5, playbackPositionMs: 1000, playbackDurationMs: 10000, playbackCanSeek: true});
            var player = item("historyRecording");
            var bar = item("recordingSeekBar");
            testContext.setSeekApplied(false);
            bar.seekTo(6000, 5);
            compare(decoderHost.playbackPositionMs, 1000, "the host shows nothing");
            compare(player.positionMs, 6000, "the target shows");
            compare(item("recordingTime").text, "0:06 / 0:10");
            testContext.publishPlaybackReading({playbackPositionMs: 1000});
            compare(player.positionMs, 6000, "a record from before the seek leaves it");
            testContext.publishPlaybackReading({playbackPositionMs: 6000, playbackSeeksLanded: 1});
            compare(player.seekTargetMs, -1, "landed");
            compare(player.positionMs, 6000);
            bar.seekTo(9000, 5);
            compare(player.positionMs, 9000);
            testContext.publishPlaybackReading({playbackCanSeek: false});
            compare(player.positionMs, 6000, "no seek any more: the host's position");
            testContext.publishPlaybackReading({playbackCanSeek: true});
            bar.seekTo(2000, 5);
            testContext.publishPlaybackReading({item: 1, playbackToken: 6, playbackPositionMs: 0});
            compare(player.positionMs, 0, "another recording: the host's position");
            bar.seekTo(3000, 6);
            compare(player.positionMs, 3000);
            tryCompare(player, "seekTargetMs", -1, 3000, "a landing that never comes is forgotten");
            compare(player.positionMs, 0);
            testContext.setSeekApplied(true);
        }

        // Asked again and refused again with the same reading, the refusal is said again.
        function test_repeated_refusal_said_again() {
            var b = rec("t-2", pathB, 8000);
            var uid = callHistory.pushWithRecordings([b]);
            playPlaylist([itemOf(b, 0, 1, 1, uid)]);
            testContext.publishPlaybackReading({playbackState: 3, playbackToken: 4, playbackPausedReason: "user", playbackDurationMs: 8000});
            openRow(uid);
            testContext.setResumeRefused(true);
            var player = item("historyRecording");
            var said = player.announcements;
            tap("recordingPlayButton");
            compare(player.announcements, said + 1, "refused");
            testContext.publishPlaybackReading({playbackPositionMs: 0});
            compare(player.announcements, said + 1, "a tick says nothing");
            tap("recordingPlayButton");
            compare(lastHostCall().name, "resumePlaybackFor");
            compare(player.announcements, said + 2, "refused again: said again");
            compare(item("recordingNotice").text, "Can't play right now. Another app has the audio, such as a phone call.");
        }
    }
}
