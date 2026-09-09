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
    signal clicked()

    radius: Theme.radiusPanel
    color: Theme.panel
    border.width: 1
    border.color: listening ? Theme.cyan : Theme.controlBorder

    Column {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 4

        Text {
            width: parent.width
            text: card.idText
            font.family: Theme.mono
            font.pixelSize: Theme.fontSize(15)
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            elide: Text.ElideRight
        }

        Text {
            width: parent.width
            text: card.name
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSecondary
            maximumLineCount: 2
            wrapMode: Text.Wrap
            elide: Text.ElideRight
        }

        MicroLabel {
            width: parent.width
            text: card.listed ? card.tags : qsTr("Heard")
            visible: text.length > 0
            elide: Text.ElideRight
        }

        Row {
            width: parent.width
            spacing: 5

            Rectangle {
                width: 6
                height: 6
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: card.listening ? Theme.cyan : Theme.textSubdued
            }

            Text {
                width: parent.width - 11
                text: card.listening ? qsTr("Listening") : qsTr("Not tuned")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(11)
                color: card.listening ? Theme.cyan : Theme.textSubdued
                elide: Text.ElideRight
            }
        }
    }

    TapHandler {
        onTapped: card.clicked()
    }
}
