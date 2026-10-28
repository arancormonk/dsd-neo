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
// block at once; the store drops it from the row soon after.
Column {
    id: player

    // The row's recordings (RecordingStore maps), oldest first.
    property var recordingList: []
    // Shared with another app and offered as a file name, with the part's own name.
    property string title: ""
    // Where the delete confirmation opens: the sheet, so it dims the whole screen.
    property Item dialogParent: null

    property int current: 0
    property bool playAll: false
    // What the last share, save or delete came to; empty when there is nothing to say.
    property string notice: ""
    property bool noticeProblem: false
    // The selection a Save to file holds while its picker and transfer run, 0 while none.
    property int saveSelection: 0
    property var saveSelectionMap: ({})

    readonly property var parts: {
        var list = [];
        var gone = decoderHost.playbackState === 4 && decoderHost.playbackError === "missing" ? decoderHost.playbackPath : "";
        for (var i = 0; i < recordingList.length; i++) {
            if (recordingList[i].path !== gone)
                list.push(recordingList[i]);
        }
        return list;
    }
    readonly property var part: parts.length > 0 ? parts[Math.min(current, parts.length - 1)] : null
    readonly property bool loaded: part !== null && decoderHost.playbackPath === part.path
    // DecoderHost::PlaybackState: 0 stopped, 1 preparing, 2 playing, 3 paused, 4 failed.
    readonly property int playState: loaded ? decoderHost.playbackState : 0
    readonly property bool wantsPlay: playState === 1 || playState === 2
    readonly property real durationMs: loaded && decoderHost.playbackDurationMs > 0 ? decoderHost.playbackDurationMs : part ? part.durationMs : 0
    readonly property real positionMs: seekBar.dragging ? seekBar.dragMs : loaded ? Math.min(decoderHost.playbackPositionMs, durationMs) : 0
    readonly property bool storageAway: part !== null && part.available === false

    visible: parts.length > 0
    spacing: 10

    function partLabel(index) {
        return qsTr("Part %1 · %2").arg(index + 1).arg(Util.playbackTimeText(parts[index].durationMs));
    }
    function statusText() {
        if (!part)
            return "";
        if (storageAway)
            return qsTr("The storage this recording is on isn't available right now.");
        if (playState === 1)
            return qsTr("Opening…");
        if (playState === 3)
            return qsTr("Paused");
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
        decoderHost.playRecording(part.path);
    }
    function playPart(index) {
        playAll = false;
        current = index;
        notice = "";
        decoderHost.playRecording(parts[index].path);
    }
    function startPlayAll() {
        current = 0;
        notice = "";
        playAll = true;
        decoderHost.playRecording(parts[0].path);
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
            current = current + 1;
            decoderHost.playRecording(parts[current].path);
            return;
        }
        playAll = false;
    }
    // The sheet went away: what it played stops with it, since nothing else shows it.
    function stopOwnPlayback() {
        playAll = false;
        for (var i = 0; i < recordingList.length; i++) {
            if (decoderHost.playbackPath === recordingList[i].path && decoderHost.playbackState !== 0) {
                decoderHost.stopPlayback();
                return;
            }
        }
    }
    function reset() {
        current = 0;
        playAll = false;
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
    }
    // DecoderHost::RecordingTransferResult for the save this block began.
    function saveFinished(selection, result) {
        if (selection !== saveSelection)
            return;
        saveSelection = 0;
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
        current = 0;
        recordings.deleteRecording(target.id);
        say("", false);
    }

    onRecordingListChanged: {
        if (current >= parts.length)
            current = Math.max(0, parts.length - 1);
    }

    Connections {
        target: decoderHost
        ignoreUnknownSignals: true
        function onPlaybackChanged() {
            player.followHost();
        }
        function onPlaybackPositionChanged() {
            player.followHost();
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
                text: qsTr("%1 / %2").arg(Util.playbackTimeText(player.positionMs)).arg(Util.playbackTimeText(player.durationMs))
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(15)
                color: Theme.textPrimary
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("%1 of %2").arg(Util.playbackTimeText(player.positionMs)).arg(Util.playbackTimeText(player.durationMs))
            }

            Text {
                objectName: "recordingStatus"
                width: parent.width
                visible: text.length > 0
                text: player.statusText()
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(12)
                color: player.playState === 4 || player.storageAway ? Theme.alert : Theme.textSubdued
                Accessible.role: player.playState === 4 ? Accessible.AlertMessage : Accessible.StaticText
                Accessible.name: text
            }
        }
    }

    // The position, and a seek by tap or drag while the recording is open.
    Item {
        id: seekBar
        objectName: "recordingSeekBar"

        readonly property bool seekable: player.loaded && (player.playState === 2 || player.playState === 3) && player.durationMs > 0
        property bool dragging: false
        property real dragMs: 0
        readonly property real fraction: player.durationMs > 0 ? Math.max(0, Math.min(1, player.positionMs / player.durationMs)) : 0

        width: parent.width
        height: Theme.minimumTouchSize
        enabled: seekable
        activeFocusOnTab: Navigation.allows(seekBar) && seekable
        Accessible.role: Accessible.Slider
        Accessible.name: qsTr("Position")
        Accessible.description: qsTr("%1 of %2").arg(Util.playbackTimeText(player.positionMs)).arg(Util.playbackTimeText(player.durationMs))
        readonly property real value: player.positionMs / 1000
        readonly property real minimumValue: 0
        readonly property real maximumValue: player.durationMs / 1000
        readonly property bool navigationAllowed: Navigation.allows(seekBar)
        Accessible.ignored: !navigationAllowed || !visible
        Accessible.onIncreaseAction: nudge(5000)
        Accessible.onDecreaseAction: nudge(-5000)
        Keys.onRightPressed: nudge(5000)
        Keys.onLeftPressed: nudge(-5000)
        FocusFrame {}

        function msAt(x) {
            return Math.round(Math.max(0, Math.min(1, x / Math.max(1, track.width))) * player.durationMs);
        }
        function seekTo(ms) {
            if (seekable && Navigation.allows(seekBar))
                decoderHost.seekPlayback(Math.max(0, Math.min(player.durationMs, ms)));
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

    // A call heard in pieces: each part, and Play all.
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

        OutlineButton {
            objectName: "recordingPlayAll"
            text: qsTr("Play all")
            checkable: true
            checked: player.playAll
            enabled: !player.storageAway
            onClicked: player.startPlayAll()
        }
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
