// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Callers disable the surface underneath while this menu is visible. Rows keep
// passive tap grabs so a drag that starts on an action can scroll the sheet.
ModalSheet {
    id: menu

    property string title: ""
    // Each action has text and optional destructive, enabled, description and
    // objectName fields. Callers may carry a key for routing conditional rows.
    property var actions: []
    signal triggered(int index)
    signal cancelled
    accessibleName: title
    dismissHandler: cancel

    function activate(index) {
        if (!visible || index < 0 || index >= actions.length || actions[index].enabled === false)
            return;
        visible = false;
        triggered(index);
    }
    function cancel() {
        if (!visible)
            return;
        visible = false;
        cancelled();
    }

    Text {
        width: parent.width
        text: menu.title
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(17)
        font.weight: Font.Bold
    }
    Repeater {
        model: menu.actions
        Column {
            id: row
            required property int index
            required property var modelData
            width: parent.width
            spacing: 4

            OutlineButton {
                objectName: row.modelData.objectName || "actionMenuAction" + row.index
                width: parent.width
                text: row.modelData.text
                textColor: row.modelData.destructive ? Theme.magenta : Theme.buttonSecondaryText
                enabled: row.modelData.enabled !== false
                accessibleName: text + (row.modelData.description ? ". " + row.modelData.description : "")
                onClicked: menu.activate(row.index)
            }
            Text {
                objectName: "actionMenuDescription" + row.index
                width: parent.width
                visible: text.length > 0
                text: row.modelData.description || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
            }
        }
    }
    OutlineButton {
        objectName: "actionMenuCancel"
        width: parent.width
        text: qsTr("Cancel")
        onClicked: menu.cancel()
    }
}
