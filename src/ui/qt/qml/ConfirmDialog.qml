// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet

    property string title: qsTr("Confirm")
    property string message: ""
    property string confirmText: qsTr("Confirm")
    property bool showCancel: true
    property bool destructive: false
    // An alert line under the message: why the last confirm did not go through.
    property string error: ""
    // False leaves the sheet open on confirm, for a handler that may keep it
    // (with an error) for another try; the handler closes it otherwise.
    property bool closeOnConfirm: true
    signal confirmed
    signal cancelled
    accessibleName: title

    function confirm() {
        if (!visible)
            return;
        if (closeOnConfirm)
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
    ReadableText {
        objectName: "confirmMessage"
        width: parent.width
        text: sheet.message
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSubdued
    }
    Text {
        id: confirmErrorText
        objectName: "confirmError"
        width: parent.width
        visible: text.length > 0
        text: sheet.error
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.alert
        Accessible.role: Accessible.AlertMessage
        Accessible.name: text
        Accessible.ignored: !Navigation.allows(confirmErrorText) || !visible
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
        visible: sheet.showCancel
        width: parent.width
        text: qsTr("Cancel")
        onClicked: sheet.cancel()
    }
}
