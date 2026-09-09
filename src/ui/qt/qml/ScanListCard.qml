// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

UiPanel {
    id: card

    property string listName: ""
    property int entryCount: 0

    signal play()
    signal edit()

    objectName: "scanListCard"
    height: 82

    Column {
        anchors.left: parent.left
        anchors.right: playButton.left
        anchors.margins: Theme.cardPadding
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5

        Text {
            width: parent.width
            text: card.listName
            elide: Text.ElideRight
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: 17
        }

        Text {
            text: qsTr("%1 entries · long-press to edit").arg(card.entryCount)
            color: Theme.textSubdued
            font.family: Theme.sans
            font.pixelSize: 12
        }

    }

    PlayCircle {
        id: playButton

        anchors.right: parent.right
        anchors.rightMargin: Theme.cardPadding
        anchors.verticalCenter: parent.verticalCenter
        enabled: !decoderHost.transitioning
        onClicked: card.play()
    }

    TapHandler {
        onLongPressed: card.edit()
    }

}
