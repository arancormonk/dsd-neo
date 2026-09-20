// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    accessibleName: qsTr("Discard changes?")
    property var discardAction: null
    function ask(action) {
        discardAction = action;
        visible = true;
    }
    Text {
        width: parent.width
        text: qsTr("Discard unsaved changes?")
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(20)
    }
    GradientButton {
        width: parent.width
        text: qsTr("Keep editing")
        onClicked: {
            sheet.discardAction = null;
            sheet.visible = false;
        }
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Discard changes")
        onClicked: {
            var action = sheet.discardAction;
            sheet.discardAction = null;
            sheet.visible = false;
            if (action)
                action();
        }
    }
    onDismissed: discardAction = null
}
