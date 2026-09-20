// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

Item {
    id: screen

    property string systemName: ""
    property string saveMessage: ""
    property var bulkRows: []
    property string bulkContext: "0"
    property int bulkGeneration: 0
    property bool bulkListen: true
    property string bulkMessage: ""
    function requestBulk(listen) {
        bulkRows = talkgroupView.snapshotSelection();
        bulkContext = talkgroups.policyContext;
        bulkGeneration = talkgroups.policyGeneration;
        bulkListen = listen;
        if (bulkRows.length)
            bulkConfirm.visible = true;
    }
    ModalSheet {
        id: bulkConfirm
        objectName: "talkgroupBulkConfirm"
        accessibleName: qsTr("Change matching talkgroups")
        Text {
            width: parent.width
            text: (screen.bulkListen ? qsTr("Listen to %1 matching rows?") : qsTr("Stop tuning %1 matching rows?")).arg(screen.bulkRows.length) + qsTr(" Both the search and category filters apply.")
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(16)
        }
        OutlineButton {
            objectName: "confirmTalkgroupBulk"
            width: parent.width
            text: qsTr("Apply to matching rows")
            onClicked: {
                var accepted = commands.setTalkgroupSelection(screen.bulkListen, screen.bulkContext, screen.bulkGeneration, screen.bulkRows);
                screen.bulkMessage = accepted ? qsTr("Update requested") : qsTr("The update could not be queued. Try again.");
                bulkConfirm.visible = false;
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Cancel")
            onClicked: bulkConfirm.visible = false
        }
    }

    property bool canSaveList: false
    property string saveListText: qsTr("Save talkgroup list")
    signal saveListRequested
    signal closed
    onVisibleChanged: {
        if (!visible)
            editSheet.visible = false;
    }
    readonly property bool hostRunning: decoderHost.running
    onHostRunningChanged: {
        if (!hostRunning)
            editSheet.visible = false;
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
            icon: "back"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            onClicked: screen.closed()
        }

        Text {
            anchors.left: back.right
            anchors.right: parent.right
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: screen.systemName.length > 0 ? screen.systemName : qsTr("Talk groups")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(22)
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
            elide: Text.ElideRight
        }
    }

    MicroLabel {
        id: countLabel

        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 6
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        text: qsTr("Talk groups · %1").arg(talkgroups.count) + (talkgroups.notTunedCount > 0 ? qsTr(" · %1 not tuned").arg(talkgroups.notTunedCount) : "")
        elide: Text.ElideRight
    }

    PlexTextField {
        id: search
        objectName: "talkgroupSearch"

        anchors.top: countLabel.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: Theme.gap
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        placeholderText: qsTr("Search talkgroups")
        onTextChanged: talkgroupView.filterText = text
    }

    ListView {
        id: chips
        objectName: "talkgroupChips"

        anchors.top: search.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: Theme.gap
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        orientation: ListView.Horizontal
        spacing: 8
        clip: true
        implicitHeight: Math.max(48, Theme.fontSize(13) + 24)
        visible: talkgroups.categories.length > 0
        model: [qsTr("All")].concat(talkgroups.categories)

        delegate: FilterPill {
            required property int index
            required property string modelData

            caret: false
            text: modelData
            active: index === 0 ? talkgroupView.filterTag === "" : talkgroupView.filterTag === modelData
            onClicked: talkgroupView.filterTag = index === 0 ? "" : modelData
        }
    }

    Row {
        id: bulkActions

        anchors.top: chips.visible ? chips.bottom : search.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: Theme.gap
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        spacing: 10

        OutlineButton {
            id: noTuneAll
            objectName: "noTuneAllButton"
            width: (parent.width - 10) / 2
            text: qsTr("Do not tune matching")
            enabled: decoderHost.running && talkgroupView.count > 0
            onClicked: screen.requestBulk(false)

            // Category names can be much wider than half a phone screen. Keep
            // the shared button styling while bounding its single-line label.
            TextMetrics {
                id: noTuneLabel
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.DemiBold
                text: talkgroupView.filterTag.length > 0 ? qsTr("Do not tune all · %1").arg(talkgroupView.filterTag) : qsTr("Do not tune all")
                elide: Text.ElideRight
                elideWidth: noTuneAll.width - 16
            }
        }

        GradientButton {
            id: listenAll
            objectName: "listenAllButton"
            width: (parent.width - 10) / 2
            text: qsTr("Listen to matching")
            enabled: decoderHost.running && talkgroupView.count > 0
            onClicked: screen.requestBulk(true)

            TextMetrics {
                id: listenLabel
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.Bold
                text: talkgroupView.filterTag.length > 0 ? qsTr("Listen all · %1").arg(talkgroupView.filterTag) : qsTr("Listen all")
                elide: Text.ElideRight
                elideWidth: listenAll.width - 16
            }
        }
    }

    FontMetrics {
        id: cardIdMetrics
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(15)
    }
    FontMetrics {
        id: cardNameMetrics
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
    }
    FontMetrics {
        id: cardTagMetrics
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(11)
    }
    FontMetrics {
        id: cardStatusMetrics
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(11)
    }

    ListView {
        id: grid
        objectName: "talkgroupGrid"

        anchors.top: bulkActions.bottom
        anchors.bottom: footer.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: Theme.gap
        anchors.bottomMargin: Theme.gap
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        clip: true
        model: talkgroupView
        spacing: 10
        enabled: decoderHost.running

        delegate: TalkgroupCard {
            required property var idStart
            required property var idEnd

            width: ListView.view.width
            height: implicitHeight
            // Snapshot truth, not a local toggle: the decoder may refuse an edit.
            onClicked: commands.setTalkgroupListening(idStart, idEnd, !listening)
            onEditRequested: editSheet.openRow(idStart, idEnd, name, listening, listed, priority, preempt, talkgroups.policyContext, talkgroups.policyGeneration)
        }

        Text {
            anchors.centerIn: parent
            width: parent.width - 24
            visible: talkgroups.count === 0
            text: qsTr("No talkgroups yet. Calls heard on this system will appear here.")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            color: Theme.textSubdued
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    Column {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        spacing: 8

        Text {
            width: parent.width
            text: talkgroups.persistenceScope === "scan" ? qsTr("This scan target · session changes; the source CSV is unchanged") : talkgroups.persistent ? qsTr("Edits are saved to the attached CSV and affect other systems sharing it") : qsTr("This session · save a talkgroup list to keep edits")
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(12)
        }
        Text {
            width: parent.width
            text: metrics.uiMessage || screen.bulkMessage
            visible: text.length > 0
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(13)
        }

        Text {
            width: parent.width
            text: screen.saveMessage
            visible: text.length > 0
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }

        Text {
            objectName: "talkgroupCommandToast"
            width: parent.width
            text: metrics.uiMessage
            visible: text.length > 0
            color: Theme.textSecondary
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }

        Text {
            objectName: "sessionOnlyNote"
            width: parent.width
            visible: !talkgroups.persistent
            text: qsTr("Changes last for this session. Give the system a talkgroup list to keep them.")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSubdued
            wrapMode: Text.Wrap
        }

        Text {
            width: parent.width
            visible: talkgroups.allowListMode
            text: qsTr("Allow list on: talkgroups not on this list are never tuned.")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSubdued
            wrapMode: Text.Wrap
        }
    }

    TalkgroupEditSheet {
        id: editSheet
        objectName: "talkgroupEditSheet"
        saveListText: screen.saveListText
        saveMessage: screen.saveMessage
        canSaveList: screen.canSaveList && !talkgroups.persistent && decoderHost.running
        onSaveListRequested: screen.saveListRequested()
    }
}
