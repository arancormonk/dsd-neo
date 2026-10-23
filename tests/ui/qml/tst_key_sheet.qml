// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    width: 420
    height: 900
    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/KeySheet.qml"
    }
    Loader {
        id: monitorLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/MonitorScreen.qml"
    }
    SignalSpy {
        id: profileSavedSpy
        target: loader.item
        signalName: "systemProfileSaved"
    }
    TestCase {
        name: "KeySheet"
        when: windowShown
        readonly property var sheet: loader.item
        function init() {
            verify(sheet !== null, "KeySheet must load");
            testContext.setHostRunning(true);
            testContext.resetCommands();
            sheet.open();
        }
        function cleanup() {
            monitorLoader.active = false;
            if (sheet) {
                sheet.visible = false;
                if (sheet.systemUid !== undefined) {
                    sheet.systemUid = "";
                    sheet.systemName = "";
                }
                sheet.protocol = "mixed";
            }
            profileSavedSpy.clear();
            testContext.setMetric("optionsKnown", false);
            testContext.setMetric("directKeys", false);
            testContext.setMetric("automaticKeys", false);
            testContext.setMetric("keyProfileRef", "");
            testContext.setHostRunning(false);
        }
        // TextInput alone may hold the secret. Check Text descendants even when hidden.
        function checkText(item, secret) {
            if (item instanceof Text)
                verify(item.text.indexOf(secret) < 0, "A Text element exposed key material");
            for (var i = 0; i < item.children.length; ++i)
                checkText(item.children[i], secret);
        }
        function test_mask_no_text_and_apply() {
            var editor = findChild(sheet, "sessionEncryptionEditor");
            editor.keyType = "basic";
            editor.keyValue = String(17 * 3);
            editor.forceMode = 2;
            var field = findChild(editor, "encryptionKeyField");
            compare(field.input.echoMode, TextInput.Password);
            checkText(sheet, editor.keyValue);
            findChild(editor, "encryptionKeyEye").clicked();
            compare(field.input.echoMode, TextInput.Normal);
            checkText(sheet, editor.keyValue);
            sheet.apply();
            compare(commands.keyApplyCalls(), 1);
            verify(commands.keyPayloadValid());
            compare(commands.lastForceMode(), 2);
            verify(sheet.visible, "submission stays Pending until the decoder result");
            uiController.finishDecryption(commands.lastDecryptionRequest(), 1);
            verify(!sheet.visible);
            verify(editor.keyValue.length === 0);
            sheet.open();
            compare(field.input.echoMode, TextInput.Password);
        }
        function test_monitor_opens_sheet_and_blocks_underlying_controls() {
            sheet.visible = false;
            monitorLoader.active = true;
            tryCompare(monitorLoader, "status", Loader.Ready);
            var monitor = monitorLoader.item;
            var button = findChild(monitor, "openKeySheetButton");
            verify(button !== null);
            verify(button.enabled);
            button.activate();
            var modal = findChild(monitor, "keySheet");
            verify(modal.visible);
            verify(!findChild(monitor, "recentCallsList").enabled);
            monitor.visible = false;
            verify(!modal.visible);
        }
        function test_monitor_effective_decryption_status_data() {
            return [
                {tag: "unknown", known: false, direct: false, automatic: false, text: "Not yet known"},
                {tag: "empty", known: true, direct: false, automatic: false, text: "No keys"},
                {tag: "direct", known: true, direct: true, automatic: false, text: "Direct override"},
                {tag: "automatic", known: true, direct: false, automatic: true, text: "Automatic keys"},
                {tag: "unresolved-profile", known: true, direct: false, automatic: false, ref: "unknown-profile", text: "Active profile"},
                {tag: "session-profile", known: true, direct: true, automatic: false, profile: "Session keys", text: "Session keys"},
                {tag: "target-profile", known: true, direct: false, automatic: true, profile: "Target keys", text: "Target keys"}
            ];
        }
        function test_monitor_effective_decryption_status(data) {
            sheet.visible = false;
            var uid = "";
            var ref = data.ref || "";
            if (data.profile) {
                var result = decryptionProfiles.saveProfile({label: data.profile, protocol: "dmr", mode: "direct", directType: "basic", directValue: "51"});
                verify(result.ok, JSON.stringify(result));
                uid = result.uid;
                ref = decryptionProfiles.get(uid).runtimeRef;
            }
            testContext.setMetric("optionsKnown", data.known);
            testContext.setMetric("directKeys", data.direct);
            testContext.setMetric("automaticKeys", data.automatic);
            testContext.setMetric("keyProfileRef", ref);
            monitorLoader.active = true;
            tryCompare(monitorLoader, "status", Loader.Ready);
            var row = findChild(monitorLoader.item, "openKeySheetButton");
            var label = findChild(row, "decryptionStatusText");
            verify(label !== null);
            compare(label.text, "Decryption · " + data.text + " ›");
            compare(row.Accessible.name, "Decryption · " + data.text);
            row.forceActiveFocus();
            keyClick(Qt.Key_Return);
            var modal = findChild(monitorLoader.item, "keySheet");
            verify(modal.visible);
            if (uid.length) compare(modal.materialProfile.uid, uid);
            testContext.setHostRunning(false);
            verify(!modal.visible);
            verify(!row.enabled);
            compare(label.text, "Decryption · Not yet known ›");
            if (uid.length) verify(decryptionProfiles.removeProfile(uid).ok);
        }

        function test_cancel_and_force_only() {
            var editor = findChild(sheet, "sessionEncryptionEditor");
            editor.keyType = "basic";
            editor.keyValue = String(17 * 3);
            sheet.visible = false;
            verify(editor.keyValue.length === 0);
            compare(commands.keyApplyCalls(), 0);
            sheet.open();
            editor.forceMode = 1;
            sheet.apply();
            compare(commands.keyApplyCalls(), 0);
            compare(commands.lastForceMode(), 1);
            verify(sheet.visible);
            uiController.finishDecryption(commands.lastDecryptionRequest(), 1);
            verify(!sheet.visible);
        }
        function tapBeside(item) {
            var x = 4, y = item.height / 2;
            verify(!item.hitsPanel(x, y), "the side margin lies outside the panel");
            mouseClick(item, x, y);
        }
        function shownDiscard(item) {
            if (!item || !item.visible)
                return null;
            if (typeof item.askWith === "function")
                return item;
            for (var child of item.children) {
                var found = shownDiscard(child);
                if (found)
                    return found;
            }
            return null;
        }
        // Beside test_cancel_and_force_only: Close, Back or Escape with an
        // unapplied change asks first; discarding applies nothing and resets the
        // draft. A tap beside the panel is not a way out (issue #718).
        function test_close_with_a_change_asks_data() {
            return [
                {tag: "key by Close", change: "key", method: "close"},
                {tag: "key by Escape", change: "key", method: "escape"},
                {tag: "key by Back", change: "key", method: "back"},
                {tag: "identifier policy by Escape", change: "force", method: "escape"},
                {tag: "profile and key by Back", change: "profile+key", method: "back"}
            ];
        }
        function test_close_with_a_change_asks(data) {
            var editor = findChild(sheet, "sessionEncryptionEditor");
            waitForRendering(sheet);
            var dismiss = function () {
                if (data.method === "close")
                    findChild(sheet, "keySheetClose").activate();
                else if (data.method === "back")
                    verify(Ui.Navigation.back(sheet.Window.window));
                else {
                    sheet.forceActiveFocus();
                    keyClick(Qt.Key_Escape);
                }
            };
            dismiss();
            verify(!sheet.visible, "an untouched sheet closes without asking");
            verify(!shownDiscard(loader.item));
            sheet.open();
            waitForRendering(sheet);
            if (data.change === "key") {
                editor.keyType = "basic";
                editor.keyValue = String(17 * 3);
            } else if (data.change === "force") {
                editor.forceChanged = true;
            } else {
                sheet.selectedProfileUid = "another-profile";
                editor.keyType = "basic";
                editor.keyValue = String(17 * 3);
            }
            dismiss();
            verify(sheet.visible, "an unapplied change survives the dismissal");
            var discard = shownDiscard(sheet);
            verify(discard !== null);
            // A typed draft is unsaved work, whatever profile is chosen.
            compare(findChild(discard, "discardDialogQuestion").text, "Discard unsaved changes?");
            findChild(discard, "discardDialogKeep").activate();
            verify(sheet.visible);
            if (data.change !== "force")
                compare(editor.keyValue, String(17 * 3));
            dismiss();
            findChild(shownDiscard(sheet), "discardDialogDiscard").activate();
            verify(!sheet.visible);
            verify(editor.keyValue.length === 0);
            compare(commands.keyApplyCalls(), 0);
            compare(commands.lastForceMode(), -1);
        }
        // A profile chosen (or saved by "Manage matching key") is saved already:
        // closing gives up only applying it, and the question says so. "No
        // profile" leaves nothing to apply, and a profile in flight asks nothing.
        function test_unapplied_profile_asks_about_applying_data() {
            return [
                {tag: "picked, Close", how: "pick", method: "close", scope: 0},
                {tag: "picked, Back", how: "pick", method: "back", scope: 0},
                {tag: "saved by the matching editor", how: "saved", method: "close", scope: 0},
                {tag: "apply refused", how: "failed", method: "close", scope: 0},
                {tag: "target scope", how: "scope", method: "close", scope: 1}
            ];
        }
        function test_unapplied_profile_asks_about_applying(data) {
            var uid = decryptionProfiles.saveProfile({label: "Fire keys", protocol: "dmr", mode: "direct", directType: "basic", directValue: "51"}).uid;
            verify(uid.length > 0);
            try {
                waitForRendering(sheet);
                if (data.how === "saved") {
                    // The matching editor reports what it saved through saved().
                    var matching = named(sheet, "Decryption profile");
                    verify(matching !== null);
                    matching.saved(uid);
                    compare(sheet.selectedProfileUid, uid);
                } else if (data.how === "failed") {
                    sheet.selectedProfileUid = uid;
                    var apply = visualText(sheet, "Apply profile to session defaults");
                    verify(apply !== null);
                    apply.activate();
                    uiController.finishDecryption(commands.lastDecryptionRequest(), -2);
                    verify(sheet.visible && sheet.notice.length > 0, "a refused apply leaves the sheet up");
                } else if (data.how === "scope") {
                    sheet.selectedProfileUid = uid;
                    sheet.profileScope = 1;
                } else {
                    sheet.selectedProfileUid = uid;
                }
                if (data.method === "close")
                    findChild(sheet, "keySheetClose").activate();
                else
                    verify(Ui.Navigation.back(sheet.Window.window));
                verify(sheet.visible, "an unapplied profile is asked about");
                var discard = shownDiscard(sheet);
                verify(discard !== null);
                compare(findChild(discard, "discardDialogQuestion").text, data.scope === 1 ? "Close without applying the profile to this target?" : "Close without applying the profile to session defaults?");
                compare(findChild(discard, "discardDialogDiscard").text, "Close without applying");
                compare(discard.accessibleName, findChild(discard, "discardDialogQuestion").text);
                findChild(discard, "discardDialogKeep").activate();
                verify(sheet.visible);
                compare(sheet.selectedProfileUid, uid);
                findChild(sheet, "keySheetClose").activate();
                findChild(shownDiscard(sheet), "discardDialogDiscard").activate();
                verify(!sheet.visible);
                compare(commands.keyApplyCalls(), 0);
                // The next question is the plain one again.
                sheet.open();
                editor().keyType = "basic";
                editor().keyValue = String(17 * 3);
                findChild(sheet, "keySheetClose").activate();
                compare(findChild(shownDiscard(sheet), "discardDialogQuestion").text, "Discard unsaved changes?");
                findChild(shownDiscard(sheet), "discardDialogDiscard").activate();
                // "No profile" leaves nothing to apply.
                sheet.open();
                sheet.selectedProfileUid = uid;
                sheet.selectedProfileUid = "";
                findChild(sheet, "keySheetClose").activate();
                verify(!sheet.visible);
            } finally {
                verify(decryptionProfiles.removeProfile(uid).ok);
            }
        }
        // Issue #718: a form sheet. A tap beside the panel keeps it, with or
        // without a change, and asks nothing.
        function test_outside_tap_keeps_the_sheet() {
            waitForRendering(sheet);
            tapBeside(sheet);
            wait(50);
            verify(sheet.visible, "an untouched sheet stays open");
            editor().keyType = "basic";
            editor().keyValue = String(17 * 3);
            tapBeside(sheet);
            wait(50);
            verify(sheet.visible, "a typed draft stays open");
            verify(!shownDiscard(sheet), "and nothing is asked");
            compare(editor().keyValue, String(17 * 3));
            findChild(sheet, "keySheetClose").activate();
            findChild(shownDiscard(sheet), "discardDialogDiscard").activate();
            verify(!sheet.visible);
        }
        function named(item, name) {
            if (!item)
                return null;
            if (item.accessibleName === name)
                return item;
            for (var child of item.children) {
                var found = named(child, name);
                if (found)
                    return found;
            }
            return null;
        }
        function visualText(item, text) {
            if (!item)
                return null;
            if (item.text === text && typeof item.activate === "function")
                return item;
            for (var child of item.children) {
                var found = visualText(child, text);
                if (found)
                    return found;
            }
            return null;
        }
        function editor() {
            return findChild(sheet, "sessionEncryptionEditor");
        }
        function hexDigits(count) {
            var digits = "";
            for (var i = 0; i < count; ++i)
                digits += "0123456789ABCDEF"[(i * 5 + 1) % 16];
            return digits;
        }
        function addSystem(name, profileUid) {
            verify(savedSystems.add({name: name, sourceType: "usb", freqMhz: "851.5", decodeFlag: "-f1", decryptionProfileUid: profileUid || ""}));
            return savedSystems.get(savedSystems.count - 1).uid;
        }
        function removeSystem(uid) {
            var row = savedSystems.rowForUid(uid);
            if (row >= 0)
                savedSystems.remove(row);
        }
        function directProfile(label) {
            var result = decryptionProfiles.saveProfile({label: label, protocol: "p25", mode: "direct", directType: "hex", directValue: hexDigits(64)});
            verify(result.ok, JSON.stringify(result));
            return result.uid;
        }
        // The override's own note says it lasts for this session only.
        function test_direct_override_note_says_session_only() {
            var note = findChild(sheet, "directOverrideNote");
            verify(note !== null);
            verify(note.text.indexOf("for this session only") >= 0, note.text);
            verify(note.text.indexOf("cleared when you stop") >= 0, note.text);
            verify(note.text.indexOf("Direct changes affect session defaults") < 0, "rewritten, not repeated");
        }
        // "Use for <system> on future starts" writes the profile to the saved
        // system found by UID at the tap, even after rows moved under it.
        function test_use_for_system_updates_by_uid() {
            var profileUid = directProfile("Ops AES");
            var before = addSystem("Before");
            var system = addSystem("County P25");
            try {
                sheet.visible = false;
                sheet.protocol = "p25";
                sheet.systemUid = system;
                sheet.systemName = "County P25";
                sheet.open();
                var use = findChild(sheet, "useProfileForSystem");
                verify(!use.visible, "nothing to use until a profile is chosen");
                sheet.selectedProfileUid = profileUid;
                verify(use.visible);
                verify(use.enabled);
                compare(use.text, "Use for County P25 on future starts");
                // The row the session started from is not the row now.
                removeSystem(before);
                use.activate();
                compare(savedSystems.getByUid(system).decryptionProfileUid, profileUid);
                compare(profileSavedSpy.count, 1);
                compare(profileSavedSpy.signalArguments[0][0], system);
                verify(!use.enabled, "the system already uses it");
                compare(use.text, "Used for County P25 on future starts");
                verify(sheet.notice.indexOf("“County P25” will start with “Ops AES”.") === 0, sheet.notice);
                compare(commands.keyApplyCalls(), 0, "the running session is not changed");
                compare(decryptionProfiles.get(profileUid).useCount, 1);
            } finally {
                removeSystem(before);
                removeSystem(system);
                verify(decryptionProfiles.removeProfile(profileUid).ok);
            }
        }
        // A system that already names another profile is asked about first.
        function test_use_for_system_asks_before_replacing() {
            var old = directProfile("Old keys");
            var chosen = directProfile("New keys");
            var system = addSystem("Metro", old);
            try {
                sheet.visible = false;
                sheet.protocol = "p25";
                sheet.systemUid = system;
                sheet.open();
                sheet.selectedProfileUid = chosen;
                var use = findChild(sheet, "useProfileForSystem");
                use.activate();
                var ask = findChild(sheet, "replaceSystemProfile");
                verify(ask.visible);
                compare(ask.title, "Replace the profile for “Metro”?");
                verify(ask.message.indexOf("“New keys” instead of “Old keys”") >= 0, ask.message);
                compare(savedSystems.getByUid(system).decryptionProfileUid, old, "nothing changes before the answer");
                findChild(ask, "confirmCancelButton").activate();
                verify(!ask.visible);
                compare(savedSystems.getByUid(system).decryptionProfileUid, old);
                compare(profileSavedSpy.count, 0);
                use.activate();
                findChild(ask, "confirmActionButton").activate();
                compare(savedSystems.getByUid(system).decryptionProfileUid, chosen);
                compare(profileSavedSpy.count, 1);
                // The question goes with the sheet.
                sheet.selectedProfileUid = old;
                use.activate();
                verify(ask.visible);
                sheet.visible = false;
                sheet.open();
                verify(!ask.visible, "an open question does not come back");
            } finally {
                removeSystem(system);
                verify(decryptionProfiles.removeProfile(old).ok);
                verify(decryptionProfiles.removeProfile(chosen).ok);
            }
        }
        // A system removed since the session started is not recreated or guessed.
        function test_use_for_removed_system_refuses() {
            var profileUid = directProfile("Orphan keys");
            var system = addSystem("Gone");
            var count = savedSystems.count;
            try {
                sheet.visible = false;
                sheet.systemUid = system;
                sheet.systemName = "Gone";
                sheet.open();
                sheet.selectedProfileUid = profileUid;
                removeSystem(system);
                var use = findChild(sheet, "useProfileForSystem");
                use.activate();
                compare(sheet.notice, "“Gone” is no longer a saved system. Nothing was changed.");
                compare(savedSystems.count, count - 1);
                compare(profileSavedSpy.count, 0);
                compare(decryptionProfiles.get(profileUid).useCount, 0);
            } finally {
                removeSystem(system);
                verify(decryptionProfiles.removeProfile(profileUid).ok);
            }
        }
        // A scan list, Explore or a wizard test passes no system UID.
        function test_use_for_system_hidden_without_a_system() {
            var profileUid = directProfile("Any keys");
            try {
                sheet.selectedProfileUid = profileUid;
                verify(!findChild(sheet, "useProfileForSystem").visible);
                sheet.visible = false;
                monitorLoader.active = true;
                tryCompare(monitorLoader, "status", Loader.Ready);
                var monitor = monitorLoader.item;
                var monitorSheet = findChild(monitor, "keySheet");
                compare(monitorSheet.systemUid, "");
                monitor.sessionSystemUid = "standalone-uid";
                compare(monitorSheet.systemUid, "standalone-uid", "Monitor hands the sheet the running system");
            } finally {
                verify(decryptionProfiles.removeProfile(profileUid).ok);
            }
        }
        // The session override saved as a profile: the editor opens in Direct
        // mode with the typed key, and the saved profile is then the selected one.
        function test_save_override_as_profile() {
            sheet.visible = false;
            sheet.protocol = "p25";
            sheet.open();
            var key = hexDigits(64);
            var draft = editor();
            draft.keyType = "hex";
            var save = findChild(sheet, "saveDirectAsProfile");
            verify(save.visible);
            draft.keyValue = key.substring(1);
            verify(!save.inputValid, "an invalid key cannot be saved");
            draft.keyValue = key;
            verify(save.inputValid);
            save.activate();
            var profileEditor = findChild(sheet, "keySheetProfileEditor");
            verify(profileEditor.visible);
            compare(profileEditor.draft.protocol, "p25");
            compare(profileEditor.draft.mode, "direct");
            compare(profileEditor.draft.directType, "hex");
            compare(findChild(profileEditor, "profileDirectMaterial").text, key);
            checkText(sheet, key);
            findChild(profileEditor, "profileName").text = "Saved override";
            findChild(profileEditor, "profileSave").activate();
            verify(!profileEditor.visible);
            var uid = sheet.selectedProfileUid;
            verify(uid.length > 0, "the saved profile is selected");
            try {
                var saved = decryptionProfiles.get(uid);
                compare(saved.label, "Saved override");
                compare(saved.mode, "direct");
                compare(saved.directType, "hex");
                verify(saved.keyConfigured);
                compare(draft.keyValue, "", "the draft moved into the profile");
                compare(commands.keyApplyCalls(), 0, "saving applies nothing");
            } finally {
                verify(decryptionProfiles.removeProfile(uid).ok);
            }
        }
        // The live check is the start path's own: grouped hex reads as the
        // decoder takes it, with the protocol's AES widths.
        function test_live_key_check_matches_the_saved_check() {
            var draft = editor();
            sheet.protocol = "nxdn";
            draft.keyType = "hex";
            draft.keyValue = hexDigits(64).replace(/(.{4})(?!$)/g, "$1-");
            verify(draft.valid, "a grouped NXDN AES-256 key: " + draft.errorText);
            draft.keyValue = hexDigits(32);
            compare(draft.errorText, "NXDN AES needs 64 hex digits.");
            sheet.protocol = "p25";
            draft.keyValue = "12 34-56:78 90";
            compare(draft.errorText, "P25 AES needs 32 or 64 hex digits.");
            draft.keyValue = "";
            draft.keyType = "";
        }
        function test_unchanged_apply_preserves_force_and_keyloader() {
            sheet.apply();
            compare(commands.keyApplyCalls(), 0);
            compare(commands.lastForceMode(), -1);
            verify(!sheet.visible);
        }
        function test_stale_result_remains_visible() {
            var editor = findChild(sheet, "sessionEncryptionEditor");
            editor.forceMode = 1;
            sheet.apply();
            uiController.finishDecryption(commands.lastDecryptionRequest(), -2);
            verify(sheet.visible && sheet.notice.length > 0);
        }
        function test_invalid_stopped_and_rejected() {
            var editor = findChild(sheet, "sessionEncryptionEditor");
            editor.keyType = "hex";
            editor.keyValue = "invalid";
            sheet.apply();
            compare(commands.keyApplyCalls(), 0);
            editor.keyType = "basic";
            editor.keyValue = String(17 * 3);
            commands.setKeyAccepted(false);
            sheet.apply();
            verify(sheet.visible);
            verify(sheet.notice.length > 0);
            checkText(sheet, editor.keyValue);
            compare(commands.lastForceMode(), -1);
            testContext.setHostRunning(false);
            verify(!sheet.visible);
            verify(editor.keyValue.length === 0);
            sheet.apply();
            compare(commands.keyApplyCalls(), 1);
        }
    }
}
