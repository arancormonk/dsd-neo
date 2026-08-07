// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// One call in a list — monitor's recent panel and the history screen share it.
// Encrypted rows dim and carry the ENC tag; they are never styled as errors.
Item {
    id: row

    property string name: ""
    property string metaText: ""
    property string rightText: ""
    property bool enc: false
    property bool showDivider: true

    implicitHeight: 58

    Column {
        anchors.left: parent.left
        anchors.right: right.left
        anchors.leftMargin: 18
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3
        opacity: row.enc ? 0.55 : 1.0

        Text {
            width: parent.width
            text: row.name
            font.family: Theme.sans
            font.pixelSize: 16
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            elide: Text.ElideRight
        }

        Text {
            width: parent.width
            text: row.metaText
            font.family: Theme.mono
            font.pixelSize: 12
            color: Theme.textSubdued
            elide: Text.ElideRight
        }
    }

    Item {
        id: right

        anchors.right: parent.right
        anchors.rightMargin: 18
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(encTag.visible ? encTag.implicitWidth : 0, timeText.visible ? timeText.implicitWidth : 0)
        height: parent.height

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
            font.pixelSize: 12
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
