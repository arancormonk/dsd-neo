// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Issue #708: a call's recordings in its detail sheet: play and pause, where it
// is and how long it runs, a bar to seek, the parts of a call heard in several
// pieces with Play all, and Share, Save to file and Delete.
//
// The host plays one thing at a time (its playback readings): a recording, a
// call's parts or History's playlist (issue #909), the queue run by the
// platform. This block reads the host as its own part's only while the host's
// recording is that part's file, by path and the identity the play listed
// (size and modification time), so a recording another sheet or a playlist
// plays reads as stopped on every other chip. The chip shown is the user's
// choice, apart from what plays: it follows the recording that plays while it
// is one of these parts, on a change of item, of the parts or the sheet
// opening (never on a position tick, and never while a Save picker or a Delete
// confirmation is out), until the user picks a chip. A play this block starts
// is its own by the generation the host gave it, and stops with the sheet; one
// it follows (a playlist) plays on. A recording the host found gone ("missing")
// leaves the block at once; the store drops it from the row soon after, or,
// when its storage went, lists it as away, and from then on the store's
// availability speaks for it: it shows again, away, and plays once the storage
// is back. What a share, save, delete or play came to is the sheet's to show
// (`notice`, on `noticeLine`), outside this block, which hides when no
// recording is left.
Column {
    id: player

    // The row's recordings (RecordingStore maps), oldest first.
    property var recordingList: []
    // Shared with another app and offered as a file name, with the part's own name.
    property string title: ""
    // Where the delete confirmation opens: the sheet, so it dims the whole screen.
    property Item dialogParent: null
    // What deleting a single recording leaves alone, as its confirmation says.
    // Only the file goes: a recording may have no History row (never linked, or
    // trimmed) and may belong to a Signal search detection (issue #809), so
    // "the call stays in History" would not hold for every one.
    property string deleteMessage: qsTr("Only the audio is deleted. History and detections are unchanged.")
    // The sheet's line that shows `notice`, through which a problem is read out
    // (null: this block reads it out itself).
    property Item noticeLine: null
    // Issue #909: whether the row these recordings belong to is still listed,
    // asked when the recordings change (null: always, a list with no row of
    // its own). A row that lists no recording any more is still there; only
    // the row going (trimmed, cleared) stops this block's Play all parts.
    property var rowPresent: null

    // The part shown, kept on its recording by path: a part that leaves (deleted,
    // or found missing) moves no other part's selection, and the index every label
    // uses is always one that exists.
    property int current: 0
    property string currentPath: ""
    // Issue #909: a chip the user picked keeps the selection where it is: it no
    // longer follows what plays, until Play all parts or the sheet opens again.
    property bool picked: false
    // The play this block started, by the generation the host answered (0:
    // none). It is this block's while the host shows that generation, and stops
    // with the sheet even once the row's list no longer names its recording (a
    // History trim).
    property int ownedGeneration: 0
    readonly property bool ownsPlay: ownedGeneration > 0 && decoderHost.playbackGeneration === ownedGeneration
    // The recording a play of this block is being asked for, while the host is
    // asked: a failure it publishes before it answers is this block's.
    property string startingPath: ""
    // A play of this block the host refused at once with a failed reading of
    // its own (generation 0, on the path asked for): cleared with the sheet, as
    // no one owns it.
    property string refusedPath: ""
    // The failure last said ("" while none stands), so each is said once.
    property string failureKey: ""
    property string failureNotice: ""
    // The refused resume read out last (its playlist and refusal count).
    property string refusalSaid: ""
    // A refused resume shown while the line could not be read (under the
    // Delete confirmation): read once the line can be, if it still stands.
    property string refusalUnsaid: ""
    // The skips said last (this block's play and how many), so each is said once.
    property int skipSaidGeneration: 0
    property int skipSaidCount: 0
    // What the last share, save, delete or play came to; empty when there is nothing to say.
    property string notice: ""
    property bool noticeProblem: false
    // The last problem read out and how many have been, for tests.
    property string announced: ""
    property int announcements: 0
    // The selection a Save to file holds while its picker and transfer run, 0 while none.
    property int saveSelection: 0
    property var saveSelectionMap: ({})
    // The recording that save is of: its end is reported only while that
    // recording is shown here, never on another row's sheet.
    property string savePath: ""
    // Where that save is written, once picked.
    property string saveReference: ""
    // The recording the host found missing, while that failure stands.
    readonly property string missingPath: decoderHost.playbackState === 4 && decoderHost.playbackError === "missing" ? decoderHost.playbackPath : ""
    // That recording once the store has listed it as away since: no longer
    // left out, and its failure no longer read as this block's.
    property string missingAway: ""

    readonly property var parts: {
        var list = [];
        var gone = missingPath !== missingAway ? missingPath : "";
        for (var i = 0; i < recordingList.length; i++) {
            if (recordingList[i].path !== gone)
                list.push(recordingList[i]);
        }
        return list;
    }
    readonly property var part: parts.length > 0 ? parts[Math.min(current, parts.length - 1)] : null
    readonly property bool loaded: part !== null && playsFile(part) && part.path !== missingAway
    // DecoderHost::PlaybackState: 0 stopped, 1 preparing, 2 playing, 3 paused,
    // 4 failed, 5 starting (issue #909, R49: a start asked for and not yet
    // made, so the button offers Pause while the position holds).
    readonly property int playState: loaded ? decoderHost.playbackState : 0
    // Issue #909 (B4): halted for another app's audio, carrying on by itself
    // after it: wanted still, so its button pauses it (that cancels the carry-on).
    readonly property bool interrupted: loaded && decoderHost.playbackInterrupted
    // Still opening, with nothing wanted (paused while it opens, or moved
    // while paused): the player names why it pauses, as the media session
    // offers Play then; it reads paused, not opening.
    readonly property bool pausedOpening: playState === 1 && String(decoderHost.playbackPausedReason || "").length > 0
    readonly property bool paused: (playState === 3 && !interrupted) || pausedOpening
    readonly property bool wantsPlay: (playState === 1 && !pausedOpening) || playState === 2 || playState === 5 || (playState === 3 && interrupted)
    // A play with no recording token yet (before the host's first poll): a
    // pause or resume from here could reach nothing, or (a queue's) another
    // part than the one shown, so none is offered until it has one. Only this
    // block's single recording, one item whatever its token, is paused and
    // resumed as a whole meanwhile.
    readonly property bool awaitingToken: loaded && ((playState >= 1 && playState <= 3) || playState === 5) && Number(decoderHost.playbackToken) <= 0
                                          && !(ownsPlay && decoderHost.playbackMode === 0)
    // This block's own queue (Play all parts) before its first token: no seek either, which would be the queue's.
    readonly property bool ownQueueAwaitingToken: awaitingToken && ownsPlay
    readonly property real durationMs: loaded && decoderHost.playbackDurationMs > 0 ? decoderHost.playbackDurationMs : part ? part.durationMs : 0
    // Issue #909: a seek waiting for its landing shows where it goes (the host shows nothing until the player's
    // record), so the thumb does not jump back to the old position for a poll.
    // Clamped to the length only when it is known, as the bar's is: a part of
    // unknown length (0) shows how far it played.
    readonly property real openPositionMs: !loaded ? 0 : clampToLength(seekTargetMs >= 0 ? seekTargetMs : decoderHost.playbackPositionMs)
    function clampToLength(ms) {
        return durationMs > 0 ? Math.min(ms, durationMs) : ms;
    }
    // A drag shows where it would seek, only on the recording it began on (its token): once another one is open, the
    // bar shows that one's own position.
    readonly property real positionMs: seekBar.previewing ? seekBar.dragMs : openPositionMs
    readonly property bool storageAway: part !== null && part.available === false
    // Issue #909 (B6): a host with no player offers no play; the parts only
    // select, for Share, Save and Delete.
    readonly property bool supported: decoderHost.playbackSupported === true
    readonly property bool anyAvailable: {
        for (var i = 0; i < parts.length; i++) {
            if (parts[i].available !== false)
                return true;
        }
        return false;
    }
    readonly property string unavailableText: qsTr("Can't play right now. Another app has the audio, such as a phone call.")

    // Whether the host's recording is @p recording's file: its path, and the
    // identity the play listed for it (the store's, at the press), unless the
    // play named its file by path alone (a single recording: size -1).
    function playsFile(recording) {
        if (!recording || decoderHost.playbackPath !== recording.path)
            return false;
        var size = Number(decoderHost.playbackSize);
        return size < 0 || (Number(recording.identitySize) === size && Number(recording.mtimeSec) === Number(decoderHost.playbackMtimeSec) && Number(recording.mtimeNanos) === Number(decoderHost.playbackMtimeNanos));
    }
    readonly property bool hostOpen: (decoderHost.playbackState >= 1 && decoderHost.playbackState <= 3) || decoderHost.playbackState === 5
    // The sheet holding this block is open.
    readonly property bool sheetShown: dialogParent ? dialogParent.visible : visible

    visible: parts.length > 0
    spacing: 10

    // The length rounds; the position floors, and reads the length once it
    // reaches the end, so a finished part reads 0:01 / 0:01, never 0:00 / 0:01.
    readonly property string lengthText: Util.playbackLengthText(durationMs)
    readonly property string positionText: timeTextAt(positionMs)
    function timeTextAt(ms) {
        return durationMs > 0 && ms >= durationMs ? lengthText : Util.playbackTimeText(ms);
    }
    // Issue #782: what a screen reader is told of the position, which moves
    // every tick while it plays: at most every few seconds (SpeechPacer), and
    // at once on play, pause, stop, the end of the part, or a seek once the
    // host reports a position near where it was asked to land (a position
    // sent before the host applied the seek does not count). Only a record
    // that counts a landing since the seek was asked (playbackSeeksLanded,
    // which moves when the player confirms the latest seek: #909 R56) can
    // land it, so a position read before the seek reached the player is
    // never its landing, however near the target it is.
    property int seekLanded: 0
    property real seekTargetMs: -1
    property int seekLandingsBefore: 0
    function noteSeekLanding() {
        if (seekTargetMs < 0 || (Number(decoderHost.playbackSeeksLanded) || 0) <= seekLandingsBefore)
            return;
        if (Math.abs(decoderHost.playbackPositionMs - seekTargetMs) < 1000) {
            seekTargetMs = -1;
            seekLanded++;
        }
    }
    // A seek the host never applied (it clamped or dropped it) is forgotten
    // when playback stops or fails, or moves to another part: a later
    // position passing near it is not a landing.
    // Also another recording open (its token), the host no longer able to
    // seek it (it would drop the seek), and a landing that never came within
    // a few polls: the position falls back on the host's record.
    readonly property string seekScope: [loaded, part ? part.path : "", playState === 0 || playState === 4, Number(decoderHost.playbackToken), decoderHost.playbackCanSeek === true].join("|")
    onSeekScopeChanged: seekTargetMs = -1
    onSeekTargetMsChanged: {
        if (seekTargetMs >= 0)
            seekWait.restart();
        else
            seekWait.stop();
    }
    Timer {
        id: seekWait
        interval: 2000
        onTriggered: player.seekTargetMs = -1
    }
    SpeechPacer {
        id: positionSpeech
        shown: Navigation.allows(player) && player.visible
        step: [player.playState, player.loaded, player.part ? player.part.path : "", player.durationMs, player.seekLanded].join("|")
        read: function () {
            return String(player.positionMs);
        }
    }
    readonly property real spokenPositionMs: positionSpeech.spoken.length > 0 ? Number(positionSpeech.spoken) : 0
    // With no known length (0), the position alone, as the bar has it: never
    // "of 0:00".
    readonly property string spokenTimeText: durationMs > 0 ? Util.fill(qsTr("%1 of %2"), [timeTextAt(spokenPositionMs), lengthText]) : timeTextAt(spokenPositionMs)

    function partLabel(index) {
        return Util.fill(qsTr("Part %1 · %2"), [index + 1, Util.playbackLengthText(parts[index].durationMs)]);
    }
    // A short state, beside the time.
    function statusText() {
        if (!part || storageAway || !supported)
            return "";
        if (interrupted)
            return qsTr("Paused for another app's audio");
        if (paused)
            return decoderHost.playbackPausedReason === "noisy" ? qsTr("Paused: headphones unplugged") : qsTr("Paused");
        if (playState === 1)
            return qsTr("Opening…");
        return "";
    }
    // Why it cannot play, or what is wrong with it: a sentence, under the
    // player. A play that failed is said on the notice line (failureText()).
    function problemText() {
        if (!part)
            return "";
        if (storageAway)
            return qsTr("The storage this recording is on isn't available right now.");
        if (!supported)
            return qsTr("Recordings can't be played on this device.");
        if (part.failed)
            return qsTr("Saving stopped early, so the end may be missing.");
        return "";
    }

    // Issue #909 (B3): what the host's reading says went wrong with a play of
    // this block, "" when nothing did: a failed play of its own (or one being
    // asked for), and, for the recording shown whoever started it, a resume the
    // platform refused the audio (it stays paused).
    function failureText() {
        var state = decoderHost.playbackState;
        if (paused && decoderHost.playbackPausedReason === "unavailable")
            return unavailableText;
        var mine = ownsPlay || (startingPath.length > 0 && decoderHost.playbackPath === startingPath);
        if (state !== 4 || !mine)
            return "";
        // Play all parts whose every part was skipped (none prepared in time): the skips are the news.
        var skipped = Number(decoderHost.playbackSkipped) || 0;
        if (decoderHost.playbackMode === 1 && decoderHost.playbackEndedReason === "unplayable" && skipped > 0)
            return Util.skippedText(skipped);
        var error = decoderHost.playbackError;
        return error === "missing" ? qsTr("This recording is gone.")
            : error === "unavailable" ? unavailableText
            : error === "unsupported" ? qsTr("Recordings can't be played on this device.")
            : qsTr("Can't play this recording. The file may be damaged.");
    }
    // Each failure is said once, and read out; the line clears once the reading
    // that said it has moved on (a resume the platform took after all). A
    // refused resume is one failure per refusal, as the bar has it: its
    // playlist and the host's refusal count, so a move that keeps it standing
    // (Previous to another part, opening then prepared) shows it without
    // reading it again, and a resume refused again (the count moved) is said.
    function noteFailure() {
        var text = failureText();
        var refusal = text.length > 0 && decoderHost.playbackState !== 4;
        var key = text.length === 0 ? "" : refusal ? ["refused", decoderHost.playbackGeneration, decoderHost.playbackRefusals].join("|") : [decoderHost.playbackGeneration, decoderHost.playbackToken, decoderHost.playbackState, decoderHost.playbackError, decoderHost.playbackPausedReason, decoderHost.playbackPath].join("|");
        // The same failure again is news only after a press asked once more
        // (failureKey cleared, its sentence still up).
        if (key === failureKey && (key.length > 0 || failureNotice.length === 0))
            return;
        var said = failureNotice;
        failureKey = key;
        // Closed, the sheet says nothing, then or once it opens again.
        failureNotice = sheetShown ? text : "";
        if (!sheetShown)
            return;
        // The refusal's play and count, as the bar and this sheet note the
        // one they read out (Navigation.refusalSaid).
        var heard = Navigation.refusalKey(decoderHost.playbackGeneration, decoderHost.playbackRefusals);
        if (text.length > 0 && refusal && (key === refusalSaid || Navigation.refusalSaid === heard)) {
            // The refusal read out already, here or by the bar: shown again,
            // not read again.
            refusalSaid = key;
            notice = text;
            noticeProblem = true;
        } else if (text.length > 0) {
            // Heard only once read out: one shown under the Delete
            // confirmation is left for the surface that can say it next (the
            // bar a playlist's refusal waits on).
            var read = false;
            if (refusal && !appActive) {
                // Shown, not read: the app is not active (R50).
                notice = text;
                noticeProblem = true;
            } else {
                read = say(text, true);
            }
            refusalUnsaid = refusal && !read ? key : "";
            if (read && refusal) {
                refusalSaid = key;
                Navigation.refusalSaid = heard;
            }
        } else if (said.length > 0 && notice === said && noticeProblem) {
            say("", false);
        }
    }

    // Issue #909: a part of this block's own Play all parts skipped (its
    // prepare never answered) is said on the notice line once, while the part
    // that played after it plays, as the bar says a playlist's skips: the
    // player's skip notice names the call that followed them (a PARTS play
    // is all call 0) and how many. Skips no part played after (the last part
    // skipped) are said when the play finishes, from its end's skip count; a
    // play whose every part was skipped says them as its failure
    // (failureText()), and one that ends on a part that failed says only
    // that failure. A play another surface started is not this block's to
    // report.
    function noteSkips() {
        if (!ownsPlay || decoderHost.playbackMode !== 1 || !sheetShown)
            return;
        var state = decoderHost.playbackState;
        var count = 0;
        if (state === 2 && Number(decoderHost.playbackSkipNoticeCall) === Number(decoderHost.playbackCallIndex))
            count = Number(decoderHost.playbackSkipNoticeCount) || 0;
        else if (state === 0 && decoderHost.playbackEndedReason === "finished")
            count = Number(decoderHost.playbackSkipped) || 0;
        var generation = Number(decoderHost.playbackGeneration);
        var said = skipSaidGeneration === generation ? skipSaidCount : 0;
        if (count <= said)
            return;
        skipSaidGeneration = generation;
        skipSaidCount = count;
        say(Util.skippedText(count), true);
    }
    // Issue #909: what a tap on Share, Save to file or Delete acts on (their
    // pressKey): the part shown, by its id and the identity the store
    // indexed, so a part shown in its place while the press is held (the
    // sheet following a playlist to its next part) drops the tap.
    function shownKey() {
        return part ? [part.id, part.path, String(part.identitySize), String(part.mtimeSec), String(part.mtimeNanos)].join("|") : "";
    }
    // Issue #909: what a tap on the play button acts on (its pressKey): its
    // action and what it would do it to. A play is of the part shown, its
    // path and the identity the store indexed (a reread that put another
    // recording under the same path is another part), whatever else plays
    // meanwhile; a pause or resume is of the host's play and recording.
    function playKey() {
        var action = wantsPlay ? "pause" : paused ? "resume" : "play";
        var shown = part ? [part.path, String(part.identitySize), String(part.mtimeSec), String(part.mtimeNanos)] : [""];
        if (action === "play")
            return [action].concat(shown).join("|");
        return [action, part ? part.path : "", Number(decoderHost.playbackGeneration), Number(decoderHost.playbackToken)].join("|");
    }
    // Play from the start, or carry on where it paused. Pause and Resume carry
    // the token of the recording open now: one pressed on a recording that has
    // moved on since does nothing. A tap whose target moved while its press
    // was held (a playlist on to another part or call) never reaches here.
    function toggle() {
        if (!supported || !part || storageAway || awaitingToken)
            return;
        var token = decoderHost.playbackToken;
        if (wantsPlay) {
            say("", false);
            if (token > 0)
                decoderHost.pausePlaybackFor(token);
            else if (ownsPlay)
                decoderHost.pausePlayback();
            return;
        }
        if (paused) {
            // A resume refused before is asked again; its sentence stays while
            // it stands, and a refusal read again (its count moved) is said again.
            if (decoderHost.playbackPausedReason !== "unavailable")
                say("", false);
            if (token > 0)
                decoderHost.resumePlaybackFor(token);
            else if (ownsPlay)
                decoderHost.resumePlayback();
            return;
        }
        playShown();
    }
    // The part shown, alone, as this block's own play. The host refuses one
    // with no reading only when its file is gone (what plays plays on, and
    // the store looks again).
    function playShown() {
        if (!supported || !part || storageAway)
            return;
        var path = part.path;
        say("", false);
        startOwned(path, function () {
            return decoderHost.playRecordingOwned(path);
        }, qsTr("This recording is gone."));
    }
    // Ask the host for a play of this block (@p ask answers its generation),
    // its first recording @p path: a failure published before the answer is
    // said, and so is a refusal with no reading, as @p refusedText when given.
    // A refusal that left this block's own play playing (the host refused the
    // new one before taking it) keeps it this block's, so the sheet's close
    // still stops it.
    function startOwned(path, ask, refusedText) {
        failureKey = "";
        failureNotice = "";
        startingPath = path;
        var generation = ask();
        startingPath = "";
        if (generation > 0)
            ownedGeneration = generation;
        else if (ownedGeneration !== decoderHost.playbackGeneration)
            ownedGeneration = 0;
        refusedPath = generation <= 0 && refusedReading(path) ? path : "";
        if (generation <= 0 && failureKey === "")
            say(refusedText !== undefined ? refusedText : qsTr("Couldn't start playing. Try again."), true);
    }
    function select(index) {
        current = Math.max(0, Math.min(index, parts.length - 1));
        currentPath = parts.length > 0 ? parts[current].path : "";
    }
    // Issue #909: what a tap on part chip @p index acts on (its pressKey): its
    // recording, by path and the identity the store indexed, and whether the
    // tap would play it or only select it. A reread that put another
    // recording at that place, or storage coming back that would turn a
    // select into a play, drops a tap held across it.
    function partKey(index) {
        var recording = parts[index];
        if (!recording)
            return "";
        var action = supported && recording.available !== false ? "play" : "select";
        return [action, recording.path, String(recording.identitySize), String(recording.mtimeSec), String(recording.mtimeNanos)].join("|");
    }
    // A chip: the part it names is shown, and played. B1: a part whose storage
    // is away is shown, with its away line, and not played; with no player
    // (B6) a chip only selects.
    function pickPart(index) {
        picked = true;
        select(index);
        say("", false);
        if (supported && !storageAway)
            playShown();
    }
    // Issue #909: the call's parts one after another as one play of the host
    // (B2), each with its identity from the store, the call's own numbering
    // and this block's title; a part whose storage is away is left out (B1).
    // The store lists every recording's identity: one without it is passed as
    // it is, and the host refuses the play.
    function startPlayAll() {
        if (!supported)
            return;
        var items = [];
        var first = -1;
        for (var i = 0; i < parts.length; i++) {
            var recording = parts[i];
            if (recording.available === false)
                continue;
            if (first < 0)
                first = i;
            items.push({path: recording.path, size: recording.identitySize, mtimeSec: recording.mtimeSec, mtimeNanos: recording.mtimeNanos, call: 0,
                partOrdinal: i + 1, partCount: parts.length, title: title, line: "", key: ""});
        }
        if (items.length === 0)
            return;
        picked = false;
        select(first);
        say("", false);
        // 1 is DecoderHost.PlaybackParts.
        startOwned(items[0].path, function () {
            return decoderHost.playRecordings(items, 1, "");
        });
    }
    // The chip shown moves to the recording that plays when it is one of these
    // parts: on a change of item, of the parts or the sheet opening, never while
    // a file action's picker or confirmation holds its target, and not once the
    // user picked a chip.
    function follow() {
        if (picked || savePicker.visible || confirmDelete.visible || !hostOpen)
            return;
        for (var i = 0; i < parts.length; i++) {
            if (playsFile(parts[i]) && parts[i].path !== missingAway) {
                if (i !== current) {
                    select(i);
                    // The selection moved (a dialog dismissed, the play
                    // moving on): reconciled once its bindings settle (R69),
                    // as a store update is (R64).
                    if (sheetShown)
                        reconcileLater.restart();
                }
                return;
            }
        }
    }
    // What a change of the host's item is told by; position ticks change none of it.
    readonly property string hostItem: [decoderHost.playbackGeneration, decoderHost.playbackPath, decoderHost.playbackSize, decoderHost.playbackMtimeSec, decoderHost.playbackMtimeNanos, decoderHost.playbackCallIndex, decoderHost.playbackPartOrdinal, hostOpen].join("|")
    onHostItemChanged: follow()
    // The sheet went away: the play this block started stops with it, since
    // nothing else shows it (a playlist it follows plays on). A failed one's
    // reading is cleared, as it is no one's now.
    function refusedReading(path) {
        return decoderHost.playbackState === 4 && decoderHost.playbackGeneration === 0 && decoderHost.playbackPath === path;
    }
    function stopOwnPlayback() {
        var refused = refusedPath;
        refusedPath = "";
        if (refused.length > 0 && refusedReading(refused)) {
            decoderHost.stopPlayback();
            return;
        }
        if (!ownsPlay)
            return;
        if (hostOpen)
            decoderHost.stopPlaybackGeneration(ownedGeneration);
        else if (decoderHost.playbackState === 4)
            decoderHost.stopPlayback();
    }
    // The sheet holding this block closed or opened again: a confirmation still
    // out ends, and forgets the file it named, which would otherwise come back
    // over the next call's sheet with the old call's file; a save picker still
    // out ends as a cancel (its recording goes again).
    function endDialogs() {
        confirmDelete.target = null;
        confirmDelete.visible = false;
        savePicker.cancel();
    }
    function reset() {
        endDialogs();
        current = 0;
        currentPath = "";
        picked = false;
        ownedGeneration = 0;
        refusedPath = "";
        failureKey = "";
        failureNotice = "";
        refusalSaid = "";
        refusalUnsaid = "";
        skipSaidGeneration = 0;
        skipSaidCount = 0;
        missingAway = "";
        notice = "";
        noticeProblem = false;
    }
    // The line can be read again (a dialog of the sheet closed, or its gate
    // opened): a refusal shown under it and not read is read now, once, if
    // it still stands, and only then marked heard; one the bar read
    // meanwhile is shown, not read again (noteFailure()).
    readonly property bool lineReachable: !!noticeLine && Navigation.allows(noticeLine) && sheetShown && appActive
    // The application is in the foreground and active (R50), followed from
    // the signal's own state as PlaylistStatus follows it: a refusal is read
    // only then, so one polled while the app is inactive (a notification's
    // Resume refused, the sheet still visible) waits, as the bar's does.
    property bool appActive: Qt.application.state === Qt.ApplicationActive
    Connections {
        target: Qt.application
        function onStateChanged(state) {
            player.appActive = state === Qt.ApplicationActive;
        }
    }
    onLineReachableChanged: {
        if (lineReachable && refusalUnsaid.length > 0)
            sayUnsaidLater.restart();
    }
    Timer {
        id: sayUnsaidLater
        interval: 0
        onTriggered: player.sayUnsaidRefusal()
    }
    function sayUnsaidRefusal() {
        // Unreachable again by now (another dialog opened): still pending, for
        // the next time the line can be read.
        if (!lineReachable)
            return;
        var pending = refusalUnsaid;
        refusalUnsaid = "";
        if (pending.length === 0 || pending !== failureKey)
            return;
        failureKey = "";
        noteFailure();
    }
    // A problem is read out once each time it is said, a repeat included, and
    // so is a result that is the only place a fact is given (@p readOut: where
    // a Download-folder save went). True when it was read out: a line under a
    // dialog of its own (the Delete confirmation) is shown but not read.
    function say(text, problem, readOut) {
        notice = text;
        noticeProblem = problem;
        if (problem || readOut === true)
            return Navigation.announce(player, noticeLine, text);
        return false;
    }

    function share() {
        if (!part)
            return;
        if (!decoderHost.shareRecording(part.path, title))
            say(qsTr("Couldn't share this recording. Try again, or use Save to file."), true);
        else
            say("", false);
    }
    function saveToFile() {
        if (!part || saveSelection > 0)
            return;
        var selection = recordings.selectRecordings([part.id]);
        if (selection.loading) {
            // Issue #902: refused, as an export is, while storage is read
            // again; nothing is selected or held.
            say(qsTr("Recordings are still loading. Try again in a moment."), true);
            return;
        }
        if (!selection.selection) {
            say(qsTr("This recording is gone."), true);
            return;
        }
        saveSelection = selection.selection;
        saveSelectionMap = selection;
        savePath = part.path;
        savePicker.suggestedName = part.fileName;
        say("", false);
        savePicker.open();
    }
    function savePicked(reference) {
        if (saveSelection <= 0) {
            // Not wanted any more: nothing will write a Download-folder row.
            if (savePicker.savedToDownloads)
                decoderHost.discardPendingDownload(reference);
            return;
        }
        saveReference = reference;
        decoderHost.saveRecording(saveSelectionMap, reference);
    }
    function saveCancelled() {
        if (saveSelection <= 0)
            return;
        recordings.releaseSelection(saveSelection);
        saveSelection = 0;
        savePath = "";
    }
    // No picker could ask (issue #829): the recording goes again, and the reason shows.
    function savePickUnavailable(message) {
        if (saveSelection <= 0)
            return;
        saveCancelled();
        say(message, true);
    }
    // DecoderHost::RecordingTransferResult for the save this block began.
    function saveFinished(selection, result) {
        if (selection !== saveSelection)
            return;
        // Everything about the save is taken and ended here, before the
        // shown-part check: the save is over whichever row shows now.
        var saved = savePicker.savedToDownloads ? decoderHost.savedDocumentName(saveReference) : "";
        saveSelection = 0;
        saveReference = "";
        var shown = false;
        for (var i = 0; i < parts.length; i++)
            shown = shown || parts[i].path === savePath;
        savePath = "";
        if (!shown)
            return;
        if (result === 0 && saved.length > 0)
            say(qsTr("Saved to Download/DSD-neo as %1.").arg(saved), false, true);
        else if (result === 0)
            say(qsTr("Saved."), false);
        else if (result === 3)
            say(qsTr("This recording is gone."), true);
        else if (result === 4)
            say(qsTr("An export is running. Save again when it finishes."), true);
        else if (result === 6)
            say(qsTr("This recording changed. Save it again."), true);
        else if (result !== 2)
            say(qsTr("Couldn't save the recording. Try another location."), true);
    }
    // Issue #909 (B10, B11): the confirmation's words and the file it deletes
    // are the ones shown at the press (its id and identity), however the parts
    // move while it is out. A recording listed without an identity is deleted
    // by its id alone, as before #909.
    function askDelete() {
        if (!part)
            return;
        var several = parts.length > 1;
        confirmDelete.target = {id: part.id, path: part.path, size: part.identitySize, mtimeSec: part.mtimeSec, mtimeNanos: part.mtimeNanos};
        confirmDelete.ordinal = several ? current + 1 : 0;
        confirmDelete.message = several ? qsTr("Part %1 is deleted from this phone. The other parts and the call stay in History.").arg(current + 1) : deleteMessage;
        confirmDelete.open();
    }
    function deleteConfirmed() {
        var target = confirmDelete.target;
        confirmDelete.target = null;
        if (!target)
            return;
        // Issue #902: refused while storage that came back is read again, or
        // while its storage is not connected; the recording plays on, as
        // nothing was deleted. Issue #909: and when the file under its id is
        // another one now, or the id is gone.
        var identified = target.size !== undefined && target.mtimeSec !== undefined && target.mtimeNanos !== undefined;
        var taken = identified ? recordings.deleteRecordingChecked(target.id, target.path, Number(target.size), Number(target.mtimeSec), Number(target.mtimeNanos))
                               : recordings.deleteRecording(target.id);
        if (taken === false) {
            var refusal = recordings.deleteRefusal();
            say(refusal === "changed" ? qsTr("This recording changed. Delete it again.")
                : refusal === "gone" ? qsTr("This recording is gone.")
                : refusal === "storageAway" ? qsTr("That storage isn't connected. Delete again once it's back.")
                : qsTr("Recordings are still loading. Try again in a moment."), true);
            return;
        }
        // A single recording's play of it stops, as the store waits for it.
        // A delete never stops parts or a playlist: the file open plays to its
        // end, and the queue goes on.
        if (decoderHost.playbackMode === 0 && decoderHost.playbackPath === target.path && hostOpen) {
            if (decoderHost.playbackGeneration > 0)
                decoderHost.stopPlaybackGeneration(decoderHost.playbackGeneration);
            else
                decoderHost.stopPlayback();
        }
        say("", false);
    }

    // Issue #829: a save picker still out belongs to the recording shown when
    // it was asked for. The block hidden (its sheet closed, its row gone) or
    // another recording shown ends it, and the recording goes again; a save
    // already writing runs on.
    onVisibleChanged: {
        if (!visible)
            endDialogs();
    }
    onPartChanged: {
        if (savePicker.visible && (!part || part.path !== savePath))
            savePicker.cancel();
    }

    onMissingPathChanged: {
        if (missingPath !== missingAway)
            missingAway = "";
    }
    onRecordingListChanged: {
        for (var i = 0; i < recordingList.length; i++) {
            if (missingPath !== "" && recordingList[i].path === missingPath && recordingList[i].available === false)
                missingAway = missingPath;
        }
    }

    onPartsChanged: {
        // What this block plays no longer listed: a single recording (the row
        // trimmed away, or its file gone from a call of several) stops, as no
        // control is left on screen for it. Its parts play on while the row is
        // there, even once it lists none (D9: a delete never stops a queue, and
        // the part open plays to its end); the row itself going stops them.
        if (ownsPlay && hostOpen) {
            var listed = false;
            for (var p = 0; p < parts.length; p++) {
                if (playsFile(parts[p]))
                    listed = true;
            }
            var rowThere = typeof rowPresent !== "function" || rowPresent();
            if (decoderHost.playbackMode === 0 ? !listed : !rowThere)
                decoderHost.stopPlaybackGeneration(ownedGeneration);
        }
        var kept = false;
        for (var i = 0; i < parts.length && !kept; i++) {
            if (parts[i].path === currentPath) {
                current = i;
                kept = true;
            }
        }
        if (!kept)
            select(current);
        follow();
        // Once the selection and the following have settled on a store
        // update, what stands for the part shown now is reconciled (R64).
        if (sheetShown)
            reconcileLater.restart();
    }
    // A refusal for a file no longer shown leaves the notice line; one that
    // now matches is shown, read once through the shared key. A paused play
    // sends no change that would do it. After the bindings the store update
    // moved (the part shown, whether it is the host's file); a Timer, which
    // dies with the block. Only while the sheet is shown: its opening
    // reconciles for itself.
    Timer {
        id: reconcileLater
        interval: 0
        onTriggered: {
            if (player.sheetShown)
                player.noteFailure();
        }
    }

    Connections {
        target: decoderHost
        ignoreUnknownSignals: true
        function onPlaybackChanged() {
            player.noteFailure();
            player.noteSkips();
            player.noteSeekLanding();
        }
        function onPlaybackPositionChanged() {
            player.noteSeekLanding();
        }
        function onRecordingTransferFinished(selection, result, missing) {
            player.saveFinished(selection, result);
        }
        function onRecordingShareFailed(path) {
            for (var i = 0; i < player.recordingList.length; i++) {
                if (player.recordingList[i].path === path) {
                    player.say(qsTr("Couldn't share this recording. Try again, or use Save to file."), true);
                    return;
                }
            }
        }
    }

    MicroLabel {
        objectName: "recordingHeader"
        width: parent.width
        wrapMode: Text.Wrap
        text: player.parts.length > 1 ? qsTr("Call recording · %1 parts").arg(player.parts.length) : qsTr("Call recording")
    }

    Item {
        width: parent.width
        height: Math.max(playButton.visible ? playButton.implicitHeight : 0, readout.implicitHeight)

        PlayCircle {
            id: playButton
            objectName: "recordingPlayButton"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            visible: player.supported
            featured: player.wantsPlay
            showPause: player.wantsPlay
            enabled: !player.storageAway && !player.awaitingToken
            accessibleName: player.wantsPlay ? qsTr("Pause recording") : player.paused ? qsTr("Resume recording") : qsTr("Play recording")
            pressKey: function () {
                return player.playKey();
            }
            onClicked: player.toggle()
        }

        Column {
            id: readout
            anchors.left: playButton.visible ? playButton.right : parent.left
            anchors.leftMargin: playButton.visible ? 14 : 0
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Text {
                id: recordingTimeText
                // Issue #902: its reach for the keyboard (PressGate).
                readonly property bool inReach: recordingTimeText.Accessible.focusable
                objectName: "recordingTime"
                width: parent.width
                // With no player there is no position: only the length.
                // With no known length, the elapsed time alone, as the bar shows it.
                text: !player.supported ? player.lengthText : player.durationMs > 0 ? Util.fill(qsTr("%1 / %2"), [player.positionText, player.lengthText]) : player.positionText
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(15)
                color: Theme.textPrimary
                Accessible.role: !Navigation.allows(recordingTimeText) || !visible ? Accessible.Pane : Accessible.StaticText
                Accessible.name: !Navigation.allows(recordingTimeText) || !visible ? "" : player.supported ? player.spokenTimeText : player.lengthText
                Accessible.focusable: Navigation.allows(recordingTimeText) && visible
                Accessible.ignored: !Navigation.allows(recordingTimeText) || !visible
            }

            // The position under the time, and a seek by tap or drag while the
            // recording is open. The bar takes a short slot; its touch area is the
            // full 48 px, centred on the track.
            Item {
                width: parent.width
                height: 20
                visible: player.supported

                Item {
                    id: seekBar
                    // Issue #902: its reach for the keyboard (PressGate).
                    readonly property bool inReach: Navigation.allows(seekBar) && seekBar.visible
                    objectName: "recordingSeekBar"

                    // Issue #909: as the host's player has it (B7: never with no length).
                    readonly property bool seekable: player.loaded && decoderHost.playbackCanSeek && !player.ownQueueAwaitingToken
                    property bool dragging: false
                    property real dragMs: 0
                    // The recording a drag began on: its seek is dropped once another one is open, and so is its
                    // preview of where it would land.
                    property real dragToken: 0
                    readonly property bool previewing: dragging && dragToken > 0 && dragToken === Number(decoderHost.playbackToken)
                    // Issue #909: the recording open when a press on the bar began; a tap or a drag that
                    // starts once another one is open does nothing.
                    property real pressToken: 0
                    readonly property real fraction: player.durationMs > 0 ? Math.max(0, Math.min(1, player.positionMs / player.durationMs)) : 0

                    width: parent.width
                    height: Theme.minimumTouchSize
                    y: (parent.height - height) / 2
                    enabled: seekable
                    // Held while it has the focus (see OutlineButton).
                    activeFocusOnTab: (Navigation.allows(seekBar) && visible && seekable) || activeFocus
                    Accessible.role: Accessible.Slider
                    Accessible.name: !navigationAllowed || !visible ? "" : qsTr("Position")
                    // Gated as the name is: covered, the pacer's reading is empty,
                    // which reads as "0:00 of …", a position the player is not at.
                    Accessible.description: !navigationAllowed || !visible ? "" : player.spokenTimeText
                    // Read when asked (Android polls it); a plain Item's value raises no
                    // ValueChanged, so it stays live (issue #782).
                    readonly property real value: player.positionMs / 1000
                    readonly property real minimumValue: 0
                    // With no known length, the position is the range's end, so the
                    // value stays within it (no seek is offered then).
                    readonly property real maximumValue: Math.max(player.durationMs, player.positionMs) / 1000
                    readonly property bool navigationAllowed: Navigation.allows(seekBar)
                    Accessible.ignored: !navigationAllowed || !visible
                    Accessible.onIncreaseAction: nudge(5000)
                    Accessible.onDecreaseAction: nudge(-5000)
                    Keys.onRightPressed: nudge(5000)
                    Keys.onLeftPressed: nudge(-5000)
                    // Around the bar's own slot, not the whole touch area, which
                    // overhangs the time above it.
                    FocusFrame {
                        objectName: "recordingSeekFocus"
                        anchors.fill: undefined
                        x: -2
                        y: (parent.height - height) / 2
                        width: parent.width + 4
                        height: 22
                    }

                    function msAt(x) {
                        return Math.round(Math.max(0, Math.min(1, x / Math.max(1, track.width))) * player.durationMs);
                    }
                    // A seek carries the token of the recording open when its gesture
                    // began (@p token): one that has moved on since drops it.
                    // Only a seek the host takes (its token still the one open) is
                    // waited for as a landing.
                    function seekTo(ms, token) {
                        if (seekable && Navigation.allows(seekBar)) {
                            var target = Math.max(0, Math.min(player.durationMs, ms));
                            if (token > 0 && token === decoderHost.playbackToken) {
                                player.seekLandingsBefore = Number(decoderHost.playbackSeeksLanded) || 0;
                                player.seekTargetMs = target;
                                // Each request waits its own 2 s, the same
                                // target asked again included (R76).
                                seekWait.restart();
                            }
                            decoderHost.seekPlaybackFor(token, target);
                        }
                    }
                    // A step a screen reader sends a covered or hidden slider it still
                    // holds does nothing. A step is from the open recording's own
                    // position, never from a pointer drag's preview.
                    function nudge(delta) {
                        if (!Navigation.allows(seekBar) || !seekBar.visible)
                            return;
                        seekTo(player.openPositionMs + delta, decoderHost.playbackToken);
                    }

                    Rectangle {
                        id: track
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        height: 4
                        radius: 2
                        color: Theme.toggleOffTrack
                        opacity: seekBar.seekable ? 1 : 0.6

                        Rectangle {
                            width: parent.width * seekBar.fraction
                            height: parent.height
                            radius: 2
                            color: Theme.cyan
                        }
                    }

                    Rectangle {
                        visible: seekBar.seekable
                        x: Math.round(track.width * seekBar.fraction - width / 2)
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        radius: 7
                        color: Theme.cyan
                        border.width: 2
                        border.color: Theme.panel
                    }

                    GatedTapHandler {
                        enabled: seekBar.seekable
                        onPressBegan: seekBar.pressToken = Number(decoderHost.playbackToken)
                        onAllowedTap: function (eventPoint) {
                            if (seekBar.pressToken > 0 && seekBar.pressToken === Number(decoderHost.playbackToken))
                                seekBar.seekTo(seekBar.msAt(eventPoint.position.x), seekBar.pressToken);
                        }
                    }
                    // Not a tap: a drag that began on a sheet over the bar takes the
                    // grab from the sheet's barrier, so it stands down under one.
                    DragHandler {
                        id: drag
                        enabled: Navigation.allows(seekBar) && seekBar.seekable
                        target: null
                        xAxis.enabled: true
                        yAxis.enabled: false
                        onActiveChanged: {
                            if (active) {
                                // Begun on a recording that is no longer the one open: no drag.
                                if (seekBar.pressToken <= 0 || seekBar.pressToken !== Number(decoderHost.playbackToken))
                                    return;
                                seekBar.dragToken = seekBar.pressToken;
                                seekBar.dragMs = seekBar.msAt(centroid.position.x);
                                seekBar.dragging = true;
                            } else if (seekBar.dragging) {
                                seekBar.dragging = false;
                                seekBar.seekTo(seekBar.dragMs, seekBar.dragToken);
                            }
                        }
                        onCentroidChanged: {
                            if (active && seekBar.dragging)
                                seekBar.dragMs = seekBar.msAt(centroid.position.x);
                        }
                    }
                }
            }

            // "Opening…", "Paused" or why it paused. Its line is kept, empty, while there is
            // none, so the player is as tall stopped, opening, playing and
            // paused: a sheet centred on its content (History's, a detection's
            // recordings) would otherwise move under the finger at a tap. An
            // empty line is not read.
            Text {
                id: recordingStatusText
                // Issue #902: its reach for the keyboard (PressGate).
                readonly property bool inReach: recordingStatusText.Accessible.focusable
                objectName: "recordingStatus"
                width: parent.width
                // With no player nothing plays, opens or pauses.
                visible: player.supported
                height: Math.max(implicitHeight, statusLine.height)
                text: player.statusText()
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(12)
                color: Theme.textSubdued
                Accessible.role: !Navigation.allows(recordingStatusText) || !visible || text.length === 0 ? Accessible.Pane : Accessible.StaticText
                Accessible.name: !Navigation.allows(recordingStatusText) || !visible || text.length === 0 ? "" : text
                Accessible.focusable: Navigation.allows(recordingStatusText) && visible && text.length !== 0
                Accessible.ignored: !Navigation.allows(recordingStatusText) || !visible || text.length === 0
                FontMetrics {
                    id: statusLine
                    font: recordingStatusText.font
                }
            }
        }
    }

    ReadableText {
        id: recordingProblemText
        objectName: "recordingProblem"
        width: parent.width
        visible: text.length > 0
        text: player.problemText()
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: player.playState === 4 || player.storageAway ? Theme.alert : Theme.textSecondary
        Accessible.role: !reachable ? Accessible.Pane : (player.playState === 4 || player.storageAway ? Accessible.AlertMessage : Accessible.StaticText)
    }

    // A call heard in pieces: a chip per part, the one shown checked, and Play all
    // as an action of its own under them. A chip selects its part, and plays it
    // where it can (pickPart()).
    Flow {
        width: parent.width
        spacing: 8
        visible: player.parts.length > 1

        Repeater {
            model: player.parts.length > 1 ? player.parts.length : 0
            OutlineButton {
                required property int index
                objectName: "recordingPart" + index
                text: player.partLabel(index)
                checkable: true
                checked: index === player.current
                pressKey: function () {
                    return player.partKey(index);
                }
                onClicked: player.pickPart(index)
            }
        }

    }

    OutlineButton {
        objectName: "recordingPlayAll"
        width: parent.width
        visible: player.parts.length > 1 && player.supported
        text: qsTr("Play all parts")
        // The parts whose storage is there (B1).
        enabled: player.anyAvailable
        onClicked: player.startPlayAll()
    }

    Row {
        width: parent.width
        spacing: 8

        OutlineButton {
            objectName: "recordingShare"
            visible: decoderHost.shareSupported
            width: (parent.width - parent.spacing) / 2
            text: qsTr("Share")
            enabled: !player.storageAway
            pressKey: function () {
                return player.shownKey();
            }
            onClicked: player.share()
        }
        OutlineButton {
            objectName: "recordingSave"
            width: decoderHost.shareSupported ? (parent.width - parent.spacing) / 2 : parent.width
            text: qsTr("Save to file")
            enabled: !player.storageAway && player.saveSelection === 0
            pressKey: function () {
                return player.shownKey();
            }
            onClicked: player.saveToFile()
        }
    }

    OutlineButton {
        objectName: "recordingDelete"
        width: parent.width
        text: player.parts.length > 1 ? qsTr("Delete part %1").arg(player.current + 1) : qsTr("Delete recording")
        textColor: Theme.magenta
        enabled: !player.storageAway
        pressKey: function () {
            return player.shownKey();
        }
        onClicked: player.askDelete()
    }

    DocumentPicker {
        id: savePicker
        objectName: "recordingSavePicker"
        mode: "save"
        mimeType: "audio/x-wav"
        nameFilters: [qsTr("WAV audio") + " (*.wav)"]
        defaultSuffix: "wav"
        // Following waits while the picker is out (B10), and catches up after.
        onVisibleChanged: {
            if (!visible)
                player.follow();
        }
        onAccepted: function (reference, displayName) {
            player.savePicked(reference);
        }
        onRejected: player.saveCancelled()
        onUnavailable: function (reason, message) {
            player.savePickUnavailable(message);
        }
    }

    ConfirmDialog {
        id: confirmDelete
        objectName: "recordingDeleteConfirm"
        parent: player.dialogParent
        // The file pressed on and its part number (0: a recording alone), kept from the press.
        property var target: null
        property int ordinal: 0
        title: ordinal > 0 ? qsTr("Delete part %1?").arg(ordinal) : qsTr("Delete this recording?")
        confirmText: ordinal > 0 ? qsTr("Delete part %1").arg(ordinal) : qsTr("Delete recording")
        destructive: true
        onConfirmed: player.deleteConfirmed()
        onCancelled: target = null
        onVisibleChanged: {
            if (!visible)
                player.follow();
        }
    }
}
