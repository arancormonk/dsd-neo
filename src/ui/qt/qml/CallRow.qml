// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// One call in a list — monitor's recent panel and the history screen share it.
// Encrypted rows carry the ENC tag while retaining readable text contrast.
Item {
    id: row
    property string accessibleName: name
    property bool interactive: false
    signal activated
    activeFocusOnTab: interactive && enabled && Navigation.allows(row)
    Accessible.role: interactive ? Accessible.Button : Accessible.StaticText
    Accessible.name: accessibleName
    readonly property bool navigationAllowed: Navigation.allows(row)
    Accessible.ignored: !visible || !navigationAllowed
    Accessible.onPressAction: activate()
    function activate() {
        if (interactive && enabled && Navigation.allows(row))
            activated();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    FocusFrame {}
    TapHandler {
        onTapped: row.activate()
    }

    property string name: ""
    property string metaText: ""
    property string rightText: ""
    property bool enc: false
    property bool emergency: false
    property bool showDivider: true

    implicitHeight: Math.max(Theme.rowHeight, labels.implicitHeight + 24)

    Column {
        id: labels
        anchors.left: parent.left
        anchors.right: right.left
        anchors.leftMargin: 18
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3

        Text {
            width: parent.width
            text: row.name
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(16)
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            elide: Text.ElideRight
        }

        Text {
            width: parent.width
            text: row.metaText
            font.family: Theme.mono
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSubdued
            elide: Text.ElideRight
        }
    }

    Item {
        id: right

        anchors.right: parent.right
        anchors.rightMargin: 18
        anchors.verticalCenter: parent.verticalCenter
        width: (emergencyTag.visible ? emergencyTag.implicitWidth + 6 : 0) + Math.max(encTag.visible ? encTag.implicitWidth : 0, timeText.visible ? timeText.implicitWidth : 0)
        height: parent.height

        EmergencyTag {
            id: emergencyTag
            objectName: "rowEmergencyTag"
            visible: row.emergency
            anchors.right: encTag.visible ? encTag.left : timeText.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
        }

        EncTag {
            id: encTag
            visible: row.enc
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            id: timeText
            visible: !row.enc
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: row.rightText
            font.family: Theme.mono
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSubdued
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 18
        height: 1
        visible: row.showDivider
        color: Theme.divider
    }
}
