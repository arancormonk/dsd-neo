// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

UiPanel {
    id: card
    property string message: ""
    property string source: ""
    property bool canRetry: false
    Accessible.role: Accessible.AlertMessage
    Accessible.name: message
    Accessible.description: source
    readonly property bool navigationAllowed: Navigation.allows(card)
    Accessible.ignored: !visible || !navigationAllowed
    property string announcedMessage: ""
    function announceFailure() {
        if (visible && message.length && announcedMessage !== message && typeof Accessible.announce === "function") {
            announcedMessage = message;
            Accessible.announce(message + " " + source);
        }
    }
    onMessageChanged: announceFailure()
    onVisibleChanged: {
        if (visible)
            announceFailure();
        else
            announcedMessage = "";
    }
    signal retry
    signal edit
    signal dismiss
    signal details
    height: body.implicitHeight + 2 * Theme.cardPadding
    border.color: Theme.alertBorder
    Column {
        id: body
        x: Theme.cardPadding
        y: Theme.cardPadding
        width: parent.width - 2 * Theme.cardPadding
        spacing: 10
        Text {
            width: parent.width - close.width
            text: card.message
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(16)
        }
        Text {
            width: parent.width
            text: card.source
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(13)
        }
        Flow {
            width: parent.width
            spacing: 8
            OutlineButton {
                width: (parent.width - 8) / 2
                text: qsTr("Retry")
                enabled: card.canRetry
                onClicked: card.retry()
            }
            OutlineButton {
                width: (parent.width - 8) / 2
                text: qsTr("Edit source")
                onClicked: card.edit()
            }
            OutlineButton {
                width: parent.width
                text: qsTr("Details")
                onClicked: card.details()
            }
        }
    }
    IconButton {
        id: close
        icon: "close"
        anchors.top: parent.top
        anchors.right: parent.right
        accessibleName: qsTr("Dismiss source error")
        onClicked: card.dismiss()
    }
}
