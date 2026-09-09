// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

ModalSheet {
    id: sheet
    property var bridge: commands
    property string backendMessage: metrics.uiMessage
    property string localMessage: ""
    property var idStart: 0
    property var idEnd: 0
    property string policyContext: "0"
    property double policyGeneration: 0
    property int priorityValue: 0
    property bool listed: false
    property bool removeArmed: false
    property string saveMessage: ""
    property bool canSaveList: false
    signal saveListRequested

    function openRow(start, end, name, listening, isListed, priority, preempt, context, generation) {
        idStart = start;
        idEnd = end;
        policyContext = context;
        policyGeneration = generation;
        nameField.text = name;
        listenToggle.checked = listening;
        listed = isListed;
        priorityValue = priority;
        preemptToggle.checked = preempt;
        removeArmed = false;
        localMessage = "";
        visible = true;
    }
    function saveRow() {
        var ok = listed ? bridge.setTalkgroupPolicy(idStart, idEnd, policyContext, policyGeneration, nameField.text, listenToggle.checked, priorityValue, preemptToggle.checked) : bridge.addTalkgroup(idStart, idEnd, policyContext, policyGeneration, nameField.text, listenToggle.checked, priorityValue, preemptToggle.checked);
        localMessage = ok ? qsTr("Update requested") : qsTr("Update refused. Use a name shorter than 50 UTF-8 bytes.");
    }

    Text {
        text: qsTr("Talkgroup %1").arg(sheet.idStart === sheet.idEnd ? sheet.idStart : sheet.idStart + "–" + sheet.idEnd)
        color: Theme.textPrimary
        font.pixelSize: 20
    }
    PlexTextField {
        id: nameField
        objectName: "talkgroupName"
        width: parent.width
        placeholderText: qsTr("Name")
    }
    Switch {
        id: listenToggle
        objectName: "listeningToggle"
        text: checked ? qsTr("Listening") : qsTr("Not tuned")
    }
    Text {
        text: qsTr("Priority · %1").arg(sheet.priorityValue)
        color: Theme.textPrimary
    }
    Row {
        width: parent.width
        spacing: 6
        Repeater {
            model: [0, 25, 50, 100]
            OutlineButton {
                required property int modelData
                objectName: "priorityPreset" + modelData
                width: (parent.width - 18) / 4
                height: 44
                text: String(modelData)
                onClicked: sheet.priorityValue = modelData
            }
        }
    }
    Row {
        spacing: 10
        OutlineButton {
            objectName: "priorityMinus"
            width: 80
            height: 44
            text: "−5"
            onClicked: sheet.priorityValue = Math.max(0, sheet.priorityValue - 5)
        }
        OutlineButton {
            objectName: "priorityPlus"
            width: 80
            height: 44
            text: "+5"
            onClicked: sheet.priorityValue = Math.min(100, sheet.priorityValue + 5)
        }
    }
    Switch {
        id: preemptToggle
        objectName: "preemptToggle"
        text: qsTr("Preempt")
        enabled: sheet.priorityValue > 0
    }
    Text {
        text: qsTr("P25 trunking only")
        color: Theme.textSecondary
    }
    GradientButton {
        objectName: "saveTalkgroup"
        width: parent.width
        text: sheet.listed ? qsTr("Apply") : qsTr("Add to list")
        onClicked: sheet.saveRow()
    }
    OutlineButton {
        objectName: "removeTalkgroup"
        width: parent.width
        height: 44
        visible: sheet.listed
        text: sheet.removeArmed ? qsTr("Tap again to remove") : qsTr("Remove")
        onClicked: {
            if (!sheet.removeArmed) {
                sheet.removeArmed = true;
                return;
            }
            sheet.removeArmed = false;
            sheet.localMessage = sheet.bridge.removeTalkgroup(sheet.idStart, sheet.idEnd, sheet.policyContext, sheet.policyGeneration) ? qsTr("Removal requested") : qsTr("Removal refused");
        }
    }
    OutlineButton {
        width: parent.width
        height: 44
        visible: sheet.canSaveList
        text: qsTr("Save talkgroup list")
        onClicked: sheet.saveListRequested()
    }
    Text {
        width: parent.width
        text: sheet.saveMessage
        visible: text.length > 0
        color: Theme.textSecondary
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "talkgroupEditToast"
        width: parent.width
        text: sheet.backendMessage.length > 0 ? sheet.backendMessage : sheet.localMessage
        color: Theme.textSecondary
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }
    OutlineButton {
        width: parent.width
        height: 44
        text: qsTr("Close")
        onClicked: {
            sheet.visible = false;
            Qt.inputMethod.hide();
        }
    }
}
