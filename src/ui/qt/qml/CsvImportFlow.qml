// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Item {
    id: flow
    property var overlayParent: parent
    property string reference: ""
    property string fileName: ""
    property string type: ""
    // The stored path of the library row an update replaces, "" for a new
    // file. Taken when the pick is asked for, never the row's index: a row
    // removed while the picker is up would turn an index onto another file
    // (issue #829).
    property string replacePath: ""
    property var companions: ({})
    // What each companion's "Selected:" line names: a picked file's own name
    // (a content reference ends in a provider id), or the library file's.
    property var companionNames: ({})
    // Bumped by every choice for a slot, so a pick that comes back after a
    // newer choice for the same slot is dropped (issue #829).
    property var companionGenerations: ({})
    property var requiredFiles: []
    property var requiredLabels: ({})
    property var requiredDetails: ({})
    property string choosing: ""
    // The chosen slot's generation when its picker was asked.
    property int choosingGeneration: 0
    // Why the last "Choose a file…" could not ask, shown on the sheet until
    // the next try, a library choice or the end of the import.
    property string companionNotice: ""
    // The last problem read out and how many have been, so a test can tell
    // each failure is read out once, and again when it happens again.
    property string announced: ""
    property int announcements: 0
    property bool active: false
    // Whether the user has chosen a companion on the sheet (a library file or a
    // picked one). Preselected matches are the app's guess, not a choice.
    property bool choiceMade: false
    // What the owner asked this import for (pick()/begin()), handed back as
    // the result's `context`.
    property var context: ({})
    // The owner's say on a pick that comes back late: a function of the
    // context, false when the owner's field took a newer choice meanwhile.
    // The import then ends as cancelled, with nothing imported. Null: always.
    property var wanted: null
    signal finished(var result)

    // One completion per pick()/begin(). A companion rejection only abandons
    // that slot's choice; the enclosing import remains active on the sheet.
    function start(kind, row) {
        cancel();
        reference = "";
        fileName = "";
        type = kind;
        replacePath = row !== undefined && row >= 0 ? importedFiles.get(row).path || "" : "";
        companions = ({});
        companionNames = ({});
        companionGenerations = ({});
        requiredFiles = [];
        requiredLabels = ({});
        requiredDetails = ({});
        companionNotice = "";
        choiceMade = false;
        active = true;
    }
    function pick(kind, row, context) {
        start(kind, row);
        flow.context = context || ({});
        primaryPicker.open();
    }
    function begin(uri, name, kind, row, context) {
        start(kind, row);
        flow.context = context || ({});
        reference = uri;
        fileName = name;
        attempt();
    }
    function complete(result) {
        if (!active)
            return;
        active = false;
        primaryPicker.close();
        picker.close();
        sheet.visible = false;
        reference = "";
        fileName = "";
        replacePath = "";
        choosing = "";
        companions = ({});
        companionNames = ({});
        companionGenerations = ({});
        requiredFiles = [];
        requiredLabels = ({});
        requiredDetails = ({});
        companionNotice = "";
        choiceMade = false;
        var outcome = Object.assign({}, result, {context: flow.context});
        flow.context = ({});
        finished(outcome);
    }
    function cancel() {
        complete({ok: false, error: "cancelled"});
    }
    function stillWanted() {
        return !wanted || wanted(context);
    }
    // The sheet's Cancel, Back and Escape (issue #718): once a companion was
    // chosen, ask before the choices go.
    function requestCancel() {
        if (active && choiceMade)
            discard.ask(cancel);
        else
            cancel();
    }
    function generationOf(key) {
        return companionGenerations[key] || 0;
    }
    // Every write to a slot: the choice, its name and a new generation.
    function setCompanion(key, path, name) {
        var next = Object.assign({}, companions);
        next[key] = path;
        companions = next;
        var names = Object.assign({}, companionNames);
        names[key] = name || "";
        companionNames = names;
        var generations = Object.assign({}, companionGenerations);
        generations[key] = generationOf(key) + 1;
        companionGenerations = generations;
    }
    function selectCompanion(key, path, name) {
        setCompanion(key, path, name);
        companionNotice = "";
        choiceMade = true;
    }
    // The current companion stays until the picker hands back a replacement:
    // a rejected picker leaves the earlier choice in place.
    function chooseFile(key) {
        companionNotice = "";
        choosing = key;
        choosingGeneration = generationOf(key);
        picker.open();
    }
    function companionPicked(reference, displayName) {
        var key = choosing;
        choosing = "";
        // A choice made for this slot since the picker was asked wins.
        if (active && key && generationOf(key) === choosingGeneration)
            selectCompanion(key, reference, displayName);
    }
    // No picker could ask: back to the sheet with every choice kept, and why.
    function companionUnavailable(message) {
        choosing = "";
        if (!active)
            return;
        sheet.visible = true;
        companionNotice = message;
        if (Navigation.allows(companionNoticeLine))
            announce(message);
    }
    function announce(text) {
        announced = text;
        announcements++;
        if (typeof Accessible.announce === "function")
            Accessible.announce(text);
    }
    function primaryPicked(reference, displayName) {
        if (!active)
            return;
        flow.reference = reference;
        flow.fileName = displayName || reference.substring(reference.lastIndexOf('/') + 1);
        attempt();
    }
    function primaryUnavailable(message) {
        if (!active)
            return;
        if (stillWanted())
            complete({ok: false, error: "unavailable", detail: message});
        else
            cancel();
    }
    function candidateName(detail, path) {
        for (var candidate of detail.candidates || []) {
            if (candidate.path === path)
                return candidate.name;
        }
        return "";
    }
    function showRequirements(result) {
        var details = result.requiredDetails || ({});
        var names = requiredFiles.filter(function (key) { return !!details[key]; });
        for (var key of result.required) {
            if (details[key] && details[key].selectionError)
                setCompanion(key, "", "");
            if (names.indexOf(key) >= 0)
                continue;
            names.push(key);
            if (companions[key] === undefined && details[key] && details[key].preselected)
                setCompanion(key, details[key].preselected, candidateName(details[key], details[key].preselected));
        }
        requiredFiles = names;
        requiredLabels = result.requiredLabels || ({});
        requiredDetails = details;
        sheet.visible = true;
    }
    function attempt() {
        if (!active)
            return;
        if (!stillWanted()) {
            cancel();
            return;
        }
        var result = importedFiles.importBundleReplacing(reference, fileName, type, companions, replacePath);
        if (result.error === "companions") {
            showRequirements(result);
            return;
        }
        complete(result);
    }
    // No MIME filter: Android providers index CSV under several MIME types.
    DocumentPicker {
        id: primaryPicker
        objectName: "csvPrimaryPicker"
        mode: "open"
        onAccepted: function (reference, displayName) {
            flow.primaryPicked(reference, displayName);
        }
        onRejected: flow.cancel()
        onUnavailable: function (reason, message) {
            flow.primaryUnavailable(message);
        }
    }
    DocumentPicker {
        id: picker
        objectName: "csvCompanionPicker"
        mode: "open"
        onAccepted: function (reference, displayName) {
            flow.companionPicked(reference, displayName);
        }
        onRejected: {
            flow.choosing = "";
            if (flow.active)
                sheet.visible = true;
        }
        onUnavailable: function (reason, message) {
            flow.companionUnavailable(message);
        }
    }
    ModalSheet {
        id: sheet
        objectName: "csvCompanionSheet"
        parent: flow.overlayParent
        accessibleName: flow.type === "trunkTargets" ? qsTr("Target CSV companion files") : qsTr("Channel-map companion files")
        // A form sheet (issue #718): an outside tap keeps the choices made here.
        closesOnOutsideTap: false
        dismissHandler: function () {
            flow.requestCancel();
        }
        DiscardDialog {
            id: discard
            parent: sheet
        }
        ReadableText {
            width: parent.width
            text: qsTr("This CSV references additional files. Choose an imported file or a file from your device for each companion to store a complete, private copy.")
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(15)
        }
        // Issue #829: why "Choose a file…" could not ask. It is about the
        // device, not one companion, so it sits above them all.
        ReadableText {
            id: companionNoticeLine
            objectName: "csvCompanionNotice"
            width: parent.width
            text: flow.companionNotice
            visible: text.length > 0
            wrapMode: Text.Wrap
            color: Theme.magenta
            font.pixelSize: Theme.fontSize(13)
            Accessible.role: Accessible.AlertMessage
        }
        Repeater {
            model: flow.requiredFiles
            Column {
                id: slot
                required property string modelData
                required property int index
                readonly property var detail: flow.requiredDetails[modelData] || ({})
                readonly property string kind: detail.kind || ""
                width: parent.width
                spacing: 8
                ReadableText {
                    objectName: "csvCompanionLabel_" + slot.index
                    width: parent.width
                    text: qsTr("%1 · %2").arg(slot.kind.charAt(0).toUpperCase() + slot.kind.slice(1)).arg(flow.requiredLabels[slot.modelData] || slot.modelData)
                    wrapMode: Text.Wrap
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSize(15)
                }
                ReadableText {
                    objectName: "csvCompanionWarning_" + slot.index
                    width: parent.width
                    text: slot.detail.warning || ""
                    visible: text.length > 0
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSize(13)
                }
                Repeater {
                    model: slot.detail.candidates || []
                    OutlineButton {
                        required property var modelData
                        required property int index
                        readonly property bool checked: flow.companions[slot.modelData] === modelData.path
                        objectName: "csvLibraryChoice_" + slot.index + "_" + index
                        width: parent.width
                        accessibleName: (modelData.accepted === 1 ? qsTr("Use imported %1 (%2, %3 row)") : qsTr("Use imported %1 (%2, %3 rows)")).arg(modelData.name).arg(modelData.kind).arg(modelData.accepted)
                        text: (checked ? "● " : "○ ") + accessibleName
                        Accessible.role: Accessible.RadioButton
                        Accessible.checkable: true
                        Accessible.checked: checked
                        onClicked: flow.selectCompanion(slot.modelData, modelData.path, modelData.name)
                    }
                }
                OutlineButton {
                    objectName: "csvChooseFile_" + slot.index
                    width: parent.width
                    text: qsTr("Choose a file…")
                    onClicked: flow.chooseFile(slot.modelData)
                }
                ReadableText {
                    width: parent.width
                    objectName: "csvCompanionSelected_" + slot.index
                    text: flow.companions[slot.modelData] ? qsTr("Selected: %1").arg(flow.companionNames[slot.modelData] || flow.companions[slot.modelData].split('/').pop()) : qsTr("No file selected")
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSize(13)
                }
            }
        }
        GradientButton {
            objectName: "csvCompleteBundle"
            width: parent.width
            text: qsTr("Import complete bundle")
            enabled: flow.requiredFiles.every(function (path) {
                return !!flow.companions[path];
            })
            onClicked: flow.attempt()
        }
        OutlineButton {
            objectName: "csvCancel"
            width: parent.width
            text: qsTr("Cancel")
            onClicked: flow.requestCancel()
        }
    }
}
