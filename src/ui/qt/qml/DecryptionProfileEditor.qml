// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    accessibleName: qsTr("Decryption profile")
    property var draft: ({})
    property var keyRows: []
    property var mapRows: []
    property bool loading: false
    property bool dirty: false
    property string errorText: ""
    property int editingKey: -1
    property int editingMap: -1
    property var originalKey: ({})
    property var originalMap: ({})
    readonly property bool available: typeof decryptionProfiles !== "undefined" && decryptionProfiles !== null
    readonly property var protocols: ["mixed", "p25", "dmr", "nxdn", "dpmr", "m17", "dstar", "ysf"]
    readonly property var protocolNames: [qsTr("Auto / mixed"), "P25", "DMR", "NXDN", "dPMR", "M17", "D-STAR", "YSF"]
    readonly property var modes: draft.protocol === "dstar" || draft.protocol === "ysf" ? ["none"] : draft.protocol === "m17" || draft.protocol === "dpmr" ? ["direct", "none"] : draft.protocol === "dmr" || draft.protocol === "mixed" ? ["automatic", "direct", "vendor", "none"] : ["automatic", "direct", "none"]
    readonly property var types: available ? decryptionProfiles.directTypes(draft.protocol || "mixed") : []
    readonly property var vendors: available ? decryptionProfiles.vendorTypes() : []
    readonly property var kinds: draft.protocol === "p25" ? ["scalar", "aes128", "aes256", "tdea"] : draft.protocol === "dmr" ? ["scalar", "aes128", "aes256", "basic"] : draft.protocol === "nxdn" ? ["scalar", "aes256", "scrambler"] : ["scalar", "aes128", "aes256", "tdea", "basic", "scrambler"]
    readonly property var forceValues: draft.protocol === "dmr" || draft.protocol === "mixed" ? [-1, 0, 1, 33] : draft.protocol === "nxdn" ? [-1, 0, 1] : [-1, 0]
    signal saved(string uid)

    function setField(field, value) {
        if (loading)
            return;
        draft = Object.assign({}, draft, {
            [field]: value
        });
        dirty = true;
    }
    function open(uid, protocol) {
        if (!available)
            return;
        loading = true;
        draft = uid.length ? decryptionProfiles.get(uid) : {
            uid: decryptionProfiles.newReference(),
            label: "",
            protocol: protocol || "mixed",
            mode: protocol === "m17" || protocol === "dpmr" ? "direct" : protocol === "dstar" || protocol === "ysf" ? "none" : "automatic",
            keySource: "managed",
            force: 0
        };
        keyRows = draft.keys || [];
        mapRows = draft.mappings || [];
        profileName.text = draft.label || "";
        directMaterial.text = "";
        vendorMaterial.text = "";
        errorText = "";
        dirty = false;
        loading = false;
        visible = true;
    }
    function openMatching(uid, protocol, kid, kind) {
        open(uid, protocol);
        if (draft.mode !== "automatic") {
            errorText = qsTr("This profile uses a direct or no-key configuration. Choose Automatic keys before adding entries selected by received ID.");
            return;
        }
        if (draft.keySource === "csv") {
            errorText = qsTr("This profile uses an imported CSV. Update that file in Imported files, or choose Manage keys here to build a replacement collection.");
            return;
        }
        var index = keyRows.findIndex(function (entry) {
            return entry.lookup !== "destination" && Number(entry.keyId) === kid;
        });
        keyDialog.openFor(index);
        if (index < 0) {
            keyId.text = kid.toString(16).toUpperCase();
            keyName.text = qsTr("Key %1").arg(keyId.text);
            keyKind.currentIndex = kinds.indexOf(kind);
        }
    }
    function openTalkgroup(uid, protocol, talkgroup) {
        open(uid, protocol);
        if (draft.mode !== "automatic" || draft.mappingCsvPath) {
            errorText = qsTr("Choose automatic keys and managed talkgroup overrides before editing a mapping here. Imported mappings can be updated in Imported files.");
            return;
        }
        var index = mapRows.findIndex(function (entry) {
            return Number(entry.talkgroup) === talkgroup;
        });
        mappingDialog.openFor(index);
        mapTg.text = String(talkgroup);
    }
    function save() {
        var value = Object.assign({}, draft, {
            keys: keyRows,
            mappings: mapRows
        });
        if (directMaterial.text.length)
            value.directValue = directMaterial.text;
        if (vendorMaterial.text.length)
            value.vendorValue = vendorMaterial.text;
        var result = decryptionProfiles.saveProfile(value);
        if (!result.ok) {
            errorText = result.error;
            return;
        }
        dirty = false;
        visible = false;
        saved(result.uid);
    }
    function requestClose() {
        if (dirty || directMaterial.text.length || vendorMaterial.text.length)
            discard.ask(function () {
                sheet.visible = false;
            });
        else
            visible = false;
    }
    dismissHandler: requestClose
    Connections {
        target: sheet
        function onVisibleChanged() {
            if (!sheet.visible) {
                keyDialog.visible = false;
                mappingDialog.visible = false;
                directMaterial.text = "";
                vendorMaterial.text = "";
                keyMaterial.text = "";
                sheet.keyRows = [];
                sheet.mapRows = [];
                sheet.draft = ({});
            }
        }
    }
    DiscardDialog {
        id: discard
        parent: sheet
    }
    ChoiceSheet {
        id: csvChoices
        parent: sheet
    }

    PlexTextField {
        id: profileName
        width: parent.width
        placeholderText: qsTr("Profile name")
        onTextChanged: sheet.setField("label", text)
    }
    MicroLabel {
        text: qsTr("Protocol")
    }
    PlexComboBox {
        width: parent.width
        Accessible.name: qsTr("Profile protocol")
        model: sheet.protocolNames
        currentIndex: Math.max(0, sheet.protocols.indexOf(sheet.draft.protocol))
        onActivated: sheet.setField("protocol", sheet.protocols[currentIndex])
    }
    MicroLabel {
        text: qsTr("Key selection")
    }
    PlexComboBox {
        width: parent.width
        Accessible.name: qsTr("Key source")
        model: sheet.modes.map(function (mode) {
            return mode === "automatic" ? qsTr("Automatic keys") : mode === "direct" ? qsTr("Direct override") : mode === "vendor" ? qsTr("Advanced vendor mode") : qsTr("No keys");
        })
        currentIndex: sheet.modes.indexOf(sheet.draft.mode)
        onActivated: sheet.setField("mode", sheet.modes[currentIndex])
    }
    Text {
        width: parent.width
        text: sheet.draft.legacy ? qsTr("Legacy configuration retained. Changing a direct key type requires replacement material.") : sheet.draft.mode === "automatic" ? qsTr("Use the received key ID for each call where supported. An empty collection means no keys supplied.") : sheet.draft.mode === "direct" ? qsTr("A direct override disables automatic key loading. It applies to the configured channel or session, not an individual talkgroup.") : sheet.draft.mode === "vendor" ? qsTr("Standalone DMR session only. Vendor modes force their own processing; stop and restart to change them. They cannot be assigned to scan entries or changed live.") : qsTr("No key material will be installed by this profile. Received encryption status and listening policy are separate.")
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    Column {
        width: parent.width
        spacing: 10
        visible: sheet.draft.mode === "direct"
        PlexComboBox {
            width: parent.width
            Accessible.name: qsTr("Direct key format")
            model: sheet.types.map(function (item) {
                return item.label;
            })
            currentIndex: sheet.types.findIndex(function (item) {
                return item.id === sheet.draft.directType;
            })
            onActivated: sheet.setField("directType", sheet.types[currentIndex].id)
        }
        Text {
            width: parent.width
            visible: sheet.draft.keyConfigured === true
            text: qsTr("Key configured. Leave the replacement field blank to keep it.")
            color: Theme.textSecondary
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSize(13)
        }
        PlexTextField {
            id: directMaterial
            width: parent.width
            placeholderText: qsTr("Replacement key material")
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
        }
    }
    Column {
        width: parent.width
        spacing: 10
        visible: sheet.draft.mode === "vendor"
        PlexComboBox {
            width: parent.width
            Accessible.name: qsTr("Vendor mode")
            model: sheet.vendors.map(function (item) {
                return item.label;
            })
            currentIndex: sheet.vendors.findIndex(function (item) {
                return item.id === sheet.draft.vendorType;
            })
            onActivated: sheet.setField("vendorType", sheet.vendors[currentIndex].id)
        }
        PlexTextField {
            id: vendorMaterial
            width: parent.width
            visible: sheet.draft.vendorType !== "vertex"
            label: sheet.draft.vendorConfigured ? qsTr("Replacement material; blank keeps current") : qsTr("Vendor key material")
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
        }
        OutlineButton {
            width: parent.width
            visible: sheet.draft.vendorType === "vertex"
            text: sheet.draft.vendorCsvPath ? sheet.draft.vendorCsvPath.split('/').pop() : qsTr("Select Vertex CSV")
            onClicked: {
                var files = importedFiles.entriesForType("vertexKeys");
                csvChoices.open(qsTr("Imported Vertex keystreams"), files.map(function (file) {
                    return file.name;
                }), function (index) {
                    sheet.setField("vendorCsvPath", files[index].path);
                });
            }
        }
    }
    Column {
        width: parent.width
        spacing: 10
        visible: sheet.draft.mode === "automatic"
        PlexComboBox {
            width: parent.width
            Accessible.name: qsTr("Key collection source")
            model: [qsTr("Manage keys here"), qsTr("Imported key CSV")]
            currentIndex: sheet.draft.keySource === "csv" ? 1 : 0
            onActivated: sheet.setField("keySource", currentIndex === 1 ? "csv" : "managed")
        }
        OutlineButton {
            width: parent.width
            visible: sheet.draft.keySource === "csv"
            text: sheet.draft.legacyCsvPath ? sheet.draft.legacyCsvPath.substring(sheet.draft.legacyCsvPath.lastIndexOf('/') + 1) : qsTr("Select imported key CSV")
            onClicked: {
                var files = importedFiles.entriesForType("keysDec").concat(importedFiles.entriesForType("keysHex"));
                csvChoices.open(qsTr("Imported key collection"), files.map(function (file) {
                    return file.name;
                }), function (index) {
                    sheet.setField("legacyCsvPath", files[index].path);
                    sheet.setField("legacyCsvHex", files[index].type === "keysHex");
                });
            }
        }
        Repeater {
            model: sheet.draft.keySource === "csv" ? [] : sheet.keyRows
            DisclosureRow {
                required property var modelData
                required property int index
                width: parent.width
                title: modelData.label || qsTr("Key")
                subtitle: qsTr("ID %1 · %2").arg(Number(modelData.keyId).toString(16).toUpperCase()).arg(modelData.kind || "scalar")
                onTapped: keyDialog.openFor(index)
            }
        }
        OutlineButton {
            width: parent.width
            visible: sheet.draft.keySource !== "csv"
            text: qsTr("Add key")
            onClicked: keyDialog.openFor(-1)
        }
    }
    Column {
        width: parent.width
        spacing: 10
        visible: sheet.draft.protocol === "dmr" && sheet.draft.mode === "automatic"
        MicroLabel {
            text: qsTr("Talkgroup key overrides")
        }
        Text {
            width: parent.width
            text: qsTr("Group calls only. A missing or incompatible mapped key falls back to the signaled key ID. Overrides do not unblock excluded talkgroups.")
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(13)
        }
        OutlineButton {
            width: parent.width
            text: sheet.draft.mappingCsvPath ? sheet.draft.mappingCsvPath.split('/').pop() : qsTr("Use imported mapping CSV")
            onClicked: {
                var files = importedFiles.entriesForType("dmrTgKeys");
                csvChoices.open(qsTr("DMR talkgroup overrides"), [qsTr("Manage overrides here")].concat(files.map(function (file) {
                    return file.name;
                })), function (index) {
                    sheet.setField("mappingCsvPath", index ? files[index - 1].path : "");
                    if (index)
                        sheet.mapRows = [];
                });
            }
        }
        Repeater {
            model: sheet.draft.mappingCsvPath ? [] : sheet.mapRows
            DisclosureRow {
                required property var modelData
                required property int index
                title: qsTr("Talkgroup %1").arg(modelData.talkgroup)
                subtitle: qsTr("Key ID %1").arg(Number(modelData.keyId).toString(16).toUpperCase())
                onTapped: mappingDialog.openFor(index)
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Add talkgroup override")
            visible: !sheet.draft.mappingCsvPath
            onClicked: mappingDialog.openFor(-1)
        }
    }
    MicroLabel {
        text: qsTr("Advanced identifier policy")
    }
    PlexComboBox {
        width: parent.width
        Accessible.name: qsTr("Identifier policy")
        model: sheet.forceValues.map(function (value) {
            return value === -1 ? qsTr("Inherit") : value === 0 ? qsTr("Use received identifiers") : value === 1 ? qsTr("Force privacy") : qsTr("Missing DMR algorithm fallback");
        })
        currentIndex: sheet.forceValues.indexOf(sheet.draft.force >= 32 ? 33 : sheet.draft.force)
        onActivated: sheet.setField("force", sheet.forceValues[currentIndex])
    }
    PlexTextField {
        width: parent.width
        visible: sheet.draft.force >= 32
        placeholderText: qsTr("Fallback algorithm ID (hex)")
        text: sheet.draft.force >= 32 ? Number(sheet.draft.force).toString(16).toUpperCase() : ""
        onEditingFinished: sheet.setField("force", /^[0-9a-f]{2}$/i.test(text) ? parseInt(text, 16) : -2)
    }
    Text {
        width: parent.width
        text: qsTr("Save changes this profile for its saved systems and scan entries on their next start. Apply to a running session is a separate action.") + (sheet.draft.useCount > 1 ? qsTr(" Shared by %1 saved references.").arg(sheet.draft.useCount) : "")
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    Text {
        width: parent.width
        visible: text.length > 0
        text: sheet.errorText
        wrapMode: Text.Wrap
        color: Theme.alert
        font.pixelSize: Theme.fontSize(14)
    }
    GradientButton {
        width: parent.width
        text: qsTr("Save profile")
        enabled: sheet.modes.indexOf(sheet.draft.mode) >= 0
        onClicked: sheet.save()
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Cancel")
        onClicked: sheet.requestClose()
    }

    ModalSheet {
        id: keyDialog
        parent: sheet
        accessibleName: qsTr("Key details")
        Connections {
            target: keyDialog
            function onVisibleChanged() {
                if (!keyDialog.visible)
                    keyMaterial.text = "";
            }
        }
        function openFor(index) {
            sheet.editingKey = index;
            sheet.originalKey = index >= 0 ? sheet.keyRows[index] : {
                uid: decryptionProfiles.newReference(),
                label: "",
                kind: "scalar",
                keyId: 0,
                lookup: "id"
            };
            keyName.text = sheet.originalKey.label || "";
            keyId.text = Number(sheet.originalKey.keyId).toString(sheet.originalKey.lookup === "destination" ? 10 : 16).toUpperCase();
            keyLookup.currentIndex = sheet.originalKey.lookup === "destination" ? 1 : 0;
            keyKind.currentIndex = sheet.kinds.indexOf(sheet.originalKey.kind);
            keyMaterial.text = "";
            visible = true;
        }
        PlexTextField {
            id: keyName
            width: parent.width
            placeholderText: qsTr("Key label")
        }
        PlexComboBox {
            id: keyKind
            width: parent.width
            Accessible.name: qsTr("Key format")
            model: sheet.kinds
        }
        PlexComboBox {
            id: keyLookup
            width: parent.width
            Accessible.name: qsTr("Key lookup")
            model: [qsTr("Received key ID"), qsTr("Legacy destination ID")]
            onActivated: keyId.text = ""
        }
        PlexTextField {
            id: keyId
            width: parent.width
            placeholderText: keyLookup.currentIndex ? qsTr("Destination ID (decimal)") : qsTr("Key ID (hex)")
        }
        PlexTextField {
            id: keyMaterial
            width: parent.width
            placeholderText: sheet.editingKey >= 0 ? qsTr("Replacement material; blank keeps current") : qsTr("Key material")
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Use this key")
            enabled: keyKind.currentIndex >= 0 && (keyLookup.currentIndex ? /^[0-9]+$/.test(keyId.text) : /^[0-9a-f]+$/i.test(keyId.text))
            onClicked: {
                var value = Object.assign({}, sheet.originalKey, {
                    label: keyName.text,
                    kind: sheet.kinds[keyKind.currentIndex],
                    lookup: keyLookup.currentIndex ? "destination" : "id",
                    keyId: parseInt(keyId.text, keyLookup.currentIndex ? 10 : 16)
                });
                if (keyMaterial.text.length)
                    value.material = keyMaterial.text;
                var rows = sheet.keyRows.slice();
                if (sheet.editingKey < 0)
                    rows.push(value);
                else
                    rows[sheet.editingKey] = value;
                sheet.keyRows = rows;
                sheet.dirty = true;
                keyMaterial.text = "";
                keyDialog.visible = false;
            }
        }
        OutlineButton {
            width: parent.width
            visible: sheet.editingKey >= 0
            text: qsTr("Remove key from draft")
            onClicked: {
                var rows = sheet.keyRows.slice();
                rows.splice(sheet.editingKey, 1);
                sheet.keyRows = rows;
                sheet.dirty = true;
                keyDialog.visible = false;
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Cancel")
            onClicked: {
                keyMaterial.text = "";
                keyDialog.visible = false;
            }
        }
    }
    ModalSheet {
        id: mappingDialog
        parent: sheet
        accessibleName: qsTr("Talkgroup key selection")
        function openFor(index) {
            sheet.editingMap = index;
            sheet.originalMap = index >= 0 ? sheet.mapRows[index] : {
                talkgroup: 0,
                keyId: 0
            };
            mapTg.text = sheet.originalMap.talkgroup ? String(sheet.originalMap.talkgroup) : "";
            mapKid.text = Number(sheet.originalMap.keyId).toString(16).toUpperCase();
            visible = true;
        }
        PlexTextField {
            id: mapTg
            width: parent.width
            placeholderText: qsTr("Talkgroup ID (decimal)")
        }
        PlexTextField {
            id: mapKid
            width: parent.width
            placeholderText: qsTr("Key ID (hex, 00–FF)")
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Use override")
            enabled: /^[0-9]+$/.test(mapTg.text) && /^[0-9a-f]{1,2}$/i.test(mapKid.text)
            onClicked: {
                var kid = parseInt(mapKid.text, 16);
                var reference = sheet.keyRows.find(function (key) {
                    return key.keyId === kid;
                });
                var value = {
                    talkgroup: parseInt(mapTg.text, 10),
                    keyId: kid,
                    keyRef: reference ? reference.uid : ""
                };
                var rows = sheet.mapRows.slice();
                if (sheet.editingMap < 0)
                    rows.push(value);
                else
                    rows[sheet.editingMap] = value;
                sheet.mapRows = rows;
                sheet.dirty = true;
                mappingDialog.visible = false;
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Automatic key selection")
            visible: sheet.editingMap >= 0
            onClicked: {
                var rows = sheet.mapRows.slice();
                rows.splice(sheet.editingMap, 1);
                sheet.mapRows = rows;
                sheet.dirty = true;
                mappingDialog.visible = false;
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Cancel")
            onClicked: mappingDialog.visible = false
        }
    }
}
