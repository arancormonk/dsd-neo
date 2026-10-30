// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import "Util.js" as Util

ModalSheet {
    id: sheet
    property var record: ({})
    readonly property string rowUid: record.systemUid || ""
    readonly property double rowTg: record.tg || 0
    // The session is the single saved system it was started as: no scan, running
    // or paused, and the operator never took the tuner off it (issue #671) -- an
    // Explore or a manual tune keeps the session's UID for the calls heard
    // before it, but those are no longer what the receiver is on.
    readonly property bool singleSystemSession: metrics.scanKind === 0 && !metrics.operatorTuned
    readonly property bool canHold: decoderHost.running && metrics.optionsKnown && singleSystemSession && rowTg > 0 && rowUid.length > 0 && rowUid === callHistory.sessionUid
    readonly property bool heldHere: metrics.heldTg > 0 && metrics.heldTg === rowTg
    readonly property var detailLines: [sheet.record.name || "", sheet.record.when > 0 ? Qt.formatDateTime(new Date(sheet.record.when * 1000), "yyyy-MM-dd hh:mm:ss") : "", sheet.record.systemName || "", sheet.record.channel || "", sheet.record.tg > 0 ? qsTr("Talkgroup %1").arg(Util.idText(sheet.record.tg)) : "", sheet.record.sourceName || "", sheet.record.src > 0 ? qsTr("Radio ID %1").arg(Util.idText(sheet.record.src)) : "", sheet.record.accessCodeText || "", sheet.record.freqHz > 0 ? qsTr("Frequency %1").arg(Util.fmtMhz(sheet.record.freqHz)) : "", sheet.record.emergency ? qsTr("Emergency") : "", sheet.record.enc ? qsTr("Encrypted") : qsTr("Unencrypted"), sheet.record.durationSecs === 1 ? qsTr("Duration: 1 second") : sheet.record.durationSecs >= 0 ? qsTr("Duration: %1 seconds").arg(sheet.record.durationSecs) : "", sheet.record.detail || "", sheet.record.receivedTone ? qsTr("Received tone %1").arg(sheet.record.receivedTone) : ""]
    // Issue #708: the row's call recordings, read again by its uid whenever the
    // log changes, so a part folded into the call or a recording deleted shows
    // while the sheet is open.
    readonly property double rowUidKey: record.uid || 0
    property int recordingsRevision: 0
    readonly property var rowRecordings: {
        var revision = recordingsRevision;
        return rowUidKey > 0 && typeof callHistory.recordingsForUid === "function" ? callHistory.recordingsForUid(rowUidKey) : [];
    }
    accessibleName: qsTr("Activity details")
    function open(value) {
        recordingPlayer.reset();
        record = value;
        recordingsRevision++;
        visible = true;
    }
    onVisibleChanged: {
        if (!visible)
            recordingPlayer.stopOwnPlayback();
    }
    Connections {
        target: callHistory
        enabled: sheet.visible && sheet.rowUidKey > 0
        ignoreUnknownSignals: true
        function onDataChanged() {
            sheet.recordingsRevision++;
        }
        function onRowPositionsChanged() {
            sheet.recordingsRevision++;
        }
        function onModelReset() {
            sheet.recordingsRevision++;
        }
    }
    Repeater {
        model: sheet.detailLines
        Text {
            required property string modelData
            width: parent.width
            visible: modelData.length > 0
            Accessible.role: Accessible.StaticText
            Accessible.name: modelData
            readonly property bool navigationAllowed: Navigation.allows(sheet)
            Accessible.ignored: !navigationAllowed || !visible
            text: modelData
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
        }
    }
    CallRecordingPlayer {
        id: recordingPlayer
        objectName: "historyRecording"
        width: parent.width
        recordingList: sheet.rowRecordings
        dialogParent: sheet
        title: sheet.record.when > 0 ? qsTr("%1 · %2").arg(sheet.record.name || "").arg(Qt.formatDateTime(new Date(sheet.record.when * 1000), "yyyy-MM-dd hh:mm")) : (sheet.record.name || "")
    }
    OutlineButton {
        id: holdButton
        objectName: "historyHoldButton"
        width: parent.width
        visible: sheet.rowTg > 0
        text: sheet.heldHere ? qsTr("Release hold") : qsTr("Hold TG %1").arg(Util.idText(sheet.rowTg))
        enabled: decoderHost.running && (sheet.canHold || sheet.heldHere)
        onClicked: {
            if (commands.holdTalkgroup(sheet.heldHere ? 0 : sheet.rowTg))
                sheet.visible = false;
        }
    }
    MicroLabel {
        objectName: "historyHoldReason"
        width: parent.width
        wrapMode: Text.Wrap
        visible: holdButton.visible && !holdButton.enabled
        text: !decoderHost.running ? qsTr("Start this system to hold") : !metrics.optionsKnown ? qsTr("Waiting for session settings") : !sheet.singleSystemSession || callHistory.sessionUid.length === 0 ? qsTr("Hold needs a single saved system") : qsTr("Heard on another system")
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Close")
        onClicked: sheet.visible = false
    }
}
