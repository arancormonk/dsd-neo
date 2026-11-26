// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import "Util.js" as Util

ModalSheet {
    id: sheet
    property var record: ({})
    readonly property string rowUid: record.systemUid || ""
    readonly property double rowTg: record.tg || 0
    // Who the call was to (CallHistoryModel "gi"): 0 a talkgroup, 1 a radio
    // (a private call), -1 not known (issue #800).
    readonly property int rowGi: record.gi === 0 || record.gi === 1 ? record.gi : -1
    // The session is the single saved system it was started as: no scan, running
    // or paused, and the operator never took the tuner off it (issue #671) -- an
    // Explore or a manual tune keeps the session's UID for the calls heard
    // before it, but those are no longer what the receiver is on.
    readonly property bool singleSystemSession: metrics.scanKind === 0 && !metrics.operatorTuned
    readonly property bool canHold: decoderHost.running && metrics.optionsKnown && singleSystemSession && rowTg > 0 && rowUid.length > 0 && rowUid === callHistory.sessionUid
    readonly property bool heldHere: metrics.heldTg > 0 && metrics.heldTg === rowTg
    // Issue #809: false where the receiver takes no hold (Signal search, which
    // refuses receiver commands): the Hold button and its reason are hidden.
    property bool showHold: true
    // Issue #800: "Show on map" for a row that saved a position. False where
    // the map is not reachable from the sheet's screen (Signal search, whose
    // layers differ). The sheet closes first; its host opens the map with the
    // row's uid, and Back from the map opens the sheet again by it.
    property bool showMap: true
    readonly property bool rowHasPosition: !!sheet.record.position && sheet.record.position.latitude !== undefined
    // What had the focus when the sheet opened (its row), kept past the
    // close a "Show on map" makes, for the focus to come back to.
    property var mapOpener: null
    signal showOnMapRequested(var record)
    // Issue #667: an analog reception has no access code. Its tone ("CTCSS
    // 100.0 Hz") takes the code's line, and what it was heard in ("Analog
    // NFM", "Analog AM", "Analog" when not known) the Encrypted line's.
    readonly property bool analogRow: sheet.record.analog === true
    readonly property string codeLine: analogRow ? (sheet.record.receivedTone || "") : (sheet.record.accessCodeText || "")
    readonly property string kindLine: analogRow ? (sheet.record.analogMode ? qsTr("Analog %1").arg(sheet.record.analogMode) : qsTr("Analog")) : sheet.record.enc ? qsTr("Encrypted") : qsTr("Unencrypted")
    // Issue #799: the line after "Frequency" names the site and channel the
    // call was heard on, in the Monitor site row's words ("DMR · LPCN 12").
    // Every line keeps its place, shown or not, so the site's is fixed.
    readonly property int siteLineIndex: 9
    readonly property var detailLines: [sheet.record.name || "", sheet.record.when > 0 ? Qt.formatDateTime(new Date(sheet.record.when * 1000), "yyyy-MM-dd hh:mm:ss") : "", sheet.record.systemName || "", sheet.record.channel || "", Util.targetLine(sheet.record.tg, sheet.rowGi), sheet.record.sourceName || "", sheet.record.src > 0 ? qsTr("Radio ID %1").arg(Util.idText(sheet.record.src)) : "", sheet.codeLine, sheet.record.freqHz > 0 ? qsTr("Frequency %1").arg(Util.fmtMhz(sheet.record.freqHz)) : "", sheet.record.siteText || "", sheet.record.emergency ? qsTr("Emergency") : "", sheet.kindLine, sheet.record.durationSecs === 1 ? qsTr("Duration: 1 second") : sheet.record.durationSecs >= 0 ? qsTr("Duration: %1 seconds").arg(sheet.record.durationSecs) : "", sheet.record.detail || ""]
    // Issue #708: the row's call recordings, read again by its uid whenever the
    // log changes, so a part folded into the call or a recording deleted shows
    // while the sheet is open.
    readonly property double rowUidKey: record.uid || 0
    property int recordingsRevision: 0
    readonly property var rowRecordings: {
        var revision = recordingsRevision;
        return rowUidKey > 0 && typeof callHistory.recordingsForUid === "function" ? callHistory.recordingsForUid(rowUidKey) : [];
    }
    // Issue #909: "Play from here", on History's own sheet only (false where
    // the list the playlist is taken from is not the one under the sheet:
    // Monitor, Home, Signal search). Shown while the host has a player and the
    // row has a recording that can be played and is among the rows History
    // shows. The press asks History first (playFromHereRequested), so a search
    // still being typed is part of what the list shows. A row whose only
    // recordings wait for their delete still shows it: the press answers that
    // nothing of this call can be played now.
    property bool showPlayFromHere: false
    signal playFromHereRequested
    readonly property bool rowPlayable: {
        for (var i = 0; i < rowRecordings.length; i++) {
            if (rowRecordings[i].available !== false)
                return true;
        }
        return false;
    }
    readonly property bool rowShown: {
        var revision = recordingsRevision;
        if (!showPlayFromHere || rowUidKey <= 0 || typeof historyView === "undefined" || !historyView)
            return false;
        var shownRows = historyView.count;
        var filter = historyView.filterText + "|" + historyView.filterSystem + "|" + historyView.filterKind + "|" + historyView.historySession;
        var row = callHistory.rowForUid(rowUidKey);
        return row >= 0 && historyView.mapFromSource(callHistory.index(row, 0)).valid;
    }
    accessibleName: qsTr("Activity details")
    // Issue #909: the row is still in the log (not trimmed or cleared), whatever recordings it lists.
    function rowListed() {
        return rowUidKey > 0 && callHistory.rowForUid(rowUidKey) >= 0;
    }
    function open(value) {
        recordingPlayer.reset();
        record = value;
        recordingsRevision++;
        visible = true;
        recordingPlayer.follow();
        // What stands for the part shown now (a resume refused) is said as it opens.
        recordingPlayer.noteFailure();
    }
    // The calls History shows, from this one: closed once the host took them
    // (the bar under the list shows them from then on); a press refused says
    // why here, and what played plays on.
    function playFromHere() {
        if (!decoderHost.playbackSupported || rowUidKey <= 0)
            return;
        playFromHereRequested();
        var outcome = historyPlaylist.start(rowUidKey) || {};
        var result = String(outcome.result || "");
        if (result === "accepted") {
            visible = false;
            return;
        }
        recordingPlayer.say(result === "rowNone" ? qsTr("This call's recording can't be played now.")
            : result === "gone" ? qsTr("This call isn't in the list now.")
            : result === "none" ? qsTr("None of the shown calls has a recording that can be played now.")
            : result === "unsupported" ? qsTr("Recordings can't be played on this device.")
            : qsTr("Couldn't start playing. Try again."), true);
    }
    // Issue #799: the record follows its row by uid while the sheet is open, so
    // a fragment folded in after it opened (a longer duration, a newer frequency,
    // the site) shows. A row that has left the log keeps the last copy read.
    function reread() {
        if (typeof callHistory.recordFor !== "function")
            return;
        var fresh = callHistory.recordFor(rowUidKey);
        if (fresh.uid !== undefined && JSON.stringify(fresh) !== JSON.stringify(record))
            record = fresh;
    }
    onVisibleChanged: {
        if (!visible)
            recordingPlayer.stopOwnPlayback();
    }
    Connections {
        target: callHistory
        enabled: sheet.visible && sheet.rowUidKey > 0
        ignoreUnknownSignals: true
        function onDataChanged(topLeft, bottomRight) {
            sheet.recordingsRevision++;
            var row = callHistory.rowForUid(sheet.rowUidKey);
            if (row >= topLeft.row && row <= bottomRight.row)
                sheet.reread();
        }
        function onRowPositionsChanged() {
            sheet.recordingsRevision++;
            sheet.reread();
        }
        function onModelReset() {
            sheet.recordingsRevision++;
            sheet.reread();
        }
    }
    Repeater {
        model: sheet.detailLines
        Text {
            id: detailLine
            // Issue #902: its reach for the keyboard (PressGate).
            readonly property bool inReach: detailLine.Accessible.focusable
            required property string modelData
            required property int index
            readonly property bool siteLine: index === sheet.siteLineIndex
            objectName: siteLine ? "historyDetailSite" : ""
            width: parent.width
            visible: modelData.length > 0
            Accessible.role: !navigationAllowed || !visible ? Accessible.Pane : Accessible.StaticText
            Accessible.name: !navigationAllowed || !visible ? "" : modelData
            Accessible.focusable: navigationAllowed && visible
            readonly property bool navigationAllowed: Navigation.allows(sheet)
            Accessible.ignored: !navigationAllowed || !visible
            // The site line wraps only between its parts: each part's spaces,
            // and the one before a separator's dot, do not break, so "Net 12"
            // stays whole and a wrapped line ends on its dot.
            text: siteLine ? modelData.split(" · ").map(function (part) {
                return part.replace(/ /g, "\u00a0");
            }).join("\u00a0· ") : modelData
            textFormat: Text.PlainText
            wrapMode: siteLine ? Text.Wrap : Text.WrapAnywhere
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
        rowPresent: sheet.rowListed
        dialogParent: sheet
        noticeLine: recordingNotice
        title: sheet.record.when > 0 ? Util.fill(qsTr("%1 · %2"), [sheet.record.name || "", Qt.formatDateTime(new Date(sheet.record.when * 1000), "yyyy-MM-dd hh:mm")]) : (sheet.record.name || "")
    }
    // What the recording's last action came to, and why Play from here could
    // not start: right under the recording, and shown when the recording has
    // gone too (B3).
    ReadableText {
        id: recordingNotice
        objectName: "recordingNotice"
        width: parent.width
        visible: text.length > 0
        text: recordingPlayer.notice
        // Names a saved file, which may hold markup characters.
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: recordingPlayer.noticeProblem ? Theme.alert : Theme.textSecondary
        Accessible.role: !reachable ? Accessible.Pane : (recordingPlayer.noticeProblem ? Accessible.AlertMessage : Accessible.StaticText)
    }
    OutlineButton {
        id: playFromHereButton
        objectName: "historyPlayFromHere"
        width: parent.width
        visible: sheet.showPlayFromHere && decoderHost.playbackSupported && sheet.rowPlayable && sheet.rowShown
        text: qsTr("Play from here")
        Accessible.description: !navigationAllowed || !visible ? "" : qsTr("Plays this call, then each newer call History shows")
        onClicked: sheet.playFromHere()
    }
    OutlineButton {
        id: holdButton
        objectName: "historyHoldButton"
        width: parent.width
        visible: sheet.showHold && sheet.rowTg > 0
        // A private call holds its radio, as the monitor's does; never called a talkgroup.
        text: sheet.heldHere ? qsTr("Release hold") : sheet.rowGi === 1 ? qsTr("Hold radio %1").arg(Util.idText(sheet.rowTg))
            : sheet.rowGi === 0 ? qsTr("Hold TG %1").arg(Util.idText(sheet.rowTg)) : qsTr("Hold %1").arg(Util.idText(sheet.rowTg))
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
        objectName: "historyShowOnMap"
        width: parent.width
        visible: sheet.showMap && sheet.rowHasPosition
        text: qsTr("Show on map")
        onClicked: {
            var record = sheet.record;
            sheet.mapOpener = sheet.previousFocus;
            sheet.visible = false;
            sheet.showOnMapRequested(record);
        }
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Close")
        onClicked: sheet.visible = false
    }
}
