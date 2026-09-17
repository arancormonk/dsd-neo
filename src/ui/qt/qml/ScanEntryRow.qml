// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import "Util.js" as Util

UiPanel {
    id: row

    property var entry: ({})
    property var inputText: ({})
    required property var parseInteger
    required property var frequencyValid
    property string label: ""
    property var overlayParent: parent
    property bool canMoveUp: true
    property bool canMoveDown: true
    readonly property var protocolIds: ["p25", "dmr", "nxdn48", "nxdn"]
    readonly property var modulationIds: ["", "c4fm", "cqpsk", "gfsk"]
    readonly property bool inputError: dwellField.error.length > 0 || holdField.error.length > 0
        || gainField.error.length > 0 || (entry.kind === "freq" && frequencyField.error.length > 0)

    function fieldText(field, fallback) {
        return inputText[field] === undefined ? fallback : inputText[field];
    }

    signal changed(string field, var value)
    signal inputEdited(string field, string text, var value)
    signal move(int delta)
    signal remove

    objectName: "scanEntryRow"
    height: fields.implicitHeight + 24

    Column {
        id: fields

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        y: 12
        spacing: 10

        PlexCheckBox {
            objectName: "scanEntryEnabled"
            width: parent.width
            text: row.label
            checked: row.entry.enabled !== false
            onToggled: row.changed("enabled", checked)
        }

        Row {
            width: parent.width
            spacing: 6

            OutlineButton {
                objectName: "scanEntryUp"
                text: qsTr("↑")
                accessibleName: qsTr("Move entry up")
                enabled: row.canMoveUp
                onClicked: row.move(-1)
            }
            OutlineButton {
                objectName: "scanEntryDown"
                text: qsTr("↓")
                accessibleName: qsTr("Move entry down")
                enabled: row.canMoveDown
                onClicked: row.move(1)
            }
            OutlineButton {
                objectName: "scanEntryRemove"
                text: qsTr("Remove")
                onClicked: row.remove()
            }
        }

        PlexTextField {
            objectName: "scanFrequencyName"
            width: parent.width
            visible: row.entry.kind === "freq"
            label: qsTr("Frequency name")
            text: row.entry.name || ""
            input.onTextEdited: row.changed("name", text)
        }
        MicroLabel {
            visible: row.entry.kind === "freq"
            text: qsTr("Protocol")
        }
        PlexComboBox {
            objectName: "scanEntryProtocol"
            width: parent.width
            visible: row.entry.kind === "freq"
            Accessible.name: qsTr("Entry protocol")
            model: [qsTr("P25"), qsTr("DMR"), qsTr("NXDN48"), qsTr("NXDN96")]
            currentIndex: Math.max(0, row.protocolIds.indexOf(row.entry.protocol || "p25"))
            onActivated: row.changed("protocol", row.protocolIds[currentIndex])
        }
        PlexTextField {
            id: frequencyField
            objectName: "scanEntryFrequency"
            width: parent.width
            visible: row.entry.kind === "freq"
            label: qsTr("Frequency")
            unit: qsTr("MHz")
            mono: true
            text: row.fieldText("freqMhz", row.entry.freqMhz || "")
            error: row.frequencyValid(text) ? "" : qsTr("Enter a valid frequency.")
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            input.validator: RegularExpressionValidator {
                regularExpression: /^[0-9]{1,5}(\.[0-9]{0,6})?$/
            }
            input.onTextEdited: row.inputEdited("freqMhz", text, row.frequencyValid(text) ? text : "")
        }

        MicroLabel {
            text: qsTr("Decryption profile")
        }
        PlexComboBox {
            objectName: "scanEntryDecryptionScope"
            width: parent.width
            Accessible.name: qsTr("Scan entry decryption scope")
            model: [row.entry.kind === "system" ? qsTr("Inherit saved-system profile") : qsTr("Inherit session defaults"), qsTr("Use a profile"), qsTr("No keys")]
            currentIndex: Math.max(0, ["inherit", "profile", "none"].indexOf(row.entry.decryptionMode || "inherit"))
            onActivated: row.changed("decryptionMode", ["inherit", "profile", "none"][currentIndex])
        }
        DecryptionProfileSelector {
            objectName: "scanEntryDecryptionProfile"
            width: parent.width
            visible: row.entry.decryptionMode === "profile" && available
            overlayParent: row.overlayParent
            profileUid: row.entry.decryptionProfileUid || ""
            protocol: row.entry.kind === "system" ? Util.decryptionProtocol(savedSystems.getByUid(row.entry.systemUid).decodeFlag) : String(row.entry.protocol || "mixed").indexOf("nxdn") === 0 ? "nxdn" : row.entry.protocol || "mixed"
            onSelected: function (uid) {
                row.changed("decryptionProfileUid", uid);
            }
        }

        Row {
            width: parent.width
            spacing: 6

            PlexTextField {
                id: dwellField
                function parsedValue() {
                    return row.parseInteger(text, 0, 600000);
                }
                objectName: "scanEntryDwell"
                width: (fields.width - 6) / 2
                label: qsTr("Dwell")
                unit: qsTr("ms")
                hint: qsTr("List default when empty")
                mono: true
                text: row.fieldText("dwellMs", Number(row.entry.dwellMs) > 0 ? String(Number(row.entry.dwellMs)) : "")
                error: text.length && isNaN(parsedValue()) ? qsTr("Enter a time from 0 to 600000 ms.") : ""
                inputMethodHints: Qt.ImhDigitsOnly
                input.onTextEdited: row.inputEdited("dwellMs", text, isNaN(parsedValue()) ? 0 : parsedValue())
                input.validator: RegularExpressionValidator {
                    regularExpression: /^-?[0-9]*$/
                }
            }
            PlexTextField {
                id: holdField
                function parsedValue() {
                    return row.parseInteger(text, 0, 600000);
                }
                objectName: "scanEntryHold"
                width: (fields.width - 6) / 2
                label: qsTr("Hold")
                unit: qsTr("ms")
                hint: qsTr("List default when empty")
                mono: true
                text: row.fieldText("holdMs", Number(row.entry.holdMs) > 0 ? String(Number(row.entry.holdMs)) : "")
                error: text.length && isNaN(parsedValue()) ? qsTr("Enter a time from 0 to 600000 ms.") : ""
                inputMethodHints: Qt.ImhDigitsOnly
                input.onTextEdited: row.inputEdited("holdMs", text, isNaN(parsedValue()) ? 0 : parsedValue())
                input.validator: RegularExpressionValidator {
                    regularExpression: /^-?[0-9]*$/
                }
            }
        }
        MicroLabel {
            text: qsTr("Modulation")
        }
        PlexComboBox {
            objectName: "scanEntryModulation"
            width: parent.width
            Accessible.name: qsTr("Entry modulation")
            model: [qsTr("Inherit"), qsTr("C4FM"), qsTr("QPSK (simulcast)"), qsTr("GFSK")]
            currentIndex: Math.max(0, row.modulationIds.indexOf(row.entry.modulation || ""))
            onActivated: row.changed("modulation", row.modulationIds[currentIndex])
        }
        PlexTextField {
            id: gainField
            function parsedValue() {
                return row.parseInteger(text, 0, 49);
            }
            objectName: "scanEntryGain"
            width: parent.width
            label: qsTr("Gain")
            unit: qsTr("dB")
            hint: qsTr("From saved system when empty")
            mono: true
            text: row.fieldText("gainDb", Number(row.entry.gainDb) >= 0 ? String(Number(row.entry.gainDb)) : "")
            error: text.length && isNaN(parsedValue()) ? qsTr("Enter a gain from 0 to 49 dB.") : ""
            inputMethodHints: Qt.ImhDigitsOnly
            input.onTextEdited: row.inputEdited("gainDb", text, isNaN(parsedValue()) ? -1 : parsedValue())
            input.validator: RegularExpressionValidator {
                regularExpression: /^-?[0-9]*$/
            }
        }
    }
}
