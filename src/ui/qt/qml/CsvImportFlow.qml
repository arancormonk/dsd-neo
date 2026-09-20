// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Dialogs

Item {
    id: flow
    property var overlayParent: parent
    property string reference: ""
    property string fileName: ""
    property string type: ""
    property int replaceRow: -1
    property var companions: ({})
    property var requiredFiles: []
    property var requiredLabels: ({})
    property var requiredDetails: ({})
    property string choosing: ""
    property bool active: false
    signal finished(var result)

    // One completion per pick()/begin(). A companion rejection only abandons
    // that slot's choice; the enclosing import remains active on the sheet.
    function start(kind, row) {
        cancel();
        reference = "";
        fileName = "";
        type = kind;
        replaceRow = row === undefined ? -1 : row;
        companions = ({});
        requiredFiles = [];
        requiredLabels = ({});
        requiredDetails = ({});
        active = true;
    }
    function pick(kind, row) {
        start(kind, row);
        primaryPicker.open();
    }
    function begin(uri, name, kind, row) {
        start(kind, row);
        reference = uri;
        fileName = name;
        attempt();
    }
    function complete(result) {
        if (!active)
            return;
        active = false;
        primaryPicker.close();
        picker.close();
        sheet.visible = false;
        reference = "";
        fileName = "";
        choosing = "";
        companions = ({});
        requiredFiles = [];
        requiredLabels = ({});
        requiredDetails = ({});
        finished(result);
    }
    function cancel() {
        complete({ok: false, error: "cancelled"});
    }
    function selectCompanion(key, path) {
        var next = Object.assign({}, companions);
        next[key] = path;
        companions = next;
    }
    function chooseFile(key) {
        choosing = key;
        selectCompanion(key, "");
        picker.open();
    }
    function showRequirements(result) {
        var details = result.requiredDetails || ({});
        var names = requiredFiles.filter(function (key) { return !!details[key]; });
        var next = Object.assign({}, companions);
        for (var key of result.required) {
            if (details[key] && details[key].selectionError)
                next[key] = "";
            if (names.indexOf(key) >= 0)
                continue;
            names.push(key);
            if (next[key] === undefined && details[key] && details[key].preselected)
                next[key] = details[key].preselected;
        }
        requiredFiles = names;
        companions = next;
        requiredLabels = result.requiredLabels || ({});
        requiredDetails = details;
        sheet.visible = true;
    }
    function attempt() {
        if (!active)
            return;
        var result = importedFiles.importBundle(reference, fileName, type, companions, replaceRow);
        if (result.error === "companions") {
            showRequirements(result);
            return;
        }
        complete(result);
    }
    // No MIME filter: Android providers index CSV under several MIME types.
    FileDialog {
        id: primaryPicker
        objectName: "csvPrimaryPicker"
        onAccepted: {
            if (!flow.active)
                return;
            flow.reference = selectedFile.toString();
            flow.fileName = flow.reference.substring(flow.reference.lastIndexOf('/') + 1);
            flow.attempt();
        }
        onRejected: flow.cancel()
    }
    FileDialog {
        id: picker
        objectName: "csvCompanionPicker"
        onAccepted: {
            if (flow.active && flow.choosing)
                flow.selectCompanion(flow.choosing, selectedFile.toString());
            flow.choosing = "";
        }
        onRejected: {
            flow.choosing = "";
            if (flow.active)
                sheet.visible = true;
        }
    }
    ModalSheet {
        id: sheet
        objectName: "csvCompanionSheet"
        parent: flow.overlayParent
        accessibleName: flow.type === "trunkTargets" ? qsTr("Target CSV companion files") : qsTr("Channel-map companion files")
        onDismissed: flow.cancel()
        Text {
            width: parent.width
            text: qsTr("This CSV references additional files. Choose an imported file or a file from your device for each companion to store a complete, private copy.")
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(15)
        }
        Repeater {
            model: flow.requiredFiles
            Column {
                id: slot
                required property string modelData
                required property int index
                readonly property var detail: flow.requiredDetails[modelData] || ({})
                readonly property string kind: detail.kind || ""
                width: parent.width
                spacing: 8
                Text {
                    objectName: "csvCompanionLabel_" + slot.index
                    width: parent.width
                    text: qsTr("%1 · %2").arg(slot.kind.charAt(0).toUpperCase() + slot.kind.slice(1)).arg(flow.requiredLabels[slot.modelData] || slot.modelData)
                    wrapMode: Text.Wrap
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSize(15)
                }
                Text {
                    objectName: "csvCompanionWarning_" + slot.index
                    width: parent.width
                    text: slot.detail.warning || ""
                    visible: text.length > 0
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSize(13)
                }
                Repeater {
                    model: slot.detail.candidates || []
                    OutlineButton {
                        required property var modelData
                        required property int index
                        readonly property bool checked: flow.companions[slot.modelData] === modelData.path
                        objectName: "csvLibraryChoice_" + slot.index + "_" + index
                        width: parent.width
                        accessibleName: (modelData.accepted === 1 ? qsTr("Use imported %1 (%2, %3 row)") : qsTr("Use imported %1 (%2, %3 rows)")).arg(modelData.name).arg(modelData.kind).arg(modelData.accepted)
                        text: (checked ? "● " : "○ ") + accessibleName
                        Accessible.role: Accessible.RadioButton
                        Accessible.checkable: true
                        Accessible.checked: checked
                        onClicked: flow.selectCompanion(slot.modelData, modelData.path)
                    }
                }
                OutlineButton {
                    objectName: "csvChooseFile_" + slot.index
                    width: parent.width
                    text: qsTr("Choose a file…")
                    onClicked: flow.chooseFile(slot.modelData)
                }
                Text {
                    width: parent.width
                    text: flow.companions[slot.modelData] ? qsTr("Selected: %1").arg(flow.companions[slot.modelData].split('/').pop()) : qsTr("No file selected")
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSize(13)
                }
            }
        }
        GradientButton {
            objectName: "csvCompleteBundle"
            width: parent.width
            text: qsTr("Import complete bundle")
            enabled: flow.requiredFiles.every(function (path) {
                return !!flow.companions[path];
            })
            onClicked: flow.attempt()
        }
        OutlineButton {
            objectName: "csvCancel"
            width: parent.width
            text: qsTr("Cancel")
            onClicked: flow.cancel()
        }
    }
}
