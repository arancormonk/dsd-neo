// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Dialogs
import "Util.js" as Util

// Issue #708: a call's recordings in its detail sheet: play and pause, where it
// is and how long it runs, a bar to seek, the parts of a call heard in several
// pieces with Play all, and Share, Save to file and Delete.
//
// The host plays one recording at a time (its playback readings). This block
// reads it as its own only while the host's path is one of its parts, so a
// recording another sheet started reads as stopped here. The host keeps no
// queue: Play all advances when a part stops at its end (the path kept and the
// position at the length), and a stop the user asked for clears the path, which
// ends Play all too. A recording the host found gone ("missing") leaves the
// block at once; the store drops it from the row soon after, or, when its
// storage went, lists it as away, and from then on the store's availability
// speaks for it: it shows again, away, and plays once the storage is back.
Column {
    id: player

    // The row's recordings (RecordingStore maps), oldest first.
    property var recordingList: []
    // Shared with another app and offered as a file name, with the part's own name.
    property string title: ""
    // Where the delete confirmation opens: the sheet, so it dims the whole screen.
    property Item dialogParent: null

    // The part shown, kept on its recording by path: a part that leaves (deleted,
    // or found missing) moves no other part's selection, and the index every label
    // uses is always one that exists.
    property int current: 0
    property string currentPath: ""
    property bool playAll: false
    // The recording this block last asked the host to play: it stops with the
    // sheet even once the row's list no longer names it (a History trim).
    property string ownedPath: ""
    // What the last share, save or delete came to; empty when there is nothing to say.
    property string notice: ""
    property bool noticeProblem: false
    // The selection a Save to file holds while its picker and transfer run, 0 while none.
    property int saveSelection: 0
    property var saveSelectionMap: ({})
    // The recording that save is of: its end is reported only while that
    // recording is shown here, never on another row's sheet.
    property string savePath: ""
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
    readonly property bool loaded: part !== null && decoderHost.playbackPath === part.path && part.path !== missingAway
    // DecoderHost::PlaybackState: 0 stopped, 1 preparing, 2 playing, 3 paused, 4 failed.
    readonly property int playState: loaded ? decoderHost.playbackState : 0
    readonly property bool wantsPlay: playState === 1 || playState === 2
    readonly property real durationMs: loaded && decoderHost.playbackDurationMs > 0 ? decoderHost.playbackDurationMs : part ? part.durationMs : 0
    readonly property real positionMs: seekBar.dragging ? seekBar.dragMs : loaded ? Math.min(decoderHost.playbackPositionMs, durationMs) : 0
    readonly property bool storageAway: part !== null && part.available === false

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
    // host reports where it landed.
    property int seekLanded: 0
    property bool seekPending: false
    SpeechPacer {
        id: positionSpeech
        shown: player.visible
        step: [player.playState, player.loaded, player.part ? player.part.path : "", player.durationMs, player.seekLanded].join("|")
        read: function () {
            return String(player.positionMs);
        }
    }
    readonly property real spokenPositionMs: positionSpeech.spoken.length > 0 ? Number(positionSpeech.spoken) : 0
    readonly property string spokenTimeText: qsTr("%1 of %2").arg(timeTextAt(spokenPositionMs)).arg(lengthText)

    function partLabel(index) {
        return qsTr("Part %1 · %2").arg(index + 1).arg(Util.playbackLengthText(parts[index].durationMs));
    }
    // A short state, beside the time.
    function statusText() {
        if (!part || storageAway)
            return "";
        if (playState === 1)
            return qsTr("Opening…");
        if (playState === 3)
            return qsTr("Paused");
        return "";
    }
    // Why it cannot play, or what is wrong with it: a sentence, under the player.
    function problemText() {
        if (!part)
            return "";
        if (storageAway)
            return qsTr("The storage this recording is on isn't available right now.");
        if (playState === 4) {
            var error = decoderHost.playbackError;
            return error === "unavailable" ? qsTr("Can't play right now. Another app has the audio, such as a phone call.")
                : error === "unsupported" ? qsTr("Recordings can't be played on this device.")
                : qsTr("Can't play this recording. The file may be damaged.");
        }
        if (part.failed)
            return qsTr("Saving stopped early, so the end may be missing.");
        return "";
    }

    // Play from the start, or carry on where it paused.
    function toggle() {
        if (!part || storageAway)
            return;
        notice = "";
        if (wantsPlay) {
            decoderHost.pausePlayback();
            return;
        }
        if (playState === 3) {
            decoderHost.resumePlayback();
            return;
        }
        play(part.path);
    }
    function play(path) {
        ownedPath = path;
        decoderHost.playRecording(path);
    }
    function select(index) {
        current = Math.max(0, Math.min(index, parts.length - 1));
        currentPath = parts.length > 0 ? parts[current].path : "";
    }
    function playPart(index) {
        playAll = false;
        select(index);
        notice = "";
        play(parts[current].path);
    }
    function startPlayAll() {
        select(0);
        notice = "";
        playAll = true;
        play(parts[0].path);
    }
    // The host's news: a part that stopped at its end hands over to the next one
    // while Play all runs, and a stop of any other kind ends Play all.
    function followHost() {
        if (!playAll)
            return;
        var stopped = decoderHost.playbackState === 0 || decoderHost.playbackState === 4;
        if (!stopped || !part)
            return;
        var atEnd = decoderHost.playbackState === 0 && decoderHost.playbackPath === part.path && decoderHost.playbackDurationMs > 0 && decoderHost.playbackPositionMs >= decoderHost.playbackDurationMs;
        if (atEnd && current + 1 < parts.length) {
            select(current + 1);
            play(parts[current].path);
            return;
        }
        playAll = false;
    }
    // The sheet went away: what it played stops with it, since nothing else shows it.
    function stopOwnPlayback() {
        playAll = false;
        if (ownedPath !== "" && decoderHost.playbackPath === ownedPath && decoderHost.playbackState !== 0)
            decoderHost.stopPlayback();
    }
    function reset() {
        current = 0;
        currentPath = "";
        playAll = false;
        ownedPath = "";
        missingAway = "";
        notice = "";
        noticeProblem = false;
    }
    function say(text, problem) {
        notice = text;
        noticeProblem = problem;
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
        if (!selection.selection) {
            say(qsTr("This recording is gone."), true);
            return;
        }
        saveSelection = selection.selection;
        saveSelectionMap = selection;
        savePath = part.path;
        savePicker.selectedFile = part.fileName;
        say("", false);
        savePicker.open();
    }
    function savePicked(reference) {
        if (saveSelection <= 0)
            return;
        decoderHost.saveRecording(saveSelectionMap, reference);
    }
    function saveCancelled() {
        if (saveSelection <= 0)
            return;
        recordings.releaseSelection(saveSelection);
        saveSelection = 0;
        savePath = "";
    }
    // DecoderHost::RecordingTransferResult for the save this block began.
    function saveFinished(selection, result) {
        if (selection !== saveSelection)
            return;
        saveSelection = 0;
        var shown = false;
        for (var i = 0; i < parts.length; i++)
            shown = shown || parts[i].path === savePath;
        savePath = "";
        if (!shown)
            return;
        if (result === 0)
            say(qsTr("Saved."), false);
        else if (result === 3)
            say(qsTr("This recording is gone."), true);
        else if (result === 4)
            say(qsTr("An export is running. Save again when it finishes."), true);
        else if (result !== 2)
            say(qsTr("Couldn't save the recording. Try another location."), true);
    }
    function askDelete() {
        if (!part)
            return;
        confirmDelete.target = part;
        confirmDelete.message = parts.length > 1 ? qsTr("Part %1 is deleted from this phone. The other parts and the call stay in History.").arg(current + 1) : qsTr("The recording is deleted from this phone. The call stays in History.");
        confirmDelete.open();
    }
    function deleteConfirmed() {
        var target = confirmDelete.target;
        confirmDelete.target = null;
        if (!target)
            return;
        if (decoderHost.playbackPath === target.path && decoderHost.playbackState !== 0)
            decoderHost.stopPlayback();
        playAll = false;
        recordings.deleteRecording(target.id);
        say("", false);
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
        // The part it plays no longer listed (the row trimmed away, or that
        // part's file gone from a call of several): what this block plays
        // stops, as no control is left on screen for it.
        var listed = false;
        for (var p = 0; p < parts.length; p++) {
            if (parts[p].path === ownedPath)
                listed = true;
        }
        if (!listed && ownedPath !== "" && decoderHost.playbackPath === ownedPath && (decoderHost.playbackState === 1 || decoderHost.playbackState === 2 || decoderHost.playbackState === 3)) {
            playAll = false;
            decoderHost.stopPlayback();
        }
        for (var i = 0; i < parts.length; i++) {
            if (parts[i].path === currentPath) {
                current = i;
                return;
            }
        }
        select(current);
    }

    Connections {
        target: decoderHost
        ignoreUnknownSignals: true
        function onPlaybackChanged() {
            player.followHost();
        }
        function onPlaybackPositionChanged() {
            player.followHost();
            if (player.seekPending) {
                player.seekPending = false;
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
        height: Math.max(playButton.implicitHeight, readout.implicitHeight)

        PlayCircle {
            id: playButton
            objectName: "recordingPlayButton"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            featured: player.wantsPlay
            showPause: player.wantsPlay
            enabled: !player.storageAway
            accessibleName: player.wantsPlay ? qsTr("Pause recording") : player.playState === 3 ? qsTr("Resume recording") : qsTr("Play recording")
            onClicked: player.toggle()
        }

        Column {
            id: readout
            anchors.left: playButton.right
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Text {
                objectName: "recordingTime"
                width: parent.width
                text: qsTr("%1 / %2").arg(player.positionText).arg(player.lengthText)
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(15)
                color: Theme.textPrimary
                Accessible.role: Accessible.StaticText
                Accessible.name: player.spokenTimeText
            }

            // The position under the time, and a seek by tap or drag while the
            // recording is open. The bar takes a short slot; its touch area is the
            // full 48 px, centred on the track.
            Item {
                width: parent.width
                height: 20

                Item {
                    id: seekBar
                    objectName: "recordingSeekBar"

                    readonly property bool seekable: player.loaded && (player.playState === 2 || player.playState === 3) && player.durationMs > 0
                    property bool dragging: false
                    property real dragMs: 0
                    readonly property real fraction: player.durationMs > 0 ? Math.max(0, Math.min(1, player.positionMs / player.durationMs)) : 0

                    width: parent.width
                    height: Theme.minimumTouchSize
                    y: (parent.height - height) / 2
                    enabled: seekable
                    activeFocusOnTab: Navigation.allows(seekBar) && seekable
                    Accessible.role: Accessible.Slider
                    Accessible.name: qsTr("Position")
                    Accessible.description: player.spokenTimeText
                    readonly property real value: player.spokenPositionMs / 1000
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
                    function seekTo(ms) {
                        if (seekable && Navigation.allows(seekBar)) {
                            player.seekPending = true;
                            decoderHost.seekPlayback(Math.max(0, Math.min(player.durationMs, ms)));
                        }
                    }
                    function nudge(delta) {
                        seekTo(player.positionMs + delta);
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

                    TapHandler {
                        enabled: seekBar.seekable
                        onTapped: function (eventPoint) {
                            seekBar.seekTo(seekBar.msAt(eventPoint.position.x));
                        }
                    }
                    DragHandler {
                        id: drag
                        enabled: seekBar.seekable
                        target: null
                        xAxis.enabled: true
                        yAxis.enabled: false
                        onActiveChanged: {
                            if (active) {
                                seekBar.dragMs = seekBar.msAt(centroid.position.x);
                                seekBar.dragging = true;
                            } else if (seekBar.dragging) {
                                seekBar.dragging = false;
                                seekBar.seekTo(seekBar.dragMs);
                            }
                        }
                        onCentroidChanged: {
                            if (active)
                                seekBar.dragMs = seekBar.msAt(centroid.position.x);
                        }
                    }
                }
            }

            Text {
                objectName: "recordingStatus"
                width: parent.width
                visible: text.length > 0
                text: player.statusText()
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(12)
                color: Theme.textSubdued
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }
    }

    Text {
        objectName: "recordingProblem"
        width: parent.width
        visible: text.length > 0
        text: player.problemText()
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: player.playState === 4 || player.storageAway ? Theme.alert : Theme.textSecondary
        Accessible.role: player.playState === 4 || player.storageAway ? Accessible.AlertMessage : Accessible.StaticText
        Accessible.name: text
    }

    // A call heard in pieces: a chip per part, the one shown checked, and Play all
    // as an action of its own under them.
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
                onClicked: player.playPart(index)
            }
        }

    }

    OutlineButton {
        objectName: "recordingPlayAll"
        width: parent.width
        visible: player.parts.length > 1
        text: qsTr("Play all parts")
        enabled: !player.storageAway
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

    Text {
        objectName: "recordingNotice"
        width: parent.width
        visible: text.length > 0
        text: player.notice
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: player.noticeProblem ? Theme.alert : Theme.textSecondary
        Accessible.role: player.noticeProblem ? Accessible.AlertMessage : Accessible.StaticText
        Accessible.name: text
    }

    FileDialog {
        id: savePicker
        objectName: "recordingSavePicker"
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("WAV audio") + " (*.wav)"]
        defaultSuffix: "wav"
        onAccepted: player.savePicked(selectedFile.toString())
        onRejected: player.saveCancelled()
    }

    ConfirmDialog {
        id: confirmDelete
        objectName: "recordingDeleteConfirm"
        parent: player.dialogParent
        property var target: null
        title: player.parts.length > 1 ? qsTr("Delete part %1?").arg(player.current + 1) : qsTr("Delete this recording?")
        confirmText: player.parts.length > 1 ? qsTr("Delete part %1").arg(player.current + 1) : qsTr("Delete recording")
        destructive: true
        onConfirmed: player.deleteConfirmed()
        onCancelled: target = null
    }
}
