// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Scan with other lists (issue #697): opened from a scan-list card's More
// menu, it starts that list and the others switched on here as one session,
// on the card's receiver and settings. One row per other ready list, in Home
// order, all off; drafts are left out. A list whose receiver or scan settings
// differ from the card's cannot run in the same session: its row is shown
// disabled, naming the first setting that differs, as
// scanListStarter.joinConflict() decides it. Every list starts switched on.
// A choice in progress, so only Cancel, Back or Escape close it.
ModalSheet {
    id: sheet
    objectName: "scanTogetherSheet"
    accessibleName: qsTr("Scan with other lists")
    closesOnOutsideTap: false

    // The card's list, by uid: rows can move while the sheet is open.
    property string primaryUid: ""
    readonly property var primary: (revision, scanLists.getByUid(primaryUid))
    // The other ready lists, each {uid, name, entryCount, reason}, in Home order.
    readonly property var others: (revision, otherLists())
    // The uids switched on here, in Home order.
    property var chosen: []
    readonly property int listCount: 1 + chosen.length
    // A session runs at most this many lists.
    readonly property int maxLists: scanListStarter.maxLists
    property int revision: 0
    Connections {
        target: scanLists
        function onDataChanged() {
            sheet.revision++;
        }
        function onCountChanged() {
            sheet.revision++;
        }
    }
    // Every library change, a file replaced in place included (#832).
    Connections {
        target: importedFiles
        function onEntriesChanged() {
            sheet.revision++;
        }
    }

    // The card's list and the uids chosen, in that order: what to start.
    signal startRequested(string primaryUid, var companionUids)

    function openFor(row) {
        primaryUid = scanLists.get(row).uid || "";
        chosen = [];
        revision++;
        open();
    }
    function otherLists() {
        var out = [];
        if (!primary || !primary.uid)
            return out;
        for (var i = 0; i < scanLists.count; ++i) {
            var list = scanLists.get(i);
            if (!list.uid || list.uid === primaryUid || list.isDraft)
                continue;
            out.push({
                uid: list.uid,
                name: list.name,
                entryCount: scanLists.entryCount(list),
                reason: scanListStarter.joinConflict(primary, list)
            });
        }
        return out;
    }
    function setChosen(uid, on) {
        var next = [];
        for (var other of others) {
            var keep = other.uid === uid ? on : chosen.indexOf(other.uid) >= 0;
            if (keep && other.reason.length === 0)
                next.push(other.uid);
        }
        chosen = next;
    }
    // A list that left, or can no longer join, leaves the choice with it.
    onOthersChanged: setChosen("", false)
    onPrimaryChanged: {
        if (visible && (!primary || !primary.uid || primary.isDraft))
            visible = false;
    }

    ReadableText {
        objectName: "scanTogetherTitle"
        heading: true
        width: parent.width
        text: qsTr("Scan with other lists")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    ReadableText {
        objectName: "scanTogetherHint"
        width: parent.width
        text: sheet.others.length > 0 ? qsTr("Lists you switch on scan together with %1, on its receiver and settings. Each can be switched off while the scan runs.").arg(sheet.primary ? sheet.primary.name : "") : qsTr("No other scan list is ready. Save another list first.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }

    Column {
        objectName: "scanTogetherRows"
        width: parent.width
        visible: sheet.others.length > 0

        Repeater {
            model: sheet.visible ? sheet.others : []
            ToggleRow {
                id: otherRow
                required property var modelData
                required property int index
                readonly property bool on: sheet.chosen.indexOf(modelData.uid) >= 0
                readonly property bool full: !on && sheet.listCount >= sheet.maxLists
                objectName: "scanTogetherRow_" + modelData.uid
                title: modelData.name
                subtitle: modelData.reason.length > 0 ? modelData.reason : full ? qsTr("A scan runs %1 lists at most").arg(sheet.maxLists) : modelData.entryCount === 1 ? qsTr("1 entry") : qsTr("%1 entries").arg(modelData.entryCount)
                checked: on
                enabled: modelData.reason.length === 0 && !full
                showDivider: index < sheet.others.length - 1
                onToggled: function (checked) {
                    sheet.setChosen(modelData.uid, checked);
                }
            }
        }
    }

    GradientButton {
        objectName: "scanTogetherStart"
        width: parent.width
        // Nothing to add yet: Play on the card starts the one list.
        text: sheet.chosen.length === 0 ? qsTr("Switch on a list first") : qsTr("Start %1 lists").arg(sheet.listCount)
        enabled: sheet.chosen.length > 0 && !decoderHost.transitioning && !!sheet.primary && !!sheet.primary.uid
        onClicked: {
            var uids = sheet.chosen.slice();
            sheet.visible = false;
            sheet.startRequested(sheet.primaryUid, uids);
        }
    }
    OutlineButton {
        objectName: "scanTogetherCancel"
        width: parent.width
        text: qsTr("Cancel")
        onClicked: sheet.requestDismiss()
    }
}
