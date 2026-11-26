// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #909: History's Play all, the now-playing bar at the bottom of both History screens, and the playing row's
// marker. Both screens (the idle tab, and the session tools' one without a title) read one PlaylistStatus, as Main's;
// the decoderHost map plays the host's queue (qml_test_context.h) and `historyPlaylist` answers each press as a case
// sets it. Names and paths are made up.
Item {
    id: root

    width: 360
    height: 1280

    Ui.PlaylistStatus {
        id: status
    }

    Loader {
        id: idleLoader

        width: root.width
        height: 640
        source: uiDir + "/HistoryScreen.qml"
        onLoaded: {
            item.systems = clockStub;
            item.playlistStatus = status;
        }
    }

    Loader {
        id: sessionLoader

        y: 640
        width: root.width
        height: 640
        source: uiDir + "/HistoryScreen.qml"
        onLoaded: {
            item.showTitle = false;
            item.systems = clockStub;
            item.playlistStatus = status;
        }
    }

    QtObject {
        id: clockStub
        function realtimeNowMs() { return new Date(2026, 10, 9, 12, 0, 0).getTime(); }
    }

    TestCase {
        name: "HistoryPlaylist"
        when: windowShown

        property int appSpeechPeriod: 0
        function initTestCase() {
            appSpeechPeriod = Ui.Navigation.speechPeriodMs;
            Ui.Navigation.speechPeriodMs = 50;
            Ui.Navigation.speechStepFloorMs = 1;
        }
        function cleanupTestCase() {
            Ui.Navigation.speechPeriodMs = appSpeechPeriod;
            Ui.Navigation.speechStepFloorMs = 1000;
            testContext.resetHostPlayback();
            testContext.setPlaybackSupported(false);
        }

        function screens() {
            return [idleLoader.item, sessionLoader.item];
        }
        function idle() {
            return idleLoader.item;
        }
        function bar(screen) {
            return findChild(screen, "nowPlayingBar");
        }
        function part(screen, name) {
            var found = findChild(bar(screen), name);
            verify(found !== null, name);
            return found;
        }
        // The line under the name, its parts' no-break spaces read as spaces.
        function line(screen) {
            return part(screen, "nowPlayingLine").text.replace(/\u00a0/g, " ");
        }
        function hostCallsNamed(name) {
            return testContext.hostCalls().filter(function (call) {
                return call.name === name;
            });
        }
        function rectIn(item, ancestor) {
            var p = item.mapToItem(ancestor, 0, 0);
            return {x: p.x, y: p.y, width: item.width, height: item.height};
        }
        function overlaps(a, b) {
            return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
        }
        function describe(r) {
            return "(" + r.x.toFixed(1) + ", " + r.y.toFixed(1) + " " + r.width.toFixed(1) + "x" + r.height.toFixed(1) + ")";
        }

        // @p count calls with a recording each, oldest first in the result: [{uid, name}].
        function logCalls(count) {
            var calls = [];
            for (var i = 0; i < count; i++) {
                var uid = callHistory.pushWithRecordings([{id: "r" + i, path: "/rec/call-" + i + ".wav", fileName: "call-" + i + ".wav", folderUid: "u1",
                    start: 1700000000 + i, durationMs: 12000, rate: 8000, bytes: 96000, failed: false, available: true,
                    identitySize: 192044, mtimeSec: 1700000100, mtimeNanos: 0}]);
                calls.push({uid: uid, name: callHistory.recordFor(uid).name});
            }
            return calls;
        }
        // The playlist items of @p calls, one part each unless @p parts says how many a call has.
        function itemsFor(calls, parts) {
            var items = [];
            for (var i = 0; i < calls.length; i++) {
                var count = parts ? parts[i] : 1;
                for (var p = 1; p <= count; p++)
                    items.push({path: "/rec/call-" + i + "-" + p + ".wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: i,
                        partOrdinal: p, partCount: count, title: calls[i].name, line: "2026-11-09 12:0" + i, key: String(calls[i].uid)});
            }
            return items;
        }
        // Play all from @p screen with @p calls as the playlist: its generation.
        function playAll(screen, calls, parts) {
            testContext.setHistoryPlaylistAnswer("accepted", itemsFor(calls, parts));
            var button = findChild(screen, "historyPlayAll");
            verify(button.visible && button.enabled, "Play all can be pressed");
            button.activate();
            verify(bar(screen).visible, "the bar shows the playlist");
            return decoderHost.playbackGeneration;
        }
        // Only @p screen shows, as in the app (one History screen at a time).
        function only(screen) {
            for (var s of screens())
                s.visible = s === screen;
        }
        function playing(extra) {
            var reading = {playbackState: 2, playbackToken: 7, playbackDurationMs: 31000, playbackPositionMs: 12000, playbackCanSeek: true};
            for (var key in (extra || {}))
                reading[key] = extra[key];
            testContext.publishPlaybackReading(reading);
        }

        function init() {
            verify(idle() !== null && sessionLoader.item !== null, "both History screens load");
            testContext.setPlaybackSupported(true);
            testContext.resetHostPlayback();
            historyExport.reset();
            historyView.filterText = "";
            historyView.filterSystem = "";
            historyView.filterKind = 0;
            callHistory.clearAll();
            status.forget();
            status.appActive = true;
            for (var screen of screens()) {
                screen.visible = true;
                bar(screen).announcements = 0;
                bar(screen).announced = "";
            }
            wait(0);
        }
        function cleanup() {
            for (var screen of screens()) {
                screen.visible = true;
                screen.exportMessage = {text: "", error: false, heard: false};
                for (var name of ["historyDetailSheet", "clearHistoryConfirm", "historyFilterChoices"]) {
                    var sheet = findChild(screen, name);
                    if (sheet)
                        sheet.visible = false;
                }
                bar(screen).notice = "";
            }
            status.appActive = true;
            decoderHost.keyboardTop = -1;
            // A field left with the keyboard would take no new focus in the next case, and reveal nothing.
            root.forceActiveFocus();
            Ui.Theme.resetFontScale();
            testContext.resetHostPlayback();
            status.forget();
            // Both lists shown again build their rows before the next case clears the log under them.
            for (screen of screens())
                verify(waitForPolish(findChild(screen, "callLogList")));
            waitForRendering(root);
        }

        // Play all sits beside Map, unframed as Map is, on Export's rule and only where the host has a player; held
        // while the list shows no row.
        function test_play_all_offered() {
            for (var screen of screens())
                verify(!findChild(screen, "historyPlayAll").visible, "an empty log offers nothing to play");
            logCalls(2);
            for (screen of screens()) {
                var button = findChild(screen, "historyPlayAll");
                verify(button.visible && button.enabled);
                compare(button.framed, false);
                compare(button.shape, "play");
                compare(button.Accessible.name, "Play all");
                compare(button.Accessible.description, "Plays the recorded calls shown, oldest first");
                var map = findChild(screen, "historyMapButton");
                verify(button.x > map.x && Math.abs(button.y - map.y) < 0.5, "beside Map");
            }
            historyView.filterText = "no call matches this";
            for (screen of screens())
                verify(findChild(screen, "historyPlayAll").visible && !findChild(screen, "historyPlayAll").enabled, "nothing shown: held");
            historyView.filterText = "";
            testContext.setPlaybackSupported(false);
            try {
                for (screen of screens()) {
                    verify(!findChild(screen, "historyPlayAll").visible, "no player: not offered");
                    verify(!bar(screen).visible);
                }
            } finally {
                testContext.setPlaybackSupported(true);
            }
        }

        // Accepted: the search still being typed is part of what plays, the header's line is cleared, and the bar
        // shows the playlist opening; nothing is said until audio starts.
        function test_play_all_accepted() {
            var calls = logCalls(3);
            var screen = idle();
            screen.setExportMessage("Exported 3 entries.", false);
            findChild(screen, "historySearch").text = "CALL";
            var generation = playAll(screen, calls);
            var asked = testContext.historyPlaylistCalls();
            compare(asked.length, 1);
            compare(asked[0].startUid, 0, "from the oldest shown");
            compare(asked[0].filterText, "CALL", "the typed search went first");
            compare(findChild(screen, "historyExportMessage").text, "", "the press replaced the export's line");
            verify(generation > 0);
            compare(part(screen, "nowPlayingTitle").text, calls[0].name);
            compare(line(screen), "Opening…");
            compare(status.generation, generation);
            verify(bar(sessionLoader.item).visible, "both History screens show it");
            only(screen);
            wait(20);
            compare(bar(screen).announcements, 0, "nothing said before audio starts");
            playing({item: 0});
            tryCompare(bar(screen), "announced", "Playing 3 calls");
            compare(bar(screen).announcements, 1);
            compare(bar(sessionLoader.item).announcements, 0, "read once, by one bar");
            playing({playbackPositionMs: 13000});
            wait(20);
            compare(bar(screen).announcements + bar(sessionLoader.item).announcements, 1, "never on a tick");
        }

        // Refused at the press: the reason on the header's message line (Export's, the newer replacing), and what
        // played plays on.
        function test_play_all_refused_data() {
            return [{tag: "none", result: "none", text: "None of the shown calls has a recording that can be played now.", error: false},
                    {tag: "unsupported", result: "unsupported", text: "Recordings can't be played on this device.", error: true},
                    {tag: "refused", result: "refused", text: "Couldn't start playing. Try again.", error: true}];
        }
        function test_play_all_refused(data) {
            logCalls(2);
            var screen = sessionLoader.item;
            only(screen);
            decoderHost.playRecordingOwned("/rec/other.wav");
            var before = decoderHost.playbackGeneration;
            screen.setExportMessage("Exported 2 entries.", false);
            testContext.setHistoryPlaylistAnswer(data.result, []);
            findChild(screen, "historyPlayAll").activate();
            var message = findChild(screen, "historyExportMessage");
            compare(message.text, data.text);
            compare(screen.exportMessage.error, data.error);
            compare(decoderHost.playbackGeneration, before, "what played plays on");
            compare(hostCallsNamed("stopPlayback").length, 0);
            verify(!bar(screen).visible, "no playlist, no bar");
            tryCompare(screen, "announced", data.text);
        }

        // What the bar says while it plays: the call's name over where it is, or what it is doing in place of the
        // time.
        function test_bar_lines_data() {
            return [{tag: "playing", reading: {}, line: "Call 1 of 3 · 0:12 / 0:31"},
                    {tag: "at the end", reading: {playbackPositionMs: 31000}, line: "Call 1 of 3 · 0:31 / 0:31"},
                    {tag: "second call", reading: {item: 1}, line: "Call 2 of 3 · 0:12 / 0:31"},
                    {tag: "part", parts: [1, 3, 1], reading: {item: 2}, line: "Call 2 of 3 · part 2 of 3 · 0:12 / 0:31"},
                    {tag: "paused", reading: {playbackState: 3, playbackPausedReason: "user"}, line: "Paused", pause: false},
                    {tag: "headphones", reading: {playbackState: 3, playbackPausedReason: "noisy"}, line: "Paused: headphones unplugged", pause: false},
                    {tag: "interrupted", reading: {playbackState: 3, playbackInterrupted: true, playbackPausedReason: "focus"}, line: "Paused for another app's audio", pause: true},
                    {tag: "refused resume", reading: {playbackState: 3, playbackPausedReason: "unavailable"}, line: "Paused: another app has the audio.", pause: false},
                    {tag: "opening", reading: {playbackState: 1}, line: "Opening…", pause: true}];
        }
        function test_bar_lines(data) {
            var calls = logCalls(3);
            playAll(idle(), calls, data.parts);
            playing(data.reading);
            for (var screen of screens()) {
                compare(line(screen), data.line);
                compare(part(screen, "nowPlayingTitle").text, data.tag === "second call" || data.tag === "part" ? calls[1].name : calls[0].name);
                var play = part(screen, "nowPlayingPlay");
                var pauses = data.pause === undefined ? true : data.pause;
                compare(play.showPause, pauses);
                compare(play.Accessible.name, pauses ? "Pause" : "Resume");
            }
        }

        // Play from here: the sheet closes on accepted and the bar shows the new playlist opening; a failure of
        // that playlist afterwards (the service refused) is the bar's, said once.
        function test_play_from_here_shows_on_the_bar() {
            var calls = logCalls(3);
            var screen = idle();
            only(screen);
            testContext.setHistoryPlaylistAnswer("accepted", itemsFor(calls.slice(1)));
            var sheet = findChild(screen, "historyDetailSheet");
            sheet.open(callHistory.recordFor(calls[1].uid));
            var button = findChild(sheet, "historyPlayFromHere");
            verify(button.visible);
            button.activate();
            verify(!sheet.visible, "the sheet closes");
            compare(testContext.historyPlaylistCalls()[0].startUid, calls[1].uid);
            verify(bar(screen).visible);
            compare(line(screen), "Opening…");
            compare(part(screen, "nowPlayingTitle").text, calls[1].name);
            testContext.publishPlaybackReading({playbackState: 4, playbackError: "failed", playbackEndedReason: "start"});
            compare(part(screen, "nowPlayingResult").text, "Couldn't start playing. Try again.");
            tryCompare(bar(screen), "announced", "Couldn't start playing. Try again.");
            compare(bar(screen).announcements, 1, "no start was said: audio never started");
        }

        // A skip is said on the call after it, singular and plural, as one sentence for any reason.
        function test_skip_said_on_the_next_call() {
            var calls = logCalls(4);
            playAll(idle(), calls);
            playing({item: 0});
            playing({item: 1, playbackSkipped: 1});
            compare(line(idle()), "Skipped 1 recording that couldn't be played");
            playing({item: 2});
            compare(line(idle()), "Call 3 of 4 · 0:12 / 0:31", "only on the call after the skip");
            playing({item: 3, playbackSkipped: 3});
            compare(line(idle()), "Skipped 2 recordings that couldn't be played");
        }

        // Previous, Pause/Resume and Next drive the whole playlist; Next is not offered at the last call.
        function test_bar_controls() {
            var calls = logCalls(2);
            var screen = sessionLoader.item;
            playAll(screen, calls);
            playing({item: 0});
            var previous = part(screen, "nowPlayingPrevious"), next = part(screen, "nowPlayingNext");
            compare(previous.Accessible.name, "Previous call");
            compare(next.Accessible.name, "Next call");
            compare(part(screen, "nowPlayingClose").Accessible.name, "Stop playing");
            next.activate();
            compare(hostCallsNamed("nextPlayback").length, 1);
            previous.activate();
            compare(hostCallsNamed("previousPlayback").length, 1);
            part(screen, "nowPlayingPlay").activate();
            compare(hostCallsNamed("pausePlayback").length, 1);
            compare(decoderHost.playbackState, 3);
            compare(part(screen, "nowPlayingPlay").Accessible.name, "Resume");
            part(screen, "nowPlayingPlay").activate();
            compare(hostCallsNamed("resumePlayback").length, 1);
            playing({item: 1});
            verify(!next.enabled, "the last call: no Next");
            verify(previous.enabled);
        }

        // ✕ while it plays stops this playlist, and only it; the bar goes with it.
        function test_close_stops_its_playlist() {
            var calls = logCalls(2);
            var generation = playAll(idle(), calls);
            playing({item: 0});
            part(idle(), "nowPlayingClose").activate();
            var stops = hostCallsNamed("stopPlaybackGeneration");
            compare(stops.length, 1);
            compare(stops[0].generation, generation);
            compare(hostCallsNamed("stopPlayback").length, 0);
            for (var screen of screens())
                verify(!bar(screen).visible, "a Stop in the app closes the bar");
        }

        // Once it ends the bar keeps how it ended until ✕ or the next playlist; ✕ then only dismisses it.
        function test_ended_bar_data() {
            return [{tag: "finished", reading: {playbackState: 0, playbackEndedReason: "finished"}, text: "Finished · 3 calls"},
                    {tag: "finished skipped", reading: {playbackState: 0, playbackEndedReason: "finished", playbackSkipped: 2}, text: "Finished · 3 calls · 2 skipped"},
                    {tag: "finished one", calls: 1, reading: {playbackState: 0, playbackEndedReason: "finished"}, text: "Finished · 1 call"},
                    {tag: "stopped", reading: {playbackState: 0, playbackEndedReason: "stopped"}, text: "Stopped"},
                    {tag: "focus", reading: {playbackState: 0, playbackEndedReason: "focus"}, text: "Stopped: another app started playing"},
                    {tag: "timeout", reading: {playbackState: 0, playbackEndedReason: "timeout"}, text: "Stopped after 10 minutes paused"},
                    {tag: "unplayable", reading: {playbackState: 4, playbackError: "failed", playbackEndedReason: "unplayable"}, text: "Couldn't play these recordings.", alert: true},
                    {tag: "start", reading: {playbackState: 4, playbackError: "failed", playbackEndedReason: "start"}, text: "Couldn't start playing. Try again.", alert: true},
                    {tag: "focus at start", reading: {playbackState: 4, playbackError: "unavailable", playbackEndedReason: "failed"}, text: "Can't play right now. Another app has the audio, such as a phone call.", alert: true}];
        }
        function test_ended_bar(data) {
            var calls = logCalls(data.calls || 3);
            var screen = idle();
            playAll(screen, calls);
            only(screen);
            playing({item: 0});
            tryCompare(bar(screen), "announcements", 1);
            testContext.publishPlaybackReading(data.reading);
            tryCompare(bar(screen), "announced", data.text);
            compare(bar(screen).announcements, 2);
            sessionLoader.item.visible = true;
            wait(20);
            compare(bar(sessionLoader.item).announcements, 0, "heard: not read again by the other screen");
            for (var s of screens()) {
                verify(bar(s).visible, "kept once ended");
                var result = part(s, "nowPlayingResult");
                verify(result.visible);
                compare(result.text, data.text);
                compare(String(result.color), String(data.alert ? Ui.Theme.alert : Ui.Theme.textPrimary));
                compare(part(s, "nowPlayingClose").Accessible.name, "Close");
                verify(!part(s, "nowPlayingPlay").visible && !part(s, "nowPlayingNext").visible && !part(s, "nowPlayingPrevious").visible,
                       "the result takes the transport's room");
                verify(!result.truncated, "the whole result");
            }
            verify(!findChild(screen, "callLogList").itemAtIndex(0).playback, "the marker leaves the row");
            // Another play after the end does not take the result away; ✕ sends nothing, so it never stops that play.
            decoderHost.playRecordingOwned("/rec/other.wav");
            verify(bar(screen).visible, "a later sheet play leaves the result");
            var calls0 = testContext.hostCalls().length;
            part(screen, "nowPlayingClose").activate();
            compare(testContext.hostCalls().length, calls0, "✕ on an ended bar sends nothing");
            compare(decoderHost.playbackState, 1, "the later play plays on");
            for (s of screens())
                verify(!bar(s).visible, "dismissed on both screens");
            // Nor is a dismissed playlist taken again from the reading it ended on.
            testContext.resetHostPlayback();
            verify(!bar(screen).visible);
        }

        // A new playlist takes the bar from an ended one.
        function test_new_playlist_replaces_the_result() {
            var calls = logCalls(2);
            playAll(idle(), calls);
            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
            verify(part(idle(), "nowPlayingResult").visible);
            var second = playAll(idle(), calls);
            compare(status.generation, second);
            verify(!part(idle(), "nowPlayingResult").visible);
            compare(line(idle()), "Opening…");
        }

        // A sheet's play that replaces the playlist hides the bar at once, and the playlist's unheard event goes.
        function test_sheet_play_hides_the_bar() {
            var calls = logCalls(2);
            var screen = idle();
            status.appActive = false;
            playAll(screen, calls);
            playing({item: 0});
            verify(!status.eventHeard, "an unheard start");
            decoderHost.playRecordingOwned("/rec/call-0-1.wav");
            for (var s of screens())
                verify(!bar(s).visible, "hidden at once");
            verify(status.eventHeard, "its event dropped");
            status.appActive = true;
            wait(20);
            compare(bar(screen).announcements, 0, "nothing read on return");
        }

        // A sheet's play of a recording gone at the press is refused before it is taken: the playlist plays on, its
        // bar stays, and the sheet says the recording is gone, read out once; closing the sheet stops nothing.
        function test_sheet_play_of_a_gone_recording_keeps_the_playlist() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            var generation = playAll(screen, calls);
            playing({item: 0});
            var sheet = findChild(screen, "historyDetailSheet");
            sheet.open(callHistory.recordFor(calls[1].uid));
            waitForRendering(sheet);
            var player = findChild(sheet, "historyRecording");
            var said = player.announcements;
            testContext.setPlaybackRefusal("missing");
            var play = findChild(sheet, "recordingPlayButton");
            verify(play.visible && play.enabled);
            mouseClick(play, play.width / 2, play.height / 2);
            testContext.setPlaybackRefusal("");
            compare(hostCallsNamed("playRecordingOwned").length, 1, "asked");
            compare(decoderHost.playbackGeneration, generation, "the playlist plays on");
            compare(decoderHost.playbackState, 2);
            compare(hostCallsNamed("stopPlayback").length + hostCallsNamed("stopPlaybackGeneration").length, 0);
            compare(findChild(sheet, "recordingNotice").text, "This recording is gone.");
            compare(player.announcements, said + 1, "read out once");
            compare(player.announced, "This recording is gone.");
            verify(bar(screen).visible, "the bar still shows the playlist");
            compare(line(screen), "Call 1 of 2 · 0:12 / 0:31");
            sheet.visible = false;
            compare(hostCallsNamed("stopPlayback").length + hostCallsNamed("stopPlaybackGeneration").length, 0, "the close stops nothing");
            verify(bar(screen).visible);
        }

        // Show in list: the playing call's row in view and its sheet open; a row the list does not show now says so.
        function test_show_in_list() {
            var calls = logCalls(30);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            playing({item: 0});
            var list = findChild(screen, "callLogList");
            list.positionViewAtBeginning();
            var show = part(screen, "nowPlayingShowInList");
            compare(show.Accessible.name, "Show in list");
            show.activate();
            var sheet = findChild(screen, "historyDetailSheet");
            verify(sheet.visible, "the row's sheet opens");
            compare(sheet.record.uid, calls[0].uid);
            sheet.visible = false;
            waitForRendering(list);
            var row = -1;
            for (var i = 0; i < list.count; i++) {
                var item = list.itemAtIndex(i);
                if (item && item.rowUid === calls[0].uid)
                    row = i;
            }
            verify(row >= 0, "the oldest row is built: in view");
            var rowRect = rectIn(list.itemAtIndex(row), list);
            verify(rowRect.y >= -0.5 && rowRect.y + rowRect.height <= list.height + 0.5, "scrolled into view: " + describe(rowRect));

            // A search still being typed counts: it is flushed first, as Play all's is.
            findChild(screen, "historySearch").text = "no call matches this";
            compare(historyView.filterText, "", "still in the debounce");
            show.activate();
            compare(historyView.filterText, "no call matches this", "flushed by the press");
            verify(!sheet.visible);
            compare(line(screen), "This call isn't in the list now.");
            compare(bar(screen).announced, "This call isn't in the list now.");
            playing({item: 1});
            compare(line(screen), "Call 2 of 30 · 0:12 / 0:31", "the next call ends the notice");
        }

        // Clear history (confirmed) ends a History playlist as ✕ does.
        function test_clear_ends_the_playlist() {
            var calls = logCalls(2);
            var generation = playAll(sessionLoader.item, calls);
            playing({item: 0});
            var confirm = findChild(sessionLoader.item, "clearHistoryConfirm");
            confirm.open();
            confirm.confirmed();
            var stops = hostCallsNamed("stopPlaybackGeneration");
            compare(stops.length, 1);
            compare(stops[0].generation, generation);
            verify(!bar(sessionLoader.item).visible);
            compare(callHistory.count, 0);
        }

        // ✕ takes a pointer's tap in both forms: while it plays it stops the playlist; once ended it only
        // dismisses, sending nothing. Ended and stacked, it is centred on the result it closes.
        function test_close_by_pointer_data() {
            return [{tag: "stacked", width: 360}, {tag: "one line", width: 900}];
        }
        function test_close_by_pointer(data) {
            var calls = logCalls(3);
            var screen = idle();
            only(screen);
            root.width = data.width;
            idleLoader.width = data.width;
            try {
                var generation = playAll(screen, calls);
                playing({item: 0});
                compare(bar(screen).stacked, data.width < 400);
                var close = part(screen, "nowPlayingClose");
                waitForRendering(screen);
                mouseClick(close, close.width / 2, close.height / 2);
                var stops = hostCallsNamed("stopPlaybackGeneration");
                compare(stops.length, 1, "a tap stops it");
                compare(stops[0].generation, generation);
                verify(!bar(screen).visible);

                playAll(screen, calls);
                testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
                verify(bar(screen).visible);
                var result = part(screen, "nowPlayingResult");
                if (bar(screen).stacked) {
                    var r = rectIn(part(screen, "nowPlayingShowInList"), screen), c = rectIn(close, screen);
                    verify(Math.abs((c.y + c.height / 2) - (r.y + r.height / 2)) < 1, "✕ centred on the result: " + describe(c) + " " + describe(r));
                }
                var before = testContext.hostCalls().length;
                waitForRendering(screen);
                mouseClick(close, close.width / 2, close.height / 2);
                compare(testContext.hostCalls().length, before, "ended: a tap sends nothing");
                verify(!bar(screen).visible, "dismissed");
            } finally {
                root.width = 360;
                idleLoader.width = 360;
            }
        }

        // Clear history also lets go of an ended playlist's result: its rows are gone. Nothing is sent.
        function test_clear_dismisses_an_ended_result() {
            var calls = logCalls(2);
            var screen = sessionLoader.item;
            playAll(screen, calls);
            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
            verify(bar(screen).visible);
            var before = testContext.hostCalls().length;
            var confirm = findChild(screen, "clearHistoryConfirm");
            confirm.open();
            confirm.confirmed();
            compare(testContext.hostCalls().length, before, "nothing sent");
            for (var s of screens())
                verify(!bar(s).visible, "the result goes with the rows");
        }

        // The playing row: two magenta bars in place of the recording glyph, named playing or paused, from the
        // playing flag alone (a deleted file plays on); gone once the playlist ends.
        function test_playing_row_marker() {
            var calls = logCalls(3);
            playAll(idle(), calls);
            playing({item: 1});
            var list = findChild(idle(), "callLogList");
            waitForRendering(list);
            function rowOf(uid) {
                for (var i = 0; i < list.count; i++) {
                    var item = list.itemAtIndex(i);
                    if (item && item.rowUid === uid)
                        return item;
                }
                return null;
            }
            var row = rowOf(calls[1].uid), other = rowOf(calls[0].uid);
            compare(row.playback, 1);
            var glyph = findChild(row, "rowRecordingGlyph");
            verify(glyph.visible && glyph.bars);
            compare(String(glyph.color), String(Ui.Theme.magenta));
            compare(row.node.Accessible.name, row.accessibleName + ", playing");
            compare(other.playback, 0);
            verify(!findChild(other, "rowRecordingGlyph").bars);
            compare(other.node.Accessible.name, other.accessibleName + ", has a recording");
            verify(callHistory.setRowRecordings(calls[1].uid, []));
            compare(row.playback, 1, "the file deleted under it: still playing");
            verify(findChild(row, "rowRecordingGlyph").visible);
            playing({playbackState: 3, playbackPausedReason: "user"});
            compare(row.playback, 2);
            compare(row.node.Accessible.name, row.accessibleName + ", paused");
            compare(String(findChild(row, "rowRecordingGlyph").color), String(Ui.Theme.textSubdued), "dimmed while paused");
            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
            compare(row.playback, 0);
            verify(!findChild(row, "rowRecordingGlyph").visible);
        }

        // Read out only by the bar of a screen that shows, uncovered, while the app is active; an event that comes
        // otherwise waits for that moment.
        function test_events_wait_until_they_can_be_read() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            var sheet = findChild(screen, "historyDetailSheet");
            sheet.open(callHistory.recordFor(calls[0].uid));
            playing({item: 0});
            wait(20);
            compare(bar(screen).announcements, 0, "covered: not read");
            sheet.requestDismiss();
            tryCompare(bar(screen), "announcements", 1);
            compare(bar(screen).announced, "Playing 2 calls");
            testContext.emitApplicationState(Qt.ApplicationSuspended);
            try {
                verify(!status.appActive);
                testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
                wait(20);
                compare(bar(screen).announcements, 1, "away: not read");
            } finally {
                testContext.emitApplicationState(Qt.ApplicationActive);
            }
            tryCompare(bar(screen), "announcements", 2);
            compare(bar(screen).announced, "Finished · 2 calls");
            screen.visible = false;
            screen.visible = true;
            wait(20);
            compare(bar(screen).announcements, 2, "once");
        }

        // One announcement across the two History screens: a start nobody heard, a finish while the app is away,
        // then the return through the other History screen reads exactly the finish, and no start.
        function test_one_announcement_across_the_two_screens() {
            var calls = logCalls(3);
            var first = idle(), second = sessionLoader.item;
            second.visible = false;
            playAll(first, calls);
            testContext.emitApplicationState(Qt.ApplicationSuspended);
            try {
                playing({item: 0});
                verify(!status.eventHeard, "the start waits");
                testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
                first.visible = false;
                second.visible = true;
                wait(20);
                compare(bar(first).announcements + bar(second).announcements, 0);
            } finally {
                testContext.emitApplicationState(Qt.ApplicationActive);
            }
            tryCompare(bar(second), "announcements", 1);
            compare(bar(second).announced, "Finished · 3 calls");
            first.visible = true;
            wait(20);
            compare(bar(first).announcements, 0, "the other screen reads nothing");
            compare(bar(second).announcements, 1);
        }

        // The header's buttons wrap right-aligned at 360 px and large text, all four reachable, in order, never over
        // the title or past the edge. Both History screens.
        function test_header_flow_data() {
            return [{tag: "idle 1.0", session: false, scale: 1.0}, {tag: "idle 1.6", session: false, scale: 1.6},
                    {tag: "idle 2.0", session: false, scale: 2.0}, {tag: "session 1.0", session: true, scale: 1.0},
                    {tag: "session 2.0", session: true, scale: 2.0}];
        }
        function test_header_flow(data) {
            logCalls(2);
            var screen = data.session ? sessionLoader.item : idle();
            Ui.Theme.fontScale = data.scale;
            verify(waitForPolish(screen));
            waitForRendering(screen);
            var names = ["historyMapButton", "historyPlayAll", "historyExportButton", "clearHistoryButton"];
            var rects = names.map(function (name) {
                var control = findChild(screen, name);
                verify(control.visible, name);
                compare(control.width, control.implicitWidth, name + " whole");
                return rectIn(control, screen);
            });
            var margin = Ui.Theme.screenPadding;
            for (var i = 0; i < rects.length; i++) {
                verify(rects[i].x >= margin - 0.5 && rects[i].x + rects[i].width <= screen.width - margin + 0.5, names[i] + " inside: " + describe(rects[i]));
                for (var j = i + 1; j < rects.length; j++) {
                    verify(!overlaps(rects[i], rects[j]), names[i] + " apart from " + names[j]);
                    // In order: left to right on a line, a later line under an earlier one.
                    verify(rects[j].y > rects[i].y + 0.5 || (Math.abs(rects[j].y + rects[j].height / 2 - rects[i].y - rects[i].height / 2) < 0.5 && rects[j].x > rects[i].x),
                           names[j] + " after " + names[i] + ": " + describe(rects[i]) + " " + describe(rects[j]));
                }
            }
            verify(Math.abs(rects[3].x + rects[3].width - (screen.width - margin)) < 0.5, "Clear on the right");
            var search = rectIn(findChild(screen, "historySearch"), screen);
            for (var r of rects)
                verify(r.y + r.height <= search.y, "above the search field");
            if (!data.session) {
                var head = findChild(screen, "historyTitle");
                var titleRect = rectIn(head, screen);
                titleRect.width = head.implicitWidth;
                for (r of rects)
                    verify(!overlaps(r, titleRect), "clear of the title");
            }
            // The glyphs together and the text buttons together: on one line, or the glyphs on the line above.
            verify(Math.abs(rects[0].y - rects[1].y) < 0.5 && Math.abs(rects[2].y - rects[3].y) < 0.5, "each pair on one line");
            if (data.session && data.scale === 1.0)
                verify(Math.abs(rects[1].y + rects[1].height / 2 - rects[2].y - rects[2].height / 2) < 0.5, "all four on one line");
            if (!data.session) {
                var middle = titleRect.y + titleRect.height / 2;
                verify(rects[0].y < middle && rects[0].y + rects[0].height > middle, "the glyphs on the title's line: " + describe(rects[0]) + " " + describe(titleRect));
                verify(rects[2].y >= titleRect.y + titleRect.height, "the text buttons under the title on a phone");
            }
        }

        // The bar docks at the bottom, keeps one height in every state, and never covers the last row: the list
        // ends above it, and the bar comes out of the list's 40% share (down to one row of list) before the header
        // gives room up, so at 360x640 and normal text the whole header, filters included, still shows. While the
        // keyboard is up the bar hides and History is laid out as without one. Both screens, normal and 2x text.
        function test_bar_never_covers_the_last_row_data() {
            var rows = [];
            for (var session of [false, true])
                for (var scale of [1.0, 2.0])
                    for (var keyboard of [false, true])
                        rows.push({tag: (session ? "session " : "idle ") + scale + (keyboard ? " keyboard" : ""), session: session, scale: scale, keyboard: keyboard});
            return rows;
        }
        function test_bar_never_covers_the_last_row(data) {
            var calls = logCalls(25);
            var screen = data.session ? sessionLoader.item : idle();
            Ui.Theme.fontScale = data.scale;
            var scroll = findChild(screen, "historyHeaderScroll");
            playAll(screen, calls);
            var barItem = bar(screen);
            var opening = barItem.height;
            playing({item: 3});
            compare(barItem.height, opening, "one height opening and playing");
            if (data.keyboard)
                decoderHost.keyboardTop = screen.mapToItem(null, 0, 0).y + 360;
            verify(waitForPolish(screen));
            waitForRendering(screen);
            var list = findChild(screen, "callLogList");
            var listRect = rectIn(list, screen);
            var share = Math.ceil(screen.height * 0.4);
            if (data.keyboard) {
                verify(!barItem.visible, "hidden while the keyboard is up");
                verify(Math.abs(listRect.y + listRect.height - screen.height) < 0.5, "the list as without a bar: " + describe(listRect));
                compare(scroll.height, Math.min(scroll.contentHeight, screen.height - share - 4), "the header as without a bar");
                decoderHost.keyboardTop = -1;
                verify(barItem.visible, "back with the keyboard down");
                verify(waitForPolish(screen));
                listRect = rectIn(list, screen);
            }
            var barRect = rectIn(barItem, screen);
            verify(Math.abs(barRect.y + barRect.height - screen.height) < 0.5, "docked at the bottom: " + describe(barRect));
            verify(listRect.y + listRect.height <= barRect.y + 0.5, "the list ends above the bar: " + describe(listRect) + " " + describe(barRect));
            verify(list.height + barItem.height >= share - 0.5, "list and bar keep the list's share: " + list.height + "+" + barItem.height);
            verify(list.height >= screen.sectionHeight + screen.rowHeight - 0.5, "at least a whole row of list and its day: " + list.height);
            // From the top: the day's heading and its first row whole above the bar.
            list.positionViewAtBeginning();
            waitForRendering(list);
            var first = list.itemAtIndex(0);
            verify(first !== null, "the first row is built");
            var firstRect = rectIn(first, screen);
            verify(firstRect.y >= listRect.y - 0.5 && firstRect.y + firstRect.height <= barRect.y + 0.5,
                   "a whole row shows above the bar: " + describe(firstRect) + " " + describe(barRect));
            verify(listRect.y >= scroll.y + scroll.height, "the list starts under the header");
            if (barItem.height + screen.rowHeight <= share)
                compare(scroll.height, Math.min(scroll.contentHeight, screen.height - share - 4), "the bar took nothing from the header");
            if (data.scale === 1.0 && !data.session) {
                verify(scroll.contentHeight <= scroll.height + 0.5, "the whole header shows at normal text: " + scroll.contentHeight + " in " + scroll.height);
                var pill = rectIn(findChild(screen, "historyActivityFilter"), screen);
                verify(pill.y + pill.height <= scroll.y + scroll.height + 0.5, "the filters fully shown: " + describe(pill));
            }
            list.positionViewAtEnd();
            waitForRendering(list);
            var last = list.itemAtIndex(list.count - 1);
            verify(last !== null, "the last row is built");
            var lastRect = rectIn(last, screen);
            verify(lastRect.y + lastRect.height <= barRect.y + 0.5, "the last row above the bar: " + describe(lastRect) + " " + describe(barRect));
            for (var name of ["nowPlayingPrevious", "nowPlayingPlay", "nowPlayingNext", "nowPlayingClose", "nowPlayingShowInList"]) {
                var control = part(screen, name);
                verify(control.width >= 48 && control.height >= 48, name + " is a 48 px target");
                var c = rectIn(control, screen);
                verify(c.x >= 0 && c.x + c.width <= screen.width && c.y >= barRect.y && c.y + c.height <= barRect.y + barRect.height + 0.5, name + " inside the bar: " + describe(c));
            }
            for (var reading of [{playbackState: 3, playbackPausedReason: "noisy"}, {playbackState: 0, playbackEndedReason: "focus"}])
                testContext.publishPlaybackReading(reading);
            compare(barItem.height, opening, "one height ended");
        }

        // The line under the name holds at most two lines: every status it shows fits them whole at 360 px, at
        // normal or 2x text (and wide), except one whose whole words Show in list's description reads. A refused
        // resume shows a short line; its full sentence is what is read. The result takes the transport's room and
        // is never cut. The bar's height never moves.
        function test_bar_text_is_never_cut_data() {
            return [{tag: "360 1x", width: 360, scale: 1.0}, {tag: "360 2x", width: 360, scale: 2.0},
                    {tag: "360 1.6x", width: 360, scale: 1.6}, {tag: "900 1x", width: 900, scale: 1.0}, {tag: "900 2x", width: 900, scale: 2.0}];
        }
        function test_bar_text_is_never_cut(data) {
            var calls = logCalls(3);
            var screen = idle();
            only(screen);
            root.width = data.width;
            idleLoader.width = data.width;
            Ui.Theme.fontScale = data.scale;
            try {
                playAll(screen, calls, [1, 8, 1]);
                var barItem = bar(screen);
                verify(waitForPolish(screen));
                var height = barItem.height;
                var lineItem = part(screen, "nowPlayingLine");
                var show = part(screen, "nowPlayingShowInList");
                verify(lineItem.maximumLineCount <= 2, "at most two lines");
                var full = "Can't play right now. Another app has the audio, such as a phone call.";
                var lines = [{reading: {item: 1, playbackState: 2, playbackDurationMs: 600000, playbackPositionMs: 599000}},
                             {reading: {playbackState: 1}, text: "Opening…"},
                             {reading: {playbackState: 3, playbackPausedReason: "user"}, text: "Paused"},
                             {reading: {playbackState: 3, playbackPausedReason: "unavailable"}, text: "Paused: another app has the audio.", said: full},
                             {reading: {playbackState: 3, playbackPausedReason: "noisy"}, text: "Paused: headphones unplugged"},
                             {reading: {playbackState: 3, playbackInterrupted: true}, text: "Paused for another app's audio"},
                             {reading: {item: 2, playbackState: 2, playbackInterrupted: false, playbackPausedReason: "", playbackSkipped: 12}, text: "Skipped 12 recordings that couldn't be played"}];
                for (var l of lines) {
                    testContext.publishPlaybackReading(l.reading);
                    verify(waitForPolish(lineItem));
                    if (l.text)
                        compare(line(screen), l.text);
                    if (l.said)
                        tryVerify(function () { return show.Accessible.description.indexOf(l.said) >= 0; }, 2000, "read whole: " + show.Accessible.description);
                    if (l.said)
                        tryCompare(barItem, "announced", l.said, 2000, "announced whole");
                    if (lineItem.truncated) {
                        verify(l.text !== undefined, "the position line fits: " + line(screen));
                        tryVerify(function () { return show.Accessible.description.indexOf(l.said || l.text) >= 0; }, 2000,
                                  "cut on the bar, read whole: " + show.Accessible.description);
                    }
                    if (data.width === 360 && l.text !== "Skipped 12 recordings that couldn't be played")
                        verify(!lineItem.truncated, "fits two lines whole: " + line(screen));
                    compare(barItem.height, height, "one height: " + line(screen));
                }
                var result = part(screen, "nowPlayingResult");
                for (var end of [{playbackState: 4, playbackError: "unavailable", playbackEndedReason: "failed"},
                                 {playbackState: 0, playbackEndedReason: "finished", playbackSkipped: 1234}]) {
                    status.forget();
                    playAll(screen, calls);
                    testContext.publishPlaybackReading(end);
                    verify(waitForPolish(result));
                    verify(result.visible);
                    verify(!result.truncated, "the whole result: " + result.text);
                    compare(barItem.height, height, "one height ended");
                }
            } finally {
                root.width = 360;
                idleLoader.width = 360;
            }
        }

        // The keyboard's order is what is seen: stacked (a phone), ✕ ends the top row before the transport under
        // it; on one line (wide), ✕ comes last.
        function test_bar_tab_order_data() {
            return [{tag: "stacked", width: 360, order: ["nowPlayingShowInList", "nowPlayingClose", "nowPlayingPrevious", "nowPlayingPlay", "nowPlayingNext"]},
                    {tag: "one line", width: 900, order: ["nowPlayingShowInList", "nowPlayingPrevious", "nowPlayingPlay", "nowPlayingNext", "nowPlayingClose"]}];
        }
        function test_bar_tab_order(data) {
            var calls = logCalls(3);
            var screen = idle();
            only(screen);
            root.width = data.width;
            idleLoader.width = data.width;
            try {
                playAll(screen, calls);
                playing({item: 0});
                compare(bar(screen).stacked, data.width < 400);
                var item = part(screen, data.order[0]);
                for (var n = 1; n < data.order.length; n++) {
                    var next = item.nextItemInFocusChain(true);
                    compare(next ? next.objectName : "", data.order[n], "after " + data.order[n - 1]);
                    item = next;
                }
            } finally {
                root.width = 360;
                idleLoader.width = 360;
            }
        }
    }
}
