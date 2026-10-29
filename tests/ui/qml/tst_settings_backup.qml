// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Dialogs
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #580: the Settings Backup card, the one backup flow Main owns for both
// Settings instances, and the restore result card on Home. `backup` is the
// recording fake in qml_backup_recorder.h (qml_test_context.h installs it); a
// case drives its results through testContext and reads back what the screens
// asked for.
Item {
    width: 420
    height: 900
    Loader {
        id: appLoader
        anchors.fill: parent
    }

    TestCase {
        name: "SettingsBackup"
        when: windowShown
        property var app: null
        property var flow: null
        property var settings: null
        property int usbRow: -1
        property bool savedAutoStart: false
        property bool savedOnboarding: false
        readonly property string pass: "correct horse battery"
        readonly property string pickedReference: "content://com.example.documents/document/primary%3ABackups%2Fphone.dsdbak"
        readonly property var fullSummary: ({
                createdAt: 1760000000,
                app: "1.4.0",
                systems: 3,
                scanLists: 2,
                libraryFiles: 5,
                profiles: 1,
                ranges: 2,
                avoids: 4,
                recordingsToChoose: 2,
                systemsWithExtraArgs: 1,
                appExtraArgs: false,
                notIncluded: []
            })

        function init() {
            testContext.useLifecycleHost(true);
            testContext.resetBackup();
            savedAutoStart = prefs.autoStartOnAttach;
            savedOnboarding = prefs.onboardingDone;
            prefs.onboardingDone = true;
            appLoader.source = uiDir + "/Main.qml";
            app = appLoader.item;
            verify(app !== null);
            app.currentTab = 2;
            settings = findChild(app, "idleSettingsScreen");
            flow = findChild(app, "backupFlow");
            verify(settings !== null);
            verify(flow !== null);
            // Keys and taps go to the active window, and Main is its own. The
            // first window a run activates reports active a turn before its
            // focus-in lands, and only then can an item take keyboard focus.
            app.requestActivate();
            tryCompare(app, "active", true);
            tryCompare(app.contentItem, "activeFocus", true);
            tryVerify(function () {
                return settings.visible && settings.opacity > 0.9;
            });
        }

        function cleanup() {
            // Close what a case left open before Main goes, so no field keeps focus through teardown.
            Ui.Navigation.clearInput(app);
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            appLoader.source = "";
            app = null;
            flow = null;
            settings = null;
            testContext.resetBackup();
            testContext.setScanTestUsb(false, false);
            testContext.setLifecyclePhase(0);
            if (usbRow >= 0)
                savedSystems.remove(usbRow);
            usbRow = -1;
            prefs.autoStartOnAttach = savedAutoStart;
            prefs.onboardingDone = savedOnboarding;
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            testContext.useLifecycleHost(false);
        }

        // ---- helpers ----

        function calls(name) {
            return testContext.backupCalls().filter(function (call) {
                return call.name === name;
            });
        }
        function lastFlowActive() {
            var list = calls("setFlowActive");
            return list.length ? list[list.length - 1].args[0] : undefined;
        }
        function part(name) {
            var found = findChild(flow, name);
            verify(found !== null, name);
            return found;
        }
        function count(item, name) {
            var n = item.objectName === name ? 1 : 0;
            for (var child of item.children || [])
                n += count(child, name);
            return n;
        }
        // Accessible names of the buttons a surface offers right now.
        function buttons(item) {
            var names = [];
            for (var child of item.children || []) {
                if (!child.visible)
                    continue;
                if (child.Accessible.role === Accessible.Button)
                    names.push(child.Accessible.name);
                names = names.concat(buttons(child));
            }
            return names;
        }
        function typePassphrases(first, second) {
            part("backupPassphraseField").text = first;
            part("backupConfirmField").text = second;
        }
        function openExport() {
            findChild(settings, "settingsBackupExportRow").activate();
            tryCompare(part("backupPassphraseSheet"), "visible", true);
        }
        function encrypt() {
            openExport();
            typePassphrases(pass, pass);
            part("backupEncryptButton").activate();
            compare(calls("prepareExport").length, 1);
        }
        function toSavePicker(notIncluded) {
            encrypt();
            verify(testContext.emitBackup("exportPrepared", {ok: true, error: "", summary: {}, notIncluded: notIncluded || []}));
            var picker = part("backupSavePicker");
            tryCompare(picker, "visible", true);
            return picker;
        }
        // The non-native dialog the offscreen run gets stays open after a bare
        // accepted(), and closing it then reports rejected too: the flow has to
        // ignore that once the pick has moved it on.
        function pick(picker, reference) {
            picker.selectedFile = reference;
            picker.accepted();
            picker.close();
        }
        function toUnlock() {
            flow.restoreFromReference(pickedReference);
            tryCompare(part("backupUnlockSheet"), "visible", true);
        }
        function unlock() {
            toUnlock();
            part("backupUnlockField").text = pass;
            part("backupUnlockButton").activate();
            compare(calls("inspectRestore").length, 1);
        }
        function toConfirm(summary) {
            unlock();
            verify(testContext.emitBackup("inspectFinished", {ok: true, error: "", summary: summary || fullSummary}));
            var confirm = part("backupConfirmRestore");
            tryCompare(confirm, "visible", true);
            return confirm;
        }
        function armUsbStart() {
            usbRow = savedSystems.count;
            savedSystems.add({name: "Backup USB", sourceType: "usb", freqMhz: "851.5"});
            testContext.setScanTestUsb(true, false);
            app.startSystem(usbRow);
            verify(app.awaitingUsbAccess);
            verify(app.pendingStart !== null);
        }
        function dismissBy(sheet, method) {
            if (method === "back")
                verify(Ui.Navigation.back(app), "Back consumes the sheet");
            else if (method === "host")
                decoderHost.backRequested();
            else if (method === "escape")
                keyClick(Qt.Key_Escape);
            else
                mouseClick(sheet, 2, 2);
        }
        function notice(screen) {
            return findChild(screen || settings, "settingsBackupNotice");
        }

        // ---- the Settings card ----

        function verifyCard(screen, listening) {
            var header = findChild(screen, "settingsBackupHeader");
            verify(header !== null);
            compare(header.text, "Backup");
            var exportRow = findChild(screen, "settingsBackupExportRow");
            compare(exportRow.title, "Export backup");
            compare(exportRow.subtitle, "Encrypted file with your settings, systems, scan lists, imported files and keys");
            var restoreRow = findChild(screen, "settingsBackupRestoreRow");
            compare(restoreRow.title, "Restore from backup");
            // A restore replaces what a session uses: it waits for Stop.
            compare(restoreRow.subtitle, listening ? "Stop listening to restore" : "Replaces your setup on this phone");
            compare(restoreRow.enabled, !listening);
            verify(exportRow.height >= Ui.Theme.minimumTouchSize);
            verify(restoreRow.height >= Ui.Theme.minimumTouchSize);
            // The card sits right before About & support.
            verify(header.mapToItem(screen, 0, 0).y < findChild(screen, "settingsSupportHeader").mapToItem(screen, 0, 0).y);
        }

        function test_card_in_both_settings_instances() {
            compare(count(app.contentItem, "backupFlow"), 1, "one flow serves both Settings screens");
            verifyCard(settings, false);
            openExport();
            compare(flow.step, "exportPassphrase");
            part("backupExportCancel").activate();
            tryCompare(part("backupPassphraseSheet"), "visible", false);
            compare(backup.flowActive, false);

            testContext.setLifecyclePhase(2);
            tryCompare(app, "monitorMode", true);
            app.sessionDestination = "settings";
            var session = findChild(app, "sessionSettingsScreen");
            tryVerify(function () {
                return session.visible;
            });
            compare(backup.restoreAllowed, false);
            verifyCard(session, true);
            findChild(session, "settingsBackupExportRow").activate();
            tryCompare(part("backupPassphraseSheet"), "visible", true);
            compare(flow.step, "exportPassphrase");
            verify(backup.flowActive);
            part("backupExportCancel").activate();
            tryCompare(part("backupPassphraseSheet"), "visible", false);
            compare(lastFlowActive(), false);
            app.sessionDestination = "";
        }

        function test_restore_row_follows_restore_allowed_data() {
            return [
                // A job on the worker shuts the row but is no reason to stop listening.
                {tag: "busy", busy: true, phase: 0, subtitle: "Replaces your setup on this phone"},
                {tag: "listening", busy: false, phase: 2, subtitle: "Stop listening to restore"},
                {tag: "starting", busy: false, phase: 1, subtitle: "Stop listening to restore"}
            ];
        }

        function test_restore_row_follows_restore_allowed(data) {
            var row = findChild(settings, "settingsBackupRestoreRow");
            verify(row.enabled);
            verify(testContext.setBackup("busy", data.busy));
            testContext.setLifecyclePhase(data.phase);
            compare(backup.restoreAllowed, false);
            compare(row.enabled, false);
            compare(row.subtitle, data.subtitle);
            compare(row.showCaret, false);
            row.activate();
            flow.beginRestore();
            compare(flow.step, "");
            compare(calls("setFlowActive").length, 0);
            compare(part("backupOpenPicker").visible, false);
            verify(testContext.setBackup("busy", false));
            testContext.setLifecyclePhase(0);
            compare(backup.restoreAllowed, true);
            verify(row.enabled);
            compare(row.subtitle, "Replaces your setup on this phone");
        }

        function test_export_focus_returns_to_its_row() {
            var row = findChild(settings, "settingsBackupExportRow");
            row.forceActiveFocus();
            keyClick(Qt.Key_Space);
            var sheet = part("backupPassphraseSheet");
            tryCompare(sheet, "visible", true);
            tryCompare(part("backupPassphraseField").input, "activeFocus", true);
            compare(part("backupPassphraseField").input.echoMode, TextInput.Password);
            verify(part("backupPassphraseField").input.Accessible.passwordEdit);
            keyClick(Qt.Key_Escape);
            tryCompare(sheet, "visible", false);
            tryCompare(row, "activeFocus", true);
            compare(lastFlowActive(), false);
        }

        // ---- export ----

        function test_export_passphrase_rules_data() {
            return [
                {tag: "empty", first: "", second: "", field: "backupPassphraseField", error: "Use at least 8 characters."},
                {tag: "seven characters", first: "seven77", second: "seven77", field: "backupPassphraseField", error: "Use at least 8 characters."},
                // Eight UTF-16 units but four characters: counted the way the model counts.
                {tag: "surrogate pairs", first: "😀😀😀😀", second: "😀😀😀😀", field: "backupPassphraseField", error: "Use at least 8 characters."},
                // Eight units that normalize to seven characters.
                {tag: "decomposed accent", first: "abcdefé", second: "abcdefé", field: "backupPassphraseField", error: "Use at least 8 characters."},
                {tag: "mismatch", first: "correct horse battery", second: "correct horse battery!", field: "backupConfirmField", error: "The passphrases don't match."}
            ];
        }

        function test_export_passphrase_rules(data) {
            openExport();
            typePassphrases(data.first, data.second);
            part("backupEncryptButton").activate();
            compare(part(data.field).error, data.error);
            compare(calls("prepareExport").length, 0);
            verify(part("backupPassphraseSheet").visible);
            compare(flow.step, "exportPassphrase");
            // Editing clears the message, and a good pair encrypts.
            typePassphrases(pass, pass);
            compare(part(data.field).error, "");
            part("backupEncryptButton").activate();
            compare(calls("prepareExport").length, 1);
            compare(calls("prepareExport")[0].args[0], pass);
        }

        function test_show_passphrases_reveals_both_fields() {
            openExport();
            var show = part("backupShowPassphrases");
            compare(show.checked, false);
            show.toggle();
            compare(part("backupPassphraseField").input.echoMode, TextInput.Normal);
            compare(part("backupConfirmField").input.echoMode, TextInput.Normal);
            show.toggle();
            compare(part("backupPassphraseField").input.echoMode, TextInput.Password);
            compare(part("backupConfirmField").input.echoMode, TextInput.Password);
            // Reopening starts hidden again.
            show.toggle();
            part("backupExportCancel").activate();
            openExport();
            compare(part("backupShowPassphrases").checked, false);
            compare(part("backupPassphraseField").input.echoMode, TextInput.Password);
        }

        function test_show_passphrase_on_unlock() {
            toUnlock();
            var show = part("backupShowUnlockPassphrase");
            var field = part("backupUnlockField");
            compare(show.checked, false);
            show.toggle();
            compare(field.input.echoMode, TextInput.Normal);
            show.toggle();
            compare(field.input.echoMode, TextInput.Password);
            show.toggle();
            part("backupUnlockCancel").activate();
            toUnlock();
            compare(part("backupShowUnlockPassphrase").checked, false, "reopening starts hidden");
            compare(field.input.echoMode, TextInput.Password);
        }

        function tapBeside(sheet) {
            var x = 4, y = sheet.height / 2;
            verify(!sheet.hitsPanel(x, y), "the side margin lies outside the panel");
            mouseClick(sheet, x, y);
        }
        function shownDiscard(item) {
            if (!item || !item.visible)
                return null;
            if (item.accessibleName === "Discard changes?")
                return item;
            for (var child of item.children) {
                var found = shownDiscard(child);
                if (found)
                    return found;
            }
            return null;
        }
        function dismissTyped(sheet, method) {
            if (method === "cancel")
                part(sheet.objectName === "backupPassphraseSheet" ? "backupExportCancel" : "backupUnlockCancel").activate();
            else
                dismissBy(sheet, method);
        }
        // A typed passphrase is not thrown away by Cancel, Back or Escape: the
        // sheet asks first (issue #718: Cancel used to skip the question).
        function test_typed_passphrase_asks_before_cancel_data() {
            return [
                {tag: "export by Cancel", step: "export", method: "cancel", field: "backupPassphraseField"},
                {tag: "export confirm only by Cancel", step: "export", method: "cancel", field: "backupConfirmField"},
                {tag: "export by Back", step: "export", method: "back", field: "backupPassphraseField"},
                {tag: "export by Escape", step: "export", method: "escape", field: "backupConfirmField"},
                {tag: "export by host Back", step: "export", method: "host", field: "backupPassphraseField"},
                {tag: "unlock by Cancel", step: "unlock", method: "cancel", field: "backupUnlockField"},
                {tag: "unlock by Back", step: "unlock", method: "back", field: "backupUnlockField"},
                {tag: "unlock by Escape", step: "unlock", method: "escape", field: "backupUnlockField"}
            ];
        }
        // Issue #718: both passphrase sheets are forms. A tap beside the panel
        // keeps them open and asks nothing, typed or not.
        function test_tap_beside_keeps_the_passphrase_sheets_data() {
            return [
                {tag: "export, empty", step: "export", field: "", text: ""},
                {tag: "export, typed", step: "export", field: "backupPassphraseField", text: "half typed"},
                {tag: "unlock, empty", step: "unlock", field: "", text: ""},
                {tag: "unlock, typed", step: "unlock", field: "backupUnlockField", text: "half typed"}
            ];
        }
        function test_tap_beside_keeps_the_passphrase_sheets(data) {
            var exporting = data.step === "export";
            if (exporting)
                openExport();
            else
                toUnlock();
            var sheet = part(exporting ? "backupPassphraseSheet" : "backupUnlockSheet");
            waitForRendering(sheet);
            if (data.field.length)
                part(data.field).text = data.text;
            tapBeside(sheet);
            wait(50);
            verify(sheet.visible, "a tap beside the panel keeps the sheet");
            verify(!shownDiscard(sheet), "and asks nothing");
            compare(flow.step, exporting ? "exportPassphrase" : "unlock");
            if (data.field.length)
                compare(part(data.field).text, data.text);
            dismissTyped(sheet, "cancel");
            if (data.field.length)
                findChild(shownDiscard(sheet), "discardDialogDiscard").activate();
            tryCompare(sheet, "visible", false);
            compare(lastFlowActive(), false);
        }
        // A passphrase still composing on the keyboard counts as typed: Cancel
        // asks instead of dropping it unseen.
        function test_composing_passphrase_asks_before_cancel() {
            toUnlock();
            var sheet = part("backupUnlockSheet");
            waitForRendering(sheet);
            var field = part("backupUnlockField");
            field.input.forceActiveFocus();
            verify(testContext.composeText("secret"));
            compare(field.text, "");
            var cancel = part("backupUnlockCancel");
            mouseClick(cancel, cancel.width / 2, cancel.height / 2);
            verify(sheet.visible, "the composing passphrase survives Cancel");
            compare(field.text, "secret");
            var discard = shownDiscard(sheet);
            verify(discard !== null, "Cancel asks first");
            findChild(discard, "discardDialogDiscard").activate();
            tryCompare(sheet, "visible", false);
            compare(lastFlowActive(), false);
        }
        function test_typed_passphrase_asks_before_cancel(data) {
            var exporting = data.step === "export";
            if (exporting)
                openExport();
            else
                toUnlock();
            var sheet = part(exporting ? "backupPassphraseSheet" : "backupUnlockSheet");
            waitForRendering(sheet);
            var field = part(data.field);
            field.input.forceActiveFocus();
            field.text = "half typed";
            dismissTyped(sheet, data.method);
            verify(sheet.visible, "the typed passphrase survives the dismissal");
            compare(flow.step, exporting ? "exportPassphrase" : "unlock");
            var discard = shownDiscard(sheet);
            verify(discard !== null, "the sheet asks before discarding");
            findChild(discard, "discardDialogKeep").activate();
            verify(sheet.visible);
            compare(field.text, "half typed");
            tryVerify(function () { return Ui.Navigation.contains(sheet, app.activeFocusItem); }, 1000, "focus returns into the sheet");
            dismissTyped(sheet, data.method);
            discard = shownDiscard(sheet);
            verify(discard !== null);
            findChild(discard, "discardDialogDiscard").activate();
            tryCompare(sheet, "visible", false);
            compare(flow.step, "");
            compare(field.text, "");
            compare(calls("cancel").length, 0);
            compare(lastFlowActive(), false);
        }
        // Discarding returns focus to the row that opened the sheet.
        function test_discarded_passphrase_returns_focus_to_its_row() {
            var row = findChild(settings, "settingsBackupExportRow");
            row.forceActiveFocus();
            keyClick(Qt.Key_Space);
            var sheet = part("backupPassphraseSheet");
            tryCompare(sheet, "visible", true);
            tryCompare(part("backupPassphraseField").input, "activeFocus", true);
            keyClick("a");
            compare(part("backupPassphraseField").text, "a");
            keyClick(Qt.Key_Escape);
            verify(sheet.visible);
            var discard = shownDiscard(sheet);
            verify(discard !== null);
            findChild(discard, "discardDialogDiscard").activate();
            tryCompare(sheet, "visible", false);
            tryCompare(row, "activeFocus", true);
            compare(lastFlowActive(), false);
        }
        // The empty sheets and the file-problem sheet (no passphrase can open
        // that file) close on Cancel without asking. Issue #718: Cancel with a
        // typed passphrase asks, as Back and Escape do.
        function test_cancel_and_untyped_sheets_close_without_asking() {
            openExport();
            typePassphrases(pass, "");
            part("backupExportCancel").activate();
            verify(part("backupPassphraseSheet").visible, "a typed passphrase survives Cancel");
            findChild(shownDiscard(flow), "discardDialogDiscard").activate();
            tryCompare(part("backupPassphraseSheet"), "visible", false);
            verify(!shownDiscard(flow));
            openExport();
            part("backupExportCancel").activate();
            tryCompare(part("backupPassphraseSheet"), "visible", false);
            verify(!shownDiscard(flow));
            toUnlock();
            part("backupUnlockField").text = pass;
            part("backupUnlockCancel").activate();
            verify(part("backupUnlockSheet").visible, "a typed passphrase survives Cancel");
            findChild(shownDiscard(flow), "discardDialogDiscard").activate();
            tryCompare(part("backupUnlockSheet"), "visible", false);
            verify(!shownDiscard(flow));
            unlock();
            verify(testContext.emitBackup("inspectFinished", {ok: false, error: "Damaged", summary: {}}));
            verify(flow.fileProblem);
            part("backupUnlockCancel").activate();
            tryCompare(part("backupUnlockSheet"), "visible", false);
            verify(!shownDiscard(flow));
            compare(lastFlowActive(), false);
        }
        function test_cancel_before_encrypting_cancels_no_job() {
            openExport();
            typePassphrases(pass, pass);
            part("backupExportCancel").activate();
            findChild(shownDiscard(flow), "discardDialogDiscard").activate();
            tryCompare(part("backupPassphraseSheet"), "visible", false);
            compare(calls("cancel").length, 0);
            compare(calls("prepareExport").length, 0);
            compare(lastFlowActive(), false);
            compare(part("backupPassphraseField").text, "");
        }

        function test_export_saves_to_the_picked_document() {
            openExport();
            var sheet = part("backupPassphraseSheet");
            compare(sheet.accessibleName, "Encrypt backup");
            verify(part("backupExportWarning").text.indexOf("There is no recovery.") >= 0);
            // Issue #708: what stays behind names both kinds of recording.
            verify(part("backupExportScope").text.indexOf("Call history, call recordings, and replay recordings stay on this phone.") >= 0);
            typePassphrases(pass, pass);
            var button = part("backupEncryptButton");
            button.activate();
            compare(calls("prepareExport")[0].args[0], pass);
            compare(button.text, "Encrypting…");
            verify(!button.enabled);
            verify(!part("backupPassphraseField").enabled);
            verify(backup.flowActive);

            verify(testContext.emitBackup("exportPrepared", {ok: true, error: "", summary: {}, notIncluded: ["Imported file old.csv"]}));
            tryCompare(sheet, "visible", false);
            compare(part("backupPassphraseField").text, "");
            compare(part("backupConfirmField").text, "");
            var picker = part("backupSavePicker");
            verify(picker.visible);
            compare(picker.fileMode, FileDialog.SaveFile);
            compare(picker.selectedFile.toString(), "dsd-neo-backup-2026-10-14.dsdbak");
            // One name filter: Android turns its unknown extension into application/octet-stream.
            compare(picker.nameFilters.length, 1);
            verify(picker.nameFilters[0].indexOf("(*.dsdbak)") > 0);
            compare(flow.step, "exportPicker");
            // No sheet is up while the picker is: flowActive alone holds auto-start.
            compare(Ui.Navigation.modals.length, 0);
            verify(backup.flowActive);
            verify(uiController.autoStartBlocked);

            pick(picker, pickedReference);
            compare(calls("saveExport").length, 1);
            compare(calls("saveExport")[0].args[0], pickedReference);
            compare(calls("discardExport").length, 0);
            var saving = part("backupSavingSheet");
            tryCompare(saving, "visible", true);
            // Saving offers no way out.
            for (var method of ["back", "host", "scrim"]) {
                dismissBy(saving, method);
                verify(saving.visible, method);
            }
            compare(buttons(saving), []);
            compare(calls("cancel").length, 0);
            compare(flow.step, "saving");

            verify(testContext.emitBackup("exportFinished", {ok: true, error: "", summary: {}, notIncluded: ["Imported file old.csv"]}));
            tryCompare(saving, "visible", false);
            compare(lastFlowActive(), false);
            verify(!backup.flowActive);
            compare(flow.step, "");
            var line = notice();
            verify(line.visible);
            compare(line.text.indexOf("Backup saved."), 0);
            verify(line.text.indexOf("Imported file old.csv") > 0);
            compare(line.textFormat, Text.PlainText);
            compare(line.color, Ui.Theme.textSecondary);
            compare(notice(findChild(app, "sessionSettingsScreen")).text, line.text);
        }

        function test_export_lists_at_most_a_few_missing_items() {
            var missing = [];
            for (var i = 1; i <= 9; ++i)
                missing.push("Imported file " + i + ".csv");
            pick(toSavePicker(missing), pickedReference);
            verify(testContext.emitBackup("exportFinished", {ok: true, error: "", summary: {}, notIncluded: missing}));
            var text = notice().text;
            verify(text.indexOf("Imported file 1.csv") > 0);
            verify(text.indexOf("Imported file 9.csv") < 0);
            verify(text.indexOf("4 more") > 0, text);
        }

        function test_export_picker_cancel_discards() {
            var picker = toSavePicker();
            picker.reject();
            compare(calls("discardExport").length, 1);
            compare(calls("saveExport").length, 0);
            compare(lastFlowActive(), false);
            compare(flow.step, "");
            compare(notice().text, "");
        }

        function test_cancel_while_busy_data() {
            return [
                {tag: "encrypting by button", step: "encrypt", method: "button"},
                {tag: "encrypting by Back", step: "encrypt", method: "back"},
                {tag: "encrypting by Escape", step: "encrypt", method: "escape"},
                {tag: "checking by button", step: "inspect", method: "button"},
                {tag: "checking by host Back", step: "inspect", method: "host"},
                {tag: "checking by Escape", step: "inspect", method: "escape"}
            ];
        }

        function test_cancel_while_busy(data) {
            var exporting = data.step === "encrypt";
            if (exporting)
                encrypt();
            else
                unlock();
            var sheet = part(exporting ? "backupPassphraseSheet" : "backupUnlockSheet");
            if (data.method === "button")
                part(exporting ? "backupExportCancel" : "backupUnlockCancel").activate();
            else {
                if (data.method === "escape")
                    sheet.forceActiveFocus();
                dismissBy(sheet, data.method);
            }
            tryCompare(sheet, "visible", false);
            compare(calls("cancel").length, 1);
            compare(lastFlowActive(), false);
            compare(flow.step, "");
            // The model drops a cancelled job's result; a late one changes nothing here either.
            if (exporting) {
                testContext.emitBackup("exportPrepared", {ok: true, error: "", summary: {}, notIncluded: []});
                verify(!part("backupSavePicker").visible);
            } else {
                testContext.emitBackup("inspectFinished", {ok: true, error: "", summary: fullSummary});
                verify(!part("backupConfirmRestore").visible);
            }
            compare(calls("commitRestore").length, 0);
            compare(calls("saveExport").length, 0);
            compare(lastFlowActive(), false);
        }

        function test_export_prepare_errors_stay_in_the_sheet_data() {
            return [
                {tag: "PassphraseTooShort", field: "backupPassphraseField", text: "Use at least 8 characters."},
                {tag: "Io", field: "", text: "Couldn't create the backup. Free some storage and try again."},
                {tag: "TooLarge", field: "", text: "Your setup is too large for one backup."},
                {tag: "Busy", field: "", text: "Another backup task is still running. Try again in a moment."},
                {tag: "SourceChanged", field: "", text: "Your setup changed while the backup was being made. Try again."}
            ];
        }

        function test_export_prepare_errors_stay_in_the_sheet(data) {
            encrypt();
            // Focus is off the field, as after a keyboard user's Encrypt (and once the open's own focus has run).
            wait(0);
            part("backupPassphraseSheet").forceActiveFocus();
            verify(part("backupPassphraseSheet").activeFocus);
            verify(testContext.emitBackup("exportPrepared", {ok: false, error: data.tag, summary: {}, notIncluded: []}));
            var sheet = part("backupPassphraseSheet");
            verify(sheet.visible);
            compare(flow.step, "exportPassphrase");
            compare(part("backupEncryptButton").text, "Encrypt");
            verify(part("backupEncryptButton").enabled);
            verify(!part("backupSavePicker").visible);
            if (data.field.length)
                compare(part(data.field).error, data.text);
            else
                compare(part("backupExportError").text, data.text);
            compare(part("backupExportError").Accessible.role, Accessible.AlertMessage);
            compare(part("backupExportError").textFormat, Text.PlainText);
            verify(backup.flowActive);
            // Back in the field, ready to change the passphrase or try again.
            tryCompare(part("backupPassphraseField").input, "activeFocus", true);
        }

        function test_export_save_error_lands_in_the_card() {
            pick(toSavePicker(), pickedReference);
            verify(testContext.emitBackup("exportFinished", {ok: false, error: "Io", summary: {}, notIncluded: []}));
            tryCompare(part("backupSavingSheet"), "visible", false);
            compare(lastFlowActive(), false);
            compare(notice().text, "Couldn't save the backup there. Try again, or choose another place.");
            compare(notice().color, Ui.Theme.alert);
        }

        // ---- restore ----

        function test_restore_picks_any_file_then_unlocks() {
            findChild(settings, "settingsBackupRestoreRow").activate();
            var picker = part("backupOpenPicker");
            tryCompare(picker, "visible", true);
            compare(picker.fileMode, FileDialog.OpenFile);
            compare(picker.nameFilters.length, 0);
            compare(flow.step, "restorePicker");
            compare(Ui.Navigation.modals.length, 0);
            verify(backup.flowActive);
            verify(uiController.autoStartBlocked);
            // Any name: the model checks the content, never the name.
            var path = testContext.writeFixtureCsv("holiday-photos.txt", "DSDNEOBK");
            verify(path.length > 0);
            var reference = "file://" + path;
            pick(picker, reference);
            var sheet = part("backupUnlockSheet");
            tryCompare(sheet, "visible", true);
            compare(sheet.accessibleName, "Unlock backup");
            compare(flow.step, "unlock");
            compare(calls("inspectRestore").length, 0);
            compare(part("backupUnlockField").input.echoMode, TextInput.Password);
            part("backupUnlockField").text = pass;
            var button = part("backupUnlockButton");
            button.activate();
            var inspect = calls("inspectRestore");
            compare(inspect.length, 1);
            compare(inspect[0].args[0], reference);
            compare(inspect[0].args[1], pass);
            compare(button.text, "Checking backup…");
            verify(!button.enabled);
            verify(!part("backupUnlockField").enabled);
            verify(backup.flowActive);
        }

        function test_restore_picker_cancel_ends_the_flow() {
            findChild(settings, "settingsBackupRestoreRow").activate();
            tryCompare(part("backupOpenPicker"), "visible", true);
            part("backupOpenPicker").reject();
            compare(flow.step, "");
            compare(lastFlowActive(), false);
            var names = testContext.backupCalls().map(function (call) {
                return call.name;
            });
            compare(names, ["setFlowActive", "setFlowActive"]);
        }

        function test_unlock_asks_for_a_passphrase() {
            toUnlock();
            // The sheet opens ready to type into.
            tryCompare(part("backupUnlockField").input, "activeFocus", true);
            verify(part("backupUnlockField").input.Accessible.passwordEdit);
            part("backupUnlockButton").activate();
            compare(part("backupUnlockField").error, "Enter the passphrase.");
            compare(calls("inspectRestore").length, 0);
            part("backupUnlockField").text = "x";
            compare(part("backupUnlockField").error, "");
        }

        function test_inspect_errors_stay_in_the_sheet_data() {
            return [
                {tag: "WrongPassphrase", field: true, text: "Wrong passphrase, or the file is damaged."},
                {tag: "NotBackup", file: true, text: "This isn't a DSD-neo backup."},
                {tag: "NewerVersion", file: true, text: "This backup was made by a newer DSD-neo. Update first."},
                {tag: "TooLarge", file: true, text: "This file is too large to be a DSD-neo backup."},
                {tag: "Damaged", file: true, text: "The backup is damaged."},
                {tag: "NoSpace", text: "Not enough storage to restore. Free some space and try again."},
                {tag: "SessionActive", text: "Stop listening before restoring."},
                {tag: "RestorePending", text: "A restore is already waiting. Cancel it in Settings → Backup, or close and reopen DSD-neo to finish it."},
                {tag: "Busy", text: "Another backup task is still running. Try again in a moment."},
                {tag: "Io", file: true, text: "Couldn't read that file. Choose it again, or copy it to this phone first."},
                // This phone, not the file: the same file can be tried again.
                {tag: "StagingFailed", text: "Couldn't prepare the restore on this phone. Free some storage and try again."}
            ];
        }

        function test_inspect_errors_stay_in_the_sheet(data) {
            unlock();
            wait(0);
            part("backupUnlockSheet").forceActiveFocus();
            verify(part("backupUnlockSheet").activeFocus);
            verify(testContext.emitBackup("inspectFinished", {ok: false, error: data.tag, summary: {}}));
            var sheet = part("backupUnlockSheet");
            verify(sheet.visible);
            compare(flow.step, "unlock");
            verify(!part("backupConfirmRestore").visible);
            var button = part("backupUnlockButton");
            compare(button.text, "Unlock");
            var choose = part("backupChooseFileButton");
            var field = part("backupUnlockField");
            var error = part("backupUnlockError");
            compare(field.error, data.field ? data.text : "");
            compare(error.text, data.field ? "" : data.text);
            compare(error.Accessible.role, Accessible.AlertMessage);
            compare(error.textFormat, Text.PlainText);
            if (data.field)
                compare(field.text, "");
            var toggle = part("backupShowUnlockPassphrase");
            if (data.file) {
                // No passphrase opens this file: Unlock goes, another file is the way on.
                verify(!button.visible && !button.enabled);
                verify(choose.visible && choose.enabled);
                compare(choose.text, "Choose another file");
                compare(buttons(sheet), ["Choose another file", "Cancel"]);
                tryCompare(choose, "activeFocus", true);
                // Nor is the passphrase offered: the field and its toggle step aside, and the keyboard's Enter (or
                // its IME action) submits nothing.
                verify(!field.visible && !toggle.visible);
                verify(!field.enabled && !toggle.enabled);
                field.accepted();
                flow.submitUnlock();
                compare(calls("inspectRestore").length, 1);
                compare(flow.step, "unlock");
                compare(error.text, data.text);
                compare(field.error, "");
                verify(choose.visible && !button.visible);
                // What a screen reader and the keyboard meet: the error, then the way on, then Cancel.
                compare(buttons(sheet), ["Choose another file", "Cancel"]);
                var cancel = part("backupUnlockCancel");
                tryVerify(function () {
                    return error.y < choose.y && choose.y < cancel.y;
                }, 2000, "the error, then Choose another file, then Cancel");
            } else {
                // A retry can work: Unlock stays, back in the field.
                verify(button.visible && button.enabled);
                verify(!choose.visible);
                verify(field.visible && toggle.visible && field.enabled && toggle.enabled);
                tryCompare(field.input, "activeFocus", true);
            }
            verify(backup.flowActive);
            // Nothing was staged, so leaving discards nothing. A passphrase still
            // in the field is asked about first (issue #718).
            part("backupUnlockCancel").activate();
            if (field.visible && field.text.length)
                findChild(shownDiscard(sheet), "discardDialogDiscard").activate();
            else
                verify(!shownDiscard(sheet));
            tryCompare(sheet, "visible", false);
            compare(calls("discardRestore").length, 0);
            compare(lastFlowActive(), false);
        }

        function test_choose_another_file() {
            unlock();
            // Where the passphrase row and Unlock sit before the file turns out to be the problem.
            var fieldY = part("backupUnlockField").y;
            var unlockY = part("backupUnlockButton").y;
            verify(testContext.emitBackup("inspectFinished", {ok: false, error: "NotBackup", summary: {}}));
            var sheet = part("backupUnlockSheet");
            part("backupChooseFileButton").activate();
            tryCompare(sheet, "visible", false);
            var picker = part("backupOpenPicker");
            tryCompare(picker, "visible", true);
            compare(flow.step, "restorePicker");
            // Still one flow: nothing let auto-start go between the two files.
            verify(backup.flowActive);
            compare(lastFlowActive(), true);
            verify(uiController.autoStartBlocked);
            compare(calls("discardRestore").length, 0);
            var path = testContext.writeFixtureCsv("other.dsdbak", "DSDNEOBK");
            verify(path.length > 0);
            var other = "file://" + path;
            pick(picker, other);
            tryCompare(sheet, "visible", true);
            // The sheet comes back as it first opened: nothing moves into place after it shows.
            verify(part("backupUnlockField").visible && part("backupShowUnlockPassphrase").visible);
            compare(part("backupUnlockField").y, fieldY);
            compare(part("backupUnlockButton").y, unlockY);
            compare(flow.step, "unlock");
            compare(part("backupUnlockError").text, "");
            verify(part("backupUnlockButton").visible && part("backupUnlockButton").enabled);
            verify(!part("backupChooseFileButton").visible);
            verify(part("backupUnlockField").visible && part("backupShowUnlockPassphrase").visible);
            verify(part("backupUnlockField").enabled && part("backupShowUnlockPassphrase").enabled);
            part("backupUnlockField").text = pass;
            part("backupUnlockButton").activate();
            var inspect = calls("inspectRestore");
            compare(inspect.length, 2);
            compare(inspect[1].args[0], other);
        }

        function test_choose_another_file_then_cancel() {
            unlock();
            verify(testContext.emitBackup("inspectFinished", {ok: false, error: "Damaged", summary: {}}));
            part("backupChooseFileButton").activate();
            var picker = part("backupOpenPicker");
            tryCompare(picker, "visible", true);
            picker.reject();
            compare(flow.step, "");
            compare(lastFlowActive(), false);
            verify(!part("backupUnlockSheet").visible);
        }

        function test_confirm_message_data() {
            var day = Qt.formatDate(new Date(1760000000 * 1000), "MMM d, yyyy");
            var head = "Replace this phone's saved systems (3), scan lists (2), imported files (5), decryption profiles (1), search ranges and settings with the backup from " + day + " (DSD-neo 1.4.0)? Call history and replay recordings on this phone are kept. Call recordings are kept up to the backup's Storage limit; oldest recordings are removed first.";
            var close = "DSD-neo will close; open it again to finish.";
            return [
                {tag: "every note", summary: fullSummary, has: [head, "2 replay systems will need their recording chosen again.", "1 system carries extra decoder options.", close], lacks: ["Your settings carry"]},
                {tag: "no notes", summary: Object.assign({}, fullSummary, {recordingsToChoose: 0, systemsWithExtraArgs: 0}), has: [head, close], lacks: ["replay system", "extra decoder"]},
                {tag: "singular and plural", summary: Object.assign({}, fullSummary, {recordingsToChoose: 1, systemsWithExtraArgs: 4}), has: ["1 replay system will need its recording chosen again.", "4 systems carry extra decoder options.", close], lacks: []},
                {tag: "settings arguments", summary: Object.assign({}, fullSummary, {appExtraArgs: true}), has: ["Your settings carry extra decoder options.", close], lacks: []},
                {tag: "no date or version", summary: Object.assign({}, fullSummary, {createdAt: 0, app: ""}), has: ["search ranges and settings with the backup? Call history", close], lacks: ["DSD-neo 1.4.0"]},
                {tag: "version as Settings shows it", summary: Object.assign({}, fullSummary, {app: "v2.10.0"}), has: ["(DSD-neo 2.10.0)?", close], lacks: ["DSD-neo v2.10.0"]},
                {tag: "left out", summary: Object.assign({}, fullSummary, {notIncluded: ["Targets.csv: couldn't be read on this phone", "Metro: groups.csv (named by full path)"]}), has: ["Not included:\nTargets.csv: couldn't be read on this phone\nMetro: groups.csv (named by full path)", close], lacks: []},
                {tag: "nothing left out", summary: fullSummary, has: [close], lacks: ["Not included"]}
            ];
        }

        function test_confirm_message(data) {
            var confirm = toConfirm(data.summary);
            compare(part("backupUnlockSheet").visible, false);
            compare(part("backupUnlockField").text, "");
            compare(confirm.title, "Replace your setup?");
            compare(confirm.confirmText, "Replace and close");
            verify(confirm.destructive);
            compare(findChild(confirm, "confirmActionButton").border.color, Ui.Theme.magenta);
            for (var text of data.has)
                verify(confirm.message.indexOf(text) >= 0, "missing: " + text + " in " + confirm.message);
            for (var absent of data.lacks)
                verify(confirm.message.indexOf(absent) < 0, "unexpected: " + absent);
            // The close sentence ends the message.
            compare(confirm.message.lastIndexOf("DSD-neo will close; open it again to finish."), confirm.message.length - "DSD-neo will close; open it again to finish.".length);
            compare(calls("commitRestore").length, 0);
            verify(backup.flowActive);
        }

        function test_backup_text_stays_plain() {
            var confirm = toConfirm(Object.assign({}, fullSummary, {app: "<b>9.9</b> %1"}));
            verify(confirm.message.indexOf("(DSD-neo <b>9.9</b> %1)") >= 0, confirm.message);
            var message = findChild(confirm, "confirmMessage");
            compare(message.text, confirm.message);
            compare(message.textFormat, Text.PlainText);
        }

        function test_confirm_commits_and_closes() {
            testContext.setBackupReply("commitRestore", {ok: true, quitting: true, error: ""});
            var confirm = toConfirm();
            findChild(confirm, "confirmActionButton").activate();
            compare(calls("commitRestore").length, 1);
            compare(calls("discardRestore").length, 0);
            tryCompare(confirm, "visible", false);
            var closing = part("backupClosingSheet");
            tryCompare(closing, "visible", true);
            compare(closing.accessibleName, "Closing DSD-neo…");
            // DSD-neo is going away: nothing leaves the sheet, and auto-start stays held.
            for (var method of ["back", "host", "scrim"]) {
                dismissBy(closing, method);
                verify(closing.visible, method);
            }
            compare(buttons(closing), []);
            verify(backup.flowActive);
            verify(uiController.autoStartBlocked);
            // The stage is committed, quitting or not.
            verify(backup.restorePending);
        }

        function test_commit_without_quitting_waits_for_a_reopen() {
            var confirm = toConfirm();
            findChild(confirm, "confirmActionButton").activate();
            tryCompare(confirm, "visible", false);
            verify(!part("backupClosingSheet").visible);
            verify(backup.restorePending);
            compare(lastFlowActive(), false);
            compare(flow.step, "");
            var line = findChild(settings, "settingsBackupPendingNotice");
            tryCompare(line, "visible", true);
            compare(line.text, "Restore is ready. Close DSD-neo and open it again to finish.");
            compare(line.color, Ui.Theme.alert);
            var restoreRow = findChild(settings, "settingsBackupRestoreRow");
            verify(!restoreRow.enabled, "a waiting restore keeps the row shut");
            compare(restoreRow.subtitle, "A restore is waiting to finish");
            var cancel = findChild(settings, "settingsBackupCancelRestore");
            verify(cancel.visible);
            compare(cancel.text, "Cancel restore");
            verify(cancel.height >= Ui.Theme.minimumTouchSize);
            cancel.activate();
            compare(calls("discardRestore").length, 1);
            verify(!backup.restorePending);
            tryCompare(line, "visible", false);
            verify(!cancel.visible);
            compare(notice().text, "Restore cancelled.");
            compare(notice().color, Ui.Theme.textSecondary);
            verify(restoreRow.enabled);
        }

        function test_cancel_restore_outcomes_data() {
            return [
                {tag: "kept", reply: {ok: false, discarded: false, error: "Io"}, pending: true, text: "Couldn't cancel the restore. It still finishes when DSD-neo opens again.", problem: true},
                {tag: "left files", reply: {ok: false, discarded: true, error: "Io"}, pending: false, text: "Restore cancelled. Its leftover files are removed the next time DSD-neo opens.", problem: false}
            ];
        }

        function test_cancel_restore_outcomes(data) {
            verify(testContext.setBackup("restorePending", true));
            testContext.setBackupReply("discardRestore", data.reply);
            findChild(settings, "settingsBackupCancelRestore").activate();
            compare(calls("discardRestore").length, 1);
            compare(backup.restorePending, data.pending);
            compare(notice().text, data.text);
            compare(notice().color, data.problem ? Ui.Theme.alert : Ui.Theme.textSecondary);
        }

        function test_waiting_restore_from_an_earlier_start() {
            // An earlier start deferred it: the card on Home and the waiting line in Settings both offer the way out.
            verify(testContext.setBackup("startupOutcome", {kind: "deferred", message: "The decoder was busy, so the restore waits for the next start"}));
            verify(testContext.setBackup("restorePending", true));
            var line = findChild(settings, "settingsBackupPendingNotice");
            tryCompare(line, "visible", true);
            compare(line.text, "Restore is ready. Close DSD-neo and open it again to finish.");
            var cancel = findChild(settings, "settingsBackupCancelRestore");
            verify(cancel.visible);
            cancel.activate();
            compare(calls("discardRestore").length, 1);
            verify(!backup.restorePending);
            compare(backup.startupOutcome.kind, "", "discarding retires the deferred card too");
            compare(notice().text, "Restore cancelled.");
        }

        function test_restore_part_way_through_cannot_be_cancelled() {
            // Its apply has started: only the next start can finish it, so nothing offers to cancel it.
            verify(testContext.setBackup("startupOutcome", {kind: "stuck", message: "Restore couldn't finish. Free up storage and reopen DSD-neo"}));
            verify(testContext.setBackup("restorePending", true));
            verify(testContext.setBackup("restoreCancellable", false));
            var line = findChild(settings, "settingsBackupPendingNotice");
            tryCompare(line, "visible", true);
            compare(line.text, "This restore is part-way through and can't be cancelled. Close and reopen DSD-neo to finish it.");
            compare(line.color, Ui.Theme.alert);
            verify(!findChild(settings, "settingsBackupCancelRestore").visible);
            verify(buttons(settings).indexOf("Cancel restore") < 0);
            verify(!findChild(settings, "settingsBackupRestoreRow").enabled);
            // Once it can be cancelled again (a later start rolled it back), the way out returns.
            verify(testContext.setBackup("restoreCancellable", true));
            compare(line.text, "Restore is ready. Close DSD-neo and open it again to finish.");
            verify(findChild(settings, "settingsBackupCancelRestore").visible);
        }

        function test_cancel_refused_once_the_apply_started() {
            // Offered while it could be cancelled, refused because the apply started since.
            verify(testContext.setBackup("restorePending", true));
            testContext.setBackupReply("discardRestore", {ok: false, discarded: false, error: "RestoreInProgress"});
            findChild(settings, "settingsBackupCancelRestore").activate();
            compare(calls("discardRestore").length, 1);
            verify(backup.restorePending);
            compare(notice().text, "This restore is part-way through and can't be cancelled. Close and reopen DSD-neo to finish it.");
            compare(notice().color, Ui.Theme.alert);
            verify(findChild(settings, "settingsBackupPendingNotice").visible);
        }

        function test_cancel_at_confirm_discards_data() {
            return [{tag: "Cancel", method: "button"}, {tag: "Back", method: "back"}, {tag: "host Back", method: "host"}, {tag: "scrim", method: "scrim"}];
        }

        function test_cancel_at_confirm_discards(data) {
            var confirm = toConfirm();
            if (data.method === "button")
                findChild(confirm, "confirmCancelButton").activate();
            else
                dismissBy(confirm, data.method);
            tryCompare(confirm, "visible", false);
            compare(calls("discardRestore").length, 1);
            compare(calls("commitRestore").length, 0);
            compare(lastFlowActive(), false);
            compare(flow.step, "");
            compare(notice().text, "");
        }

        function test_decline_with_files_left_reports_nothing() {
            // An unconfirmed stage never applies: what a partial discard leaves, the next start removes.
            testContext.setBackupReply("discardRestore", {ok: false, discarded: true, error: "Io"});
            var confirm = toConfirm();
            findChild(confirm, "confirmCancelButton").activate();
            tryCompare(confirm, "visible", false);
            compare(calls("discardRestore").length, 1);
            compare(lastFlowActive(), false);
            compare(notice().text, "");
        }

        function test_refused_commit() {
            testContext.setBackupReply("commitRestore", {ok: false, quitting: false, error: "Io"});
            var confirm = toConfirm();
            findChild(confirm, "confirmActionButton").activate();
            tryCompare(confirm, "visible", false);
            compare(calls("commitRestore").length, 1);
            // The unpacked backup is dropped so nothing half-done waits for the next start.
            compare(calls("discardRestore").length, 1);
            verify(!part("backupClosingSheet").visible);
            compare(lastFlowActive(), false);
            compare(notice().text, "Couldn't start the restore. Try again.");
            compare(notice().color, Ui.Theme.alert);
        }

        function test_commit_refused_for_a_moment_data() {
            return [
                {tag: "SessionActive", text: "DSD-neo is still stopping a session. Try again in a moment."},
                {tag: "Busy", text: "Another backup task is still running. Try again in a moment."}
            ];
        }

        // A session still stopping (or a job about to end) passes: the unpacked
        // backup stays, and confirming again retries without a new pick.
        function test_commit_refused_for_a_moment(data) {
            testContext.setBackupReply("commitRestore", {ok: false, quitting: false, error: data.tag});
            var confirm = toConfirm();
            findChild(confirm, "confirmActionButton").activate();
            compare(calls("commitRestore").length, 1);
            verify(confirm.visible);
            compare(flow.step, "confirm");
            var error = findChild(confirm, "confirmError");
            verify(error.visible);
            compare(error.text, data.text);
            compare(error.color, Ui.Theme.alert);
            compare(error.Accessible.role, Accessible.AlertMessage);
            compare(error.textFormat, Text.PlainText);
            compare(calls("discardRestore").length, 0);
            verify(backup.flowActive);
            compare(notice().text, "");
            compare(buttons(confirm), ["Replace and close", "Cancel"]);
            // Confirming again retries.
            testContext.setBackupReply("commitRestore", {ok: true, quitting: true, error: ""});
            findChild(confirm, "confirmActionButton").activate();
            compare(calls("commitRestore").length, 2);
            tryCompare(confirm, "visible", false);
            tryCompare(part("backupClosingSheet"), "visible", true);
            compare(calls("discardRestore").length, 0);
        }

        function test_commit_refused_for_a_moment_then_cancel() {
            testContext.setBackupReply("commitRestore", {ok: false, quitting: false, error: "SessionActive"});
            var confirm = toConfirm();
            findChild(confirm, "confirmActionButton").activate();
            verify(confirm.visible);
            // Only the user's Cancel drops the unpacked backup.
            findChild(confirm, "confirmCancelButton").activate();
            tryCompare(confirm, "visible", false);
            compare(calls("discardRestore").length, 1);
            compare(lastFlowActive(), false);
            compare(findChild(confirm, "confirmError").text, "");
            // The next confirmation starts without the old error.
            toUnlock();
            part("backupUnlockField").text = pass;
            part("backupUnlockButton").activate();
            verify(testContext.emitBackup("inspectFinished", {ok: true, error: "", summary: fullSummary}));
            tryCompare(confirm, "visible", true);
            compare(findChild(confirm, "confirmError").text, "");
            verify(!findChild(confirm, "confirmError").visible);
        }

        // ---- layering ----

        function test_backup_sheets_cover_the_scan_layers() {
            // The scan-list editor (z 20) and the session's scan warnings (z 21) are Main's highest layers.
            var editor = findChild(app, "scanListScreen");
            verify(flow.z > editor.z);
            app.scanListOpen = true;
            testContext.setLifecyclePhase(2);
            tryCompare(app, "monitorMode", true);
            app.scanWarnings = "Target 2 skipped: no channel map";
            app.sessionDestination = "settings";
            findChild(findChild(app, "sessionSettingsScreen"), "settingsBackupExportRow").activate();
            var sheet = part("backupPassphraseSheet");
            tryCompare(sheet, "visible", true);
            waitForRendering(app.contentItem);
            // A point inside the warning panel, over the scrim: the flow is on top there.
            var point = sheet.mapToItem(app.contentItem, 10, sheet.height - 10);
            compare(app.contentItem.childAt(point.x, point.y), flow);
            mouseClick(app.contentItem, point.x, point.y);
            // Issue #718: the passphrase sheet is a form, so the tap keeps it.
            wait(50);
            verify(sheet.visible, "the tap beside the form keeps it");
            compare(app.scanWarnings, "Target 2 skipped: no channel map", "the tap landed on the scrim, not the warning");
            part("backupExportCancel").activate();
            tryCompare(sheet, "visible", false);
            compare(app.scanWarnings, "Target 2 skipped: no channel map");
            compare(lastFlowActive(), false);
            app.scanWarnings = "";
            app.scanListOpen = false;
            app.sessionDestination = "";
        }

        // ---- the restore flow and USB starts ----

        function test_usb_grant_during_restore_data() {
            return [{tag: "picker", step: "picker"}, {tag: "unlock sheet", step: "unlock"}, {tag: "confirmation", step: "confirm"}];
        }

        function test_usb_grant_during_restore(data) {
            armUsbStart();
            app.pendingRestart = {uid: savedSystems.get(usbRow).uid};
            if (data.step === "picker") {
                findChild(settings, "settingsBackupRestoreRow").activate();
                tryCompare(part("backupOpenPicker"), "visible", true);
            } else if (data.step === "unlock") {
                toUnlock();
            } else {
                toConfirm();
            }
            // Entering the restore dropped the start waiting on USB access and the pending restart.
            compare(app.pendingStart, null);
            compare(app.pendingStartRow, -1);
            compare(app.awaitingUsbAccess, false);
            compare(app.pendingRestart, null);
            verify(uiController.autoStartBlocked);
            // A wait armed meanwhile is refused by the grant rather than resumed.
            app.pendingStart = savedSystems.get(usbRow);
            app.pendingStartRow = usbRow;
            app.awaitingUsbAccess = true;
            testContext.setScanTestUsb(true, true);
            compare(decoderHost.sessionState, 0);
            compare(app.pendingStart, null);
            compare(app.awaitingUsbAccess, false);
            verify(backup.flowActive);
        }

        function test_usb_grant_during_export_picker() {
            armUsbStart();
            toSavePicker();
            testContext.setScanTestUsb(true, true);
            compare(decoderHost.sessionState, 0);
            compare(app.pendingStart, null);
            compare(app.awaitingUsbAccess, false);
            // After the flow, a grant starts as before.
            part("backupSavePicker").reject();
            compare(lastFlowActive(), false);
            testContext.setScanTestUsb(true, false);
            app.startSystem(usbRow);
            verify(app.awaitingUsbAccess);
            testContext.setScanTestUsb(true, true);
            compare(decoderHost.sessionState, 1);
        }

        function test_attach_during_picker_starts_nothing() {
            usbRow = savedSystems.count;
            savedSystems.add({name: "Attached USB", sourceType: "usb", freqMhz: "851.5"});
            prefs.autoStartOnAttach = true;
            prefs.lastStartedKind = "saved";
            prefs.lastStartedUid = savedSystems.get(usbRow).uid;
            testContext.setScanTestUsb(true, true);
            tryCompare(uiController, "autoStartBlocked", false);
            findChild(settings, "settingsBackupRestoreRow").activate();
            tryCompare(part("backupOpenPicker"), "visible", true);
            verify(uiController.autoStartBlocked);
            testContext.emitLocalDeviceAttached();
            compare(decoderHost.sessionState, 0);
            part("backupOpenPicker").reject();
            tryCompare(uiController, "autoStartBlocked", false);
            // Positive control: with the flow over, an attachment starts.
            testContext.emitLocalDeviceAttached();
            compare(decoderHost.sessionState, 1);
        }

        // ---- the Home result card ----

        function test_result_card_data() {
            var counts = {systems: 3, scanLists: 2, libraryFiles: 5, profiles: 1, ranges: 2, avoids: 4, systemsWithExtraArgs: 0, appExtraArgs: false, createdAt: 1760000000, app: "1.4.0"};
            return [
                {tag: "applied", outcome: Object.assign({kind: "applied", message: "", recordingsToChoose: 2}, counts), title: "Backup restored", lines: ["This phone now has the backup's saved systems (3), scan lists (2), imported files (5), decryption profiles (1), search ranges and settings.", "2 replay systems need their recording chosen again."], problem: false, actions: ["Dismiss"]},
                {tag: "applied, one recording", outcome: Object.assign({kind: "applied", message: "", recordingsToChoose: 1}, counts), title: "Backup restored", lines: ["1 replay system needs its recording chosen again."], problem: false, actions: ["Dismiss"]},
                {tag: "applied, no recordings", outcome: Object.assign({kind: "applied", message: "", recordingsToChoose: 0}, counts), title: "Backup restored", lines: [], lacks: "replay system", problem: false, actions: ["Dismiss"]},
                {tag: "applied, left out", outcome: Object.assign({kind: "applied", message: "", recordingsToChoose: 0, notIncluded: ["Targets.csv: couldn't be read on this phone"]}, counts), title: "Backup restored", lines: ["Not included:\nTargets.csv: couldn't be read on this phone"], problem: false, actions: ["Dismiss"]},
                {tag: "failed", outcome: Object.assign({kind: "failed", message: "A file of the current configuration couldn't be read. Nothing was changed", recordingsToChoose: 0}, counts), title: "Restore not applied. Nothing was changed.", lines: ["A file of the current configuration couldn't be read."], problem: true, actions: ["Dismiss"]},
                {tag: "deferred", outcome: Object.assign({kind: "deferred", message: "The decoder was busy, so the restore waits for the next start", recordingsToChoose: 0}, counts), pending: true, title: "The decoder was busy, so the restore waits for the next start.", lines: ["Reopen DSD-neo to try again."], problem: true, actions: ["Discard restore", "Dismiss"]},
                {tag: "deferred for storage", outcome: Object.assign({kind: "deferred", message: "Not enough free storage to restore. Free up storage and reopen DSD-neo", recordingsToChoose: 0}, counts), pending: true, title: "Not enough free storage to restore. Free up storage and reopen DSD-neo.", lines: [], lacks: "Reopen DSD-neo to try again.", problem: true, actions: ["Discard restore", "Dismiss"]},
                {tag: "deferred part-way through", outcome: Object.assign({kind: "deferred", message: "The decoder was busy, so the restore waits for the next start", recordingsToChoose: 0}, counts), pending: true, partWay: true, title: "The decoder was busy, so the restore waits for the next start.", lines: ["This restore is part-way through and can't be cancelled. Close and reopen DSD-neo to finish it."], lacks: "Reopen DSD-neo to try again.", problem: true, actions: ["Dismiss"]},
                {tag: "stuck", outcome: Object.assign({kind: "stuck", message: "Restore couldn't finish. Free up storage and reopen DSD-neo", recordingsToChoose: 0}, counts), title: "Restore couldn't finish. Free up storage and reopen DSD-neo.", lines: [], problem: true, actions: ["Dismiss"]}
            ];
        }

        function test_result_card(data) {
            app.currentTab = 0;
            var home = findChild(app, "homeScreen");
            tryVerify(function () {
                return home.visible;
            });
            var card = findChild(home, "backupResultCard");
            verify(card !== null);
            verify(!card.visible);
            if (data.pending)
                verify(testContext.setBackup("restorePending", true));
            if (data.partWay)
                verify(testContext.setBackup("restoreCancellable", false));
            verify(testContext.setBackup("startupOutcome", data.outcome));
            tryCompare(card, "visible", true);
            compare(card.Accessible.role, Accessible.AlertMessage);
            tryCompare(card, "announced", card.Accessible.name);
            var title = findChild(card, "backupResultTitle");
            compare(title.text, data.title);
            compare(title.textFormat, Text.PlainText);
            var detail = findChild(card, "backupResultDetail");
            compare(detail.textFormat, Text.PlainText);
            for (var line of data.lines)
                verify(detail.text.indexOf(line) >= 0, "missing: " + line + " in " + detail.text);
            if (data.lacks)
                verify(detail.text.indexOf(data.lacks) < 0);
            verify(card.Accessible.name.indexOf(data.title) === 0);
            compare(card.border.color, data.problem ? Ui.Theme.alertBorder : Ui.Theme.panelBorder);
            compare(buttons(card), data.actions);
            // At the top of Home: only the screen title sits above it.
            tryVerify(function () {
                return card.parent.children.filter(function (item) {
                    return item !== card && item.visible && item.height > 0 && item.y < card.y;
                }).length === 1;
            }, 2000, "only the title sits above the card");
            var dismiss = findChild(card, "backupResultDismiss");
            verify(dismiss.height >= Ui.Theme.minimumTouchSize);
            dismiss.activate();
            compare(calls("acknowledgeStartupOutcome").length, 1);
            compare(calls("discardDeferredRestore").length, 0);
            tryCompare(card, "visible", false);
        }

        function test_deferred_discard_data() {
            return [
                {tag: "discarded", reply: {ok: true, discarded: true, error: ""}, stays: false},
                {tag: "refused", reply: {ok: false, discarded: false, error: "Io"}, stays: true, text: "Couldn't discard the restore. DSD-neo tries it again the next time it opens."},
                {tag: "part-way through", reply: {ok: false, discarded: false, error: "RestoreInProgress"}, stays: true, text: "This restore is part-way through and can't be cancelled. Close and reopen DSD-neo to finish it."}
            ];
        }

        function test_deferred_discard(data) {
            app.currentTab = 0;
            var card = findChild(findChild(app, "homeScreen"), "backupResultCard");
            verify(testContext.setBackup("startupOutcome", {kind: "deferred", message: "Not enough free storage to restore. Free up storage and reopen DSD-neo"}));
            verify(testContext.setBackup("restorePending", true));
            tryCompare(card, "visible", true);
            testContext.setBackupReply("discardDeferredRestore", data.reply);
            findChild(card, "backupResultDiscard").activate();
            compare(calls("discardDeferredRestore").length, 1);
            compare(calls("acknowledgeStartupOutcome").length, 0);
            if (!data.stays) {
                tryCompare(card, "visible", false);
                return;
            }
            verify(card.visible);
            var error = findChild(card, "backupResultError");
            compare(error.text, data.text);
            compare(error.color, Ui.Theme.alert);
            compare(error.textFormat, Text.PlainText);
        }

        function test_result_card_announces_once() {
            app.currentTab = 0;
            var card = findChild(findChild(app, "homeScreen"), "backupResultCard");
            verify(testContext.setBackup("startupOutcome", {kind: "stuck", message: "Restore couldn't finish. Free up storage and reopen DSD-neo"}));
            tryCompare(card, "visible", true);
            tryCompare(card, "announcements", 1);
            compare(card.announced, "Restore couldn't finish. Free up storage and reopen DSD-neo.");
            app.currentTab = 2;
            tryCompare(card, "visible", false);
            app.currentTab = 0;
            tryCompare(card, "visible", true);
            wait(50);
            compare(card.announcements, 1, "showing the tab again repeats nothing");
        }
    }
}
