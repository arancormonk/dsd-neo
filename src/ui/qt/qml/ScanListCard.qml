// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

UiPanel {
    id: card

    property string listName: ""
    property int entryCount: 0
    property bool isDraft: false

    signal play
    signal edit

    objectName: "scanListCard"
    height: Math.max(122, labels.implicitHeight + 2 * Theme.cardPadding)

    Column {
        id: labels
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
            font.pixelSize: Theme.fontSize(17)
        }

        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: card.isDraft ? qsTr("Not ready · draft") : qsTr("%1 entries").arg(card.entryCount)
            color: Theme.textSubdued
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
        }
    }

    PlayCircle {
        id: playButton
        accessibleName: qsTr("Listen to %1").arg(card.listName)
        objectName: "scanListPlay"

        anchors.right: parent.right
        anchors.rightMargin: Theme.cardPadding
        anchors.top: parent.top
        anchors.topMargin: Theme.minimumTouchSize + 6
        enabled: !decoderHost.transitioning && !card.isDraft
        onClicked: card.play()
    }

    IconButton {
        id: editButton
        objectName: "scanListManageButton"
        icon: "more"
        accessibleName: qsTr("More options for %1").arg(card.listName)
        anchors.right: parent.right
        anchors.rightMargin: Theme.cardPadding + (playButton.width - width) / 2
        anchors.top: parent.top
        onClicked: card.edit()
    }

    TapHandler {
        onLongPressed: card.edit()
    }
}
