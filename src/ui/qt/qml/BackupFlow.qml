// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Dialogs
import QtQuick.Window
import "Util.js" as Util

// Issue #580: export this phone's setup to a passphrase-encrypted file, and
// restore one. Main owns the single instance both Settings screens use.
//
// Every path runs between backup.setFlowActive(true) and (false), the system
// pickers included, so Main can hold auto-start and USB resumes for the whole
// flow. `step` names what is on screen; a reply that does not belong to the
// current step (one that lands after a cancel) is ignored.
Item {
    id: flow
    objectName: "backupFlow"

    // "" when idle, else one of: exportPassphrase, encrypting, exportPicker,
    // saving, restorePicker, unlock, checking, confirm, closing.
    property string step: ""
    readonly property bool active: step.length > 0
    // The last result, shown under the Settings card's rows.
    property string notice: ""
    property bool noticeProblem: false
    // The picked document, passed to the model exactly as the picker gave it.
    property string reference: ""
    property var notIncluded: []
    // The unlock sheet's last error was about the file, not the passphrase:
    // retrying cannot help, so the sheet offers another file instead.
    property bool fileProblem: false

    // Entering a restore: Main drops a start still waiting on USB access.
    signal restoreBegun

    function beginExport() {
        if (active)
            return;
        start("exportPassphrase");
        passphraseField.text = "";
        confirmField.text = "";
        showPassphrases.checked = false;
        passphraseSheet.open();
        Qt.callLater(focusField, passphraseField);
    }

    function beginRestore() {
        if (!restoreOpen())
            return;
        start("restorePicker");
        restoreBegun();
        openPicker.open();
    }

    // The restore past its picker, for a document already chosen.
    function restoreFromReference(ref) {
        if (!restoreOpen())
            return;
        start("unlock");
        restoreBegun();
        showUnlock(ref);
    }

    // Settings' "Cancel restore": drop a restore that waits for the next start,
    // whether confirmed here or left by an earlier start.
    function cancelPendingRestore() {
        if (active)
            return;
        var result = backup.discardRestore();
        if (result.ok)
            setNotice(qsTr("Restore cancelled."), false);
        else if (result.discarded)
            setNotice(qsTr("Restore cancelled. Its leftover files are removed the next time DSD-neo opens."), false);
        else
            setNotice(discardRefusedText(result.error), true);
    }

    // Settings offers "Cancel restore" only while backup.restoreCancellable, but
    // the apply can start between that and the tap.
    function discardRefusedText(name) {
        if (name === "RestoreInProgress")
            return Util.restorePartWayText();
        return qsTr("Couldn't cancel the restore. It still finishes when DSD-neo opens again.");
    }

    function restoreOpen() {
        return !active && backup.restoreAllowed && !backup.restorePending;
    }

    function start(first) {
        setNotice("", false);
        reference = "";
        notIncluded = [];
        step = first;
        backup.setFlowActive(true);
    }

    // Drop the field's focus (and the keyboard) before the sheet goes, so no
    // focused field outlives the sheet it sat in.
    function closeSheet(sheet) {
        if (!sheet.visible)
            return;
        Navigation.clearInput(flow.Window.window);
        sheet.visible = false;
    }

    // The end of every path but the closing one: whatever the flow shows goes.
    function finish(text, problem) {
        step = "";
        closeSheet(passphraseSheet);
        closeSheet(unlockSheet);
        closeSheet(savingSheet);
        closeSheet(confirmRestore);
        if (savePicker.visible)
            savePicker.close();
        if (openPicker.visible)
            openPicker.close();
        passphraseField.text = "";
        confirmField.text = "";
        unlockField.text = "";
        reference = "";
        notIncluded = [];
        fileProblem = false;
        confirmRestore.error = "";
        setNotice(text, problem);
        backup.setFlowActive(false);
    }

    function setNotice(text, problem) {
        notice = text;
        noticeProblem = problem;
        announce(text);
    }

    function announce(text) {
        if (text.length && typeof Accessible.announce === "function")
            Accessible.announce(text);
    }

    function focusField(field) {
        if (field.visible && field.enabled)
            field.input.forceActiveFocus();
    }

    // Characters as the model counts them: code points after NFC.
    function characters(text) {
        var normal = text.normalize("NFC");
        var count = 0;
        for (var i = 0; i < normal.length; ++i) {
            var unit = normal.charCodeAt(i);
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < normal.length) {
                var next = normal.charCodeAt(i + 1);
                if (next >= 0xDC00 && next <= 0xDFFF)
                    ++i;
            }
            ++count;
        }
        return count;
    }

    function tooShortText() {
        return qsTr("Use at least %1 characters.").arg(backup.minPassphraseLength);
    }

    // One message per model error name. `action` picks the wording where the
    // same name means different things: prepare, save, inspect or commit.
    function errorText(name, action) {
        switch (name) {
        case "WrongPassphrase":
            return qsTr("Wrong passphrase, or the file is damaged.");
        case "NotBackup":
            return qsTr("This isn't a DSD-neo backup.");
        case "NewerVersion":
            return qsTr("This backup was made by a newer DSD-neo. Update first.");
        case "TooLarge":
            return action === "prepare" ? qsTr("Your setup is too large for one backup.") : qsTr("This file is too large to be a DSD-neo backup.");
        case "Damaged":
            return qsTr("The backup is damaged.");
        case "NoSpace":
            return qsTr("Not enough storage to restore. Free some space and try again.");
        case "SessionActive":
            // At the confirmation nothing can start, so a session still there is stopping.
            return action === "commit" ? qsTr("DSD-neo is still stopping a session. Try again in a moment.") : qsTr("Stop listening before restoring.");
        case "PassphraseTooShort":
            return tooShortText();
        case "Busy":
            return qsTr("Another backup task is still running. Try again in a moment.");
        case "StagingFailed":
            // This phone, not the file: the same file can be tried again.
            return qsTr("Couldn't prepare the restore on this phone. Free some storage and try again.");
        case "SourceChanged":
            // A session appends to its lists while it listens.
            return qsTr("Your setup changed while the backup was being made. Try again.");
        case "RestorePending":
            return qsTr("A restore is already waiting. Cancel it in Settings → Backup, or close and reopen DSD-neo to finish it.");
        }
        // Io, and any name this screen does not know yet.
        if (action === "prepare")
            return qsTr("Couldn't create the backup. Free some storage and try again.");
        if (action === "save")
            return qsTr("Couldn't save the backup there. Try again, or choose another place.");
        if (action === "commit")
            return qsTr("Couldn't start the restore. Try again.");
        return qsTr("Couldn't read that file. Choose it again, or copy it to this phone first.");
    }

    // ---- export ----

    function submitExport() {
        if (step !== "exportPassphrase")
            return;
        var first = passphraseField.text;
        exportError.text = "";
        if (characters(first) < backup.minPassphraseLength) {
            passphraseField.error = tooShortText();
            announce(passphraseField.error);
            focusField(passphraseField);
            return;
        }
        if (first.normalize("NFC") !== confirmField.text.normalize("NFC")) {
            confirmField.error = qsTr("The passphrases don't match.");
            announce(confirmField.error);
            focusField(confirmField);
            return;
        }
        step = "encrypting";
        backup.prepareExport(first);
    }

    // Cancel, Back or Escape: a passphrase already typed is asked about first.
    // The encrypting step cancels at once. Callers commit composing text first
    // (Cancel's activation, or the sheet's dismissal), so a word still being
    // typed counts as typed.
    function dismissExport() {
        if (step === "exportPassphrase" && (passphraseField.text.length || confirmField.text.length))
            passphraseDiscard.ask(cancelExport);
        else
            cancelExport();
    }

    function cancelExport() {
        if (step === "encrypting")
            backup.cancel();
        else if (step !== "exportPassphrase")
            return;
        finish("", false);
    }

    function exportPrepared(result) {
        if (step !== "encrypting")
            return;
        if (!result.ok) {
            step = "exportPassphrase";
            if (result.error === "PassphraseTooShort") {
                passphraseField.error = errorText(result.error, "prepare");
                announce(passphraseField.error);
            } else {
                exportError.text = errorText(result.error, "prepare");
                announce(exportError.text);
            }
            focusField(passphraseField);
            return;
        }
        notIncluded = result.notIncluded || [];
        step = "exportPicker";
        closeSheet(passphraseSheet);
        passphraseField.text = "";
        confirmField.text = "";
        savePicker.selectedFile = backup.suggestedFileName();
        savePicker.open();
    }

    function savePicked(ref) {
        if (step !== "exportPicker")
            return;
        step = "saving";
        savingSheet.open();
        backup.saveExport(ref);
    }

    function savePickCancelled() {
        if (step !== "exportPicker")
            return;
        backup.discardExport();
        finish("", false);
    }

    function exportFinished(result) {
        if (step !== "saving")
            return;
        if (!result.ok) {
            finish(errorText(result.error, "save"), true);
            return;
        }
        finish(savedText(result.notIncluded || notIncluded), false);
    }

    // The backup leaves out what it cannot carry; name the first few.
    function savedText(items) {
        if (!items || !items.length)
            return qsTr("Backup saved.");
        return qsTr("Backup saved.") + " " + Util.notIncludedText(items);
    }

    // ---- restore ----

    function showUnlock(ref) {
        reference = ref;
        fileProblem = false;
        unlockField.text = "";
        unlockField.error = "";
        unlockError.text = "";
        showUnlockPassphrase.checked = false;
        unlockSheet.open();
        Qt.callLater(focusField, unlockField);
    }

    // After an error about the file: back to the picker, the flow still active.
    function chooseAnotherFile() {
        if (step !== "unlock" || !fileProblem)
            return;
        step = "restorePicker";
        closeSheet(unlockSheet);
        unlockField.text = "";
        reference = "";
        openPicker.open();
    }

    function restorePicked(ref) {
        if (step !== "restorePicker")
            return;
        step = "unlock";
        showUnlock(ref);
    }

    function restorePickCancelled() {
        if (step !== "restorePicker")
            return;
        finish("", false);
    }

    function submitUnlock() {
        // No passphrase opens a file that is itself the problem.
        if (step !== "unlock" || fileProblem)
            return;
        unlockError.text = "";
        if (!unlockField.text.length) {
            unlockField.error = qsTr("Enter the passphrase.");
            announce(unlockField.error);
            focusField(unlockField);
            return;
        }
        step = "checking";
        backup.inspectRestore(reference, unlockField.text);
    }

    // As dismissExport(), for the unlock sheet. Once the file itself is the
    // problem no passphrase opens it, so there is nothing to keep.
    function dismissUnlock() {
        if (step === "unlock" && !fileProblem && unlockField.text.length)
            unlockDiscard.ask(cancelUnlock);
        else
            cancelUnlock();
    }

    function cancelUnlock() {
        if (step === "checking")
            backup.cancel();
        else if (step !== "unlock")
            return;
        finish("", false);
    }

    function inspected(result) {
        if (step !== "checking")
            return;
        if (!result.ok) {
            step = "unlock";
            if (result.error === "WrongPassphrase") {
                unlockField.text = "";
                unlockField.error = errorText(result.error, "inspect");
                announce(unlockField.error);
                focusField(unlockField);
                return;
            }
            unlockError.text = errorText(result.error, "inspect");
            announce(unlockError.text);
            // Errors about the file itself: no passphrase opens it.
            fileProblem = ["NotBackup", "NewerVersion", "TooLarge", "Damaged", "Io"].indexOf(result.error) >= 0;
            if (fileProblem)
                chooseFileButton.forceActiveFocus();
            else
                focusField(unlockField);
            return;
        }
        step = "confirm";
        closeSheet(unlockSheet);
        unlockField.text = "";
        confirmRestore.error = "";
        confirmRestore.message = restoreQuestion(result.summary || ({}));
        confirmRestore.open();
    }

    function count(value) {
        var n = Number(value);
        return n > 0 ? Math.floor(n) : 0;
    }

    // The confirmation names what goes and what stays. The version comes from
    // the file, so it is inserted last, where no later arg() can rewrite it.
    function restoreQuestion(summary) {
        var day = count(summary.createdAt) > 0 ? Qt.formatDate(new Date(count(summary.createdAt) * 1000), "MMM d, yyyy") : "";
        // Without a leading "v", as Settings shows this build's version.
        var version = String(summary.app || "").replace(/^v/, "");
        var source = day.length && version.length ? qsTr("the backup from %1 (DSD-neo %2)").arg(day).arg(version) : day.length ? qsTr("the backup from %1").arg(day) : version.length ? qsTr("the backup (DSD-neo %1)").arg(version) : qsTr("the backup");
        var question = qsTr("Replace this phone's saved systems (%1), scan lists (%2), imported files (%3), decryption profiles (%4), search ranges and settings with %5? Call history and replay recordings on this phone are kept. Call recordings are kept up to the backup's Storage limit; oldest recordings are removed first.").arg(count(summary.systems)).arg(count(summary.scanLists)).arg(count(summary.libraryFiles)).arg(count(summary.profiles)).arg(source);
        var notes = [];
        var replays = count(summary.recordingsToChoose);
        if (replays === 1)
            notes.push(qsTr("1 replay system will need its recording chosen again."));
        else if (replays > 1)
            notes.push(qsTr("%1 replay systems will need their recording chosen again.").arg(replays));
        var extra = count(summary.systemsWithExtraArgs);
        if (extra === 1)
            notes.push(qsTr("1 system carries extra decoder options."));
        else if (extra > 1)
            notes.push(qsTr("%1 systems carry extra decoder options.").arg(extra));
        if (summary.appExtraArgs === true)
            notes.push(qsTr("Your settings carry extra decoder options."));
        var parts = [question];
        if (notes.length)
            parts.push(notes.join(" "));
        // What the export could not carry, and library files this phone cannot read.
        if ((summary.notIncluded || []).length)
            parts.push(Util.notIncludedText(summary.notIncluded));
        parts.push(qsTr("DSD-neo will close; open it again to finish."));
        return parts.join("\n\n");
    }

    // The confirmation stays up until this settles it: a refusal that passes
    // keeps the staged restore and the dialog, so confirming again retries.
    function commit() {
        if (step !== "confirm")
            return;
        var result = backup.commitRestore();
        if (result.ok && result.quitting) {
            // DSD-neo is closing: the flow stays active, holding auto-start,
            // until the process goes.
            step = "closing";
            closeSheet(confirmRestore);
            closingSheet.open();
            return;
        }
        if (result.ok) {
            // The host could not close the app; the card says what is waiting.
            finish("", false);
            return;
        }
        if (result.error === "SessionActive" || result.error === "Busy") {
            // A session still stopping, or a job about to end: only Cancel drops
            // the unpacked backup.
            confirmRestore.error = errorText(result.error, "commit");
            announce(confirmRestore.error);
            return;
        }
        // Nothing half-done should wait for the next start.
        backup.discardRestore();
        finish(errorText(result.error, "commit"), true);
    }

    // The stage is not confirmed, and an unconfirmed stage never applies:
    // whatever a partial discard leaves, the next start removes. A committed
    // stage cannot be here (the model refuses an inspection while one waits),
    // so there is nothing to report either way.
    function declineRestore() {
        if (step !== "confirm")
            return;
        backup.discardRestore();
        finish("", false);
    }

    Connections {
        target: backup
        function onExportPrepared(result) {
            flow.exportPrepared(result);
        }
        function onExportFinished(result) {
            flow.exportFinished(result);
        }
        function onInspectFinished(result) {
            flow.inspected(result);
        }
    }

    FileDialog {
        id: savePicker
        objectName: "backupSavePicker"
        fileMode: FileDialog.SaveFile
        // Android takes the new document's MIME type from the first filter; an
        // extension it does not know becomes application/octet-stream.
        nameFilters: [qsTr("DSD-neo backup") + " (*.dsdbak)"]
        defaultSuffix: "dsdbak"
        onAccepted: flow.savePicked(selectedFile.toString())
        onRejected: flow.savePickCancelled()
    }

    // No filter: providers label files freely, and the model identifies a
    // backup by its content, never by its name.
    FileDialog {
        id: openPicker
        objectName: "backupOpenPicker"
        fileMode: FileDialog.OpenFile
        nameFilters: []
        onAccepted: flow.restorePicked(selectedFile.toString())
        onRejected: flow.restorePickCancelled()
    }

    ModalSheet {
        id: passphraseSheet
        objectName: "backupPassphraseSheet"
        accessibleName: qsTr("Encrypt backup")
        // A form sheet (issue #718): an outside tap keeps it; Cancel, Back and
        // Escape all go through dismissExport().
        closesOnOutsideTap: false
        dismissHandler: function () {
            flow.dismissExport();
        }
        DiscardDialog {
            id: passphraseDiscard
            parent: passphraseSheet
        }

        Text {
            width: parent.width
            text: qsTr("Encrypt backup")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(17)
            font.weight: Font.Bold
            color: Theme.textPrimary
        }
        ReadableText {
            objectName: "backupExportScope"
            width: parent.width
            text: qsTr("Includes your settings, saved systems, scan lists, imported files, decryption profiles with their keys, and search ranges. Call history, call recordings, and replay recordings stay on this phone.")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.textSecondary
        }
        ReadableText {
            objectName: "backupExportWarning"
            width: parent.width
            text: qsTr("Holds your keys. Without this passphrase the backup can't be restored. There is no recovery.")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            font.weight: Font.DemiBold
            color: Theme.alert
        }
        PlexTextField {
            id: passphraseField
            objectName: "backupPassphraseField"
            width: parent.width
            label: qsTr("Passphrase")
            hint: qsTr("At least %1 characters").arg(backup.minPassphraseLength)
            echoMode: showPassphrases.checked ? TextInput.Normal : TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | (showPassphrases.checked ? Qt.ImhNone : Qt.ImhHiddenText)
            enabled: flow.step === "exportPassphrase"
            nextField: confirmField
            onTextChanged: {
                error = "";
                confirmField.error = "";
            }
        }
        PlexTextField {
            id: confirmField
            objectName: "backupConfirmField"
            width: parent.width
            label: qsTr("Confirm passphrase")
            echoMode: showPassphrases.checked ? TextInput.Normal : TextInput.Password
            inputMethodHints: passphraseField.inputMethodHints
            enabled: flow.step === "exportPassphrase"
            onTextChanged: error = ""
            onAccepted: flow.submitExport()
        }
        PlexCheckBox {
            id: showPassphrases
            objectName: "backupShowPassphrases"
            text: qsTr("Show passphrases")
            enabled: flow.step === "exportPassphrase"
        }
        Text {
            id: exportError
            objectName: "backupExportError"
            width: parent.width
            visible: text.length > 0
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.alert
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }
        GradientButton {
            objectName: "backupEncryptButton"
            width: parent.width
            text: flow.step === "encrypting" ? qsTr("Encrypting…") : qsTr("Encrypt")
            enabled: flow.step === "exportPassphrase"
            onClicked: flow.submitExport()
        }
        OutlineButton {
            objectName: "backupExportCancel"
            width: parent.width
            text: qsTr("Cancel")
            onClicked: flow.dismissExport()
        }
    }

    // Saving cannot be stopped part way, so this sheet offers no way out.
    ModalSheet {
        id: savingSheet
        objectName: "backupSavingSheet"
        accessibleName: qsTr("Saving…")
        dismissHandler: function () {}

        Text {
            width: parent.width
            text: qsTr("Saving…")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(17)
            font.weight: Font.Bold
            color: Theme.textPrimary
        }
        ReadableText {
            width: parent.width
            text: qsTr("Keep DSD-neo open until the backup is saved.")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.textSecondary
        }
    }

    ModalSheet {
        id: unlockSheet
        objectName: "backupUnlockSheet"
        accessibleName: qsTr("Unlock backup")
        // As the passphrase sheet: every exit goes through dismissUnlock().
        closesOnOutsideTap: false
        dismissHandler: function () {
            flow.dismissUnlock();
        }
        DiscardDialog {
            id: unlockDiscard
            parent: unlockSheet
        }

        Text {
            width: parent.width
            text: qsTr("Unlock backup")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(17)
            font.weight: Font.Bold
            color: Theme.textPrimary
        }
        ReadableText {
            width: parent.width
            text: qsTr("Enter the passphrase this backup was made with.")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.textSecondary
        }
        // Once the file itself is the problem no passphrase can help: the field
        // and its toggle step aside, leaving the error, "Choose another file"
        // and Cancel. A new pick opens the sheet with them back in place.
        PlexTextField {
            id: unlockField
            objectName: "backupUnlockField"
            width: parent.width
            visible: !flow.fileProblem
            label: qsTr("Passphrase")
            echoMode: showUnlockPassphrase.checked ? TextInput.Normal : TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | (showUnlockPassphrase.checked ? Qt.ImhNone : Qt.ImhHiddenText)
            enabled: flow.step === "unlock" && !flow.fileProblem
            onTextChanged: error = ""
            onAccepted: flow.submitUnlock()
        }
        PlexCheckBox {
            id: showUnlockPassphrase
            objectName: "backupShowUnlockPassphrase"
            visible: !flow.fileProblem
            text: qsTr("Show passphrase")
            enabled: flow.step === "unlock" && !flow.fileProblem
        }
        Text {
            id: unlockError
            objectName: "backupUnlockError"
            width: parent.width
            visible: text.length > 0
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.alert
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }
        // The primary action: Unlock, or another file once this one is the problem.
        GradientButton {
            objectName: "backupUnlockButton"
            width: parent.width
            visible: !flow.fileProblem
            text: flow.step === "checking" ? qsTr("Checking backup…") : qsTr("Unlock")
            enabled: flow.step === "unlock" && !flow.fileProblem
            onClicked: flow.submitUnlock()
        }
        GradientButton {
            id: chooseFileButton
            objectName: "backupChooseFileButton"
            width: parent.width
            visible: flow.fileProblem
            text: qsTr("Choose another file")
            enabled: flow.step === "unlock" && flow.fileProblem
            onClicked: flow.chooseAnotherFile()
        }
        OutlineButton {
            objectName: "backupUnlockCancel"
            width: parent.width
            text: qsTr("Cancel")
            onClicked: flow.dismissUnlock()
        }
    }

    ConfirmDialog {
        id: confirmRestore
        objectName: "backupConfirmRestore"
        title: qsTr("Replace your setup?")
        confirmText: qsTr("Replace and close")
        destructive: true
        closeOnConfirm: false
        onConfirmed: flow.commit()
        onCancelled: flow.declineRestore()
    }

    // The restore is committed and DSD-neo is on its way out: nothing to undo here.
    ModalSheet {
        id: closingSheet
        objectName: "backupClosingSheet"
        accessibleName: qsTr("Closing DSD-neo…")
        dismissHandler: function () {}

        Text {
            width: parent.width
            text: qsTr("Closing DSD-neo…")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(17)
            font.weight: Font.Bold
            color: Theme.textPrimary
        }
        ReadableText {
            width: parent.width
            text: qsTr("Open it again to finish restoring your backup.")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.textSecondary
        }
    }
}
