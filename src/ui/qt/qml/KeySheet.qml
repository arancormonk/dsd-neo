// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    objectName: "keySheet"
    panelObjectName: "keySheetPanel"
    property string notice: ""
    readonly property bool sessionRunning: decoderHost.running

    function open() {
        editor.reset();
        notice = "";
        visible = sessionRunning;
    }
    function apply() {
        if (!visible || !sessionRunning || !editor.valid)
            return;
        notice = "";
        if (editor.keyType.length > 0 && !commands.applyEncryptionKey(editor.keyType, editor.keyValue)) {
            notice = qsTr("Key request was not queued. Try again.");
            return;
        }
        if (!commands.setForceKeyMode(editor.forceMode)) {
            notice = qsTr("Force key request was not queued. Try again.");
            return;
        }
        visible = false;
    }
    onSessionRunningChanged: {
        if (!sessionRunning)
            visible = false;
    }
    onVisibleChanged: {
        if (!visible) {
            editor.reset();
            notice = "";
            Qt.inputMethod.hide();
        }
    }
    EncryptionEditor {
        id: editor
        objectName: "sessionEncryptionEditor"
        width: parent.width
    }
    Text {
        width: parent.width
        text: qsTr("Applies only to this session. Saved systems are unchanged. None leaves the current key in place.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: 13 * Theme.fontScale
        color: Theme.textSecondary
    }
    Text {
        width: parent.width
        visible: text.length > 0
        text: sheet.notice
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: 13 * Theme.fontScale
        color: Theme.textSecondary
    }
    GradientButton {
        objectName: "applySessionKeyButton"
        width: parent.width
        text: qsTr("Apply to this session")
        enabled: sheet.sessionRunning && editor.valid
        onClicked: sheet.apply()
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Cancel")
        onClicked: sheet.visible = false
    }
}
