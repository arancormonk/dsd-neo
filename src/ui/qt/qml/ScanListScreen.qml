// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

Rectangle {
    id: screen

    property int editRow: -1
    property string editUid: ""
    property var draft: ({
    })
    property var entries: []
    property string validationText: ""

    signal closed()

    function openFor(row) {
        editRow = row;
        draft = row >= 0 ? scanLists.get(row) : {
            "name": "",
            "sourceType": "usb",
            "host": "",
            "port": 1234,
            "gainDb": -1,
            "ppm": "",
            "bandwidthKhz": -1,
            "biasTee": -1,
            "voiceOnly": false,
            "defaultDwellMs": 0,
            "defaultHoldMs": 0,
            "groupCsvPath": "",
            "srcCsvPath": ""
        };
        editUid = draft.uid || "";
        entries = draft.entries || [];
        validationText = "";
    }

    function setEntry(row, field, value) {
        entries[row][field] = value;
        entryRows.setProperty(row, "entryData", entries[row]);
    }

    function addFrequency(name, protocol, freq) {
        entries = entries.concat([{
            "kind": "freq",
            "name": name,
            "protocol": protocol,
            "freqMhz": freq,
            "enabled": true,
            "dwellMs": 0,
            "holdMs": 0,
            "modulation": "",
            "gainDb": -1
        }]);
    }

    function addSystem(uid) {
        entries = entries.concat([{
            "kind": "system",
            "systemUid": uid,
            "enabled": true,
            "dwellMs": 0,
            "holdMs": 0,
            "modulation": "",
            "gainDb": -1
        }]);
    }

    function removeEntry(row) {
        var copy = entries.slice();
        copy.splice(row, 1);
        entries = copy;
    }

    function moveEntry(row, delta) {
        var dest = row + delta;
        if (dest < 0 || dest >= entries.length)
            return ;

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
            scanLists.update(row, draft);
        } else {
            scanLists.add(draft);
            row = scanLists.count - 1;
        }
        editRow = row;
        draft = scanLists.get(row);
        editUid = draft.uid;
        entries = draft.entries;
        return true;
    }

    function save() {
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
        for (var i = 0; i < entries.length; ++i) entryRows.append({
            "entryData": entries[i]
        })
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

    Flickable {
        anchors.fill: parent
        clip: true
        contentHeight: content.implicitHeight + 32

        Column {
            id: content

            x: 16
            y: 16
            width: parent.width - 32
            spacing: 10

            Text {
                text: qsTr("Scan list")
                color: Theme.textPrimary
                font.family: Theme.sans
                font.pixelSize: 24
            }

            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("The list's tuner replaces each system's source, host, port, PPM, bandwidth and bias tee. System groups and keys stay isolated. 0 timing, -1 gain/bandwidth and blank PPM inherit defaults.")
                color: Theme.textSecondary
                font.family: Theme.sans
            }

            TextField {
                width: parent.width
                placeholderText: qsTr("List name")
                text: screen.draft.name || ""
                onTextEdited: screen.draft.name = text
            }

            ComboBox {
                width: parent.width
                model: ["usb", "rtltcp"]
                currentIndex: screen.draft.sourceType === "rtltcp" ? 1 : 0
                onActivated: {
                    var copy = screen.draft;
                    copy.sourceType = currentText;
                    screen.draft = Object.assign({
                    }, copy);
                }
            }

            TextField {
                width: parent.width
                visible: screen.draft.sourceType === "rtltcp"
                placeholderText: qsTr("RTL-TCP host")
                text: screen.draft.host || ""
                onTextEdited: screen.draft.host = text
            }

            TextField {
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
                model: [{
                    "key": "gainDb",
                    "label": qsTr("Gain dB (-1 inherits)"),
                    "fallback": -1
                }, {
                    "key": "ppm",
                    "label": qsTr("PPM (blank inherits)"),
                    "fallback": ""
                }, {
                    "key": "bandwidthKhz",
                    "label": qsTr("Bandwidth kHz (-1 inherits)"),
                    "fallback": -1
                }, {
                    "key": "defaultDwellMs",
                    "label": qsTr("Default dwell ms (0 inherits)"),
                    "fallback": 0
                }, {
                    "key": "defaultHoldMs",
                    "label": qsTr("Default hold ms (0 inherits)"),
                    "fallback": 0
                }]

                Column {
                    required property var modelData

                    width: content.width
                    spacing: 3

                    Text {
                        text: modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.sans
                    }

                    TextField {
                        width: parent.width
                        text: screen.draft[modelData.key] === undefined ? modelData.fallback : screen.draft[modelData.key]
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        onTextEdited: screen.draft[modelData.key] = text
                    }

                }

            }

            ComboBox {
                width: parent.width
                model: [qsTr("Bias tee: inherit"), qsTr("Bias tee: off"), qsTr("Bias tee: on")]
                currentIndex: (screen.draft.biasTee === undefined ? -1 : screen.draft.biasTee) + 1
                onActivated: screen.draft.biasTee = currentIndex - 1
            }

            CheckBox {
                text: qsTr("Voice only")
                checked: screen.draft.voiceOnly || false
                onToggled: screen.draft.voiceOnly = checked
            }

            Repeater {
                model: [{
                    "key": "groupCsvPath",
                    "type": "group",
                    "label": qsTr("Fallback group list")
                }, {
                    "key": "srcCsvPath",
                    "type": "src",
                    "label": qsTr("Global source aliases")
                }]

                Column {
                    required property var modelData

                    width: content.width
                    spacing: 4

                    Text {
                        text: modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.sans
                    }

                    ComboBox {
                        property var files: (importedFiles.count, importedFiles.entriesForType(modelData.type))

                        width: parent.width
                        model: [{
                            "name": qsTr("None"),
                            "path": ""
                        }].concat(files)
                        textRole: "name"
                        currentIndex: {
                            for (var i = 0; i < model.length; ++i) if (model[i].path === (screen.draft[modelData.key] || "")) {
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
                font.pixelSize: 19
            }

            Repeater {
                model: entryRows

                ScanEntryRow {
                    required property int index
                    required property var entryData

                    width: content.width
                    entry: entryData
                    label: screen.entryLabel(entryData)
                    onChanged: function(field, value) {
                        screen.setEntry(index, field, value);
                    }
                    onMove: function(delta) {
                        screen.moveEntry(index, delta);
                    }
                    onRemove: screen.removeEntry(index)
                }

            }

            ComboBox {
                id: systemPicker

                width: parent.width
                model: savedSystems
                textRole: "name"
            }

            Button {
                width: parent.width
                text: qsTr("Add saved system")
                enabled: systemPicker.currentIndex >= 0
                onClicked: screen.addSystem(savedSystems.get(systemPicker.currentIndex).uid)
            }

            TextField {
                id: freqName

                width: parent.width
                placeholderText: qsTr("Frequency name")
            }

            ComboBox {
                id: protocol

                width: parent.width
                model: ["p25", "dmr", "nxdn48", "nxdn"]
            }

            TextField {
                id: frequency

                width: parent.width
                placeholderText: qsTr("Frequency MHz")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
            }

            Button {
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

            Button {
                width: parent.width
                text: qsTr("Save and validate")
                onClicked: {
                    if (screen.persist()) {
                        var result = scanListStarter.build(screen.draft);
                        screen.validationText = result.ok ? qsTr("%1 targets ready.").arg(result.targetCount) + "\n" + result.warnings.join("\n") : result.error;
                    }
                }
            }

            Button {
                width: parent.width
                text: qsTr("Save")
                onClicked: screen.save()
            }

            Button {
                width: parent.width
                text: qsTr("Remove list")
                visible: screen.editUid.length > 0
                onClicked: {
                    scanLists.remove(scanLists.rowForUid(screen.editUid));
                    screen.closed();
                }
            }

            Button {
                width: parent.width
                text: qsTr("Cancel")
                onClicked: screen.closed()
            }

        }

    }

}
