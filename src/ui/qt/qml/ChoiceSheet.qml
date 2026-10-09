// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    property var choices: []
    property var chooseAction: null
    // When set, choice button i is named prefix + i, so a test can tap the real one.
    property string choiceObjectNamePrefix: ""
    function open(title, values, action) {
        accessibleName = title;
        choices = values;
        chooseAction = action;
        visible = true;
    }
    Text {
        width: parent.width
        text: sheet.accessibleName
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(20)
    }
    Repeater {
        model: sheet.choices
        OutlineButton {
            required property var modelData
            required property int index
            objectName: sheet.choiceObjectNamePrefix.length > 0 ? sheet.choiceObjectNamePrefix + index : ""
            width: parent.width
            text: modelData
            onClicked: {
                var action = sheet.chooseAction;
                sheet.visible = false;
                if (action)
                    action(index);
            }
        }
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Cancel")
        onClicked: sheet.visible = false
    }
}
