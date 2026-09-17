// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet

    property string title: qsTr("Confirm")
    property string message: ""
    property string confirmText: qsTr("Confirm")
    property bool destructive: false
    signal confirmed
    signal cancelled
    accessibleName: title

    function confirm() {
        if (!visible)
            return;
        visible = false;
        confirmed();
    }
    function cancel() {
        if (!visible)
            return;
        visible = false;
        cancelled();
    }
    dismissHandler: cancel

    Text {
        width: parent.width
        text: sheet.title
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(17)
        font.weight: Font.Bold
        color: Theme.textPrimary
    }
    Text {
        width: parent.width
        text: sheet.message
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSubdued
    }
    OutlineButton {
        objectName: "confirmActionButton"
        width: parent.width
        text: sheet.confirmText
        border.color: sheet.destructive ? Theme.magenta : Theme.controlBorder
        onClicked: sheet.confirm()
    }
    OutlineButton {
        objectName: "confirmCancelButton"
        width: parent.width
        text: qsTr("Cancel")
        onClicked: sheet.cancel()
    }
}
