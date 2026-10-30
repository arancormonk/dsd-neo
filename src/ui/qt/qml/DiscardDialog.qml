// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    accessibleName: question.length ? question : qsTr("Discard changes?")
    property var discardAction: null
    // A surface whose outstanding work is not an unsaved edit names what
    // closing gives up instead (askWith()); ask() keeps the usual wording.
    property string question: ""
    property string discardLabel: ""
    function ask(action) {
        askWith(action, "", "");
    }
    function askWith(action, questionText, discardText) {
        question = questionText;
        discardLabel = discardText;
        discardAction = action;
        visible = true;
    }
    ReadableText {
        objectName: "discardDialogQuestion"
        width: parent.width
        text: sheet.question.length ? sheet.question : qsTr("Discard unsaved changes?")
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(20)
    }
    GradientButton {
        objectName: "discardDialogKeep"
        width: parent.width
        text: qsTr("Keep editing")
        onClicked: {
            sheet.discardAction = null;
            sheet.visible = false;
        }
    }
    OutlineButton {
        objectName: "discardDialogDiscard"
        width: parent.width
        text: sheet.discardLabel.length ? sheet.discardLabel : qsTr("Discard changes")
        onClicked: {
            var action = sheet.discardAction;
            sheet.discardAction = null;
            sheet.visible = false;
            if (action)
                action();
        }
    }
    onDismissed: discardAction = null
    // Whatever closes the surface under the question takes the question with
    // it. Hidden only through its parent, the dialog would keep its own visible
    // flag and stand over the surface again the next time that opens.
    Connections {
        target: sheet.parent
        function onVisibleChanged() {
            if (sheet.parent.visible)
                return;
            sheet.discardAction = null;
            sheet.visible = false;
        }
    }
}
