// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import "Util.js" as Util

ModalSheet {
    id: sheet
    property var bridge: commands
    // A form sheet (issue #718): an outside tap keeps it; Cancel, Back and Escape ask first.
    closesOnOutsideTap: false
    property string backendMessage: metrics.uiMessage
    property string localMessage: ""
    property var idStart: 0
    property var idEnd: 0
    // The listed row's token: its table index (-1 for a heard talkgroup) and, on a pattern row, its pattern.
    property int tableIndex: -1
    property string pattern: ""
    property string policyContext: "0"
    property double policyGeneration: 0
    property int priorityValue: 0
    property bool listed: false
    property var original: ({})
    property bool removeArmed: false
    property bool submitted: false
    property string saveMessage: ""
    property var observedCrypto: ({})
    readonly property var keyProfile: {
        if (typeof decryptionProfiles === "undefined")
            return ({});
        var count = decryptionProfiles.count;
        return decryptionProfiles.forRuntimeReference(observedCrypto.profileRef || metrics.keyProfileRef || "");
    }
    readonly property bool canAssignDmrKey: pattern.length === 0 && idStart === idEnd && (keyProfile.uid || "").length > 0 && keyProfile.mode === "automatic" && (observedCrypto.dmr === true || (listed && keyProfile.protocol === "dmr"))
    property bool canSaveList: false
    property string saveListText: qsTr("Save talkgroup list")
    signal saveListRequested

    // @p record is the row's record flag (issue #708); a heard talkgroup without a
    // row records whenever it is tuned, so it reads as listening.
    function openRow(start, end, name, listening, isListed, priority, preempt, context, generation, record, index, rowPattern) {
        var patternText = rowPattern || "";
        observedCrypto = patternText.length === 0 && typeof talkgroups !== "undefined" && typeof talkgroups.observedDecryption === "function" ? talkgroups.observedDecryption(Number(start)) : ({});
        idStart = start;
        idEnd = end;
        tableIndex = index === undefined ? -1 : index;
        pattern = patternText;
        policyContext = context;
        policyGeneration = generation;
        original = {
            name: name,
            listening: listening,
            priority: priority,
            preempt: preempt,
            record: listening && (record === undefined ? true : record)
        };
        nameField.text = name;
        listenToggle.checked = listening;
        recordToggle.checked = original.record;
        listed = isListed;
        priorityValue = priority;
        preemptToggle.checked = preempt;
        removeArmed = false;
        submitted = false;
        localMessage = "";
        visible = true;
    }
    function requestClose() {
        if (!submitted && (nameField.text !== original.name || listenToggle.checked !== original.listening || priorityValue !== original.priority || preemptToggle.checked !== original.preempt || recordToggle.checked !== original.record)) {
            discard.ask(function () {
                sheet.visible = false;
            });
        } else
            visible = false;
    }
    dismissHandler: requestClose
    DiscardDialog {
        id: discard
        parent: sheet
    }
    function saveRow() {
        if (submitted)
            return;
        var changes = {};
        if (nameField.text !== original.name)
            changes.name = nameField.text;
        if (listenToggle.checked !== original.listening)
            changes.listening = listenToggle.checked;
        if (priorityValue !== original.priority)
            changes.priority = priorityValue;
        if (preemptToggle.checked !== original.preempt)
            changes.preempt = preemptToggle.checked;
        // D11: a row tuned again records again, so Record is sent only where it
        // differs from what the decoder will hold; a row not tuned records nothing.
        var heldRecord = listenToggle.checked !== original.listening || original.record;
        if (listenToggle.checked && recordToggle.checked !== heldRecord)
            changes.record = recordToggle.checked;
        if (listed && Object.keys(changes).length === 0) {
            localMessage = qsTr("No changes to apply");
            return;
        }
        var ok = listed ? bridge.setTalkgroupPolicy(idStart, idEnd, policyContext, policyGeneration, changes, tableIndex, pattern) : bridge.addTalkgroup(idStart, idEnd, policyContext, policyGeneration, nameField.text, listenToggle.checked, priorityValue, preemptToggle.checked, recordToggle.checked);
        localMessage = ok ? qsTr("Update requested") : qsTr("Update refused. Use a name shorter than 50 UTF-8 bytes.");
        if (ok) {
            // A submitted edit can advance generation; reopen from fresh model state.
            submitted = true;
            visible = false;
            Qt.inputMethod.hide();
        }
    }

    Text {
        objectName: "talkgroupEditHeading"
        text: qsTr("Talkgroup %1").arg(sheet.pattern.length > 0 ? sheet.pattern : sheet.idStart === sheet.idEnd ? Util.idText(sheet.idStart) : Util.idText(sheet.idStart) + "–" + Util.idText(sheet.idEnd))
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    Text {
        width: parent.width
        visible: sheet.observedCrypto.algorithm !== undefined
        text: sheet.observedCrypto.crypto === 1 ? qsTr("Last observed: unencrypted") : qsTr("Last observed algorithm: %1").arg(sheet.observedCrypto.algorithm || "") + (sheet.observedCrypto.keyId ? qsTr(" · key ID %1").arg(sheet.observedCrypto.keyId) : "")
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    OutlineButton {
        width: parent.width
        visible: typeof decryptionProfiles !== "undefined" && (sheet.canAssignDmrKey || (sheet.observedCrypto.keyId || "").length > 0)
        text: sheet.canAssignDmrKey ? qsTr("Decryption key selection") : qsTr("Manage matching decryption key")
        onClicked: {
            var profile = sheet.keyProfile;
            if (sheet.canAssignDmrKey)
                profileEditor.openTalkgroup(profile.uid, "dmr", Number(sheet.idStart));
            else if (sheet.observedCrypto.keyId.length)
                profileEditor.openMatching(profile.uid || "", sheet.observedCrypto.dmr ? "dmr" : "p25", parseInt(sheet.observedCrypto.keyId, 16), "scalar");
        }
    }
    DecryptionProfileEditor {
        id: profileEditor
        parent: sheet
    }
    PlexTextField {
        id: nameField
        objectName: "talkgroupName"
        width: parent.width
        placeholderText: qsTr("Name")
    }
    PlexToggle {
        id: listenToggle
        objectName: "listeningToggle"
        text: checked ? qsTr("Listening") : qsTr("Not tuned")
        // D11: Record follows Listening, off while not tuned and on again when tuned.
        onToggled: recordToggle.checked = checked
    }
    PlexToggle {
        id: recordToggle
        objectName: "recordToggle"
        text: qsTr("Record")
        enabled: listenToggle.checked
    }
    Text {
        objectName: "recordHint"
        width: parent.width
        wrapMode: Text.Wrap
        // D4: Only unknown talkgroups skips every listed talkgroup, this one included.
        readonly property bool unknownOnly: sheet.pattern !== "*" && typeof prefs !== "undefined" && !!prefs && prefs.recordCalls === true && prefs.recordUnknownOnly === true
        text: !listenToggle.checked ? qsTr("Off while not tuned. Tuning it again turns Record back on.") : unknownOnly ? qsTr("This talkgroup's calls aren't saved while Only unknown talkgroups is on in Settings.") : qsTr("Saves this talkgroup's calls when call recording is on.")
        color: Theme.textSecondary
    }
    Text {
        text: qsTr("Priority · %1").arg(sheet.priorityValue)
        color: Theme.textPrimary
    }
    Row {
        width: parent.width
        spacing: 6
        Repeater {
            model: [0, 25, 50, 100]
            OutlineButton {
                required property int modelData
                objectName: "priorityPreset" + modelData
                width: (parent.width - 18) / 4
                height: Math.max(48, implicitHeight)
                text: String(modelData)
                onClicked: sheet.priorityValue = modelData
            }
        }
    }
    Row {
        spacing: 10
        OutlineButton {
            objectName: "priorityMinus"
            width: 80
            height: Math.max(48, implicitHeight)
            text: "−5"
            onClicked: sheet.priorityValue = Math.max(0, sheet.priorityValue - 5)
        }
        OutlineButton {
            objectName: "priorityPlus"
            width: 80
            height: Math.max(48, implicitHeight)
            text: "+5"
            onClicked: sheet.priorityValue = Math.min(100, sheet.priorityValue + 5)
        }
    }
    PlexToggle {
        id: preemptToggle
        objectName: "preemptToggle"
        text: qsTr("Preempt")
        enabled: sheet.priorityValue > 0
    }
    Text {
        objectName: "preemptHint"
        width: parent.width
        wrapMode: Text.Wrap
        text: listenToggle.checked ? qsTr("Preempt interrupts a lower-priority call when the system announces this talkgroup during it.") : qsTr("Not tuned: priority and preempt have no effect.")
        color: Theme.textSecondary
    }
    GradientButton {
        objectName: "saveTalkgroup"
        width: parent.width
        text: sheet.listed ? qsTr("Apply") : qsTr("Add to list")
        onClicked: sheet.saveRow()
    }
    OutlineButton {
        objectName: "removeTalkgroup"
        width: parent.width
        height: Math.max(48, implicitHeight)
        visible: sheet.listed
        text: sheet.removeArmed ? qsTr("Tap again to remove") : qsTr("Remove")
        onClicked: {
            if (sheet.submitted)
                return;
            if (!sheet.removeArmed) {
                sheet.removeArmed = true;
                return;
            }
            sheet.removeArmed = false;
            var ok = sheet.bridge.removeTalkgroup(sheet.idStart, sheet.idEnd, sheet.policyContext, sheet.policyGeneration, sheet.tableIndex, sheet.pattern);
            sheet.localMessage = ok ? qsTr("Removal requested") : qsTr("Removal refused");
            if (ok) {
                sheet.submitted = true;
                sheet.visible = false;
                Qt.inputMethod.hide();
            }
        }
    }
    OutlineButton {
        width: parent.width
        height: Math.max(48, implicitHeight)
        visible: sheet.canSaveList
        text: sheet.saveListText
        onClicked: sheet.saveListRequested()
    }
    Text {
        width: parent.width
        text: sheet.saveMessage
        visible: text.length > 0
        color: Theme.textSecondary
        wrapMode: Text.Wrap
    }
    Text {
        objectName: "talkgroupEditToast"
        width: parent.width
        text: sheet.backendMessage.length > 0 ? sheet.backendMessage : sheet.localMessage
        color: Theme.textSecondary
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }
    // The way out a form sheet keeps visible, now that a tap beside it stays
    // (issue #718); it asks before an edit goes.
    OutlineButton {
        objectName: "talkgroupEditClose"
        width: parent.width
        height: Math.max(48, implicitHeight)
        text: qsTr("Close")
        onClicked: sheet.requestClose()
    }
}
