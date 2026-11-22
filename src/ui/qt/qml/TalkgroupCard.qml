// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
import QtQuick
import "MapText.js" as MapText
import "Util.js" as Util

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
    // Issue #903: a heard card the system remembers, and when it was last heard (decode-domain wall seconds).
    required property var lastHeard
    required property bool remembered
    // The owner's minute tick: an age keeps moving while the decoder is quiet (as MonitorScreen's ageTick).
    property int ageTick: 0
    readonly property var rank: priority > 0 ? [qsTr("Priority %1").arg(priority)].concat(preempt ? [qsTr("Preempt")] : []) : []
    signal editRequested
    signal clicked
    // "12 min ago", "just now"; a time ahead of the decode clock (a replay of an older capture) by its date and time.
    function heardAgo() {
        var now = (card.ageTick, metrics.decodeNowMs);
        var age = now / 1000 - card.lastHeard;
        return MapText.agoText(age < 0 ? -1 : Math.floor(age / 60) * 60, MapText.clockText(card.lastHeard, now));
    }
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
            // A heard card has no name: its ID, then when it was heard (issue #903).
            visible: text.length > 0
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
            // Issue #903: a remembered card says when it was last heard, to the minute, on the decode clock its time
            // is stamped on; one heard only in this session keeps its line.
            text: card.listed ? card.tags : card.remembered && card.lastHeard > 0 ? qsTr("Last heard %1").arg(card.heardAgo()) : qsTr("Heard in this session")
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
                // A heard card has no name to read: the talkgroup alone.
                accessibleName: card.name.length > 0 ? Util.fill(qsTr("Listen to %1, talkgroup %2"), [card.name, card.idText]) : qsTr("Listen to talkgroup %1").arg(card.idText)
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
    GatedTapHandler {
        onAllowedLongPress: card.editRequested()
    }
}
