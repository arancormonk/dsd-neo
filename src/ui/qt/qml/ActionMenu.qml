// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Callers disable the surface underneath while this menu is visible. Rows keep
// passive tap grabs so a drag that starts on an action can scroll the sheet.
ModalSheet {
    id: menu

    property string title: ""
    // Each action has text and optional destructive, enabled, description,
    // accessibleName (what a screen reader says for text) and objectName
    // fields. Callers may carry a key for routing conditional rows. enabled
    // and description may also be functions: the row reads them in its
    // bindings, so what they read keeps the row current while the menu is
    // open (a state that ends meanwhile enables it in place).
    property var actions: []
    signal triggered(int index)
    signal cancelled
    accessibleName: title
    dismissHandler: cancel

    function actionField(index, field) {
        var value = index >= 0 && index < actions.length ? actions[index][field] : undefined;
        return typeof value === "function" ? value() : value;
    }
    function actionEnabled(index) {
        return index >= 0 && index < actions.length && actionField(index, "enabled") !== false;
    }
    function actionDescription(index) {
        return actionField(index, "description") || "";
    }
    function activate(index) {
        if (!visible || !actionEnabled(index))
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
                enabled: menu.actionEnabled(row.index)
                readonly property string descriptionText: menu.actionDescription(row.index)
                accessibleName: (row.modelData.accessibleName || text)
                    + (descriptionText ? ". " + descriptionText : "")
                onClicked: menu.activate(row.index)
            }
            // Read in the action's own name above (issue #782).
            Text {
                objectName: "actionMenuDescription" + row.index
                width: parent.width
                Accessible.ignored: true
                visible: text.length > 0
                text: menu.actionDescription(row.index)
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
