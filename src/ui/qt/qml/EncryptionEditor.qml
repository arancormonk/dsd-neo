// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Shared draft editor. QString/QML copies cannot promise secure erasure.
// Only the TextInput holds/displays key material; labels contain shapes only.
Column {
    id: editor
    property string keyType: ""
    property alias keyValue: keyField.text
    property int forceMode: 0
    property string csvPath: ""
    property bool revealed: false
    readonly property string errorText: sessionArgs.keyError(keyType, keyValue, csvPath, forceMode)
    readonly property bool valid: errorText.length === 0
    spacing: 10

    function reset() {
        keyType = "";
        keyValue = "";
        forceMode = 0;
        revealed = false;
    }
    onKeyTypeChanged: revealed = false

    MicroLabel {
        text: qsTr("Encryption")
    }
    Flow {
        width: parent.width
        spacing: 8
        Repeater {
            model: [
                {
                    kind: "",
                    label: qsTr("None")
                },
                {
                    kind: "basic",
                    label: qsTr("Basic")
                },
                {
                    kind: "hex",
                    label: qsTr("Hex / AES")
                },
                {
                    kind: "rc4",
                    label: qsTr("RC4")
                },
                {
                    kind: "scrambler",
                    label: qsTr("Scrambler")
                }
            ]
            OutlineButton {
                required property var modelData
                width: implicitLabel.implicitWidth + 24
                text: modelData.label
                border.color: editor.keyType === modelData.kind ? Theme.cyan : Theme.controlBorder
                Text {
                    id: implicitLabel
                    visible: false
                    text: modelData.label
                    font.pixelSize: 15 * Theme.fontScale
                }
                onClicked: {
                    editor.keyType = modelData.kind;
                    editor.keyValue = "";
                }
            }
        }
    }
    Row {
        width: parent.width
        spacing: 8
        visible: editor.keyType.length > 0
        PlexTextField {
            id: keyField
            objectName: "encryptionKeyField"
            width: parent.width - eye.width - parent.spacing
            mono: true
            placeholderText: qsTr("Key")
            input.echoMode: editor.revealed ? TextInput.Normal : TextInput.Password
            inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
        }
        OutlineButton {
            id: eye
            objectName: "encryptionKeyEye"
            width: 82
            // Eye affordance plus a readable label; avoids relying on emoji fonts.
            Canvas {
                x: 5
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 10
                onPaint: {
                    var ctx = getContext("2d");
                    ctx.reset();
                    ctx.strokeStyle = Theme.textSecondary;
                    ctx.lineWidth = 1;
                    ctx.beginPath();
                    ctx.moveTo(0, 5);
                    ctx.quadraticCurveTo(7, -3, 14, 5);
                    ctx.quadraticCurveTo(7, 13, 0, 5);
                    ctx.stroke();
                    ctx.beginPath();
                    ctx.arc(7, 5, 2, 0, 2 * Math.PI);
                    ctx.stroke();
                }
            }
            text: editor.revealed ? qsTr("Hide") : qsTr("Show")
            onClicked: editor.revealed = !editor.revealed
        }
    }
    Text {
        objectName: "encryptionValidity"
        width: parent.width
        visible: text.length > 0
        text: editor.errorText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: 13 * Theme.fontScale
        color: Theme.textSecondary
    }
    ToggleRow {
        objectName: "forceKeyToggle"
        title: qsTr("Force key")
        subtitle: qsTr("Override signaled privacy identifiers")
        checked: editor.forceMode !== 0
        onToggled: function (on) {
            editor.forceMode = on ? (editor.keyType === "rc4" ? 2 : 1) : 0;
        }
    }
    Row {
        width: parent.width
        spacing: 8
        visible: editor.forceMode !== 0
        OutlineButton {
            width: (parent.width - 8) / 2
            text: qsTr("Privacy")
            border.color: editor.forceMode === 1 ? Theme.cyan : Theme.controlBorder
            onClicked: editor.forceMode = 1
        }
        OutlineButton {
            width: (parent.width - 8) / 2
            text: qsTr("RC4")
            border.color: editor.forceMode === 2 ? Theme.cyan : Theme.controlBorder
            onClicked: editor.forceMode = 2
        }
    }
}
