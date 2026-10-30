// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
import QtQuick

Rectangle {
    id: card
    required property string idText
    required property string name
    required property string tags
    required property bool listening
    required property bool listed
    required property int priority
    required property bool preempt
    required property var temporaryAvoidCount
    readonly property var rank: priority > 0 ? [qsTr("Priority %1").arg(priority)].concat(preempt ? [qsTr("Preempt")] : []) : []
    signal editRequested
    signal clicked
    implicitHeight: content.implicitHeight + 28
    radius: Theme.radiusPanel
    color: Theme.panel
    border.width: 1
    border.color: listening ? Theme.cyan : Theme.controlBorder
    Column {
        id: content
        objectName: "talkgroupCardContent"
        x: 14
        y: 14
        width: parent.width - 28
        spacing: 5
        // The card's lines are read with its switch (issue #782). A card is
        // rebuilt with its table, so it adds no node of its own to the two
        // controls it already had. The switch's name says the talkgroup and
        // its state whether it is listened to, so its description gives only
        // the rest: the rank, the tags and an avoid.
        Text {
            width: parent.width
            objectName: "talkgroupBadge"
            text: [card.idText].concat(card.rank).join(" · ")
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            font.family: Theme.mono
            font.pixelSize: Theme.fontSize(14)
            color: Theme.textSecondary
        }
        Text {
            objectName: "talkgroupCardName"
            width: parent.width
            text: card.name
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(17)
            font.weight: Font.DemiBold
            color: Theme.textPrimary
        }
        Text {
            id: tagsLine
            objectName: "talkgroupCardTags"
            width: parent.width
            visible: text.length > 0
            text: card.listed ? card.tags : qsTr("Heard in this session")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSecondary
        }
        Text {
            id: avoidBadge
            objectName: "temporaryTgAvoidBadge"
            width: parent.width
            visible: card.temporaryAvoidCount > 0
            text: qsTr("Temporarily avoided: %1. Clear in Settings → Listening.").arg(card.temporaryAvoidCount)
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSecondary
        }
        Item {
            width: parent.width
            height: Theme.minimumTouchSize
            Text {
                objectName: "talkgroupCardListening"
                anchors.left: parent.left
                anchors.right: listenSwitch.left
                anchors.verticalCenter: parent.verticalCenter
                text: card.listening ? qsTr("Listening") : qsTr("Not tuned")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
                color: Theme.textPrimary
            }
            PlexSwitch {
                id: listenSwitch
                objectName: "talkgroupListeningSwitch"
                anchors.right: edit.left
                anchors.rightMargin: 12
                checked: card.listening
                accessibleName: qsTr("Listen to %1, talkgroup %2").arg(card.name).arg(card.idText)
                Accessible.description: [card.rank.join(" · "), tagsLine.visible ? tagsLine.text : "", avoidBadge.visible ? avoidBadge.text : ""].filter(function (part) {
                    return part.length > 0;
                }).join(", ")
                onToggled: card.clicked()
            }
            IconButton {
                id: edit
                icon: "more"
                objectName: "talkgroupEditButton"
                anchors.right: parent.right
                accessibleName: qsTr("Edit talkgroup %1").arg(card.idText)
                onClicked: card.editRequested()
            }
        }
    }
    TapHandler {
        onLongPressed: card.editRequested()
    }
}
