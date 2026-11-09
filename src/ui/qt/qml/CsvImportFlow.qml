// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Dialogs

// One CSV import, from the pick to its answer (#832). The copy and the checks
// run on the import worker, so the flow waits for importFinished():
//
//   idle → picking → copying(id) → [companions sheet → copying(id′) …] → idle
//
// It finishes exactly once per start, handing back the host's context with the
// result; a host that has moved on (closed, reseeded) ignores the answer by
// that context.
Item {
    id: flow
    property var overlayParent: parent
    property string reference: ""
    property string fileName: ""
    property string type: ""
    // The stored path of the library row this import replaces, or "" for a new
    // row: a path, not a row index, since rows move while a copy runs.
    property string replacePath: ""
    // The host's, from start() to finished(), unchanged.
    property var context: ({})
    property var companions: ({})
    property var requiredFiles: []
    property var requiredLabels: ({})
    property var requiredDetails: ({})
    property string choosing: ""
    property bool active: false
    // Whether the user has chosen a companion on the sheet (a library file or a
    // picked one). Preselected matches are the app's guess, not a choice.
    property bool choiceMade: false
    // The import in flight (0 when none), and whether it was asked to stop.
    property int request: 0
    property bool cancelling: false
    readonly property bool copying: request > 0
    readonly property bool choosingCompanions: requiredFiles.length > 0
    // The picked name as a person reads it.
    readonly property string displayName: readableName(fileName)
    signal finished(var result, var context)

    // A file's name as a person reads it, from a name or a reference: a
    // provider's arrives URL-encoded ("primary%3ADownload%2Fmap.csv").
    function readableName(name) {
        var decoded = name;
        try {
            decoded = decodeURIComponent(name);
        } catch (e) {
            // Not an encoded name: shown as it came.
        }
        return decoded.substring(decoded.lastIndexOf('/') + 1);
    }

    // One completion per pick()/begin(). A companion rejection only abandons
    // that slot's choice; the enclosing import remains active on the sheet.
    // Refused (false) while a copy runs: that import keeps the flow until its
    // answer is in. @a context is the host's, handed back by finished(); its
    // replacePath, when set, names the row to replace.
    function start(kind, context) {
        if (copying)
            return false;
        cancel();
        reference = "";
        fileName = "";
        type = kind;
        flow.context = context || ({});
        replacePath = flow.context.replacePath || "";
        companions = ({});
        requiredFiles = [];
        requiredLabels = ({});
        requiredDetails = ({});
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
        replacePath = "";
        flow.context = ({});
        choosing = "";
        companions = ({});
        requiredFiles = [];
        requiredLabels = ({});
        requiredDetails = ({});
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
    function selectCompanion(key, path) {
        var next = Object.assign({}, companions);
        next[key] = path;
        companions = next;
        choiceMade = true;
    }
    // The current companion stays until the picker hands back a replacement:
    // a rejected picker leaves the earlier choice in place.
    function chooseFile(key) {
        choosing = key;
        picker.open();
    }
    function showRequirements(result) {
        var details = result.requiredDetails || ({});
        var names = requiredFiles.filter(function (key) { return !!details[key]; });
        var next = Object.assign({}, companions);
        for (var key of result.required) {
            if (details[key] && details[key].selectionError)
                next[key] = "";
            if (names.indexOf(key) >= 0)
                continue;
            names.push(key);
            if (next[key] === undefined && details[key] && details[key].preselected)
                next[key] = details[key].preselected;
        }
        requiredFiles = names;
        companions = next;
        requiredLabels = result.requiredLabels || ({});
        requiredDetails = details;
        sheet.visible = true;
    }
    // The sheet shows at once, as it is (D5): a quick copy shows it only
    // briefly, and nothing behind it takes input while a copy runs.
    function attempt() {
        if (!active || copying)
            return;
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
    FileDialog {
        id: primaryPicker
        objectName: "csvPrimaryPicker"
        onAccepted: {
            if (!flow.active || flow.copying)
                return;
            flow.reference = selectedFile.toString();
            flow.fileName = flow.reference.substring(flow.reference.lastIndexOf('/') + 1);
            flow.attempt();
        }
        // A dialog closing after it handed back a file is not a cancel.
        onRejected: {
            if (!flow.copying)
                flow.cancel();
        }
    }
    FileDialog {
        id: picker
        objectName: "csvCompanionPicker"
        onAccepted: {
            if (flow.active && flow.choosing)
                flow.selectCompanion(flow.choosing, selectedFile.toString());
            flow.choosing = "";
        }
        onRejected: {
            flow.choosing = "";
            if (flow.active)
                sheet.visible = true;
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
                        onClicked: flow.selectCompanion(slot.modelData, modelData.path)
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
                    text: flow.companions[slot.modelData] ? qsTr("Selected: %1").arg(flow.readableName(flow.companions[slot.modelData])) : qsTr("No file selected")
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
