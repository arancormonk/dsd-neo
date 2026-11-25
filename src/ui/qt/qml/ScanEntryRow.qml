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
    // RTL gain applies only to an RTL-SDR or rtl_tcp list. On any other
    // source the gain is hidden, holds nothing and is left as stored.
    property bool gainOffered: true
    // The scan-list protocols in picker order (issue #890), from the one C++ catalog.
    readonly property var protocols: scanLists.protocols
    readonly property var protocolIds: protocols.map(function (p) {
        return p.id;
    })
    readonly property var modulationIds: ["", "c4fm", "cqpsk", "gfsk"]
    readonly property string systemFlag: entry.kind === "system" ? String(savedSystems.getByUid(entry.systemUid).decodeFlag || "") : ""
    // The catalog's entry for this one: a frequency entry's protocol, a saved system's decode
    // flags; empty for flags a scan list does not map (the list refuses those).
    readonly property var protocol: entry.kind === "freq" ? (protocols[protocolIds.indexOf(entry.protocol || "p25")] || ({}))
        : entry.kind === "system" ? scanLists.protocolForFlag(systemFlag) : ({})
    // An nfm or am entry (issue #526) holds on carrier: it has no digital modulation, keys or
    // talkgroups, so the modulation and decryption choices are hidden and never written for it.
    readonly property bool analog: entry.kind === "freq" ? (entry.protocol === "nfm" || entry.protocol === "am")
        : entry.kind === "system" && Util.decodeFlagIsAnalog(systemFlag)
    // Issue #890: dPMR, D-STAR, YSF, M17 and TETRA DMO run their class's own modulation, so the
    // choice shows only where the catalog offers one.
    readonly property bool modulationOffered: protocol.id === undefined ? !analog : protocol.modulation === true
    // The key matrix: P25, DMR and NXDN take any keys, dPMR its scrambler; M17 runs the
    // session's keys and the rest decrypt nothing, so neither shows a choice of its own.
    readonly property string keys: protocol.keys || (analog ? "none" : "full")
    readonly property bool decryptionOffered: keys === "full" || keys === "scrambler"
    // The gain choice: 0 Inherit, 1 Automatic, 2 Set. Once chosen it lives in
    // the editor's draft for this entry, so a Set whose text is refused stays
    // Set; until then it follows the stored -1 / 0 / n.
    readonly property int gainMode: {
        if (inputText.gainMode !== undefined)
            return inputText.gainMode;
        var stored = Number(entry.gainDb);
        return stored > 0 ? 2 : stored === 0 ? 1 : 0;
    }
    // The gain field has an error only under Set, while the gain is offered.
    readonly property bool inputError: dwellField.error.length > 0 || holdField.error.length > 0
        || gainField.error.length > 0 || (entry.kind === "freq" && frequencyField.error.length > 0)

    function fieldText(field, fallback) {
        return inputText[field] === undefined ? fallback : inputText[field];
    }

    signal changed(string field, var value)
    signal inputEdited(string field, string text, var value)
    // A gain choice or a keystroke in its field, with the field's text.
    signal gainEdited(int mode, string text)
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
            accessibleName: qsTr("Entry protocol")
            model: row.protocols.map(function (p) {
                return p.label;
            })
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
            visible: row.decryptionOffered || row.keys === "session"
            text: qsTr("Decryption profile")
        }
        // M17 has no keys of its own in a scan list: say where they come from instead of
        // offering a choice the row would refuse.
        ReadableText {
            objectName: "scanEntrySessionKeys"
            width: parent.width
            visible: row.keys === "session"
            text: qsTr("%1 entries use the session's keys. Set them in the app's decoder options.").arg(row.protocol.label || "")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSecondary
        }
        PlexComboBox {
            objectName: "scanEntryDecryptionScope"
            width: parent.width
            visible: row.decryptionOffered
            accessibleName: qsTr("Scan entry decryption scope")
            model: [row.entry.kind === "system" ? qsTr("Inherit saved-system profile") : qsTr("Inherit session defaults"), qsTr("Use a profile"), qsTr("No keys")]
            currentIndex: Math.max(0, ["inherit", "profile", "none"].indexOf(row.entry.decryptionMode || "inherit"))
            onActivated: row.changed("decryptionMode", ["inherit", "profile", "none"][currentIndex])
        }
        DecryptionProfileSelector {
            objectName: "scanEntryDecryptionProfile"
            width: parent.width
            visible: row.decryptionOffered && row.entry.decryptionMode === "profile" && available
            overlayParent: row.overlayParent
            profileUid: row.entry.decryptionProfileUid || ""
            // The catalog's profile protocol (dPMR's own for its scrambler), else what the flags read as.
            protocol: row.protocol.profileProtocol || (row.entry.kind === "system" ? Util.decryptionProtocol(row.systemFlag) : "mixed")
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
            visible: row.modulationOffered
            text: qsTr("Modulation")
        }
        PlexComboBox {
            objectName: "scanEntryModulation"
            width: parent.width
            visible: row.modulationOffered
            accessibleName: qsTr("Entry modulation")
            model: [qsTr("Inherit"), qsTr("C4FM"), qsTr("QPSK (simulcast)"), qsTr("GFSK")]
            currentIndex: Math.max(0, row.modulationIds.indexOf(row.entry.modulation || ""))
            onActivated: row.changed("modulation", row.modulationIds[currentIndex])
        }
        Column {
            width: parent.width
            spacing: 6
            visible: row.gainOffered

            MicroLabel {
                text: qsTr("Gain")
            }
            PlexComboBox {
                objectName: "scanEntryGainMode"
                width: parent.width
                accessibleName: qsTr("Entry gain")
                model: [qsTr("Inherit"), qsTr("Automatic"), qsTr("Set")]
                currentIndex: row.gainMode
                onActivated: function (index) {
                    if (index !== row.gainMode)
                        row.gainEdited(index, gainField.text);
                }
            }
            ReadableText {
                objectName: "scanEntryGainHint"
                width: parent.width
                visible: row.gainMode === 0
                text: row.entry.kind === "system" ? qsTr("Uses the saved system's gain, else the list's") : qsTr("Uses the list's gain")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(12)
                color: Theme.textSecondary
            }
            PlexTextField {
                id: gainField
                function parsedValue() {
                    return row.parseInteger(text, 1, 49);
                }
                objectName: "scanEntryGain"
                width: parent.width
                visible: row.gainMode === 2
                label: qsTr("Gain")
                unit: qsTr("dB")
                mono: true
                text: row.fieldText("gainDb", Number(row.entry.gainDb) > 0 ? String(Number(row.entry.gainDb)) : "")
                error: row.gainOffered && row.gainMode === 2 && isNaN(parsedValue()) ? qsTr("Enter a gain from 1 to 49 dB.") : ""
                inputMethodHints: Qt.ImhDigitsOnly
                input.onTextEdited: row.gainEdited(2, text)
                input.validator: RegularExpressionValidator {
                    regularExpression: /^-?[0-9]*$/
                }
                // A hidden field keeps no focus or keyboard: the field's own
                // release (see GainEditor).
            }
        }
    }
}
