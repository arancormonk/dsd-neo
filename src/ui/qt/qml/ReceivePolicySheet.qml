// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    objectName: namesEditor ? "serviceNamesSheet" : "receiveFiltersSheet"
    accessibleName: namesEditor ? qsTr("Service names") : qsTr("Receive filters")
    closesOnOutsideTap: false
    property var bridge: commands
    property bool namesEditor: false
    property int kind: 0
    property bool filterEnabled: false
    property var rowContext: ({})
    property var defaults: ({})
    property bool rowScope: false
    property string openedValue: ""
    property string localMessage: ""
    property int rulesRevision: 0
    readonly property int editKind: namesEditor ? 4 : kind
    readonly property int field: 16 << editKind
    readonly property bool rowCompatible: !rowScope || (rowContext.editable & field) !== 0
    readonly property string validation: visible && rowCompatible
        ? (scopeError() || bridge.receiveError(editKind, filterEnabled, allowField.text, blockField.text, namesValue())) : ""

    ListModel { id: rules }
    function nameApplies(identifier) {
        return !rowScope || (rowContext.editable & (identifier === 4 ? 4 : (16 << identifier))) !== 0;
    }
    function scopeError() {
        if (namesEditor && rowScope) {
            var names = namesValue();
            for (var i = 0; i < names.length; ++i) {
                if (!nameApplies(names[i].kind))
                    return qsTr("This identifier does not apply to this channel. Choose an identifier for its protocol.");
            }
        }
        return "";
    }
    function addName() {
        var initialKind = 1;
        if (rowScope) {
            for (initialKind = 0; initialKind < 5 && !nameApplies(initialKind); ++initialKind) {}
            if (initialKind === 5)
                return;
        }
        rules.append({"kind": initialKind, "value": "", "name": ""});
        rulesRevision++;
    }
    function namesValue() {
        var revision = rulesRevision;
        var result = [];
        for (var i = 0; i < rules.count; ++i) {
            var r = rules.get(i);
            result.push({"kind": r.kind, "value": r.value, "name": r.name});
        }
        return result;
    }
    function value() {
        return namesEditor ? JSON.stringify(namesValue())
            : JSON.stringify([kind, filterEnabled, allowField.text, blockField.text]);
    }
    function guard(action) {
        if (visible && value() !== openedValue)
            discard.ask(action);
        else
            action();
    }
    function loadPolicy() {
        var policy = rowScope ? rowContext.receive : defaults;
        var filters = policy.filters || [];
        var f = filters[kind] || {"enabled": false, "allow": "", "block": ""};
        filterEnabled = f.enabled;
        allowField.text = f.allow;
        blockField.text = f.block;
        rules.clear();
        var names = policy.names || [];
        for (var i = 0; i < names.length; ++i) {
            if (nameApplies(names[i].kind))
                rules.append(names[i]);
        }
        rulesRevision++;
        openedValue = value();
        localMessage = "";
    }
    function chooseScope(useRow) {
        guard(function () {
            rowScope = useRow;
            loadPolicy();
        });
    }
    function openEditor() {
        defaults = bridge.receiveContext();
        // Capture the destination once: scan rotation cannot redirect this edit.
        rowContext = bridge.scanRowContext();
        rowScope = false;
        loadPolicy();
        open();
    }
    function closeEditor() {
        visible = false;
        Qt.inputMethod.hide();
    }
    function requestClose() { guard(closeEditor); }
    dismissHandler: requestClose
    DiscardDialog { id: discard; parent: sheet }
    function submit(action) {
        if (!rowCompatible || (action === bridge.scanRowEditSet && validation.length))
            return;
        if (bridge.setReceive(editKind, filterEnabled, allowField.text, blockField.text,
                               namesValue(), rowScope ? rowContext : ({}), action))
            closeEditor();
        else
            localMessage = qsTr("Not sent. Check the fields and that the decoder is running.");
    }
    function changeRule(index, key, text) {
        if (index < 0 || index >= rules.count || rules.get(index)[key] === text)
            return;
        rules.setProperty(index, key, text);
        rulesRevision++;
    }

    Text {
        width: parent.width
        text: sheet.accessibleName
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    ReadableText {
        width: parent.width
        text: sheet.namesEditor
            ? qsTr("Name a received identifier. Access codes and tones take priority over slot names. Names alone do not filter calls.")
            : qsTr("Allow only listed identifiers and reject any in Block. Leave Allow empty to accept every known identifier. Access codes must be verified before reception is allowed.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    PlexComboBox {
        id: kindPicker
        objectName: "receiveFilterKind"
        visible: !sheet.namesEditor
        width: parent.width
        Accessible.name: qsTr("Filter identifier")
        model: [qsTr("Slot · TDMA only"), qsTr("DCC · DMR"), qsTr("RAN · NXDN"), qsTr("NAC · P25")]
        currentIndex: sheet.kind
        onActivated: function (index) {
            // Activation wrote currentIndex; bind it to sheet.kind again so the picker shows the kind in force while
            // the unsaved-edit guard asks, and follows every later change of sheet.kind.
            currentIndex = Qt.binding(function () {
                return sheet.kind;
            });
            sheet.guard(function () {
                sheet.kind = index;
                sheet.loadPolicy();
            });
        }
    }
    SegmentedControl {
        objectName: "receiveScope"
        visible: (sheet.rowContext.active === true && (sheet.rowContext.editable & sheet.field) !== 0) || sheet.rowScope
        width: parent.width
        model: [qsTr("All channels"), qsTr("This channel")]
        currentIndex: sheet.rowScope ? 1 : 0
        onSelected: function (index) { sheet.chooseScope(index === 1); }
    }
    ReadableText {
        width: parent.width
        text: sheet.rowScope
            ? qsTr("Session edit for %1. The imported list stays unchanged.").arg(sheet.rowContext.label || "")
            : qsTr("Configured default. A channel's own setting takes priority.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    ReadableText {
        width: parent.width
        visible: sheet.rowScope && !sheet.rowCompatible
        text: qsTr("This identifier does not apply to the selected channel. Choose All channels or another identifier.")
        wrapMode: Text.Wrap
        color: Theme.alert
        font.pixelSize: Theme.fontSize(13)
    }
    SegmentedControl {
        objectName: "receiveFilterEnabled"
        visible: !sheet.namesEditor
        enabled: sheet.rowCompatible
        width: parent.width
        model: [qsTr("Off"), qsTr("On")]
        currentIndex: sheet.filterEnabled ? 1 : 0
        onSelected: function (index) { sheet.filterEnabled = index === 1; }
    }
    PlexTextField {
        id: allowField
        objectName: "receiveAllow"
        visible: !sheet.namesEditor
        width: parent.width
        mono: true
        label: qsTr("Allow")
        hint: sheet.kind === 3 ? qsTr("Hexadecimal NACs, e.g. 293/0xA12") : qsTr("Separate identifiers with /, e.g. 1/2")
        inputMethodHints: Qt.ImhNoPredictiveText
        nextField: blockField
    }
    PlexTextField {
        id: blockField
        objectName: "receiveBlock"
        visible: !sheet.namesEditor
        width: parent.width
        mono: true
        label: qsTr("Block")
        hint: qsTr("Block takes priority over Allow. Empty means none.")
        inputMethodHints: Qt.ImhNoPredictiveText
    }
    ReadableText {
        width: parent.width
        visible: sheet.namesEditor && rules.count === 0
        text: qsTr("No service names. Calls use their channel label.")
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(14)
    }
    Repeater {
        model: sheet.namesEditor ? rules : null
        Column {
            id: ruleRow
            required property int index
            required property int kind
            required property string value
            required property string name
            width: parent.width
            spacing: 8
            PlexComboBox {
                width: parent.width
                Accessible.name: qsTr("Identifier for name %1").arg(ruleRow.index + 1)
                model: [qsTr("Slot"), qsTr("DCC"), qsTr("RAN"), qsTr("NAC"), qsTr("CTCSS / DCS")]
                currentIndex: ruleRow.kind
                onActivated: function (index) { sheet.changeRule(ruleRow.index, "kind", index); }
            }
            PlexTextField {
                width: parent.width
                mono: true
                label: qsTr("Identifier value")
                text: ruleRow.value
                hint: ruleRow.kind === 4 ? qsTr("For example 141.3 or D023N") : ""
                inputMethodHints: Qt.ImhNoPredictiveText
                onTextChanged: sheet.changeRule(ruleRow.index, "value", text)
            }
            PlexTextField {
                width: parent.width
                label: qsTr("Service name")
                text: ruleRow.name
                hint: qsTr("Up to 63 UTF-8 bytes; no commas or double quotes")
                onTextChanged: sheet.changeRule(ruleRow.index, "name", text)
            }
            OutlineButton {
                text: qsTr("Remove name")
                accessibleName: qsTr("Remove service name %1").arg(ruleRow.index + 1)
                onClicked: { rules.remove(ruleRow.index); sheet.rulesRevision++; }
            }
            Item { width: 1; height: 12 }
        }
    }
    OutlineButton {
        objectName: "receiveAddName"
        visible: sheet.namesEditor
        width: parent.width
        enabled: rules.count < 32
        text: qsTr("Add service name")
        onClicked: sheet.addName()
    }
    ReadableText {
        objectName: "receiveValidation"
        width: parent.width
        visible: text.length > 0
        text: sheet.validation || sheet.localMessage
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.alert
        font.pixelSize: Theme.fontSize(13)
    }
    Flow {
        width: parent.width
        spacing: 8
        visible: sheet.rowScope && sheet.rowCompatible
        OutlineButton {
            text: qsTr("Use default")
            onClicked: sheet.submit(sheet.bridge.scanRowEditInherit)
        }
        OutlineButton {
            text: qsTr("List value")
            onClicked: sheet.submit(sheet.bridge.scanRowEditReset)
        }
    }
    GradientButton {
        objectName: "receiveApply"
        width: parent.width
        text: qsTr("Apply")
        inputValid: sheet.rowCompatible && sheet.validation.length === 0
        onClicked: sheet.submit(sheet.bridge.scanRowEditSet)
    }
    OutlineButton {
        objectName: "receiveCancel"
        width: parent.width
        text: qsTr("Cancel")
        onClicked: sheet.requestClose()
    }
}
