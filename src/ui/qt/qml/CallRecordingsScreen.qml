// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Dialogs
import QtQuick.Window
import "Util.js" as Util

// Issue #708: the call recordings on this phone, one row per saved system, scan
// list or Explore, and the ones an earlier run left unfinished. A row exports
// its recordings as one zip for a date range, or deletes them; the screen
// exports or deletes everything. History keeps its calls either way.
//
// An export holds its files from the selection to its end (the store), so the
// picker comes after the selection: an empty range says so without asking for a
// file, and a picker dismissed lets the files go again.
Item {
    id: screen
    signal closed

    // The export this screen began, 0 while none: its result is this screen's to show.
    property int exportSelection: 0
    property var exportSelectionMap: ({})
    property string notice: ""
    property bool noticeProblem: false
    // What the open folder menu, the date choice and the delete confirmation are about.
    property var subject: null
    // What the storage limit removed and which deletes failed, taken from the store
    // while this screen shows them; taking them acknowledges them, so Settings stops
    // repeating what the user has seen here.
    property int removedShown: 0
    property int failuresShown: 0
    property string failureReason: ""
    readonly property int unseenRemoved: recordings.removedByLimit
    readonly property int unseenFailures: recordings.deleteFailures

    readonly property var folders: {
        var list = [];
        var all = recordings.folders || [];
        for (var i = 0; i < all.length; i++) {
            if (all[i].count > 0)
                list.push(all[i]);
        }
        return list;
    }
    readonly property bool empty: folders.length === 0 && recordings.unfinishedCount === 0
    readonly property bool exporting: exportSelection > 0 && decoderHost.recordingTransferActive
    readonly property var presetKeys: ["today", "yesterday", "last7", "last30", "all"]
    readonly property var presetNames: [qsTr("Today"), qsTr("Yesterday"), qsTr("Last 7 days"), qsTr("Last 30 days"), qsTr("All")]

    Keys.onEscapePressed: Navigation.back(screen.Window.window)
    Keys.onBackPressed: Navigation.back(screen.Window.window)
    // The layer clears input during activation; take focus after its bindings settle.
    onVisibleChanged: {
        if (!visible)
            return;
        removedShown = 0;
        failuresShown = 0;
        failureReason = "";
        takeNotices();
        Qt.callLater(function () {
            if (screen.visible && Navigation.allows(screen))
                screen.forceActiveFocus();
        });
    }
    // Later, not inside the store's change: taking them changes the same readings.
    Component.onCompleted: Qt.callLater(takeNotices)
    onUnseenRemovedChanged: Qt.callLater(takeNotices)
    onUnseenFailuresChanged: Qt.callLater(takeNotices)

    function takeNotices() {
        if (!visible || (unseenRemoved <= 0 && unseenFailures <= 0))
            return;
        removedShown += Math.max(0, unseenRemoved);
        if (unseenFailures > 0) {
            failuresShown += unseenFailures;
            failureReason = recordings.lastDeleteError;
        }
        recordings.acknowledgeNotices();
    }

    function spanText(folder) {
        var now = new Date(savedSystems.realtimeNowMs());
        var format = function (stamp) {
            var date = new Date(stamp * 1000);
            return Qt.formatDate(date, date.getFullYear() === now.getFullYear() ? "d MMM" : "d MMM yyyy");
        };
        var first = format(folder.first);
        var last = format(folder.last);
        return first === last ? first : qsTr("%1 – %2").arg(first).arg(last);
    }
    function folderText(folder) {
        var counted = folder.count === 1 ? qsTr("1 recording") : qsTr("%1 recordings").arg(Util.countText(folder.count));
        var parts = [counted, Util.sizeText(folder.bytes)];
        if (folder.available === false)
            parts.push(qsTr("storage not available"));
        else if (folder.first > 0)
            parts.push(spanText(folder));
        return parts.join(" · ");
    }
    function say(text, problem) {
        notice = text;
        noticeProblem = problem;
    }

    function openFolder(folder) {
        subject = folder;
        var items = [];
        if (decoderHost.recordingExportSupported)
            items.push({text: qsTr("Export as zip"), key: "export", objectName: "folderActionExport", enabled: folder.available !== false && screen.exportSelection === 0});
        items.push({text: folder.kind === "scan" ? qsTr("Delete all for this scan list") : folder.kind === "saved" ? qsTr("Delete all for this system") : qsTr("Delete all from Explore"), key: "delete", destructive: true, objectName: "folderActionDelete", enabled: folder.available !== false});
        folderMenu.title = folder.name;
        folderMenu.actions = items;
        folderMenu.open();
    }
    function openUnfinished() {
        subject = null;
        folderMenu.title = qsTr("Unfinished recordings");
        folderMenu.actions = [{text: qsTr("Delete unfinished"), key: "deleteUnfinished", destructive: true, objectName: "unfinishedActionDelete"}];
        folderMenu.open();
    }
    function folderAction(key) {
        if (key === "export")
            chooseRange(subject);
        else if (key === "delete")
            askDelete("folder", subject);
        else if (key === "deleteUnfinished")
            askDelete("unfinished", null);
    }
    // Pick a date range for @p folder, or for every recording when it is null.
    function chooseRange(folder) {
        subject = folder;
        rangeChoices.open(folder ? qsTr("Export %1").arg(folder.name) : qsTr("Export all"), presetNames, function (index) {
            screen.beginExport(screen.subject, screen.presetKeys[index]);
        });
    }
    function beginExport(folder, preset) {
        if (exportSelection > 0) {
            say(qsTr("An export is running. Export again when it finishes."), true);
            return;
        }
        var selection = recordings.selectForExport(folder ? folder.uid : "", preset);
        if (!selection.selection) {
            say(qsTr("No recordings in that range."), false);
            return;
        }
        exportSelection = selection.selection;
        exportSelectionMap = selection;
        var stamp = Qt.formatDate(new Date(savedSystems.realtimeNowMs()), "yyyy-MM-dd");
        say("", false);
        var owner = folder ? Util.fileNameText(folder.name) : "";
        exportPicker.selectedFile = (owner.length > 0 ? qsTr("Call recordings %1 %2").arg(owner).arg(stamp) : qsTr("Call recordings %1").arg(stamp)) + ".zip";
        exportPicker.open();
    }
    function exportPicked(reference) {
        if (exportSelection <= 0)
            return;
        decoderHost.exportRecordings(exportSelectionMap, reference);
    }
    function exportPickCancelled() {
        if (exportSelection <= 0)
            return;
        recordings.releaseSelection(exportSelection);
        exportSelection = 0;
    }
    // DecoderHost::RecordingTransferResult for the export this screen began.
    function exportFinished(selection, result, missing) {
        if (selection !== exportSelection)
            return;
        var count = exportSelectionMap.count - missing;
        exportSelection = 0;
        if (result === 0) {
            var done = count === 1 ? qsTr("Exported 1 recording.") : qsTr("Exported %1 recordings.").arg(Util.countText(count));
            if (missing > 0)
                done += " " + (missing === 1 ? qsTr("1 was gone and left out.") : qsTr("%1 were gone and left out.").arg(Util.countText(missing)));
            say(done, false);
        } else if (result === 2) {
            say(qsTr("Export cancelled."), false);
        } else if (result === 3) {
            say(qsTr("Those recordings are gone."), true);
        } else if (result === 4) {
            say(qsTr("Another export is running. Try again when it finishes."), true);
        } else if (result === 5) {
            say(qsTr("This device can't export call recordings."), true);
        } else {
            say(qsTr("The export couldn't be written. Try another location."), true);
        }
    }

    function askDelete(kind, folder) {
        subject = folder;
        confirmDelete.kind = kind;
        confirmDelete.confirmText = kind === "unfinished" ? qsTr("Delete unfinished") : kind === "all" ? qsTr("Delete all") : qsTr("Delete recordings");
        if (kind === "folder") {
            confirmDelete.title = qsTr("Delete recordings of %1?").arg(folder.name);
            confirmDelete.message = folder.count === 1 ? qsTr("1 recording is deleted from this phone. The calls stay in History.") : qsTr("%1 recordings are deleted from this phone. The calls stay in History.").arg(Util.countText(folder.count));
        } else if (kind === "unfinished") {
            confirmDelete.title = qsTr("Delete unfinished recordings?");
            confirmDelete.message = qsTr("They were left when listening stopped unexpectedly, and may be cut short.");
        } else {
            var total = recordings.recordingCount + recordings.unfinishedCount;
            confirmDelete.title = qsTr("Delete all call recordings?");
            confirmDelete.message = total === 1 ? qsTr("1 recording is deleted from this phone. The calls stay in History.") : qsTr("%1 recordings are deleted from this phone. The calls stay in History.").arg(Util.countText(total));
        }
        confirmDelete.open();
    }
    function deleteConfirmed() {
        var kind = confirmDelete.kind;
        if (kind === "folder" && subject)
            recordings.deleteFolder(subject.uid);
        else if (kind === "unfinished")
            recordings.deleteUnfinished();
        else if (kind === "all")
            recordings.deleteAll();
        say("", false);
    }

    Connections {
        target: decoderHost
        ignoreUnknownSignals: true
        function onRecordingTransferFinished(selection, result, missing) {
            screen.exportFinished(selection, result, missing);
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Item {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 46

        IconButton {
            id: back
            objectName: "callRecordingsBack"
            icon: "back"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            onClicked: screen.closed()
        }

        ReadableText {
            heading: true
            anchors.left: back.right
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Call recordings")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(22)
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
            elide: Text.ElideRight
        }
    }

    PlexFlickable {
        id: scroll
        objectName: "callRecordingsScroll"
        accessibleName: qsTr("Call recordings")
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        contentHeight: content.height + 2 * Theme.screenPadding
        clip: true

        Column {
            id: content
            x: (parent.width - width) / 2
            y: Theme.screenPadding
            width: Math.min(Theme.formWidth, parent.width - 2 * Theme.screenPadding)
            spacing: Theme.gap

            ReadableText {
                objectName: "callRecordingsUsage"
                width: parent.width
                visible: !screen.empty || !recordings.indexed
                text: !recordings.indexed ? qsTr("Counting recordings…") : Util.recordingUsageText(recordings.usedBytes, prefs.recordingLimitMb, recordings.recordingCount + recordings.unfinishedCount)
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
                color: Theme.textSubdued
                wrapMode: Text.Wrap
            }

            Text {
                objectName: "callRecordingsAway"
                width: parent.width
                visible: !recordings.available
                text: Util.recordingStorageAwayText()
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.alert
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }

            // The export running, with its progress and a way out.
            UiPanel {
                objectName: "callRecordingsExportProgress"
                width: parent.width
                visible: screen.exporting
                height: progressColumn.height + 2 * Theme.cardPadding

                Column {
                    id: progressColumn
                    x: Theme.cardPadding
                    y: Theme.cardPadding
                    width: parent.width - 2 * Theme.cardPadding
                    spacing: 10

                    LiveText {
                        objectName: "callRecordingsExportText"
                        step: String(decoderHost.recordingTransferActive)
                        width: parent.width
                        text: decoderHost.recordingTransferTotalBytes > 0 ? qsTr("Exporting… %1 of %2").arg(Util.sizeText(decoderHost.recordingTransferBytes)).arg(Util.sizeText(decoderHost.recordingTransferTotalBytes)) : qsTr("Exporting…")
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(15)
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                    }

                    Rectangle {
                        width: parent.width
                        height: 4
                        radius: 2
                        color: Theme.toggleOffTrack
                        Accessible.role: Accessible.ProgressBar
                        Accessible.name: qsTr("Export progress")
                        // Read when asked (Android polls it); a plain Item's value raises no
                        // ValueChanged, so it stays live (issue #782).
                        readonly property real value: decoderHost.recordingTransferBytes
                        readonly property real minimumValue: 0
                        readonly property real maximumValue: Math.max(0, decoderHost.recordingTransferTotalBytes)

                        Rectangle {
                            width: decoderHost.recordingTransferTotalBytes > 0 ? parent.width * Math.min(1, decoderHost.recordingTransferBytes / decoderHost.recordingTransferTotalBytes) : 0
                            height: parent.height
                            radius: 2
                            color: Theme.cyan
                        }
                    }

                    OutlineButton {
                        objectName: "callRecordingsCancelExport"
                        width: parent.width
                        text: qsTr("Cancel export")
                        onClicked: decoderHost.cancelRecordingTransfer()
                    }
                }
            }

            Text {
                objectName: "callRecordingsNotice"
                width: parent.width
                visible: text.length > 0
                text: screen.notice
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: screen.noticeProblem ? Theme.alert : Theme.textSecondary
                Accessible.role: screen.noticeProblem ? Accessible.AlertMessage : Accessible.StaticText
                Accessible.name: text
            }

            // What the limit removed and the deletes the store could not do, since this
            // screen opened (taken from the store, see takeNotices()), and the ones
            // waiting for a hold.
            ReadableText {
                objectName: "callRecordingsRemovedByLimit"
                width: parent.width
                visible: screen.removedShown > 0
                text: Util.removedByLimitText(screen.removedShown)
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSecondary
            }

            Text {
                objectName: "callRecordingsDeleteFailures"
                width: parent.width
                visible: screen.failuresShown > 0
                text: Util.deleteFailuresText(screen.failuresShown, screen.failureReason)
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.alert
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }

            ReadableText {
                objectName: "callRecordingsDeletesWaiting"
                width: parent.width
                visible: recordings.deletesWaiting > 0
                text: recordings.deletesWaiting === 1 ? qsTr("1 recording is deleted once it stops playing, uploading or exporting.") : qsTr("%1 recordings are deleted once they stop playing, uploading or exporting.").arg(Util.countText(recordings.deletesWaiting))
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSecondary
            }

            UiPanel {
                objectName: "callRecordingsFolders"
                width: parent.width
                visible: !screen.empty
                height: folderColumn.height

                Column {
                    id: folderColumn
                    width: parent.width

                    Repeater {
                        model: screen.folders
                        DisclosureRow {
                            required property var modelData
                            required property int index
                            objectName: "callRecordingsFolder" + index
                            title: modelData.name
                            subtitle: screen.folderText(modelData)
                            subtitleColor: modelData.available === false ? Theme.alert : Theme.textSubdued
                            showDivider: index + 1 < screen.folders.length || recordings.unfinishedCount > 0
                            onTapped: screen.openFolder(modelData)
                        }
                    }

                    DisclosureRow {
                        objectName: "callRecordingsUnfinished"
                        visible: recordings.unfinishedCount > 0
                        title: qsTr("Unfinished recordings")
                        subtitle: (recordings.unfinishedCount === 1 ? qsTr("1 recording") : qsTr("%1 recordings").arg(Util.countText(recordings.unfinishedCount))) + " · " + Util.sizeText(recordings.unfinishedBytes) + " · " + qsTr("left when listening stopped unexpectedly")
                        onTapped: screen.openUnfinished()
                    }
                }
            }

            Row {
                width: parent.width
                spacing: 8
                visible: !screen.empty

                OutlineButton {
                    objectName: "callRecordingsExportAll"
                    visible: decoderHost.recordingExportSupported && recordings.recordingCount > 0
                    width: (parent.width - parent.spacing) / 2
                    text: qsTr("Export all")
                    enabled: !screen.exporting && recordings.available
                    onClicked: screen.chooseRange(null)
                }
                OutlineButton {
                    objectName: "callRecordingsDeleteAll"
                    width: decoderHost.recordingExportSupported && recordings.recordingCount > 0 ? (parent.width - parent.spacing) / 2 : parent.width
                    text: qsTr("Delete all")
                    textColor: Theme.magenta
                    onClicked: screen.askDelete("all", null)
                }
            }

            // First use, and after everything was deleted: say how recordings get here.
            Column {
                objectName: "callRecordingsEmpty"
                width: parent.width
                visible: screen.empty && recordings.indexed
                topPadding: 48
                spacing: 8

                ReadableText {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("No call recordings yet")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(16)
                    font.weight: Font.DemiBold
                    color: Theme.textSecondary
                    wrapMode: Text.Wrap
                }

                ReadableText {
                    objectName: "callRecordingsEmptyDetail"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: prefs.recordCalls ? qsTr("Calls you hear are saved here. Open one in History to play it.") : qsTr("Turn on Record calls in Settings, and every call you hear is saved here to play from History.")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    ActionMenu {
        id: folderMenu
        objectName: "callRecordingsFolderMenu"
        onTriggered: function (index) {
            screen.folderAction(actions[index].key);
        }
    }

    ChoiceSheet {
        id: rangeChoices
        objectName: "callRecordingsRange"
        choiceObjectNamePrefix: "callRecordingsRange"
    }

    ConfirmDialog {
        id: confirmDelete
        objectName: "callRecordingsDeleteConfirm"
        property string kind: ""
        destructive: true
        onConfirmed: screen.deleteConfirmed()
    }

    FileDialog {
        id: exportPicker
        objectName: "callRecordingsExportPicker"
        fileMode: FileDialog.SaveFile
        // Android takes the new document's MIME type from the first filter.
        nameFilters: [qsTr("Zip archive") + " (*.zip)"]
        defaultSuffix: "zip"
        onAccepted: screen.exportPicked(selectedFile.toString())
        onRejected: screen.exportPickCancelled()
    }
}
