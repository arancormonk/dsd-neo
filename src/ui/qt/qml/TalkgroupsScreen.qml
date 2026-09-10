// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

Item {
    id: screen

    property string systemName: ""
    property string saveMessage: ""
    property bool canSaveList: false
    property string saveListText: qsTr("Save talkgroup list")
    signal saveListRequested()
    signal closed()
    onVisibleChanged: { if (!visible) editSheet.visible = false }
    readonly property bool hostRunning: decoderHost.running
    onHostRunningChanged: { if (!hostRunning) editSheet.visible = false }


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

        Text {
            id: back
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "‹"
            font.pixelSize: Theme.fontSize(28)
            color: Theme.textSecondary

            TapHandler {
                onTapped: screen.closed()
            }
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
        text: qsTr("Talk groups · %1").arg(talkgroups.count)
              + (talkgroups.notTunedCount > 0 ? qsTr(" · %1 not tuned").arg(talkgroups.notTunedCount) : "")
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
        implicitHeight: 34
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
            height: 50
            text: noTuneLabel.elidedText
            enabled: decoderHost.running && talkgroups.count > 0
            onClicked: commands.setAllTalkgroupsListening(false, talkgroupView.filterTag)

            // Category names can be much wider than half a phone screen. Keep
            // the shared button styling while bounding its single-line label.
            TextMetrics {
                id: noTuneLabel
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.DemiBold
                text: talkgroupView.filterTag.length > 0
                      ? qsTr("Do not tune all · %1").arg(talkgroupView.filterTag) : qsTr("Do not tune all")
                elide: Text.ElideRight
                elideWidth: noTuneAll.width - 16
            }
        }

        GradientButton {
            id: listenAll
            objectName: "listenAllButton"
            width: (parent.width - 10) / 2
            text: listenLabel.elidedText
            enabled: decoderHost.running && talkgroups.count > 0
            onClicked: commands.setAllTalkgroupsListening(true, talkgroupView.filterTag)

            TextMetrics {
                id: listenLabel
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.Bold
                text: talkgroupView.filterTag.length > 0
                      ? qsTr("Listen all · %1").arg(talkgroupView.filterTag) : qsTr("Listen all")
                elide: Text.ElideRight
                elideWidth: listenAll.width - 16
            }
        }
    }

    GridView {
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
        cellWidth: Math.floor(width / 3)
        cellHeight: 122
        enabled: decoderHost.running

        delegate: TalkgroupCard {
            required property var idStart
            required property var idEnd

            width: GridView.view.cellWidth - 8
            height: GridView.view.cellHeight - 8
            // Snapshot truth, not a local toggle: the decoder may refuse an edit.
            onClicked: commands.setTalkgroupListening(idStart, idEnd, !listening)
            onEditRequested: editSheet.openRow(idStart, idEnd, name, listening, listed, priority, preempt,
                                               talkgroups.policyContext, talkgroups.policyGeneration)
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

        Row {
            spacing: 8

            Rectangle {
                width: 6
                height: 6
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.cyan
            }

            Text {
                text: qsTr("Listening")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(11)
                color: Theme.textSecondary
            }

            Rectangle {
                width: 6
                height: 6
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textSubdued
            }

            Text {
                text: qsTr("Not tuned")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(11)
                color: Theme.textSecondary
            }
        }

        OutlineButton {
            objectName: "saveTalkgroupListButton"
            width: parent.width; height: 44
            visible: !talkgroups.persistent
            enabled: decoderHost.running && screen.canSaveList
            text: screen.saveListText
            onClicked: screen.saveListRequested()
        }

        Text { width: parent.width; text: screen.saveMessage; visible: text.length > 0; color: Theme.textSecondary; wrapMode: Text.Wrap }

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
