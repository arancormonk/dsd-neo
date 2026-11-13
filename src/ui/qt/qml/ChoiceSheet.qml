// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    property var choices: []
    property var chooseAction: null
    // When set, choice button i is named prefix + i, so a test can tap the real one.
    property string choiceObjectNamePrefix: ""
    // The choice in force, shown and read as checked; -1 (the default) for a
    // sheet whose choices are plain actions.
    property int currentIndex: -1
    function open(title, values, action, current) {
        accessibleName = title;
        choices = values;
        chooseAction = action;
        currentIndex = typeof current === "number" ? current : -1;
        if (visible)
            return;
        // The opener is read before the sheet shows, as ModalSheet.open()
        // does: one the sheet disables as it shows (Home's Sort pill, under
        // the body a management sheet disables) has lost the focus by then,
        // and the focus, TalkBack's with it, went back to the screen instead.
        var window = sheet.Window.window;
        var opener = window ? window.activeFocusItem : null;
        visible = true;
        previousFocus = opener;
    }
    Text {
        width: parent.width
        text: sheet.accessibleName
        // A title can quote a name ("Export <folder>"): drawn as written.
        textFormat: Text.PlainText
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
            checkable: sheet.currentIndex >= 0
            checked: index === sheet.currentIndex
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
