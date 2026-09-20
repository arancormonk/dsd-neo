// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window

Item {
    id: screen
    signal closed

    Keys.onEscapePressed: Navigation.back(screen.Window.window)
    Keys.onBackPressed: Navigation.back(screen.Window.window)
    // The layer clears input during activation; take focus after its bindings settle.
    onVisibleChanged: if (visible) Qt.callLater(function () {
        if (screen.visible && Navigation.allows(screen))
            screen.forceActiveFocus();
    })

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
            objectName: "licensesBack"
            icon: "back"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            onClicked: screen.closed()
        }

        Text {
            anchors.left: back.right
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Open source licenses")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(22)
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
            elide: Text.ElideRight
        }
    }

    PlexFlickable {
        id: scroll
        objectName: "licensesScroll"
        accessibleName: qsTr("Open source licenses")
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        contentHeight: licenseText.height
        clip: true

        TextEdit {
            id: licenseText
            objectName: "licenseNoticesText"
            width: scroll.width
            text: screen.visible ? decoderHost.licenseNotices() : ""
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
        }
    }
}
