// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

Rectangle {
    id: screen

    property int editRow: -1
    property string editUid: ""
    property var draft: ({})
    property var entries: []
    property string validationText: ""

    signal closed
    property string initialDraft: ""
    function fingerprint() {
        return JSON.stringify({
            draft: draft,
            entries: entries
        });
    }
    function requestClose() {
        if (initialDraft !== fingerprint())
            discardDialog.ask(function () {
                screen.closed();
            });
        else
            closed();
    }
    DiscardDialog {
        id: discardDialog
    }
    ModalSheet {
        id: removeDialog
        accessibleName: qsTr("Remove scan list")
        Text {
            width: parent.width
            text: qsTr("Remove %1?").arg(screen.draft.name || "")
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(18)
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Cancel")
            onClicked: removeDialog.visible = false
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Remove list")
            onClicked: {
                removeDialog.visible = false;
                if (scanLists.remove(scanLists.rowForUid(screen.editUid)))
                    screen.closed();
                else
                    screen.validationText = qsTr("Could not remove the scan list.");
            }
        }
    }

    function openFor(row) {
        editRow = row;
        draft = row >= 0 ? scanLists.get(row) : scanLists.newDraft();
        editUid = row >= 0 ? draft.uid || "" : "";
        entries = draft.entries || [];
        validationText = "";
        initialDraft = fingerprint();
    }

    function setEntry(row, field, value) {
        entries[row][field] = value;
        entryRows.setProperty(row, "entryData", entries[row]);
    }

    function addFrequency(name, protocol, freq) {
        entries = entries.concat([
            {
                "uid": scanLists.newEntryId(),
                "kind": "freq",
                "name": name,
                "protocol": protocol,
                "freqMhz": freq,
                "enabled": true,
                "dwellMs": 0,
                "holdMs": 0,
                "modulation": "",
                "gainDb": -1
            }
        ]);
    }

    function addSystem(uid) {
        entries = entries.concat([
            {
                "uid": scanLists.newEntryId(),
                "kind": "system",
                "systemUid": uid,
                "enabled": true,
                "dwellMs": 0,
                "holdMs": 0,
                "modulation": "",
                "gainDb": -1
            }
        ]);
    }

    function removeEntry(row) {
        var copy = entries.slice();
        copy.splice(row, 1);
        entries = copy;
    }

    function moveEntry(row, delta) {
        var dest = row + delta;
        if (dest < 0 || dest >= entries.length)
            return;

        var copy = entries.slice();
        var value = copy.splice(row, 1)[0];
        copy.splice(dest, 0, value);
        entries = copy;
    }

    function persist() {
        if (!draft.name || !draft.name.trim().length) {
            validationText = qsTr("Name this scan list.");
            return false;
        }
        draft.entries = entries;
        var row = editUid.length ? scanLists.rowForUid(editUid) : -1;
        if (editUid.length && row < 0) {
            validationText = qsTr("This scan list was removed.");
            return false;
        }
        if (row >= 0) {
            if (!scanLists.update(row, draft)) {
                validationText = qsTr("Could not save the scan list.");
                return false;
            }
        } else {
            if (!scanLists.add(draft)) {
                validationText = qsTr("Could not save the scan list.");
                return false;
            }
            row = scanLists.count - 1;
        }
        editRow = row;
        draft = scanLists.get(row);
        editUid = draft.uid;
        entries = draft.entries;
        return true;
    }

    function validate() {
        var result = scanListStarter.validate(Object.assign({}, draft, {
            entries: entries,
            isDraft: false
        }));
        validationText = result.ok ? qsTr("%1 targets ready.").arg(result.targetCount) + "\n" + result.warnings.join("\n") : result.error;
        return result.ok;
    }
    function save() {
        if (!validate())
            return;
        draft.isDraft = false;
        if (persist())
            closed();
    }
    function saveDraft() {
        draft.isDraft = true;
        if (persist())
            closed();
    }

    function entryLabel(entry) {
        if (entry.kind === "freq")
            return (entry.name || entry.protocol) + " · " + entry.freqMhz + " MHz";

        var sys = savedSystems.getByUid(entry.systemUid);
        return sys.name || qsTr("Missing saved system");
    }

    function rebuildEntryRows() {
        entryRows.clear();
        for (var i = 0; i < entries.length; ++i)
            entryRows.append({
                "entryData": entries[i]
            });
    }

    // A stable model updates fields without destroying the focused editor.
    onEntriesChanged: {
        if (entryRows)
            rebuildEntryRows();
    }
    Component.onCompleted: rebuildEntryRows()
    color: Theme.bg

    ListModel {
        id: entryRows

        dynamicRoles: true
    }

    PlexFlickable {
        anchors.fill: parent
        clip: true
        contentHeight: content.implicitHeight + 32

        Column {
            id: content

            x: (parent.width - width) / 2
            y: 16
            width: Math.min(Theme.formWidth, parent.width - 32)
            spacing: 10

            Text {
                text: qsTr("Scan list")
                color: Theme.textPrimary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(24)
            }

            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("The list's tuner replaces each system's source, host, port, PPM, bandwidth and bias tee. System groups and keys stay isolated. 0 timing, -1 gain/bandwidth and blank PPM inherit defaults.")
                color: Theme.textSecondary
                font.family: Theme.sans
            }

            PlexInput {
                width: parent.width
                placeholderText: qsTr("List name")
                text: screen.draft.name || ""
                onTextEdited: screen.draft.name = text
            }

            PlexComboBox {
                width: parent.width
                Accessible.name: qsTr("Input source")
                model: ["usb", "rtltcp"]
                currentIndex: screen.draft.sourceType === "rtltcp" ? 1 : 0
                onActivated: {
                    var copy = screen.draft;
                    copy.sourceType = currentText;
                    screen.draft = Object.assign({}, copy);
                }
            }

            PlexInput {
                width: parent.width
                visible: screen.draft.sourceType === "rtltcp"
                placeholderText: qsTr("RTL-TCP host")
                text: screen.draft.host || ""
                onTextEdited: screen.draft.host = text
            }

            PlexInput {
                width: parent.width
                visible: screen.draft.sourceType === "rtltcp"
                placeholderText: qsTr("Port")
                text: screen.draft.port || "1234"
                onTextEdited: screen.draft.port = Number(text)

                validator: IntValidator {
                    bottom: 1
                    top: 65535
                }
            }

            Repeater {
                model: [
                    {
                        "key": "gainDb",
                        "label": qsTr("Gain dB (-1 inherits)"),
                        "fallback": -1
                    },
                    {
                        "key": "ppm",
                        "label": qsTr("PPM (blank inherits)"),
                        "fallback": ""
                    },
                    {
                        "key": "bandwidthKhz",
                        "label": qsTr("Bandwidth kHz (-1 inherits)"),
                        "fallback": -1
                    },
                    {
                        "key": "defaultDwellMs",
                        "label": qsTr("Default dwell ms (0 inherits)"),
                        "fallback": 0
                    },
                    {
                        "key": "defaultHoldMs",
                        "label": qsTr("Default hold ms (0 inherits)"),
                        "fallback": 0
                    }
                ]

                Column {
                    required property var modelData

                    width: content.width
                    spacing: 3

                    Text {
                        text: modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.sans
                    }

                    PlexInput {
                        width: parent.width
                        Accessible.name: modelData.label
                        text: screen.draft[modelData.key] === undefined ? modelData.fallback : screen.draft[modelData.key]
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        onTextEdited: screen.draft[modelData.key] = text
                    }
                }
            }

            PlexComboBox {
                width: parent.width
                Accessible.name: qsTr("Bias tee")
                model: [qsTr("Bias tee: inherit"), qsTr("Bias tee: off"), qsTr("Bias tee: on")]
                currentIndex: (screen.draft.biasTee === undefined ? -1 : screen.draft.biasTee) + 1
                onActivated: screen.draft.biasTee = currentIndex - 1
            }

            PlexCheckBox {
                text: qsTr("Voice only")
                checked: screen.draft.voiceOnly || false
                onToggled: screen.draft.voiceOnly = checked
            }

            Repeater {
                model: [
                    {
                        "key": "groupCsvPath",
                        "type": "group",
                        "label": qsTr("Fallback group list")
                    },
                    {
                        "key": "srcCsvPath",
                        "type": "src",
                        "label": qsTr("Global source aliases")
                    }
                ]

                Column {
                    required property var modelData

                    width: content.width
                    spacing: 4

                    Text {
                        text: modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.sans
                    }

                    PlexComboBox {
                        property var files: (importedFiles.count, importedFiles.entriesForType(modelData.type))

                        width: parent.width
                        Accessible.name: modelData.label
                        model: [
                            {
                                "name": qsTr("None"),
                                "path": ""
                            }
                        ].concat(files)
                        textRole: "name"
                        currentIndex: {
                            for (var i = 0; i < model.length; ++i)
                                if (model[i].path === (screen.draft[modelData.key] || "")) {
                                    return i;
                                }
                            return -1;
                        }
                        onActivated: screen.draft[modelData.key] = model[currentIndex].path
                    }
                }
            }

            Text {
                text: qsTr("Entries")
                color: Theme.textPrimary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(19)
            }

            Repeater {
                model: entryRows

                ScanEntryRow {
                    overlayParent: screen
                    required property int index
                    required property var entryData

                    width: content.width
                    entry: entryData
                    label: screen.entryLabel(entryData)
                    onChanged: function (field, value) {
                        screen.setEntry(index, field, value);
                    }
                    onMove: function (delta) {
                        screen.moveEntry(index, delta);
                    }
                    onRemove: screen.removeEntry(index)
                }
            }

            PlexComboBox {
                id: systemPicker

                width: parent.width
                Accessible.name: qsTr("Saved system to add")
                model: savedSystems
                textRole: "name"
            }

            OutlineButton {
                width: parent.width
                text: qsTr("Add saved system")
                enabled: systemPicker.currentIndex >= 0
                onClicked: screen.addSystem(savedSystems.get(systemPicker.currentIndex).uid)
            }

            PlexInput {
                id: freqName

                width: parent.width
                placeholderText: qsTr("Frequency name")
            }

            PlexComboBox {
                id: protocol

                width: parent.width
                Accessible.name: qsTr("Frequency protocol")
                model: ["p25", "dmr", "nxdn48", "nxdn"]
            }

            PlexInput {
                id: frequency

                width: parent.width
                placeholderText: qsTr("Frequency MHz")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
            }

            OutlineButton {
                width: parent.width
                text: qsTr("Add frequency")
                enabled: sessionArgs.freqValid(frequency.text)
                onClicked: {
                    screen.addFrequency(freqName.text, protocol.currentText, frequency.text);
                    freqName.clear();
                    frequency.clear();
                }
            }

            Text {
                width: parent.width
                text: screen.validationText
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.sans
            }

            OutlineButton {
                width: parent.width
                text: qsTr("Validate")
                objectName: "validateScanDraft"
                onClicked: screen.validate()
            }
            OutlineButton {
                width: parent.width
                text: qsTr("Save draft")
                onClicked: screen.saveDraft()
            }

            OutlineButton {
                width: parent.width
                text: qsTr("Save")
                onClicked: screen.save()
            }

            OutlineButton {
                width: parent.width
                text: qsTr("Remove list")
                visible: screen.editUid.length > 0
                onClicked: removeDialog.visible = true
            }

            OutlineButton {
                width: parent.width
                text: qsTr("Cancel")
                onClicked: screen.requestClose()
            }
        }
    }
}
