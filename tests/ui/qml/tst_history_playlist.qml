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
        function detailSheet(screen) {
            return findChild(screen, "historyDetailSheet");
        }
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
            decoderHost.reportsKeyboard = false;
            // A field left with the keyboard would take no new focus in the next case, and reveal nothing.
            root.forceActiveFocus();
            Ui.Theme.resetFontScale();
            testContext.resetHostPlayback();
            status.forget();
            // Both lists shown again build their rows before the next case clears the log under them. A frame is
            // asked for, so the wait for it ends at that frame: with nothing changed since the last one, no frame
            // would come, and the wait would run out its 5 s.
            for (screen of screens())
                verify(waitForPolish(findChild(screen, "callLogList")));
            root.Window.window.update();
            verify(waitForRendering(root), "the frame after the cleanup is drawn");
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
                    {tag: "opening", reading: {playbackState: 1}, line: "Opening…", pause: true},
                    {tag: "opening paused", reading: {playbackState: 1, playbackPausedReason: "user"}, line: "Paused", pause: false},
                    {tag: "opening unplugged", reading: {playbackState: 1, playbackPausedReason: "noisy"}, line: "Paused: headphones unplugged", pause: false}];
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

        // A skip is said on the call after it, singular and plural, as one sentence for any reason: the player's
        // notice names that call and the count, and the bar says it while that call is the one playing.
        function test_skip_said_on_the_next_call() {
            var calls = logCalls(4);
            playAll(idle(), calls);
            playing({item: 0});
            playing({item: 1, playbackSkipped: 1, playbackSkipNoticeCall: 1, playbackSkipNoticeCount: 1});
            compare(line(idle()), "Skipped 1 recording that couldn't be played");
            playing({item: 1, playbackPositionMs: 20000});
            compare(line(idle()), "Skipped 1 recording that couldn't be played", "while that call plays");
            playing({item: 2, playbackSkipNoticeCall: -1, playbackSkipNoticeCount: 0});
            compare(line(idle()), "Call 3 of 4 · 0:12 / 0:31", "only on the call after the skip");
            playing({item: 3, playbackSkipped: 3, playbackSkipNoticeCall: 3, playbackSkipNoticeCount: 2});
            compare(line(idle()), "Skipped 2 recordings that couldn't be played");
            playing({item: 2, playbackSkipNoticeCall: 3});
            compare(line(idle()), "Call 3 of 4 · 0:12 / 0:31", "a notice that names another call says nothing here");
        }

        // Previous on the call with the skip notice opens it again under a new token: the notice stays on that call,
        // and Show in list still names its row.
        function test_a_reopen_keeps_the_skip_notice() {
            var calls = logCalls(3);
            playAll(idle(), calls);
            playing({item: 1, playbackSkipped: 1, playbackSkipNoticeCall: 1, playbackSkipNoticeCount: 1});
            compare(line(idle()), "Skipped 1 recording that couldn't be played");
            testContext.publishPlaybackReading({item: 1, playbackState: 1, playbackToken: 8, playbackPositionMs: 0});
            compare(line(idle()), "Opening…", "opening it again");
            playing({item: 1, playbackToken: 8, playbackPositionMs: 0});
            compare(line(idle()), "Skipped 1 recording that couldn't be played", "the notice stays on its call");
        }

        // A skip observed late is no skip on the call observed: A skipped, B played while the app was away (no reading
        // reached Qt), and the first reading is C's, with the cumulative count moved but no notice on C.
        function test_skip_not_said_on_a_later_call_seen_first() {
            var calls = logCalls(3);
            playAll(idle(), calls);
            playing({item: 2, playbackToken: 9, playbackSkipped: 1});
            compare(line(idle()), "Call 3 of 3 · 0:12 / 0:31", "C plays with no notice of A's skip");
            playing({item: 2, playbackToken: 9, playbackPositionMs: 20000, playbackSkipped: 1});
            compare(line(idle()), "Call 3 of 3 · 0:20 / 0:31", "nor later on C");
            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished", playbackSkipped: 1});
            compare(part(idle(), "nowPlayingResult").text, "Finished · 3 calls · 1 skipped", "the total is the summary's");
        }

        // Paused while a call opens (or moved while paused): Kotlin publishes Preparing with the reason it pauses for,
        // and the bar offers Resume, which resumes, never another Pause.
        function test_opening_paused_offers_resume() {
            var calls = logCalls(2);
            var screen = idle();
            playAll(screen, calls);
            playing({item: 0});
            testContext.publishPlaybackReading({item: 1, playbackState: 1, playbackToken: 8, playbackPausedReason: "user"});
            var play = part(screen, "nowPlayingPlay");
            verify(!play.showPause);
            compare(play.Accessible.name, "Resume");
            compare(line(screen), "Paused");
            play.activate();
            compare(hostCallsNamed("resumePlayback").length, 1, "Resume is sent");
            compare(hostCallsNamed("pausePlayback").length, 0, "no Pause");
        }

        // A refused resume stands across Next while paused: the next call opens and prepares with the same reason
        // and refusal count, which is no new refusal to say.
        function test_standing_refusal_not_said_again_after_a_move() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            var barItem = bar(screen);
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1});
            tryCompare(barItem, "announcements", 1);
            testContext.publishPlaybackReading({item: 1, playbackState: 1, playbackToken: 8});
            compare(line(screen), "Paused: another app has the audio.", "opening, still paused for it");
            playing({item: 1, playbackState: 3, playbackToken: 8});
            wait(30);
            compare(barItem.announcements, 1, "not said again");
        }

        // Pause pressed while it plays, a headset pausing it before the release: the tap acts on what it was pressed
        // on, so it does not resume the playlist the user meant to pause.
        function test_a_press_acts_on_what_it_began_on() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            playing({item: 0});
            var play = part(screen, "nowPlayingPlay");
            verify(play.showPause);
            mousePress(play, play.width / 2, play.height / 2);
            playing({item: 0, playbackState: 3, playbackPausedReason: "user"});
            verify(!play.showPause, "paused meanwhile");
            mouseRelease(play, play.width / 2, play.height / 2);
            compare(hostCallsNamed("resumePlayback").length + hostCallsNamed("pausePlayback").length, 0, "nothing sent");
            mouseClick(play, play.width / 2, play.height / 2);
            compare(hostCallsNamed("resumePlayback").length, 1, "a press begun now resumes");
        }

        // ✕ held on playlist A while Play all starts B: the release is dropped (its press was A's Stop), so B plays
        // on; a ✕ press on A's ended result never becomes a Stop of B either. A press begun on B stops B.
        function test_close_press_belongs_to_its_playlist() {
            var calls = logCalls(3);
            var screen = idle();
            only(screen);
            var a = playAll(screen, calls);
            playing({item: 0});
            var close = part(screen, "nowPlayingClose");
            waitForRendering(screen);
            mousePress(close, close.width / 2, close.height / 2);
            var b = playAll(screen, calls);
            verify(b !== a, "B replaced A");
            mouseRelease(close, close.width / 2, close.height / 2);
            compare(hostCallsNamed("stopPlaybackGeneration").length, 0, "A's Stop does not stop B");
            compare(decoderHost.playbackGeneration, b, "B plays on");
            verify(bar(screen).live, "its bar too");

            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
            verify(!!bar(screen).end, "B ended");
            waitForRendering(screen);
            mousePress(close, close.width / 2, close.height / 2);
            var c = playAll(screen, calls);
            mouseRelease(close, close.width / 2, close.height / 2);
            compare(hostCallsNamed("stopPlaybackGeneration").length, 0, "a press on an ended result stops no new playlist");
            verify(bar(screen).live && decoderHost.playbackGeneration === c, "C plays on");

            mouseClick(close, close.width / 2, close.height / 2);
            var stops = hostCallsNamed("stopPlaybackGeneration");
            compare(stops.length, 1, "a press begun on C stops it");
            compare(stops[0].generation, c);
        }

        // Previous or Next held on playlist A while B starts: the release moves nothing (it would move B); a press
        // begun on B moves B.
        function test_skip_press_belongs_to_its_playlist_data() {
            return [{tag: "next", name: "nowPlayingNext", call: "nextPlayback"}, {tag: "previous", name: "nowPlayingPrevious", call: "previousPlayback"}];
        }
        function test_skip_press_belongs_to_its_playlist(data) {
            var calls = logCalls(3);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            playing({item: 1});
            var control = part(screen, data.name);
            verify(control.enabled);
            waitForRendering(screen);
            mousePress(control, control.width / 2, control.height / 2);
            playAll(screen, calls);
            playing({item: 1, playbackToken: 9});
            mouseRelease(control, control.width / 2, control.height / 2);
            compare(hostCallsNamed(data.call).length, 0, "the press was A's: B is not moved");
            mouseClick(control, control.width / 2, control.height / 2);
            compare(hostCallsNamed(data.call).length, 1, "a press begun on B moves it");
        }

        // Show in list held on call A while the playlist moves on to call B: the release opens no row (it would be
        // B's); a press begun on B opens B's.
        function test_show_in_list_press_belongs_to_its_call() {
            var calls = logCalls(3);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            playing({item: 0});
            var show = part(screen, "nowPlayingShowInList");
            var sheet = findChild(screen, "historyDetailSheet");
            waitForRendering(screen);
            mousePress(show, show.width / 2, show.height / 2);
            playing({item: 1, playbackToken: 8});
            mouseRelease(show, show.width / 2, show.height / 2);
            verify(!sheet.visible, "the press was A's: no row opened");
            compare(bar(screen).notice, "", "and nothing said");
            mouseClick(show, show.width / 2, show.height / 2);
            tryVerify(function () { return sheet.visible; }, 2000, "a press begun on B opens its row");
            compare(sheet.rowUidKey, calls[1].uid);
        }

        // A keyboard or screen reader activation while the pointer holds Pause/Resume acts at once and leaves the
        // held press: Pause held, Pause activated, then the release (now on Resume) is dropped.
        function test_activation_leaves_a_held_bar_press() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            playing({item: 0});
            var play = part(screen, "nowPlayingPlay");
            waitForRendering(screen);
            mousePress(play, play.width / 2, play.height / 2);
            play.activate();
            compare(hostCallsNamed("pausePlayback").length, 1, "the activation pauses");
            verify(!play.showPause);
            mouseRelease(play, play.width / 2, play.height / 2);
            compare(hostCallsNamed("resumePlayback").length, 0, "the held Pause is not a Resume");
            compare(hostCallsNamed("pausePlayback").length, 1);
            mouseClick(play, play.width / 2, play.height / 2);
            compare(hostCallsNamed("resumePlayback").length, 1, "a press begun now resumes");
        }

        // A row's delete confirmation does not outlive History hiding: Delete on call A, History hidden and shown,
        // call B's sheet opened: no confirmation shows, and nothing is deleted.
        function test_hidden_history_ends_a_pending_delete() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            var sheet = findChild(screen, "historyDetailSheet");
            sheet.open(callHistory.recordFor(calls[0].uid));
            var del = findChild(sheet, "recordingDelete");
            verify(del.visible && del.enabled);
            del.activate();
            var confirm = findChild(sheet, "recordingDeleteConfirm");
            verify(confirm.visible && confirm.target.path === "/rec/call-0.wav", "asked for A");
            screen.visible = false;
            screen.visible = true;
            sheet.open(callHistory.recordFor(calls[1].uid));
            verify(sheet.visible, "B's sheet");
            verify(!confirm.visible, "no confirmation over B");
            compare(confirm.target, null, "A's file forgotten");
            var deletes = function () {
                return recordings.calls().filter(function (call) { return call.name === "deleteRecordingChecked"; }).length;
            };
            var before = deletes();
            confirm.confirm();
            compare(deletes(), before, "nothing deleted");
        }

        // A Resume refused, then refused again with the same reading but for the host's refusal count: each refusal
        // is said on the bar (the fake host, like the real one, publishes nothing for a reading that did not change).
        function test_repeated_refusal_said_again() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            playing({item: 0, playbackState: 3, playbackPausedReason: "user"});
            testContext.setResumeRefused(true);
            var full = "Can't play right now. Another app has the audio, such as a phone call.";
            var barItem = bar(screen);
            part(screen, "nowPlayingPlay").activate();
            compare(decoderHost.playbackRefusals, 1);
            tryCompare(barItem, "announced", full);
            compare(barItem.announcements, 1, "refused: said");
            playing({item: 0, playbackState: 3, playbackPositionMs: 12500});
            wait(30);
            compare(barItem.announcements, 1, "a tick says nothing");
            part(screen, "nowPlayingPlay").activate();
            compare(hostCallsNamed("resumePlayback").length, 2);
            compare(decoderHost.playbackRefusals, 2);
            tryCompare(barItem, "announcements", 2);
            compare(barItem.announced, full, "refused again: said again");
            testContext.setResumeRefused(false);
        }

        // A refused resume nobody heard (the sheet over the bar, here the app away) is dropped once the host no longer
        // reports it: a resume taken after all is never told "Can't play right now" when the bar is read again. A
        // refusal still standing waits, and so does any other event (the start, the end).
        // A refused resume is read out once across the sheet and the bar: one the sheet that follows the playing
        // call read out is heard, and the bar says nothing of it when the sheet closes. A sheet on another call reads
        // nothing of it, so the bar's event waits and is read once the sheet closes. Each refusal once.
        function test_refusal_said_once_across_the_sheet_and_the_bar() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            var full = "Can't play right now. Another app has the audio, such as a phone call.";
            var barItem = bar(screen);
            // The playlist names the calls' own recordings, so a sheet on the playing call follows it.
            testContext.setHistoryPlaylistAnswer("accepted", calls.map(function (call, i) {
                return {path: "/rec/call-" + i + ".wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: i, partOrdinal: 1,
                    partCount: 1, title: call.name, line: "", key: String(call.uid)};
            }));
            findChild(screen, "historyPlayAll").activate();
            verify(barItem.visible);
            playing({item: 0});
            tryCompare(barItem, "announcements", 1);
            var sheet = findChild(screen, "historyDetailSheet");
            var block = findChild(sheet, "historyRecording");

            // The sheet follows the playing call (Show in list opens it): it reads the refusal.
            part(screen, "nowPlayingShowInList").activate();
            verify(sheet.visible);
            compare(sheet.record.uid, calls[0].uid);
            var said = block.announcements;
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1});
            compare(block.announcements, said + 1, "the sheet reads the refusal");
            compare(block.announced, full);
            verify(!status.eventHeard && status.eventRefusal, "the bar's event is noted");
            sheet.visible = false;
            // The close schedules the bar's turn to say its event; it marks it heard there, saying nothing.
            tryCompare(status, "eventHeard", true, 2000, "heard: the sheet said it");
            compare(barItem.announcements, 1, "the bar says nothing for it");

            // A sheet on another call reads nothing of the next refusal: the bar reads it once the sheet closes.
            detailSheet(screen).open(callHistory.recordFor(calls[1].uid));
            verify(sheet.visible);
            said = block.announcements;
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 2});
            compare(block.announcements, said, "the other call's sheet says nothing");
            wait(30);
            verify(!status.eventHeard, "the bar's event waits under the sheet");
            sheet.visible = false;
            tryCompare(barItem, "announcements", 2);
            compare(barItem.announced, full, "and the bar reads it");
        }

        // A refusal is heard only once it is read out: shown on a sheet under its Delete confirmation, it is not read
        // there, so the bar reads it once the confirmation and the sheet are closed.
        function test_refusal_shown_under_a_dialog_is_read_by_the_bar() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            var full = "Can't play right now. Another app has the audio, such as a phone call.";
            var barItem = bar(screen);
            testContext.setHistoryPlaylistAnswer("accepted", calls.map(function (call, i) {
                return {path: "/rec/call-" + i + ".wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: i, partOrdinal: 1,
                    partCount: 1, title: call.name, line: "", key: String(call.uid)};
            }));
            findChild(screen, "historyPlayAll").activate();
            playing({item: 0});
            tryCompare(barItem, "announcements", 1);
            var sheet = detailSheet(screen);
            var block = findChild(sheet, "historyRecording");
            part(screen, "nowPlayingShowInList").activate();
            verify(sheet.visible);
            block.askDelete();
            var confirm = findChild(block, "recordingDeleteConfirm");
            verify(confirm.visible, "the Delete confirmation covers the sheet");
            var said = block.announcements;
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1});
            compare(block.notice, full, "the sheet shows the refusal");
            compare(block.announcements, said, "but cannot read it under the confirmation");
            compare(Ui.Navigation.refusalSaid === Ui.Navigation.refusalKey(decoderHost.playbackGeneration, 1), false,
                    "so it is not marked heard");
            confirm.visible = false;
            sheet.visible = false;
            tryCompare(barItem, "announcements", 2);
            compare(barItem.announced, full, "the bar reads it");
            verify(status.eventHeard);
        }

        // A refusal left unread under the Delete confirmation is read on the sheet once the confirmation closes and the
        // sheet stays: once, and marked heard there, so the bar says nothing of it when the sheet closes after.
        function test_refusal_unread_under_a_dialog_is_read_by_the_sheet_after() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            var full = "Can't play right now. Another app has the audio, such as a phone call.";
            var barItem = bar(screen);
            testContext.setHistoryPlaylistAnswer("accepted", calls.map(function (call, i) {
                return {path: "/rec/call-" + i + ".wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: i, partOrdinal: 1,
                    partCount: 1, title: call.name, line: "", key: String(call.uid)};
            }));
            findChild(screen, "historyPlayAll").activate();
            playing({item: 0});
            tryCompare(barItem, "announcements", 1);
            var sheet = detailSheet(screen);
            var block = findChild(sheet, "historyRecording");
            part(screen, "nowPlayingShowInList").activate();
            block.askDelete();
            var confirm = findChild(block, "recordingDeleteConfirm");
            verify(confirm.visible);
            var said = block.announcements;
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1});
            compare(block.announcements, said, "not read under the confirmation");
            findChild(confirm, "confirmCancelButton").activate();
            verify(!confirm.visible && sheet.visible, "the confirmation cancelled, the sheet stays");
            tryCompare(block, "announcements", said + 1, 2000, "the sheet reads the refusal now");
            compare(block.announced, full);
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1, playbackToken: 8});
            compare(block.announcements, said + 1, "once");
            sheet.visible = false;
            tryCompare(status, "eventHeard", true, 2000, "the bar takes it as heard");
            compare(barItem.announcements, 1, "and says nothing of it");
        }

        // The line reachable for a moment, then covered again before the sheet's turn to read comes (the confirmation
        // cancelled and opened again at once): nothing is read and the refusal stays pending; reachable again, it is
        // read once, and the bar is silent after the sheet closes.
        function test_refusal_unread_stays_pending_while_the_line_is_covered_again() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            var full = "Can't play right now. Another app has the audio, such as a phone call.";
            var barItem = bar(screen);
            testContext.setHistoryPlaylistAnswer("accepted", calls.map(function (call, i) {
                return {path: "/rec/call-" + i + ".wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: i, partOrdinal: 1,
                    partCount: 1, title: call.name, line: "", key: String(call.uid)};
            }));
            findChild(screen, "historyPlayAll").activate();
            playing({item: 0});
            tryCompare(barItem, "announcements", 1);
            var sheet = detailSheet(screen);
            var block = findChild(sheet, "historyRecording");
            part(screen, "nowPlayingShowInList").activate();
            block.askDelete();
            var confirm = findChild(block, "recordingDeleteConfirm");
            var said = block.announcements;
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1});
            compare(block.announcements, said, "not read under the confirmation");
            findChild(confirm, "confirmCancelButton").activate();
            verify(block.lineReachable, "the line is reachable for a moment");
            block.askDelete();
            verify(confirm.visible && !block.lineReachable, "and covered again before the sheet's turn");
            wait(0);
            wait(0);
            compare(block.announcements, said, "nothing is read while it is covered");
            verify(block.refusalUnsaid.length > 0, "the refusal is still pending");
            findChild(confirm, "confirmCancelButton").activate();
            tryCompare(block, "announcements", said + 1, 2000, "reachable again: read now");
            compare(block.announced, full);
            sheet.visible = false;
            tryCompare(status, "eventHeard", true, 2000, "the bar takes it as heard");
            compare(barItem.announcements, 1, "and says nothing of it");
        }

        // R50: a refusal polled while the app is inactive, its sheet still visible (a notification's Resume refused),
        // is shown and not read; once the app is active the sheet reads it, once, and the bar is silent after.
        function test_refusal_waits_for_the_app_to_be_active() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            var full = "Can't play right now. Another app has the audio, such as a phone call.";
            var barItem = bar(screen);
            testContext.setHistoryPlaylistAnswer("accepted", calls.map(function (call, i) {
                return {path: "/rec/call-" + i + ".wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: i, partOrdinal: 1,
                    partCount: 1, title: call.name, line: "", key: String(call.uid)};
            }));
            findChild(screen, "historyPlayAll").activate();
            playing({item: 0});
            tryCompare(barItem, "announcements", 1);
            var sheet = detailSheet(screen);
            var block = findChild(sheet, "historyRecording");
            part(screen, "nowPlayingShowInList").activate();
            var said = block.announcements;
            testContext.emitApplicationState(Qt.ApplicationInactive);
            try {
                playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1});
                compare(block.notice, full, "the sheet shows the refusal");
                wait(0);
                compare(block.announcements, said, "but reads nothing while the app is inactive");
                verify(block.refusalUnsaid.length > 0, "it stays pending");
            } finally {
                testContext.emitApplicationState(Qt.ApplicationActive);
            }
            tryCompare(block, "announcements", said + 1, 2000, "active: the sheet reads it");
            compare(block.announced, full);
            sheet.visible = false;
            tryCompare(status, "eventHeard", true, 2000, "the bar takes it as heard");
            compare(barItem.announcements, 1, "and says nothing of it");
        }

        // The reverse: a refused resume the bar read out is heard by a sheet opened after it. Show in list opens the
        // playing call's sheet, and a move that keeps the refusal standing (the part opened again under a new token)
        // shows it on the sheet's notice line without reading it again; the next refusal is read.
        function test_refusal_the_bar_said_is_not_read_by_a_later_sheet() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            var full = "Can't play right now. Another app has the audio, such as a phone call.";
            var barItem = bar(screen);
            testContext.setHistoryPlaylistAnswer("accepted", calls.map(function (call, i) {
                return {path: "/rec/call-" + i + ".wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: i, partOrdinal: 1,
                    partCount: 1, title: call.name, line: "", key: String(call.uid)};
            }));
            findChild(screen, "historyPlayAll").activate();
            playing({item: 0});
            tryCompare(barItem, "announcements", 1);
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1});
            tryCompare(barItem, "announcements", 2);
            compare(barItem.announced, full, "the bar reads the refusal");

            var sheet = detailSheet(screen);
            var block = findChild(sheet, "historyRecording");
            var said = block.announcements;
            part(screen, "nowPlayingShowInList").activate();
            verify(sheet.visible);
            compare(sheet.record.uid, calls[0].uid);
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1, playbackToken: 8});
            compare(block.notice, full, "the sheet shows the refusal");
            compare(block.announcements, said, "and does not read it again");
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 2, playbackToken: 8});
            compare(block.announcements, said + 1, "a new refusal is read");
            compare(block.announced, full);
            sheet.visible = false;
            tryCompare(status, "eventHeard", true, 2000, "the bar takes the sheet's refusal as heard");
            compare(barItem.announcements, 2, "and not again by the bar");
        }

        function test_unheard_refusal_dropped_once_resumed() {
            var calls = logCalls(2);
            var screen = idle();
            only(screen);
            var full = "Can't play right now. Another app has the audio, such as a phone call.";
            var barItem = bar(screen);
            status.appActive = false;
            playAll(screen, calls);
            playing({item: 0});
            playing({item: 0, playbackPositionMs: 12500});
            verify(!status.eventHeard && status.eventText === "Playing 2 calls", "the start waits through a reading");
            status.appActive = true;
            tryCompare(barItem, "announcements", 1);
            playing({item: 0, playbackState: 3, playbackPausedReason: "user"});
            status.appActive = false;
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1});
            verify(!status.eventHeard && status.eventText === full, "the refusal waits, unheard");
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 1, playbackPositionMs: 12500});
            verify(!status.eventHeard, "still refused: it still waits");
            playing({item: 0, playbackRefusals: 1});
            verify(status.eventHeard && status.eventText === "", "resumed: the refusal is dropped");
            status.appActive = true;
            wait(30);
            compare(barItem.announcements, 1, "nothing is read over the playing recording");
            compare(barItem.announced, "Playing 2 calls");

            // Refused, then paused by the user from the sheet: the refusal no longer stands either.
            status.appActive = false;
            playing({item: 0, playbackState: 3, playbackPausedReason: "unavailable", playbackRefusals: 2});
            verify(!status.eventHeard);
            playing({item: 0, playbackState: 3, playbackPausedReason: "user", playbackRefusals: 2});
            verify(status.eventHeard, "paused for another reason: dropped");

            // The end waiting unheard is left alone by a later reading with no refusal.
            playing({item: 1, playbackState: 0, playbackEndedReason: "finished", playbackCallCount: 2, playbackRefusals: 2});
            verify(!status.eventHeard && status.eventText.indexOf("Finished") === 0, "the end waits: " + status.eventText);
            status.appActive = true;
            tryCompare(barItem, "announcements", 2);
            verify(barItem.announced.indexOf("Finished") === 0, "and is read once the app is back");
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
            // R49: the resume's start asked for and not yet made (5): Pause, and a tap pauses it.
            playing({item: 0, playbackState: 5});
            compare(part(screen, "nowPlayingPlay").Accessible.name, "Pause");
            part(screen, "nowPlayingPlay").activate();
            compare(hostCallsNamed("pausePlayback").length, 2, "a tap on a pending start pauses");
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

        // The header's Column laid out for the empty log (one line of controls), then calls logged: Export… and
        // Clear are offered on a second line, which the Column makes room for only on its next polish, after
        // whatever the case does before it renders. The rows' list hangs under that Column.
        function logCallsUnderAPendingHeader(screen, count) {
            verify(waitForPolish(findChild(screen, "historySearch").parent));
            var calls = logCalls(count);
            verifyHeaderPending(screen);
            return calls;
        }
        // Checked again right before the press it guards: a frame in between would have laid the Column out.
        function verifyHeaderPending(screen) {
            var search = findChild(screen, "historySearch");
            var clear = findChild(screen, "clearHistoryButton");
            verify(clear.visible);
            var second = rectIn(clear, search.parent);
            verify(second.y + second.height > search.y + 0.5, "the Column has not made room for the second line yet: " + describe(second));
        }

        // Show in list: the playing call's row in view and its sheet open; a row the list does not show now says so.
        // Play all and the press come with the header's layout pending, as when the log changed since the last
        // frame: the row is contained against the header the next frame draws, not the last one's.
        function test_show_in_list() {
            var screen = idle();
            only(screen);
            var calls = logCallsUnderAPendingHeader(screen, 30);
            playAll(screen, calls);
            playing({item: 0});
            var list = findChild(screen, "callLogList");
            list.positionViewAtBeginning();
            var show = part(screen, "nowPlayingShowInList");
            compare(show.Accessible.name, "Show in list");
            verifyHeaderPending(screen);
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

        // The row placed by Show in list, or by Back from the map, against a search still in its debounce: the
        // press flushes it, and the filter change queues the list's return to its top, which would run after the row
        // was placed and take it off screen again. Once the row is found the return is cancelled, so with 40 matching
        // calls and the oldest playing, the row is still in view when the queued work has run and the sheet closes.
        function rowInView(list, uid) {
            for (var i = 0; i < list.count; i++) {
                var item = list.itemAtIndex(i);
                if (item && item.rowUid === uid) {
                    var r = rectIn(item, list);
                    return r.y >= -0.5 && r.y + r.height <= list.height + 0.5;
                }
            }
            return false;
        }
        function test_placed_row_outlasts_the_filter_reset_data() {
            return [{tag: "show in list", press: "show"}, {tag: "row focus", press: "focus"}];
        }
        function test_placed_row_outlasts_the_filter_reset(data) {
            var screen = idle();
            only(screen);
            var calls = logCalls(40);
            playAll(screen, calls);
            playing({item: 0});
            var list = findChild(screen, "callLogList");
            list.positionViewAtBeginning();
            waitForRendering(list);
            verify(!rowInView(list, calls[0].uid), "the oldest call starts off screen");
            findChild(screen, "historySearch").text = "CALL";
            compare(historyView.filterText, "", "still in the debounce");
            var sheet = findChild(screen, "historyDetailSheet");
            if (data.press === "show") {
                part(screen, "nowPlayingShowInList").activate();
                verify(sheet.visible, "the row's sheet opens");
                compare(sheet.record.uid, calls[0].uid);
            } else {
                screen.flushPendingSearch();
                verify(screen.rowFocusFor(calls[0].uid) !== null, "the row is found");
            }
            compare(historyView.filterText, "CALL", "the search was flushed");
            compare(historyView.count, 40, "every call matches");
            // Everything the press queued runs.
            wait(50);
            sheet.visible = false;
            verify(waitForPolish(list));
            verify(rowInView(list, calls[0].uid), "the row is still in view");
        }

        // Back from the map brings its row into view (rowFocusFor) the same way, against the header the next frame
        // draws.
        function test_row_focus_with_the_header_pending() {
            var screen = idle();
            only(screen);
            var calls = logCallsUnderAPendingHeader(screen, 30);
            var list = findChild(screen, "callLogList");
            list.positionViewAtBeginning();
            verifyHeaderPending(screen);
            var node = screen.rowFocusFor(calls[0].uid);
            verify(node !== null, "the oldest row is built");
            waitForRendering(list);
            var row = node;
            while (row && row.rowUid === undefined)
                row = row.parent;
            verify(row !== null && row.rowUid === calls[0].uid);
            var rowRect = rectIn(row, list);
            verify(rowRect.y >= -0.5 && rowRect.y + rowRect.height <= list.height + 0.5, "scrolled into view: " + describe(rowRect));
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

        // The bar hides while the keyboard shows, by the host's visible-IME indicator rather than by overlap: a
        // window Android resized above the keyboard reports the keyboard's top at or below the screen's bottom, and
        // the bar still hides there, History laid out as without one.
        function test_bar_hides_for_a_keyboard_below_the_screen_data() {
            return [{tag: "at the bottom", below: 0}, {tag: "below it", below: 120}];
        }
        function test_bar_hides_for_a_keyboard_below_the_screen(data) {
            var calls = logCalls(25);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            playing({item: 3});
            var barItem = bar(screen);
            verify(barItem.visible);
            decoderHost.keyboardTop = screen.mapToItem(null, 0, screen.height).y + data.below;
            compare(Ui.Theme.keyboardTop(screen), screen.height, "no overlap with the screen");
            verify(waitForPolish(screen));
            verify(!barItem.visible, "hidden while the keyboard shows");
            compare(screen.barHeight, 0, "and it takes no room");
            var list = findChild(screen, "callLogList");
            var listRect = rectIn(list, screen);
            verify(Math.abs(listRect.y + listRect.height - screen.height) < 0.5, "the list as without a bar: " + describe(listRect));
            decoderHost.keyboardTop = -1;
            verify(barItem.visible, "back once the keyboard hides");
        }

        // The keyboard's visibility is decided by the host first: a top it reports is the answer, and a host that
        // reports the keyboard itself (Android) saying none means hidden, whatever Qt's input method says. Only a host
        // with no word of its own falls back to Qt's, which counts a keyboard only with some height.
        function test_keyboard_shown_by_the_host_first() {
            var up = {visible: true, keyboardRectangle: {height: 300}};
            var flat = {visible: true, keyboardRectangle: {height: 0}};
            var down = {visible: false, keyboardRectangle: {height: 0}};
            decoderHost.reportsKeyboard = true;
            decoderHost.keyboardTop = -1;
            verify(!Ui.Theme.keyboardShown(up), "Android says hidden: Qt's flag, stuck up, is not asked");
            decoderHost.keyboardTop = 700;
            verify(Ui.Theme.keyboardShown(down), "Android's top is the answer");
            decoderHost.reportsKeyboard = false;
            verify(Ui.Theme.keyboardShown(down), "any host's top is the answer");
            decoderHost.keyboardTop = -1;
            verify(Ui.Theme.keyboardShown(up), "no word from the host: Qt's flag");
            verify(!Ui.Theme.keyboardShown(flat), "a keyboard of no height is none");
            verify(!Ui.Theme.keyboardShown(down), "and a hidden one is none");

            // The bar follows the host's word: shown while Android says the keyboard is hidden.
            var calls = logCalls(3);
            var screen = idle();
            only(screen);
            playAll(screen, calls);
            decoderHost.reportsKeyboard = true;
            verify(bar(screen).visible && !screen.keyboardUp, "the bar shows");
            decoderHost.keyboardTop = screen.mapToItem(null, 0, screen.height).y;
            verify(!bar(screen).visible && screen.keyboardUp, "and hides once Android reports a keyboard");
        }

        // In a short window at 2x text the bar and a row of list would leave the header no room at all: it keeps what
        // the bar leaves (56 px here), at least a 48 px control row, with its top row's controls (Map, Play all) whole
        // in it, and scrolls, so Search (and the filters, Export and Clear under it) stays in reach without ending the
        // playlist.
        function test_short_window_keeps_the_header_data() {
            return [{tag: "idle", session: false}, {tag: "session", session: true}];
        }
        function test_short_window_keeps_the_header(data) {
            var calls = logCalls(5);
            var loader = data.session ? sessionLoader : idleLoader;
            var screen = loader.item;
            only(screen);
            Ui.Theme.fontScale = 2.0;
            loader.height = 240;
            try {
                playAll(screen, calls);
                playing({item: 0});
                verify(waitForPolish(screen));
                waitForRendering(screen);
                compare(screen.height, 240);
                var scroll = findChild(screen, "historyHeaderScroll");
                var barRect = rectIn(bar(screen), screen);
                verify(barRect.height > 0 && bar(screen).visible, "the bar shows: " + describe(barRect));
                verify(scroll.height > 0, "the header keeps room: " + scroll.height);
                verify(scroll.y + scroll.height <= barRect.y + 0.5, "above the bar: " + scroll.height + " " + describe(barRect));
                verify(scroll.interactive, "and it scrolls for the rest");
                // The top row's controls lie whole inside the header's room, with no tolerance.
                var view = rectIn(scroll, screen);
                tryVerify(function () {
                    for (var name of ["historyMapButton", "historyPlayAll"]) {
                        var c = rectIn(findChild(screen, name), screen);
                        if (c.y < view.y || c.y + c.height > view.y + view.height)
                            return false;
                    }
                    return true;
                }, 2000, "Map and Play all whole in the header: "
                         + describe(rectIn(findChild(screen, "historyMapButton"), screen)) + " in " + describe(view));
                verify(scroll.height >= 48, "at least a 48 px control row: " + scroll.height);
                var search = findChild(screen, "historySearch");
                search.forceActiveFocus();
                tryVerify(function () {
                    var r = rectIn(search, screen);
                    return r.y < scroll.y + scroll.height - 1 && r.y + r.height > scroll.y + 1;
                }, 2000, "Search scrolled into the header's room");
            } finally {
                loader.height = 640;
                root.forceActiveFocus();
            }
        }

        // With no playlist the header floor does not apply: the list keeps the 40% share it had before the
        // playlist, however tall the header is. In a 260x145 window at 2x text the header scrolls in what is left and
        // the list is 58 px (ceil(145 * 0.4)), not squeezed to nothing under the header's top row.
        function test_no_bar_keeps_the_list_share_data() {
            return [{tag: "idle", session: false}, {tag: "session", session: true}];
        }
        function test_no_bar_keeps_the_list_share(data) {
            logCalls(5);
            var loader = data.session ? sessionLoader : idleLoader;
            var screen = loader.item;
            only(screen);
            Ui.Theme.fontScale = 2.0;
            loader.width = 260;
            loader.height = 145;
            try {
                verify(waitForPolish(screen));
                waitForRendering(screen);
                compare([screen.width, screen.height], [260, 145]);
                verify(!bar(screen).visible, "no playlist, no bar");
                var scroll = findChild(screen, "historyHeaderScroll");
                var list = findChild(screen, "callLogList");
                verify(scroll.contentHeight > 145 - 58 - 4, "a header taller than its room: " + scroll.contentHeight);
                compare(scroll.height, 145 - 58 - 4, "the header as before the playlist");
                compare(list.height, 58, "the list keeps its 40% share");
                verify(scroll.interactive, "and the header scrolls for the rest");
            } finally {
                loader.width = root.width;
                loader.height = 640;
            }
        }

        // R57: the call counter, the part counter and the time are each kept whole, and the line may break between
        // them: on a narrow bar at 2x text a call in parts wraps between its counters, never inside one.
        function test_bar_counters_wrap_between_each_other() {
            var calls = logCalls(3);
            var screen = idle();
            only(screen);
            root.width = 360;
            idleLoader.width = 360;
            Ui.Theme.fontScale = 2.0;
            try {
                playAll(screen, calls, [1, 3, 1]);
                testContext.publishPlaybackReading({item: 2, playbackState: 2, playbackDurationMs: 60000, playbackPositionMs: 1000});
                var lineItem = part(screen, "nowPlayingLine");
                verify(waitForPolish(lineItem));
                var shown = lineItem.text;
                compare(shown.split(" "), ["Call\u00a02\u00a0of\u00a03\u00a0·", "part\u00a02\u00a0of\u00a03\u00a0·", "0:01\u00a0/\u00a01:00"],
                        "a wrap point between the counters and before the time, none inside them");
                compare(lineItem.lineCount, 2, "the narrow bar breaks the line");
                verify(!lineItem.truncated, "and shows it whole: " + line(screen));
            } finally {
                Ui.Theme.fontScale = 1.0;
                root.width = 360;
                idleLoader.width = 360;
            }
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
                             {reading: {item: 2, playbackState: 2, playbackInterrupted: false, playbackPausedReason: "", playbackSkipped: 12, playbackSkipNoticeCall: 1, playbackSkipNoticeCount: 12}, text: "Skipped 12 recordings that couldn't be played"}];
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
