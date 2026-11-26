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
    // DecoderHost::PlaybackState: 0 stopped, 1 preparing, 2 playing, 3 paused, 4 failed.
    readonly property int playState: loaded ? decoderHost.playbackState : 0
    // Issue #909 (B4): halted for another app's audio, carrying on by itself
    // after it: wanted still, so its button pauses it (that cancels the carry-on).
    readonly property bool interrupted: loaded && decoderHost.playbackInterrupted
    // Still opening, with nothing wanted (paused while it opens, or moved
    // while paused): the player names why it pauses, as the media session
    // offers Play then; it reads paused, not opening.
    readonly property bool pausedOpening: playState === 1 && String(decoderHost.playbackPausedReason || "").length > 0
    readonly property bool paused: (playState === 3 && !interrupted) || pausedOpening
    readonly property bool wantsPlay: (playState === 1 && !pausedOpening) || playState === 2 || (playState === 3 && interrupted)
    // A play of someone else's (a playlist) still opening, with no recording
    // token yet: a pause or resume from here could reach nothing, so none is
    // offered until it has one.
    readonly property bool awaitingToken: loaded && playState >= 1 && playState <= 3 && !ownsPlay && Number(decoderHost.playbackToken) <= 0
    readonly property real durationMs: loaded && decoderHost.playbackDurationMs > 0 ? decoderHost.playbackDurationMs : part ? part.durationMs : 0
    readonly property real positionMs: seekBar.dragging ? seekBar.dragMs : loaded ? Math.min(decoderHost.playbackPositionMs, durationMs) : 0
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
    readonly property bool hostOpen: decoderHost.playbackState >= 1 && decoderHost.playbackState <= 3
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
    // sent before the host applied the seek does not count).
    property int seekLanded: 0
    property real seekTargetMs: -1
    // A seek the host never applied (it clamped or dropped it) is forgotten
    // when playback stops or fails, or moves to another part: a later
    // position passing near it is not a landing.
    readonly property string seekScope: [loaded, part ? part.path : "", playState === 0 || playState === 4].join("|")
    onSeekScopeChanged: seekTargetMs = -1
    SpeechPacer {
        id: positionSpeech
        shown: Navigation.allows(player) && player.visible
        step: [player.playState, player.loaded, player.part ? player.part.path : "", player.durationMs, player.seekLanded].join("|")
        read: function () {
            return String(player.positionMs);
        }
    }
    readonly property real spokenPositionMs: positionSpeech.spoken.length > 0 ? Number(positionSpeech.spoken) : 0
    readonly property string spokenTimeText: Util.fill(qsTr("%1 of %2"), [timeTextAt(spokenPositionMs), lengthText])

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
        var error = decoderHost.playbackError;
        return error === "missing" ? qsTr("This recording is gone.")
            : error === "unavailable" ? unavailableText
            : error === "unsupported" ? qsTr("Recordings can't be played on this device.")
            : qsTr("Can't play this recording. The file may be damaged.");
    }
    // Each failure is said once, and read out; the line clears once the reading
    // that said it has moved on (a resume the platform took after all). A
    // resume refused again reads the same but for the host's refusal count,
    // which makes it a failure of its own, said again.
    function noteFailure() {
        var text = failureText();
        var key = text.length === 0 ? "" : [decoderHost.playbackGeneration, decoderHost.playbackToken, decoderHost.playbackState, decoderHost.playbackError, decoderHost.playbackPausedReason, decoderHost.playbackPath, decoderHost.playbackRefusals].join("|");
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
        if (text.length > 0)
            say(text, true);
        else if (said.length > 0 && notice === said && noticeProblem)
            say("", false);
    }

    // Issue #909: what the play button would do now and what it would do it
    // to: its action, the host's play and recording, and the part shown.
    function playGesture() {
        return {action: wantsPlay ? "pause" : paused ? "resume" : "play", generation: Number(decoderHost.playbackGeneration), token: Number(decoderHost.playbackToken), path: part ? part.path : ""};
    }
    // The gesture a press on the play button began, for its tap.
    property var playPress: null
    // Play from the start, or carry on where it paused. Pause and Resume carry
    // the token of the recording open when they were pressed: one pressed on a
    // recording that has moved on since does nothing. A tap acts on what its
    // press began on (@p pressed, from playGesture()): one whose target moved
    // while the press was held (a playlist on to another part or call) is
    // dropped, never turned into another action on another recording.
    function toggle(pressed) {
        if (!supported || !part || storageAway || awaitingToken)
            return;
        var now = playGesture();
        if (pressed && (pressed.action !== now.action || pressed.generation !== now.generation || pressed.token !== now.token || pressed.path !== now.path))
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
            items.push({path: recording.path, size: recording.identitySize !== undefined ? Number(recording.identitySize) : -1, mtimeSec: Number(recording.mtimeSec) || 0, mtimeNanos: Number(recording.mtimeNanos) || 0, call: 0,
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
                if (i !== current)
                    select(i);
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
    function reset() {
        current = 0;
        currentPath = "";
        picked = false;
        ownedGeneration = 0;
        refusedPath = "";
        failureKey = "";
        failureNotice = "";
        missingAway = "";
        notice = "";
        noticeProblem = false;
    }
    // A problem is read out once each time it is said, a repeat included, and
    // so is a result that is the only place a fact is given (@p readOut: where
    // a Download-folder save went).
    function say(text, problem, readOut) {
        notice = text;
        noticeProblem = problem;
        if (problem || readOut === true)
            Navigation.announce(player, noticeLine, text);
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
    // move while it is out.
    function askDelete() {
        if (!part)
            return;
        var several = parts.length > 1;
        confirmDelete.target = {id: part.id, path: part.path, size: part.identitySize !== undefined ? Number(part.identitySize) : -1, mtimeSec: Number(part.mtimeSec) || 0, mtimeNanos: Number(part.mtimeNanos) || 0};
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
        if (recordings.deleteRecordingChecked(target.id, target.size, target.mtimeSec, target.mtimeNanos) === false) {
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
            savePicker.cancel();
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
        // control is left on screen for it. Its parts play on while the row
        // lists any recording (a part deleted plays to its end and the queue
        // goes on), and stop once it lists none.
        if (ownsPlay && hostOpen) {
            var listed = false;
            for (var p = 0; p < parts.length; p++) {
                if (playsFile(parts[p]))
                    listed = true;
            }
            if (decoderHost.playbackMode === 0 ? !listed : parts.length === 0)
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
    }

    Connections {
        target: decoderHost
        ignoreUnknownSignals: true
        function onPlaybackChanged() {
            player.noteFailure();
        }
        function onPlaybackPositionChanged() {
            if (player.seekTargetMs >= 0 && Math.abs(decoderHost.playbackPositionMs - player.seekTargetMs) < 1000) {
                player.seekTargetMs = -1;
                player.seekLanded++;
            }
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
            onPressBegan: player.playPress = player.playGesture()
            onClicked: {
                var pressed = playButton.tapping ? player.playPress : null;
                player.playPress = null;
                player.toggle(pressed);
            }
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
                text: player.supported ? Util.fill(qsTr("%1 / %2"), [player.positionText, player.lengthText]) : player.lengthText
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
                    readonly property bool seekable: player.loaded && decoderHost.playbackCanSeek
                    property bool dragging: false
                    property real dragMs: 0
                    // The recording a drag began on: its seek is dropped once another one is open.
                    property real dragToken: 0
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
                    readonly property real maximumValue: player.durationMs / 1000
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
                            if (token > 0 && token === decoderHost.playbackToken)
                                player.seekTargetMs = target;
                            decoderHost.seekPlaybackFor(token, target);
                        }
                    }
                    // A step a screen reader sends a covered or hidden slider it still
                    // holds does nothing.
                    function nudge(delta) {
                        if (!Navigation.allows(seekBar) || !seekBar.visible)
                            return;
                        seekTo(player.positionMs + delta, decoderHost.playbackToken);
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
            onClicked: player.share()
        }
        OutlineButton {
            objectName: "recordingSave"
            width: decoderHost.shareSupported ? (parent.width - parent.spacing) / 2 : parent.width
            text: qsTr("Save to file")
            enabled: !player.storageAway && player.saveSelection === 0
            onClicked: player.saveToFile()
        }
    }

    OutlineButton {
        objectName: "recordingDelete"
        width: parent.width
        text: player.parts.length > 1 ? qsTr("Delete part %1").arg(player.current + 1) : qsTr("Delete recording")
        textColor: Theme.magenta
        enabled: !player.storageAway
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
