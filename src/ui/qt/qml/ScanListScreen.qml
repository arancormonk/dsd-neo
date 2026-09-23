// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Window

Rectangle {
    id: screen

    property int editRow: -1
    property string editUid: ""
    property var draft: ({})
    property var entries: []
    property string validationText: ""
    property Item validationField: null
    property bool validationRevealPending: false
    readonly property bool csvMode: draft.targetSource === "csv"
    property var targetRows: []
    property bool advancedOpen: false
    property int gainMode: 0
    property bool ppmSet: false
    property bool bandwidthSet: false
    property string gainText: ""
    property string ppmText: ""
    property string bandwidthText: ""
    property string portText: ""
    property var timingText: ({})
    property var entryInputText: ({})
    readonly property var protocolIds: ["p25", "dmr", "nxdn48", "nxdn"]
    readonly property var protocolLabels: [qsTr("P25"), qsTr("DMR"), qsTr("NXDN48"), qsTr("NXDN96")]

    // Match the wizard's ASCII checks and explicit fallback values. Locale-aware
    // validators accept digits/grouping that JavaScript's Number cannot parse.
    function parseIntStrict(text, bottom, top) {
        if (!/^-?[0-9]+$/.test(text))
            return NaN;
        var value = Number(text);
        return isFinite(value) && value >= bottom && value <= top ? value : NaN;
    }

    // A target's own squelch (--squelch-db). Absent inherits the list's; 0 is off.
    function squelchSummary(db) {
        if (db === undefined || db === null)
            return qsTr("Squelch: inherit");
        return db === 0 ? qsTr("Squelch: off") : qsTr("Squelch: %1 dB").arg(db);
    }

    function frequencyValid(text) {
        return /^[0-9]{1,5}(\.[0-9]{0,6})?$/.test(text) && sessionArgs.freqValid(text);
    }

    function setDraft(field, value) {
        var copy = Object.assign({}, draft);
        copy[field] = value;
        draft = copy;
    }

    function revealFocus() {
        if (validationRevealPending) {
            revealValidation();
            return;
        }
        var window = screen.Window.window;
        if (window)
            Theme.revealFocus(scroll, content, window.activeFocusItem);
    }
    function revealValidation() {
        if (!visible || !validationRevealPending || !validationText.length || advancedAnimation.running)
            return;
        // PlexTextField includes its error in its height. Reveal the whole field
        // after layout; scroll.height already excludes the keyboard rectangle.
        Theme.revealFocus(scroll, content, validationField || validationMessage);
    }
    function showValidation(text, field) {
        validationField = field || null;
        validationText = text;
        validationRevealPending = text.length > 0;
        // Also handle repeated failures whose text and geometry do not change.
        Qt.callLater(screen.revealValidation);
    }
    Connections {
        target: screen.Window.window
        function onActiveFocusItemChanged() {
            if (Navigation.contains(content, screen.Window.window.activeFocusItem))
                screen.validationRevealPending = false;
            if (screen.visible)
                Qt.callLater(screen.revealFocus);
        }
    }

    function selectTargets(path) {
        draft = Object.assign({}, draft, {
            "targetSource": "csv",
            "targetsCsvPath": path
        });
        var preview = importedFiles.targetPreview(path);
        targetRows = preview.rows || [];
        showValidation(preview.ok ? "" : preview.error);
    }

    function importTargets() {
        targetImport.pick("trunkTargets");
    }

    CsvImportFlow {
        id: targetImport
        objectName: "targetCsvImport"

        overlayParent: screen
        onFinished: function (result) {
            if (result.ok)
                screen.selectTargets(result.path);
            else if (result.error !== "cancelled")
                screen.showValidation(result.detail || qsTr("Could not import the target CSV."));
        }
    }

    signal closed
    property string initialDraft: ""
    function fingerprint() {
        return JSON.stringify({
            draft: draft,
            entries: entries,
            overrides: [gainMode, ppmSet, bandwidthSet, gainText, ppmText, bandwidthText, portText, timingText],
            entryInputText: entryInputText
        });
    }
    function requestClose() {
        Navigation.clearInput(screen.Window.window);
        if (initialDraft !== fingerprint())
            discardDialog.ask(function () {
                screen.closed();
            });
        else
            closed();
    }
    DiscardDialog {
        id: discardDialog
        objectName: "scanListDiscardDialog"
    }
    ModalSheet {
        id: removeDialog
        objectName: "scanListRemoveDialog"
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
            objectName: "scanRemoveCancel"
            text: qsTr("Cancel")
            onClicked: removeDialog.visible = false
        }
        OutlineButton {
            width: parent.width
            objectName: "scanRemoveConfirm"
            text: qsTr("Remove list")
            textColor: Theme.magenta
            border.color: Theme.magenta
            onClicked: {
                removeDialog.visible = false;
                if (scanLists.remove(scanLists.rowForUid(screen.editUid)))
                    screen.closed();
                else
                    screen.showValidation(qsTr("Could not remove the scan list."));
            }
        }
    }

    function openFor(row) {
        editRow = row;
        draft = row >= 0 ? scanLists.get(row) : scanLists.newDraft();
        editUid = row >= 0 ? draft.uid || "" : "";
        entries = draft.entries || [];
        showValidation("");
        advancedOpen = false;
        // Earlier releases persisted numeric fields as TextInput strings.
        var gain = Number(draft.gainDb);
        var bandwidth = Number(draft.bandwidthKhz);
        gainMode = gain > 0 ? 2 : gain === 0 ? 1 : 0;
        ppmSet = draft.ppm !== undefined && String(draft.ppm).length > 0;
        bandwidthSet = bandwidth > 0;
        gainText = gainMode === 2 ? String(gain) : "";
        ppmText = ppmSet ? String(draft.ppm) : "";
        bandwidthText = bandwidthSet ? String(bandwidth) : "";
        portText = draft.port === undefined ? "" : String(draft.port);
        timingText = {
            defaultDwellMs: Number(draft.defaultDwellMs) > 0 ? String(Number(draft.defaultDwellMs)) : "",
            defaultHoldMs: Number(draft.defaultHoldMs) > 0 ? String(Number(draft.defaultHoldMs)) : ""
        };
        entryInputText = {};
        freqName.text = "";
        frequency.text = "";
        protocol.currentIndex = 0;
        scroll.contentY = 0;
        targetRows = [];
        if (csvMode && draft.targetsCsvPath) {
            var preview = importedFiles.targetPreview(draft.targetsCsvPath);
            targetRows = preview.rows || [];
            showValidation(preview.ok ? "" : preview.error);
        }
        initialDraft = fingerprint();
    }

    function setEntry(row, field, value) {
        var updated = Object.assign({}, entries[row]);
        updated[field] = value;
        entries[row] = updated;
        entryRows.setProperty(row, "entryData", updated);
    }

    function setEntryInput(row, field, text, value) {
        // Keep rejected text visible while the draft carries a safe sentinel.
        // Entry IDs preserve the edit and its error across add/remove/reorder.
        var uid = entries[row].uid;
        var fields = Object.assign({}, entryInputText[uid] || ({}));
        fields[field] = text;
        var copy = Object.assign({}, entryInputText);
        copy[uid] = fields;
        entryInputText = copy;
        setEntry(row, field, value);
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
        var inputCopy = Object.assign({}, entryInputText);
        delete inputCopy[entries[row].uid];
        entryInputText = inputCopy;
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

    // Keep both modes in the editor. The store receives only the selected mode.
    function selectedDraft() {
        return Object.assign({}, draft, {
            entries: csvMode ? [] : entries,
            targetsCsvPath: csvMode ? draft.targetsCsvPath || "" : ""
        });
    }

    function persist() {
        if (!draft.name || !draft.name.trim().length) {
            showValidation(qsTr("Name this scan list."), nameField);
            return false;
        }
        var value = selectedDraft();
        var row = editUid.length ? scanLists.rowForUid(editUid) : -1;
        if (editUid.length && row < 0) {
            showValidation(qsTr("This scan list was removed."));
            return false;
        }
        if (row >= 0) {
            if (!scanLists.update(row, value)) {
                showValidation(qsTr("Could not save the scan list."));
                return false;
            }
        } else {
            if (!scanLists.add(value)) {
                showValidation(qsTr("Could not save the scan list."));
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

    function validateTunerSettings() {
        var field = null;
        if (draft.sourceType !== "airspy") {
            if (gainMode === 2 && gainField.error.length)
                field = gainField;
            else if (ppmSet && ppmField.error.length)
                field = ppmField;
        }
        if (!field && bandwidthSet && bandwidthField.error.length)
            field = bandwidthField;
        for (var i = 0; !field && i < timingFields.count; ++i) {
            if (timingFields.itemAt(i).field.error.length)
                field = timingFields.itemAt(i).field;
        }
        if (field)
            advancedOpen = true;
        else if (draft.sourceType === "rtltcp")
            field = hostField.error.length ? hostField : portField.error.length ? portField : null;
        if (field) {
            showValidation(field.error, field);
            return false;
        }
        return true;
    }

    function validateEntrySettings() {
        if (!csvMode) {
            for (var i = 0; i < entryEditors.count; ++i) {
                if (entryEditors.itemAt(i).inputError) {
                    showValidation(qsTr("Check the highlighted entry settings."));
                    return false;
                }
            }
        }
        return true;
    }

    function validate() {
        Navigation.clearInput(screen.Window.window);
        if (!draft.name || !draft.name.trim().length) {
            showValidation(qsTr("Name this scan list."), nameField);
            return false;
        }
        if (!validateTunerSettings() || !validateEntrySettings())
            return false;
        var result = scanListStarter.validate(Object.assign({}, selectedDraft(), {
            isDraft: false
        }));
        showValidation(result.ok ? qsTr("%1 targets ready.").arg(result.targetCount) + "\n" + result.warnings.join("\n") : result.error);
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
        Navigation.clearInput(screen.Window.window);
        if (!validateTunerSettings() || !validateEntrySettings())
            return;
        draft.isDraft = true;
        if (persist())
            closed();
    }

    function entryLabel(entry) {
        if (entry.kind === "freq")
            return (entry.name || protocolLabels[protocolIds.indexOf(entry.protocol)] || entry.protocol) + " · " + entry.freqMhz + " MHz";

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

    Item {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 46

        IconButton {
            id: back
            objectName: "scanListBack"
            icon: "back"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            onClicked: screen.requestClose()
        }

        Text {
            anchors.left: back.right
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: screen.editRow >= 0 ? qsTr("Edit scan list") : qsTr("New scan list")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(22)
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
            elide: Text.ElideRight
        }
    }

    PlexFlickable {
        id: scroll
        objectName: "scanListScroll"
        anchors.top: header.bottom
        anchors.topMargin: 14
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.max(0, screen.height - Theme.keyboardTop(screen))
        onHeightChanged: Qt.callLater(screen.revealFocus)
        onContentHeightChanged: Qt.callLater(screen.revealFocus)
        onMovementStarted: screen.validationRevealPending = false
        clip: true
        contentHeight: content.implicitHeight + Theme.screenPadding

        Column {
            id: content

            x: (parent.width - width) / 2
            width: Math.min(Theme.formWidth, parent.width - 2 * Theme.screenPadding)
            spacing: Theme.gap
            onPositioningComplete: Qt.callLater(screen.revealValidation)

            MicroLabel {
                text: qsTr("Name")
            }

            PlexTextField {
                id: nameField
                objectName: "scanListName"
                width: parent.width
                label: qsTr("List name")
                text: screen.draft.name || ""
                error: screen.validationField === nameField ? screen.validationText : ""
                input.onTextEdited: {
                    screen.draft.name = text;
                    if (screen.validationField === nameField)
                        screen.showValidation("");
                }
            }

            MicroLabel {
                text: qsTr("Entries")
            }

            SegmentedControl {
                objectName: "scanTargetMode"
                width: parent.width
                Accessible.name: qsTr("Scan targets")
                model: [qsTr("Saved systems & frequencies"), qsTr("Target CSV")]
                currentIndex: screen.csvMode ? 1 : 0
                onSelected: function (index) {
                    screen.setDraft("targetSource", index ? "csv" : "entries");
                }
            }

            Column {
                visible: !screen.csvMode
                width: parent.width
                spacing: 10

                Repeater {
                    id: entryEditors
                    model: entryRows

                    ScanEntryRow {
                        required property int index
                        required property var entryData

                        overlayParent: screen
                        width: content.width
                        entry: entryData
                        inputText: screen.entryInputText[entryData.uid] || ({})
                        parseInteger: screen.parseIntStrict
                        frequencyValid: screen.frequencyValid
                        label: screen.entryLabel(entryData)
                        canMoveUp: index > 0
                        canMoveDown: index < screen.entries.length - 1
                        onChanged: function (field, value) {
                            screen.setEntry(index, field, value);
                        }
                        onInputEdited: function (field, text, value) {
                            screen.setEntryInput(index, field, text, value);
                        }
                        onMove: function (delta) {
                            screen.moveEntry(index, delta);
                        }
                        onRemove: screen.removeEntry(index)
                    }
                }

                MicroLabel {
                    text: qsTr("Add a saved system")
                }
                PlexComboBox {
                    id: systemPicker
                    objectName: "scanSystemPicker"
                    width: parent.width
                    Accessible.name: qsTr("Saved system to add")
                    model: savedSystems
                    textRole: "name"
                }
                OutlineButton {
                    objectName: "scanAddSystem"
                    width: parent.width
                    text: qsTr("Add saved system")
                    enabled: systemPicker.currentIndex >= 0
                    onClicked: screen.addSystem(savedSystems.get(systemPicker.currentIndex).uid)
                }

                MicroLabel {
                    text: qsTr("Add a frequency")
                }
                PlexTextField {
                    id: freqName
                    objectName: "scanAddFrequencyName"
                    width: parent.width
                    label: qsTr("Frequency name")
                    nextField: frequency
                }
                MicroLabel {
                    text: qsTr("Protocol")
                }
                PlexComboBox {
                    id: protocol
                    objectName: "scanAddProtocol"
                    width: parent.width
                    Accessible.name: qsTr("Frequency protocol")
                    model: screen.protocolLabels
                }
                PlexTextField {
                    id: frequency
                    objectName: "scanAddFrequency"
                    width: parent.width
                    label: qsTr("Frequency")
                    unit: qsTr("MHz")
                    mono: true
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    input.validator: RegularExpressionValidator {
                        regularExpression: /^[0-9]{1,5}(\.[0-9]{0,6})?$/
                    }
                    error: text.length && !screen.frequencyValid(text) ? qsTr("Enter a valid frequency.") : ""
                }
                OutlineButton {
                    objectName: "scanAddFrequencyButton"
                    width: parent.width
                    text: qsTr("Add frequency")
                    enabled: screen.frequencyValid(frequency.text)
                    onClicked: {
                        screen.addFrequency(freqName.text, screen.protocolIds[protocol.currentIndex], frequency.text);
                        freqName.text = "";
                        frequency.text = "";
                    }
                }
            }

            Column {
                visible: screen.csvMode
                width: parent.width
                spacing: 10

                UiPanel {
                    width: parent.width
                    visible: (screen.draft.targetsCsvPath || "").length > 0
                    height: csvSummary.implicitHeight + 24

                    Column {
                        id: csvSummary
                        x: 12
                        y: 12
                        width: parent.width - 24
                        spacing: 6

                        Text {
                            width: parent.width
                            text: targetCsvPicker.currentIndex > 0 ? targetCsvPicker.currentText : (screen.draft.targetsCsvPath || "").split("/").pop()
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: Theme.textPrimary
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(15)
                            font.weight: Font.DemiBold
                        }
                        Row {
                            width: parent.width
                            spacing: 10
                            Text {
                                text: qsTr("%1 targets").arg(screen.targetRows.length)
                                color: Theme.textSecondary
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(13)
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            CsvTypeBadge {
                                objectName: "scanTargetCsvBadge"
                                type: "trunkTargets"
                            }
                        }
                    }
                }

                PlexComboBox {
                    id: targetCsvPicker
                    property var files: (importedFiles.count, importedFiles.entriesForType("trunkTargets"))

                    objectName: "scanTargetCsvPicker"
                    width: parent.width
                    Accessible.name: qsTr("Target CSV")
                    model: [{"name": qsTr("Select target CSV"), "path": ""}].concat(files)
                    textRole: "name"
                    currentIndex: {
                        for (var i = 0; i < model.length; ++i)
                            if (model[i].path === (screen.draft.targetsCsvPath || ""))
                                return i;
                        return 0;
                    }
                    onActivated: screen.selectTargets(model[currentIndex].path)
                }
                OutlineButton {
                    objectName: "scanImportTargets"
                    width: parent.width
                    text: qsTr("Import target CSV")
                    onClicked: screen.importTargets()
                }
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Saving uses the CSV targets. Target settings override list defaults; blank fields inherit. Replace the imported file to change its targets.")
                    color: Theme.textSecondary
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                }
                ListView {
                    objectName: "scanTargetPreview"
                    width: parent.width
                    height: Math.min(320, contentHeight)
                    clip: true
                    model: screen.targetRows

                    delegate: Text {
                        required property var modelData

                        width: ListView.view.width
                        padding: 6
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.textPrimary
                        text: modelData.id + " · " + modelData.type + " · " + modelData.frequency + " MHz\n" + qsTr("Dwell: %1 · Hold: %2 · Modulation: %3 · Gain: %4").arg(modelData.dwellMs < 0 ? qsTr("inherit") : modelData.dwellMs + " ms").arg(modelData.holdMs < 0 ? qsTr("inherit") : modelData.holdMs + " ms").arg(modelData.modulation || qsTr("inherit")).arg(modelData.gainDb < 0 ? qsTr("inherit") : modelData.gainDb === 0 ? qsTr("auto") : modelData.gainDb + " dB") + " · " + screen.squelchSummary(modelData.squelchDb)
                    }
                }
            }

            MicroLabel {
                text: qsTr("Tuner")
            }
            Flow {
                width: parent.width
                spacing: 8

                Repeater {
                    model: [
                        {"value": "usb", "label": qsTr("USB dongle")},
                        {"value": "airspy", "label": qsTr("Airspy R2 / Mini")},
                        {"value": "rtltcp", "label": qsTr("RTL-TCP")}
                    ]
                    DecodeChip {
                        required property var modelData
                        objectName: "scanSource_" + modelData.value
                        text: modelData.label
                        selected: screen.draft.sourceType === modelData.value
                        onClicked: {
                            if (modelData.value === "rtltcp" && !screen.draft.host)
                                screen.setDraft("host", "192.168.1.10");
                            screen.setDraft("sourceType", modelData.value);
                        }
                    }
                }
            }
            Column {
                width: parent.width
                spacing: 6
                visible: screen.draft.sourceType === "rtltcp"
                PlexTextField {
                    id: hostField
                    objectName: "scanHost"
                    width: parent.width
                    label: qsTr("Host")
                    nextField: portField
                    mono: true
                    text: screen.draft.host || ""
                    error: text.trim().length && /^[A-Za-z0-9_.-]+$/.test(text.trim()) ? "" : qsTr("Enter a host name or IPv4 address.")
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                    input.onTextEdited: screen.draft.host = text
                }
            }
            Column {
                width: parent.width
                spacing: 6
                visible: screen.draft.sourceType === "rtltcp"
                PlexTextField {
                    id: portField
                    function parsedValue() {
                        return screen.parseIntStrict(text, 1, 65535);
                    }
                    objectName: "scanPort"
                    width: parent.width
                    label: qsTr("Port")
                    mono: true
                    text: screen.portText
                    error: isNaN(parsedValue()) ? qsTr("Enter a port from 1 to 65535.") : ""
                    inputMethodHints: Qt.ImhDigitsOnly
                    input.validator: RegularExpressionValidator {
                        regularExpression: /^-?[0-9]*$/
                    }
                    input.onTextEdited: {
                        screen.portText = text;
                        screen.draft.port = isNaN(parsedValue()) ? 1234 : parsedValue();
                    }
                }
            }
            AirspyControls {
                objectName: "scanAirspyControls"
                width: parent.width
                visible: screen.draft.sourceType === "airspy"
                settings: screen.draft.airspy || ({})
                onEdited: function (key, value) {
                    var settings = Object.assign({}, screen.draft.airspy || ({}));
                    settings[key] = value;
                    screen.setDraft("airspy", settings);
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("This list's tuner settings replace each system's own source, host, port, PPM, bandwidth and bias tee.")
                color: Theme.textSecondary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
            }

            UiPanel {
                width: parent.width
                height: advHeader.height + (screen.advancedOpen ? advBody.height + Theme.cardPadding : 0)
                clip: true

                Behavior on height {
                    NumberAnimation {
                        id: advancedAnimation
                        duration: 150
                        easing.type: Easing.OutCubic
                        onRunningChanged: {
                            if (!running)
                                Qt.callLater(screen.revealValidation);
                        }
                    }
                }

                Item {
                    id: advHeader
                    objectName: "scanAdvancedToggle"
                    width: parent.width
                    height: Math.max(66, advHeading.implicitHeight + 24)
                    activeFocusOnTab: enabled && Navigation.allows(advHeader)
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Advanced")
                    Accessible.description: screen.advancedOpen ? qsTr("Expanded") : qsTr("Collapsed")
                    Accessible.ignored: !visible || !Navigation.allows(advHeader)
                    Accessible.onPressAction: toggle()
                    Keys.onSpacePressed: toggle()
                    Keys.onReturnPressed: toggle()
                    Keys.onEnterPressed: toggle()
                    function toggle() {
                        if (enabled && Navigation.allows(advHeader))
                            screen.advancedOpen = !screen.advancedOpen;
                    }
                    FocusFrame {}

                    Column {
                        id: advHeading
                        anchors.left: parent.left
                        anchors.right: chevron.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text {
                            width: parent.width
                            text: qsTr("Advanced")
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(15)
                            font.weight: Font.DemiBold
                            color: Theme.textSecondary
                        }
                        Text {
                            width: parent.width
                            text: qsTr("Tuner overrides, timing and aliases")
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(13)
                            color: Theme.textSubdued
                            wrapMode: Text.Wrap
                        }
                    }
                    Caret {
                        id: chevron
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        rotation: screen.advancedOpen ? 0 : -90
                        color: Theme.textSubdued
                        Behavior on rotation {
                            NumberAnimation {
                                duration: 150
                            }
                        }
                    }
                    TapHandler {
                        objectName: "scanAdvancedTap"
                        onTapped: advHeader.toggle()
                    }
                }

                Column {
                    id: advBody
                    objectName: "scanAdvancedBody"
                    anchors.top: advHeader.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.cardPadding
                    anchors.rightMargin: Theme.cardPadding
                    spacing: 10
                    visible: screen.advancedOpen
                    onPositioningComplete: Qt.callLater(screen.revealValidation)

                    Text {
                        width: parent.width
                        text: qsTr("System groups and keys stay isolated. Voice hang time uses the app default, including when a system has an override.")
                        color: Theme.textSecondary
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                        wrapMode: Text.Wrap
                    }

                    Column {
                        width: parent.width
                        spacing: 6
                        visible: screen.draft.sourceType !== "airspy"
                        MicroLabel {
                            text: qsTr("Gain")
                        }
                        SegmentedControl {
                            objectName: "scanGainMode"
                            Accessible.name: qsTr("Gain")
                            width: parent.width
                            model: [qsTr("App default"), qsTr("Automatic"), qsTr("Set")]
                            currentIndex: screen.gainMode
                            onSelected: function (index) {
                                screen.gainMode = index;
                                screen.setDraft("gainDb", index === 0 ? -1 : index === 1 ? 0 : isNaN(gainField.parsedValue()) ? -1 : gainField.parsedValue());
                            }
                        }
                        Text {
                            width: parent.width
                            visible: screen.gainMode === 0
                            text: qsTr("App default · %1").arg(prefs.gainDb === 0 ? qsTr("Automatic") : qsTr("%1 dB").arg(prefs.gainDb))
                            wrapMode: Text.Wrap
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSecondary
                        }
                        PlexTextField {
                            id: gainField
                            function parsedValue() {
                                return screen.parseIntStrict(text, 1, 49);
                            }
                            objectName: "scanTuner_gainDb"
                            width: parent.width
                            visible: screen.gainMode === 2
                            label: qsTr("Gain")
                            unit: qsTr("dB")
                            mono: true
                            text: screen.gainText
                            inputMethodHints: Qt.ImhDigitsOnly
                            input.validator: RegularExpressionValidator {
                                regularExpression: /^-?[0-9]*$/
                            }
                            error: isNaN(parsedValue()) ? qsTr("Enter a gain from 1 to 49 dB.") : ""
                            input.onTextEdited: {
                                screen.gainText = text;
                                screen.draft.gainDb = isNaN(parsedValue()) ? -1 : parsedValue();
                            }
                        }
                    }

                    Column {
                        width: parent.width
                        spacing: 6
                        visible: screen.draft.sourceType !== "airspy"
                        MicroLabel {
                            text: qsTr("Frequency correction")
                        }
                        SegmentedControl {
                            objectName: "scanPpmMode"
                            Accessible.name: qsTr("Frequency correction")
                            width: parent.width
                            model: [qsTr("App default"), qsTr("Set")]
                            currentIndex: screen.ppmSet ? 1 : 0
                            onSelected: function (index) {
                                screen.ppmSet = index === 1;
                                screen.setDraft("ppm", index && !isNaN(ppmField.parsedValue()) ? screen.ppmText : "");
                            }
                        }
                        Text {
                            width: parent.width
                            visible: !screen.ppmSet
                            text: qsTr("App default · %1 ppm").arg(prefs.ppm)
                            wrapMode: Text.Wrap
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSecondary
                        }
                        PlexTextField {
                            id: ppmField
                            function parsedValue() {
                                return screen.parseIntStrict(text, -2147483648, 2147483647);
                            }
                            objectName: "scanTuner_ppm"
                            width: parent.width
                            visible: screen.ppmSet
                            label: qsTr("Frequency correction")
                            unit: qsTr("ppm")
                            mono: true
                            text: screen.ppmText
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                            input.validator: RegularExpressionValidator {
                                regularExpression: /^-?[0-9]*$/
                            }
                            error: isNaN(parsedValue()) ? qsTr("Enter a PPM correction, including 0.") : ""
                            input.onTextEdited: {
                                screen.ppmText = text;
                                screen.draft.ppm = isNaN(parsedValue()) ? "" : text;
                            }
                        }
                    }

                    Column {
                        width: parent.width
                        spacing: 6
                        MicroLabel {
                            text: qsTr("Bandwidth")
                        }
                        SegmentedControl {
                            objectName: "scanBandwidthMode"
                            Accessible.name: qsTr("Bandwidth")
                            width: parent.width
                            model: [qsTr("App default"), qsTr("Set")]
                            currentIndex: screen.bandwidthSet ? 1 : 0
                            onSelected: function (index) {
                                screen.bandwidthSet = index === 1;
                                screen.setDraft("bandwidthKhz", index && !isNaN(bandwidthField.parsedValue()) ? bandwidthField.parsedValue() : -1);
                            }
                        }
                        Text {
                            width: parent.width
                            visible: !screen.bandwidthSet
                            text: qsTr("App default · %1 kHz").arg(prefs.bandwidthKhz)
                            wrapMode: Text.Wrap
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSecondary
                        }
                        PlexTextField {
                            id: bandwidthField
                            function parsedValue() {
                                return screen.parseIntStrict(text, 1, 2147483647);
                            }
                            objectName: "scanTuner_bandwidthKhz"
                            width: parent.width
                            visible: screen.bandwidthSet
                            label: qsTr("Bandwidth")
                            unit: qsTr("kHz")
                            mono: true
                            text: screen.bandwidthText
                            inputMethodHints: Qt.ImhDigitsOnly
                            input.validator: RegularExpressionValidator {
                                regularExpression: /^-?[0-9]*$/
                            }
                            error: isNaN(parsedValue()) ? qsTr("Enter a positive bandwidth.") : ""
                            input.onTextEdited: {
                                screen.bandwidthText = text;
                                screen.draft.bandwidthKhz = isNaN(parsedValue()) ? -1 : parsedValue();
                            }
                        }
                    }

                    MicroLabel {
                        visible: screen.draft.sourceType !== "airspy"
                        text: qsTr("Bias tee")
                    }
                    SegmentedControl {
                        objectName: "scanBiasTee"
                        Accessible.name: qsTr("Bias tee")
                        width: parent.width
                        visible: screen.draft.sourceType !== "airspy"
                        model: [qsTr("App default"), qsTr("On"), qsTr("Off")]
                        currentIndex: Number(screen.draft.biasTee) === 1 ? 1 : Number(screen.draft.biasTee) === 0 ? 2 : 0
                        onSelected: function (index) {
                            screen.setDraft("biasTee", index === 1 ? 1 : index === 2 ? 0 : -1);
                        }
                    }
                    Text {
                        width: parent.width
                        visible: screen.draft.sourceType !== "airspy" && Number(screen.draft.biasTee) === -1
                        text: qsTr("App default · %1").arg(prefs.biasTee ? qsTr("On") : qsTr("Off"))
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(12)
                        color: Theme.textSecondary
                    }
                    PlexCheckBox {
                        objectName: "scanVoiceOnly"
                        width: parent.width
                        text: qsTr("Voice only")
                        checked: screen.draft.voiceOnly || false
                        onToggled: screen.draft.voiceOnly = checked
                    }
                    Repeater {
                        id: timingFields
                        model: [
                            {"key": "defaultDwellMs", "label": qsTr("Default dwell")},
                            {"key": "defaultHoldMs", "label": qsTr("Default hold")}
                        ]
                        Column {
                            required property var modelData
                            property alias field: timingField
                            width: advBody.width
                            spacing: 6
                            PlexTextField {
                                id: timingField
                                function parsedValue() {
                                    return screen.parseIntStrict(text, 0, 600000);
                                }
                                objectName: "scanTuner_" + modelData.key
                                width: parent.width
                                label: modelData.label
                                unit: qsTr("ms")
                                hint: qsTr("Scan default when empty")
                                mono: true
                                text: screen.timingText[modelData.key] || ""
                                error: text.length && isNaN(parsedValue()) ? qsTr("Enter a time from 0 to 600000 ms.") : ""
                                inputMethodHints: Qt.ImhDigitsOnly
                                input.validator: RegularExpressionValidator {
                                    regularExpression: /^-?[0-9]*$/
                                }
                                input.onTextEdited: {
                                    var copy = Object.assign({}, screen.timingText);
                                    copy[modelData.key] = text;
                                    screen.timingText = copy;
                                    screen.draft[modelData.key] = isNaN(parsedValue()) ? 0 : parsedValue();
                                }
                            }
                        }
                    }
                    Repeater {
                        model: [
                            {"key": "groupCsvPath", "type": "group", "label": qsTr("Fallback talkgroups")},
                            {"key": "srcCsvPath", "type": "src", "label": qsTr("Radio IDs / source aliases")}
                        ]
                        Column {
                            required property var modelData
                            width: advBody.width
                            spacing: 6
                            MicroLabel {
                                width: parent.width
                                text: modelData.label
                                wrapMode: Text.Wrap
                            }
                            PlexComboBox {
                                property var files: (importedFiles.count, importedFiles.entriesForType(modelData.type))
                                objectName: "scanFile_" + modelData.key
                                width: parent.width
                                Accessible.name: modelData.label
                                model: [{"name": qsTr("None"), "path": ""}].concat(files)
                                textRole: "name"
                                currentIndex: {
                                    for (var i = 0; i < model.length; ++i)
                                        if (model[i].path === (screen.draft[modelData.key] || ""))
                                            return i;
                                    return -1;
                                }
                                onActivated: screen.draft[modelData.key] = model[currentIndex].path
                            }
                        }
                    }
                }
            }

            Text {
                id: validationMessage
                objectName: "scanValidation"
                width: parent.width
                text: screen.validationText
                textFormat: Text.PlainText
                visible: !screen.validationField && text.length > 0
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                onYChanged: Qt.callLater(screen.revealValidation)
                onHeightChanged: Qt.callLater(screen.revealValidation)
            }
            Row {
                width: parent.width
                spacing: 10
                OutlineButton {
                    objectName: "scanSaveDraft"
                    width: (parent.width - parent.spacing) / 2
                    text: qsTr("Save draft")
                    onClicked: screen.saveDraft()
                }
                OutlineButton {
                    objectName: "validateScanDraft"
                    width: (parent.width - parent.spacing) / 2
                    text: qsTr("Validate")
                    onClicked: screen.validate()
                }
            }
            GradientButton {
                objectName: "scanSave"
                width: parent.width
                text: qsTr("Save")
                onClicked: screen.save()
            }
            OutlineButton {
                objectName: "scanRemoveList"
                width: parent.width
                text: qsTr("Remove list")
                textColor: Theme.magenta
                border.color: Theme.magenta
                visible: screen.editUid.length > 0
                onClicked: removeDialog.visible = true
            }
        }
    }
}
