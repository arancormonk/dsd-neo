// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window
import "Util.js" as Util

// Issue #809: the recordings of one Signal search detection, opened from its
// Recent detections row. The player sits under the title, on the recording
// selected, so it is in the same place however long the list below it runs;
// the list is newest first, and a tap on a row selects that recording and
// plays it, as a part's chip does in History. Call details hands over to the
// selected recording's History details while History holds its call, and is
// disabled, saying why, while it does not: always there, so selecting another
// recording never moves the list under the finger.
//
// The store answers by the detection's key (`<session>:<hit>`), and every
// change to its recordings moves `recordings.revision`: the list is read again
// then, only while the sheet is open. The selection follows its recording by
// id; when that recording leaves (deleted, or trimmed by the storage limit) the
// row that takes its place is selected, the next older one, or the one before
// it (newer) when it was the oldest, and the sheet closes when none is left. A
// recording whose storage is away stays listed and says so.
ModalSheet {
    id: sheet
    objectName: "detectionRecordingsSheet"
    // The detection, as its row's recordingKey names it, and its frequency in MHz.
    property string detectionKey: ""
    property string frequencyText: ""
    // The recording selected, by id; the ids listed last, newest first, so a
    // recording that leaves hands the selection to the one after it.
    property string selectedId: ""
    property var listedIds: []
    // The History row the selected recording's call is in now, 0 when none is
    // (never logged, trimmed or cleared). History changes it without the
    // store, so it is resolved again on each History change while open.
    property double detailsUid: 0
    signal callDetailsRequested(var record)
    accessibleName: qsTr("Recordings, %1 MHz").arg(frequencyText)

    // Closed, the list is empty, so no row is built (issue #782).
    readonly property var recordingList: {
        var revision = recordings.revision;
        return visible && detectionKey.length > 0 ? recordings.recordingsForDetection(detectionKey) : [];
    }
    readonly property var selected: {
        for (var i = 0; i < recordingList.length; i++) {
            if (String(recordingList[i].id) === selectedId)
                return recordingList[i];
        }
        return null;
    }

    function openFor(hit, mhz) {
        recordingPlayer.reset();
        detectionKey = String(hit.recordingKey || "");
        frequencyText = mhz;
        selectedId = "";
        listedIds = [];
        open();
    }
    /** "14:03:22 · 0:42": when the recording started, and its length. */
    function timeLine(recording) {
        var start = Number(recording.start) || 0;
        var length = Util.playbackLengthText(recording.durationMs);
        return start > 0 ? qsTr("%1 · %2").arg(Qt.formatDateTime(new Date(start * 1000), "hh:mm:ss")).arg(length) : length;
    }
    /** Who the call was, as the recording names it: a number reads as the
     * screen's "Target"/"Source" do, then the system (an analog recording's
     * mode). A recording that names none of them says so of itself: History
     * may still hold its call (Call details), so the line speaks only of what
     * was recorded with it. */
    function identityLine(recording) {
        var number = /^[0-9]+$/;
        var target = String(recording.target || "");
        var source = String(recording.source || "");
        var parts = [number.test(target) ? qsTr("Target %1").arg(target) : target, number.test(source) ? qsTr("Source %1").arg(source) : source,
            String(recording.system || "")].filter(function (part) {
            return part.length > 0;
        });
        return parts.length > 0 ? parts.join(" · ") : qsTr("No target or source recorded");
    }
    function rowLine(recording) {
        return [timeLine(recording), identityLine(recording)].concat(recording.available === false ? [qsTr("Storage not available")] : []).join("\n");
    }
    function indexOfId(ids, id) {
        for (var i = 0; i < ids.length; i++) {
            if (ids[i] === id)
                return i;
        }
        return -1;
    }
    /** The recording the selection passes to when the selected one left
     * @p before for @p ids, by place in the list (newest first): the one now in
     * its place, the next older one still there, else the nearest newer one
     * when it was the oldest, else (nothing selected yet) the newest. */
    function successor(before, ids) {
        var at = indexOfId(before, selectedId);
        for (var i = at + 1; at >= 0 && i < before.length; i++) {
            if (indexOfId(ids, before[i]) >= 0)
                return before[i];
        }
        for (i = at - 1; i >= 0; i--) {
            if (indexOfId(ids, before[i]) >= 0)
                return before[i];
        }
        return ids[0];
    }
    // The list changed: keep the selection on its recording, or hand it on.
    function follow() {
        if (!visible)
            return;
        var ids = recordingList.map(function (recording) {
            return String(recording.id);
        });
        var before = listedIds;
        listedIds = ids;
        if (ids.length === 0) {
            emptied.restart();
            return;
        }
        if (indexOfId(ids, selectedId) >= 0)
            return;
        var window = sheet.Window.window;
        var focusInList = selectedId.length > 0 && window !== null && Navigation.contains(rowList, window.activeFocusItem);
        selectedId = successor(before, ids);
        if (focusInList)
            focusMove.restart();
    }
    onRecordingListChanged: follow()
    onSelectedChanged: resolveDetails()
    onVisibleChanged: {
        if (!visible)
            recordingPlayer.stopOwnPlayback();
    }
    function resolveDetails() {
        var fragment = selected ? String(selected.fragment || "") : "";
        detailsUid = fragment.length > 0 ? callHistory.uidForFragment(fragment) : 0;
    }
    function choose(id) {
        selectedId = id;
        if (recordingPlayer.parts.length > 0 && !recordingPlayer.storageAway)
            recordingPlayer.playPart(0);
    }
    function showDetails() {
        resolveDetails();
        if (detailsUid <= 0)
            return;
        var record = callHistory.recordFor(String(detailsUid));
        visible = false;
        callDetailsRequested(record);
    }
    function focusSelected() {
        for (var i = 0; i < rowRepeater.count; i++) {
            var row = rowRepeater.itemAt(i);
            if (row && row.key === selectedId) {
                row.forceActiveFocus();
                return;
            }
        }
    }
    // The last recording gone, the sheet closes, once the list's change is over.
    Timer {
        id: emptied
        interval: 0
        onTriggered: {
            if (sheet.recordingList.length === 0)
                sheet.visible = false;
        }
    }
    // Focus follows the selection to its new row once the rows have settled.
    Timer {
        id: focusMove
        interval: 0
        onTriggered: sheet.focusSelected()
    }
    Connections {
        target: callHistory
        enabled: sheet.visible
        ignoreUnknownSignals: true
        function onDataChanged() {
            sheet.resolveDetails();
        }
        function onRowPositionsChanged() {
            sheet.resolveDetails();
        }
        function onModelReset() {
            sheet.resolveDetails();
        }
        function onRowsRemoved() {
            sheet.resolveDetails();
        }
    }

    FontMetrics { id: identityMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(13) }
    /** @p text wrapped only between its values, as the search screen's
     * details are (SignalSearchScreen.keepValues()): a value that fits @p width
     * with the separator it carries keeps its spaces whole, so no line starts
     * with a separator. One that does not keeps its spaces and wraps at its
     * words: kept whole, the line would break inside it, before its separator. */
    function keepValues(text, width) {
        var values = text.split(" · ");
        return values.map(function (value, i) {
            var carried = i + 1 < values.length ? value + " ·" : value;
            return identityMetrics.advanceWidth(carried) > width ? value : value.replace(/ /g, "\u00a0");
        }).join("\u00a0· ");
    }

    ReadableText {
        objectName: "detectionRecordingsTitle"
        heading: true
        width: parent.width
        text: qsTr("%1 MHz").arg(sheet.frequencyText)
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(17)
        font.weight: Font.Bold
    }
    CallRecordingPlayer {
        id: recordingPlayer
        objectName: "detectionRecordingPlayer"
        width: parent.width
        recordingList: sheet.selected ? [sheet.selected] : []
        dialogParent: sheet
        title: sheet.selected && Number(sheet.selected.start) > 0 ? qsTr("%1 MHz · %2").arg(sheet.frequencyText).arg(Qt.formatDateTime(new Date(Number(sheet.selected.start) * 1000), "yyyy-MM-dd hh:mm")) : qsTr("%1 MHz").arg(sheet.frequencyText)
    }
    OutlineButton {
        id: callDetailsButton
        objectName: "detectionCallDetails"
        width: parent.width
        enabled: sheet.detailsUid > 0
        text: qsTr("Call details")
        Accessible.description: enabled ? "" : callDetailsReason.text
        onClicked: sheet.showDetails()
    }
    // Why Call details is disabled, as History's Hold says why. Its line is
    // kept while the button is enabled, only not shown or read, so the list
    // below stays where it is whichever recording is selected.
    MicroLabel {
        id: callDetailsReason
        objectName: "detectionCallDetailsReason"
        width: parent.width
        wrapMode: Text.Wrap
        opacity: callDetailsButton.enabled ? 0 : 1
        Accessible.focusable: navigationAllowed && visible && !callDetailsButton.enabled
        Accessible.ignored: !navigationAllowed || !visible || callDetailsButton.enabled
        text: qsTr("This call is not in History")
    }
    // The list's own section, named as the player's is.
    MicroLabel {
        objectName: "detectionRecordingsCount"
        width: parent.width
        topPadding: 6
        wrapMode: Text.Wrap
        text: sheet.recordingList.length === 1 ? qsTr("1 recording") : qsTr("%1 recordings").arg(Util.countText(sheet.recordingList.length))
    }
    // A row per recording, kept on its recording across refreshes
    // (StableRows): each change to the store hands over a new list, and a list
    // model would rebuild every row a screen reader is told about.
    Column {
        id: rowList
        objectName: "detectionRecordingList"
        width: parent.width
        spacing: 8
        Repeater {
            id: rowRepeater
            model: StableRows {
                id: rows
                entries: sheet.recordingList
                keyOf: function (recording) {
                    return String(recording.id);
                }
                lineOf: function (recording) {
                    return sheet.rowLine(recording);
                }
            }
            Rectangle {
                id: row
                required property int index
                required property string key
                required property string line
                readonly property var lines: line.split("\n")
                readonly property bool live: index >= 0 && index < rows.live
                readonly property bool selected: live && key === sheet.selectedId
                objectName: "detectionRecordingRow"
                width: parent.width
                height: Math.max(Theme.minimumTouchSize, texts.height + 20)
                // A row past the end of a shrinking list goes at the end of the
                // turn; until then it is not drawn and reads as nothing.
                opacity: index >= 0 && index < rows.shown ? 1 : 0
                radius: Theme.radiusButton
                // Selected: the checked look of an OutlineButton, a heavier
                // border and the play mark History rows carry, so it shows
                // without its colour too.
                color: selected || (tap.pressed && live) ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
                border.width: selected ? 2 : 1
                border.color: selected ? Theme.cyan : Theme.controlBorder
                activeFocusOnTab: (navigationAllowed && visible && live) || activeFocus
                Accessible.role: Accessible.Button
                Accessible.name: navigationAllowed && visible && live ? (selected ? qsTr("%1, selected") : "%1").arg(lines.join(", ")) : ""
                Accessible.focusable: navigationAllowed && visible && live
                readonly property bool navigationAllowed: Navigation.allows(row)
                Accessible.ignored: !navigationAllowed || !visible || !live
                Accessible.onPressAction: activate()
                function activate() {
                    if (live && Navigation.allows(row))
                        sheet.choose(key);
                }
                Keys.onSpacePressed: activate()
                Keys.onReturnPressed: activate()
                Keys.onEnterPressed: activate()
                FocusFrame {}
                TapHandler {
                    id: tap
                    onTapped: row.activate()
                }
                PlayGlyph {
                    id: selectedMark
                    objectName: "detectionRecordingSelectedMark"
                    visible: row.selected
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                }
                // The row's words are read in its own name (`lines`), so these
                // stay out of the tree. Their width leaves the play mark's room
                // on every row, so selecting one moves no text.
                Column {
                    id: texts
                    x: 14
                    width: parent.width - 28 - selectedMark.implicitWidth - 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Text {
                        objectName: "detectionRecordingTime"
                        Accessible.ignored: true
                        width: parent.width
                        text: row.lines[0]
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.textPrimary
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(15)
                    }
                    // What the air delivered (aliases), so plain text.
                    Text {
                        objectName: "detectionRecordingIdentity"
                        Accessible.ignored: true
                        width: parent.width
                        text: {
                            var font = identityMetrics.font;
                            return sheet.keepValues(row.lines.length > 1 ? row.lines[1] : "", width);
                        }
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                    }
                    Text {
                        objectName: "detectionRecordingAway"
                        Accessible.ignored: true
                        width: parent.width
                        visible: row.lines.length > 2
                        text: row.lines.length > 2 ? row.lines[2] : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.textSubdued
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                    }
                }
            }
        }
    }
    OutlineButton {
        objectName: "detectionRecordingsClose"
        width: parent.width
        text: qsTr("Close")
        onClicked: sheet.visible = false
    }
}
