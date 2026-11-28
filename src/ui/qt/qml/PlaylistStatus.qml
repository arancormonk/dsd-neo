// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Issue #909: History's playlist as the app follows it, one object in Main
// that both History screens' now-playing bars read, so neither keeps its own
// copy. It takes the playlist the host's readings name (a playlist play from
// "history", each new one by its generation), keeps how it ended once it has
// (the reading it ended on: why, the recordings skipped, its calls) until ✕
// or the next playlist, and lets it go at once when another play replaced it
// or the app stopped it. Its events (audio started, the end, a refused
// resume) wait in one slot with an id and a heard flag: the bar of the
// History screen that can be read says the latest one once (NowPlayingBar),
// so a background tick never takes it, moving between the two History
// screens never reads it twice, and a newer event takes the place of one
// nobody heard (a start, then a finish while the app was away: only the
// finish is said).
QtObject {
    id: status

    // The playlist the bars show (0: none), and the highest one taken, so a
    // playlist dismissed is never taken again from the reading it ended on.
    property int generation: 0
    property int seen: 0
    // How it ended, once it has: {reason, error, skipped, calls, title, key};
    // null while it plays, or with none.
    property var ended: null
    // Its audio started ("Playing 41 calls" was said).
    property bool started: false
    property string pausedReason: ""
    // The host's refusal count last read: a resume refused again reads the
    // same but for it, and is said again.
    property int refusalsSeen: 0
    // The latest event: its id, its words, whether it was read out, and
    // whether it is a refused resume (said only while the host still reports
    // the refusal), with the refusal count it was noted at.
    property int eventId: 0
    property string eventText: ""
    property bool eventHeard: true
    property bool eventRefusal: false
    property int eventRefusals: 0
    // The application is in the foreground and active. Followed from the
    // signal's own state: the host acts on the same signal first (a return
    // reads the platform before anything here is said).
    property bool appActive: Qt.application.state === Qt.ApplicationActive

    readonly property bool shown: generation > 0
    readonly property bool live: generation > 0 && ended === null

    readonly property string unavailableText: qsTr("Can't play right now. Another app has the audio, such as a phone call.")
    function playingText(calls) {
        return calls === 1 ? qsTr("Playing 1 call") : qsTr("Playing %1 calls").arg(Util.countText(calls));
    }
    function skippedText(count) {
        return Util.skippedText(count);
    }
    // What the bar says once @p end (an `ended` record) is how it ended.
    function endedText(end) {
        if (!end)
            return "";
        if (end.reason === "finished") {
            var done = end.calls === 1 ? qsTr("Finished · 1 call") : qsTr("Finished · %1 calls").arg(Util.countText(end.calls));
            return end.skipped > 0 ? Util.fill(qsTr("%1 · %2 skipped"), [done, Util.countText(end.skipped)]) : done;
        }
        if (end.reason === "stopped")
            return qsTr("Stopped");
        if (end.reason === "focus")
            return qsTr("Stopped: another app started playing");
        if (end.reason === "timeout")
            return qsTr("Stopped after 10 minutes paused");
        if (end.reason === "start")
            return qsTr("Couldn't start playing. Try again.");
        if (end.error === "unavailable")
            return unavailableText;
        return qsTr("Couldn't play these recordings.");
    }

    function note(text, refusal) {
        eventId++;
        eventText = text;
        eventHeard = false;
        eventRefusal = refusal === true;
        eventRefusals = refusalsSeen;
    }
    // The waiting refused resume was read out already, by the sheet that
    // follows the play (Navigation.refusalSaid): it is not said again.
    readonly property bool eventSaidElsewhere: eventRefusal && Navigation.refusalSaid === Navigation.refusalKey(generation, eventRefusals)
    function drop() {
        eventText = "";
        eventHeard = true;
        eventRefusal = false;
    }
    // Event @p id was read out; a newer one waits on.
    function heard(id) {
        if (id === eventId)
            eventHeard = true;
    }
    function forget() {
        generation = 0;
        ended = null;
        started = false;
        pausedReason = "";
        refusalsSeen = 0;
        drop();
    }
    // ✕ on an ended bar: its saved result goes, and nothing is sent, so a
    // later play is never stopped by it.
    function dismiss() {
        if (ended !== null)
            forget();
    }
    // ✕ on a playing bar, and Clear history: this playlist stops, and only it.
    function stop() {
        if (!live)
            return;
        var playing = generation;
        forget();
        decoderHost.stopPlaybackGeneration(playing);
    }

    // The host's readings, read whenever they change (never on a position
    // tick). @p quiet: taken as it stands, nothing said (at startup).
    function follow(quiet) {
        var host = decoderHost;
        var reading = Number(host.playbackGeneration) || 0;
        var history = reading > 0 && host.playbackOrigin === "history" && host.playbackMode === 2;
        if (history && reading > seen) {
            // A new History playlist (Play all, Play from here) takes the bar
            // from whatever it held, an ended result included.
            forget();
            seen = reading;
            generation = reading;
        }
        if (!live)
            return;
        if (!history || reading !== generation) {
            // Another play replaced it, or the app stopped it: the bar goes at
            // once, and what it had not said yet with it.
            forget();
            return;
        }
        var state = host.playbackState;
        if (state === 0 || state === 4) {
            ended = {
                reason: host.playbackEndedReason || (state === 4 ? "failed" : "stopped"),
                error: host.playbackError || "",
                skipped: Number(host.playbackSkipped) || 0,
                calls: Number(host.playbackCallCount) || 0,
                title: host.playbackTitle || "",
                key: host.playbackKey || ""
            };
            if (quiet)
                drop();
            else
                note(endedText(ended));
            return;
        }
        if (state === 2 && !started) {
            started = true;
            if (!quiet)
                note(playingText(Number(host.playbackCallCount) || 0));
        }
        // Opening counts too: a call opening paused (Next while paused) keeps
        // the reason it paused for, so a refusal standing is not said again.
        var reason = state === 1 || state === 3 ? String(host.playbackPausedReason || "") : "";
        var refusals = Number(host.playbackRefusals) || 0;
        // A resume the platform refused the audio (a phone call), the first
        // time or again (the refusal count moved on the same reading).
        var refused = reason === "unavailable" && (reason !== pausedReason || refusals !== refusalsSeen);
        pausedReason = reason;
        refusalsSeen = refusals;
        if (refused && !quiet)
            note(unavailableText, true);
        else if (reason !== "unavailable" && !eventHeard && eventRefusal)
            // A refusal nobody heard, the host no longer reporting it (a
            // resume taken after all, from the sheet or a media key): it is
            // no longer true, so it is not said over the playing recording.
            // Any other event waiting (the start, the end) is left alone.
            drop();
    }

    readonly property Connections hostWatch: Connections {
        target: decoderHost
        ignoreUnknownSignals: true
        function onPlaybackChanged() {
            status.follow(false);
        }
    }
    readonly property Connections applicationWatch: Connections {
        target: Qt.application
        function onStateChanged(state) {
            status.appActive = state === Qt.ApplicationActive;
        }
    }
    // A playlist already open or ended when the app's screens are made (the
    // Activity made again) is shown as it stands, with nothing said of it.
    Component.onCompleted: follow(true)
}
