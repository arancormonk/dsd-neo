// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// One CSV import, from the pick to its answer (#832). Every file is picked
// through DocumentPicker (issue #829), and the copy and the checks run on the
// import worker, so the flow waits for importFinished():
//
//   idle → picking → copying(id) → [companions sheet → copying(id′) …] → idle
//
// It finishes exactly once per start, handing back the host's context with the
// result; a host that has moved on (closed, reseeded) ignores the answer by
// that context, and `wanted` drops a pick that comes back after it did.
Item {
    id: flow
    property var overlayParent: parent
    property string reference: ""
    property string fileName: ""
    property string type: ""
    // An update: the stored path of the library row it replaces, taken when
    // the pick is asked for, never the row's index: rows move while a picker
    // is up or a copy runs (issues #829, #832). An update whose row has no
    // path is refused like one whose row went.
    property bool replacing: false
    property string replacePath: ""
    // The host's, from start() to finished(), unchanged.
    property var context: ({})
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
    // The owner's say on a pick that comes back late: a function of the
    // context, false when the owner's field took a newer choice meanwhile.
    // The import then ends as cancelled, with nothing imported. Null: always.
    property var wanted: null
    // The import in flight (0 when none), and whether it was asked to stop.
    property int request: 0
    property bool cancelling: false
    readonly property bool copying: request > 0
    readonly property bool choosingCompanions: requiredFiles.length > 0
    // The picked name as a person reads it. The host decodes the name it
    // gives, so it shows as it came: an escape left in it ("map%20v2.csv")
    // is part of the file's real name (issue #830).
    readonly property string displayName: fileName
    signal finished(var result, var context)

    // A file's name as a person reads it, from a picked reference, which is
    // URL-encoded ("content://…/primary%3ADownload%2Fmap.csv"). Never for a
    // name the host gave: that one is decoded already.
    function readableName(name) {
        var decoded = name;
        try {
            decoded = decodeURIComponent(name);
        } catch (e) {
            // Not an encoded name: shown as it came.
        }
        return decoded.substring(decoded.lastIndexOf('/') + 1);
    }
    // A chosen companion's name as a person reads it. Only a picked file's
    // reference (a content: or file: URI) is URL-encoded; a library file's
    // stored path carries its real name, shown as it is ("keys%20old.csv").
    function companionName(reference) {
        return /^(content|file):/i.test(reference) ? readableName(reference) : reference.substring(reference.lastIndexOf('/') + 1);
    }

    // One completion per pick()/begin(). A companion rejection only abandons
    // that slot's choice; the enclosing import remains active on the sheet.
    // Refused (false) while a copy runs: that import keeps the flow until its
    // answer is in. @a context is the host's, handed back by finished(); its
    // replacePath names the row to replace, and `replacing: true` marks an
    // update even when the row had no path to give.
    function start(kind, context) {
        if (copying)
            return false;
        cancel();
        reference = "";
        fileName = "";
        type = kind;
        flow.context = context || ({});
        replacePath = flow.context.replacePath || "";
        replacing = flow.context.replacing === true || replacePath.length > 0;
        companions = ({});
        companionNames = ({});
        companionGenerations = ({});
        requiredFiles = [];
        requiredLabels = ({});
        requiredDetails = ({});
        companionNotice = "";
        choiceMade = false;
        active = true;
        return true;
    }
    function pick(kind, context) {
        if (!start(kind, context))
            return false;
        primaryPicker.open();
        return true;
    }
    function begin(uri, name, kind, context) {
        if (!start(kind, context))
            return false;
        reference = uri;
        fileName = name;
        attempt();
        return true;
    }
    function complete(result) {
        if (!active)
            return;
        var context = flow.context;
        active = false;
        request = 0;
        cancelling = false;
        primaryPicker.close();
        picker.close();
        sheet.visible = false;
        reference = "";
        fileName = "";
        replacing = false;
        replacePath = "";
        flow.context = ({});
        choosing = "";
        companions = ({});
        companionNames = ({});
        companionGenerations = ({});
        requiredFiles = [];
        requiredLabels = ({});
        requiredDetails = ({});
        companionNotice = "";
        choiceMade = false;
        finished(result, context);
    }
    // With a copy running, ask it to stop. The library answers "cancelled" on
    // the next turn, without waiting for the copy (a provider may never let
    // go of it), so Cancel stays disabled only until then. Otherwise the flow
    // ends here.
    function cancel() {
        if (!copying) {
            complete({ok: false, error: "cancelled"});
            return;
        }
        if (cancelling)
            return;
        cancelling = true;
        importedFiles.cancelImport(request);
    }
    function stillWanted() {
        return !wanted || wanted(context);
    }
    // The sheet's Cancel, Back and Escape (issue #718): once a companion was
    // chosen, ask before the choices go.
    function requestCancel() {
        if (cancelling)
            return;
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
    // a rejected picker leaves the earlier choice in place. A change would not
    // reach a copy running now, so none is asked for then.
    function chooseFile(key) {
        if (copying)
            return;
        companionNotice = "";
        choosing = key;
        choosingGeneration = generationOf(key);
        picker.open();
    }
    function companionPicked(reference, displayName) {
        var key = choosing;
        choosing = "";
        // A choice made for this slot since the picker was asked wins.
        if (active && !copying && key && generationOf(key) === choosingGeneration)
            selectCompanion(key, reference, displayName);
    }
    // No picker could ask: back to the sheet with every choice kept, and why.
    function companionUnavailable(message) {
        choosing = "";
        if (!active)
            return;
        sheet.visible = true;
        companionNotice = message;
        Navigation.announce(flow, companionNoticeLine, message);
    }
    function primaryPicked(reference, displayName) {
        if (!active || copying)
            return;
        flow.reference = reference;
        // No name from the host: the reference's last segment, decoded once.
        flow.fileName = displayName || readableName(reference);
        attempt();
    }
    function primaryUnavailable(message) {
        if (!active || copying)
            return;
        if (stillWanted())
            complete({ok: false, error: "unavailable", detail: message, type: type});
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
    // The sheet shows at once, as it is (D5): a quick copy shows it only
    // briefly, and nothing behind it takes input while a copy runs. A pick the
    // owner no longer wants ends as cancelled, and an update whose row gave no
    // path is refused before anything is copied (issue #829).
    function attempt() {
        if (!active || copying)
            return;
        if (!stillWanted()) {
            cancel();
            return;
        }
        if (replacing && !replacePath) {
            complete({ok: false, error: "changed", detail: qsTr("That file changed while you were picking. Try again."), name: displayName, type: type});
            return;
        }
        request = importedFiles.beginImport(reference, fileName, type, companions, replacePath);
        sheet.visible = true;
    }
    // The import's answer. One that needs companions keeps the flow on the
    // sheet with its request retired, unless the user had asked to stop.
    function settle(result) {
        var stopped = cancelling;
        request = 0;
        cancelling = false;
        if (result.error === "companions") {
            if (!stopped) {
                showRequirements(result);
                return;
            }
            result = {ok: false, error: "cancelled", name: result.name, type: result.type};
        }
        complete(result);
    }
    Connections {
        target: importedFiles
        function onImportFinished(id, result) {
            if (flow.request > 0 && id === flow.request)
                flow.settle(result);
        }
    }
    // No MIME filter: Android providers index CSV under several MIME types.
    DocumentPicker {
        id: primaryPicker
        objectName: "csvPrimaryPicker"
        mode: "open"
        onAccepted: function (reference, displayName) {
            flow.primaryPicked(reference, displayName);
        }
        // A picker answering after its file is copying is not a cancel.
        onRejected: {
            if (!flow.copying)
                flow.cancel();
        }
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
    // The flow's one sheet: "Copying <name>…" while the first copy runs, then
    // the companion choices when the CSV needs them.
    ModalSheet {
        id: sheet
        objectName: "csvCompanionSheet"
        parent: flow.overlayParent
        accessibleName: !flow.choosingCompanions ? qsTr("Copying %1").arg(flow.displayName) : flow.type === "trunkTargets" ? qsTr("Target CSV companion files") : qsTr("Channel-map companion files")
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
            objectName: "csvCopyingText"
            width: parent.width
            visible: !flow.choosingCompanions
            text: qsTr("Copying %1…").arg(flow.displayName)
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(17)
            font.weight: Font.Bold
        }
        ReadableText {
            width: parent.width
            visible: flow.choosingCompanions
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
                        // A change would not reach the copy running now.
                        enabled: !flow.copying
                        onClicked: flow.selectCompanion(slot.modelData, modelData.path, modelData.name)
                    }
                }
                OutlineButton {
                    objectName: "csvChooseFile_" + slot.index
                    width: parent.width
                    text: qsTr("Choose a file…")
                    enabled: !flow.copying
                    onClicked: flow.chooseFile(slot.modelData)
                }
                ReadableText {
                    objectName: "csvCompanionSelected_" + slot.index
                    width: parent.width
                    text: flow.companions[slot.modelData] ? qsTr("Selected: %1").arg(flow.companionNames[slot.modelData] || flow.companionName(flow.companions[slot.modelData])) : qsTr("No file selected")
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSize(13)
                }
            }
        }
        GradientButton {
            objectName: "csvCompleteBundle"
            width: parent.width
            visible: flow.choosingCompanions
            text: flow.copying ? qsTr("Importing…") : qsTr("Import complete bundle")
            enabled: !flow.copying && flow.requiredFiles.every(function (path) {
                return !!flow.companions[path];
            })
            onClicked: flow.attempt()
        }
        OutlineButton {
            objectName: "csvCancel"
            width: parent.width
            text: flow.cancelling ? qsTr("Cancelling…") : qsTr("Cancel")
            enabled: !flow.cancelling
            onClicked: flow.requestCancel()
        }
    }
}
