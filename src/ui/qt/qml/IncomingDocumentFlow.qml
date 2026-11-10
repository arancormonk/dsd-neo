// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Issue #831: a file another app opened with DSD-neo, or shared to it. Main
// calls begin() once the screen is free (mainRoot.intakeFree); this takes the
// document from the host, asks the import worker what it is, and shows it in
// one sheet: a backup goes on to Restore, anything else is asked what it
// contains and imported. Where it goes is Main's to decide
// (handOffRequested), and Main answers whether it got there (handOffDone).
//
// `busy` holds from before the take until the document is handed off or
// dropped, so Main waits with the next one and holds auto-start meanwhile.
// Every way out of the sheet but a hand-off drops the document.
Item {
    id: flow
    objectName: "incomingDocumentFlow"

    // "" when idle, else probing, presenting or handingOff.
    property string phase: ""
    readonly property bool busy: phase.length > 0
    // The document as the host gave it, a plain string end to end.
    property string reference: ""
    // Its name as the other app gave it: shown as written, never as markup.
    property string fileName: ""
    // What the probe found (ImportedFilesModel::inspectDocument()).
    property string kind: ""
    property int probeId: 0
    // The last hand-off was refused: the sheet is back, saying so.
    property bool refused: false
    // The session on screen replays a file: its Stop says so, as the Monitor's does.
    property bool replaying: false
    // Main's BackupFlow, whose words a backup that can't be restored is told
    // in. Without one, a backup's problem is shown without words, never thrown.
    property Item restoreFlow: null
    // The last problem read out and how many have been, for tests.
    property string announced: ""
    property int announcements: 0

    // Main routes the document: target is "restore", "import" (as @p type) or
    // "targets" (a new scan list). Answer with handOffDone().
    signal handOffRequested(string target, string reference, string name, string type)

    readonly property bool isBackup: kind === "backup"
    // Which backup sheet shows, followed live: a session that stops, a backup
    // job that ends or a restore that is cancelled changes it in place.
    readonly property string backupState: !isBackup ? "" : backup.restorePending ? (backup.restoreCancellable ? "pending" : "pendingFixed") : decoderHost.sessionActive ? "session" : backup.busy ? "busy" : "open"
    readonly property bool restoreAllowed: backup.restoreAllowed && !backup.restorePending
    // The one problem line the sheet shows, if any.
    readonly property string problemText: phase !== "presenting" ? "" : stateProblem() || (refused ? qsTr("Couldn't open that now. Try again.") : "")
    onProblemTextChanged: announceProblem()

    function stateProblem() {
        if (!restoreFlow && kind !== "unreadable")
            return "";
        switch (backupState) {
        case "session":
            return restoreFlow.errorText("SessionActive", "inspect");
        case "busy":
            return restoreFlow.errorText("Busy", "inspect");
        case "pending":
            return restoreFlow.errorText("RestorePending", "inspect");
        case "pendingFixed":
            return Util.restorePartWayText();
        }
        switch (kind) {
        case "newerBackup":
            return restoreFlow.errorText("NewerVersion", "inspect");
        case "damagedBackup":
            return restoreFlow.errorText("Damaged", "inspect");
        case "tooLargeBackup":
            return restoreFlow.errorText("TooLarge", "inspect");
        case "unreadable":
            return qsTr("Couldn't read that file. Open it again from the other app.");
        }
        return "";
    }

    // Take the document waiting, if any, and start telling what it is. False
    // when none waited.
    function begin() {
        if (busy)
            return false;
        phase = "probing";
        var taken = decoderHost.takeIncomingDocument();
        if (!taken.length) {
            phase = "";
            return false;
        }
        reference = taken;
        fileName = "";
        kind = "";
        refused = false;
        kindPicker.kind = "";
        kindPicker.keyHex = false;
        probeId = importedFiles.inspectDocument(taken);
        sheet.open();
        return true;
    }

    function inspected(id, result) {
        if (phase !== "probing" || id !== probeId)
            return;
        probeId = 0;
        if (result.kind === "cancelled") {
            drop();
            return;
        }
        fileName = result.name || "";
        kind = result.kind;
        // Its problem line, if any, is read out as it appears (onProblemTextChanged).
        phase = "presenting";
    }

    // Cancel, Back and an outside tap: the document goes, and a probe still
    // reading is told to stop.
    function drop() {
        if (probeId > 0)
            importedFiles.cancelInspect(probeId);
        probeId = 0;
        reset();
        sheet.visible = false;
    }

    function reset() {
        phase = "";
        reference = "";
        fileName = "";
        kind = "";
        refused = false;
    }

    function handOff(target, type) {
        if (phase !== "presenting")
            return;
        // Copied before the sheet goes: nothing below reads the flow's own.
        var ref = reference;
        var name = fileName;
        phase = "handingOff";
        refused = false;
        sheet.visible = false;
        handOffRequested(target, ref, name, type || "");
    }

    // Main's answer: accepted, the destination has the document now; refused,
    // it stays here, with the sheet back to try again.
    function handOffDone(accepted) {
        if (phase !== "handingOff")
            return;
        if (accepted) {
            reset();
            return;
        }
        // Open first: the refusal is read out only from the sheet on top.
        sheet.open();
        phase = "presenting";
        refused = true;
    }

    // A problem line is read out once each time it appears.
    function announceProblem() {
        if (problemText.length)
            Navigation.announce(flow, problemLine, problemText);
    }

    Connections {
        target: importedFiles
        function onDocumentInspected(id, result) {
            flow.inspected(id, result);
        }
    }

    ModalSheet {
        id: sheet
        objectName: "incomingDocumentSheet"
        // Not "File from another app": the line under the name says that.
        accessibleName: qsTr("Incoming file")
        dismissHandler: function () {
            flow.drop();
        }

        Column {
            width: parent.width
            spacing: 4

            ReadableText {
                objectName: "incomingDocumentName"
                heading: true
                width: parent.width
                text: flow.fileName.length ? flow.fileName : qsTr("This file")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(17)
                font.weight: Font.Bold
                color: Theme.textPrimary
            }
            ReadableText {
                width: parent.width
                text: qsTr("From another app")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSecondary
            }
        }

        ReadableText {
            objectName: "incomingDocumentBody"
            width: parent.width
            visible: text.length > 0
            text: flow.phase === "probing" ? qsTr("Checking the file…") : flow.backupState === "open" ? qsTr("A DSD-neo backup. Next you enter its passphrase and see what it holds. Nothing changes until you confirm.") : ""
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.textSecondary
        }

        Text {
            id: problemLine
            objectName: "incomingDocumentProblem"
            width: parent.width
            visible: text.length > 0
            text: flow.problemText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.alert
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
            Accessible.ignored: !Navigation.allows(problemLine) || !visible
        }

        // Anything that is no backup: what it holds decides where it goes.
        Column {
            width: parent.width
            visible: flow.kind === "other"
            spacing: 12

            ReadableText {
                width: parent.width
                text: qsTr("What does this file contain?")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }
            ImportKindPicker {
                id: kindPicker
                width: parent.width
            }
            // Unlike Imported files' Target CSV, which only keeps the file.
            ReadableText {
                objectName: "incomingDocumentTargetsHint"
                width: parent.width
                visible: kindPicker.kind === "trunkTargets"
                text: qsTr("Starts a new scan list with these targets.")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSecondary
            }
        }

        // D3: Restore needs the session stopped; the stop is offered here,
        // since the Monitor's own is out of reach under the sheet. Never
        // pressed for the user.
        GradientButton {
            objectName: "incomingDocumentStop"
            width: parent.width
            visible: flow.backupState === "session"
            // 3 is DecoderHost.Stopping, as on the Monitor.
            text: decoderHost.sessionState === 3 ? qsTr("Stopping…") : flow.replaying ? qsTr("Stop replay") : qsTr("Stop listening")
            enabled: !decoderHost.transitioning
            onClicked: decoderHost.stop()
        }
        GradientButton {
            objectName: "incomingDocumentRestore"
            width: parent.width
            visible: flow.backupState === "open" || flow.backupState === "busy"
            text: qsTr("Restore…")
            enabled: flow.phase === "presenting" && flow.restoreAllowed
            onClicked: flow.handOff("restore")
        }
        // While the session runs, Restore waits below the Stop that frees it.
        OutlineButton {
            objectName: "incomingDocumentRestoreLater"
            width: parent.width
            visible: flow.backupState === "session"
            text: qsTr("Restore…")
            enabled: false
        }
        GradientButton {
            objectName: "incomingDocumentImport"
            width: parent.width
            visible: flow.kind === "other"
            text: qsTr("Import")
            enabled: flow.phase === "presenting" && kindPicker.kind.length > 0
            onClicked: flow.handOff(kindPicker.kind === "trunkTargets" ? "targets" : "import", kindPicker.type)
        }
        // Cancel while there is something to go on with; Close once there is not.
        OutlineButton {
            objectName: "incomingDocumentCancel"
            width: parent.width
            text: flow.phase === "probing" || flow.kind === "other" || ["open", "session", "busy"].indexOf(flow.backupState) >= 0 ? qsTr("Cancel") : qsTr("Close")
            onClicked: flow.drop()
        }
    }
}
