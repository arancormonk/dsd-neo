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
    property string choosing: ""
    signal finished(var result)
    function begin(uri, name, kind, row) {
        reference = uri;
        fileName = name;
        type = kind;
        replaceRow = row === undefined ? -1 : row;
        companions = ({});
        requiredFiles = [];
        attempt();
    }
    function attempt() {
        var result = importedFiles.importBundle(reference, fileName, type, companions, replaceRow);
        if (result.error === "companions") {
            var names = requiredFiles.slice();
            for (var name of result.required)
                if (names.indexOf(name) < 0)
                    names.push(name);
            requiredFiles = names;
            sheet.visible = true;
            return;
        }
        sheet.visible = false;
        reference = "";
        companions = ({});
        finished(result);
    }
    FileDialog {
        id: picker
        onAccepted: {
            var next = Object.assign({}, flow.companions);
            next[flow.choosing] = selectedFile.toString();
            flow.companions = next;
        }
    }
    ModalSheet {
        id: sheet
        parent: flow.overlayParent
        accessibleName: qsTr("Channel-map companion files")
        onDismissed: {
            flow.reference = "";
            flow.companions = ({});
        }
        Text {
            width: parent.width
            text: qsTr("This channel map references additional files. Choose each file to store a complete, private copy with the map.")
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(15)
        }
        Repeater {
            model: flow.requiredFiles
            OutlineButton {
                required property string modelData
                width: parent.width
                text: (flow.companions[modelData] ? qsTr("Selected: ") : qsTr("Choose: ")) + modelData
                onClicked: {
                    flow.choosing = modelData;
                    picker.open();
                }
            }
        }
        GradientButton {
            width: parent.width
            text: qsTr("Import complete bundle")
            enabled: flow.requiredFiles.every(function (path) {
                return !!flow.companions[path];
            })
            onClicked: flow.attempt()
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Cancel")
            onClicked: {
                sheet.visible = false;
                flow.reference = "";
                flow.companions = ({});
                flow.finished({
                    ok: false,
                    error: "cancelled"
                });
            }
        }
    }
}
