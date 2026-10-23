// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The decryption profile editor and its key and talkgroup-override dialogs keep
// what was typed through Cancel, Back or Escape: each asks before dropping a
// change, and closes at once when nothing changed. They are form sheets, so a
// tap beside the panel keeps them open (issue #718).
Item {
    width: 420
    height: 900
    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/DecryptionProfileEditor.qml"
    }
    TestCase {
        name: "DecryptionProfileEditor"
        when: windowShown
        readonly property var sheet: loader.item
        property var created: []

        function cleanup() {
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            if (sheet)
                sheet.visible = false;
            for (var uid of created)
                decryptionProfiles.removeProfile(uid);
            created = [];
        }
        function profile(value) {
            var result = decryptionProfiles.saveProfile(value);
            verify(result.ok, JSON.stringify(result));
            created = created.concat([result.uid]);
            return result.uid;
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
        function tapBeside(item) {
            var x = 4, y = item.height / 2;
            verify(!item.hitsPanel(x, y), "the side margin lies outside the panel");
            mouseClick(item, x, y);
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
        function dismiss(item, method) {
            if (method === "back")
                verify(Ui.Navigation.back(item.Window.window));
            else if (method === "escape")
                keyClick(Qt.Key_Escape);
            else
                findChild(item, item === sheet ? "profileCancel" : item.accessibleName === "Key details" ? "profileKeyCancel" : "profileMapCancel").activate();
        }
        function expectAsk(dialog, method) {
            dismiss(dialog, method);
            verify(dialog.visible, "the change survives the dismissal");
            var discard = shownDiscard(dialog);
            verify(discard !== null, "the dialog asks before discarding");
            findChild(discard, "discardDialogKeep").activate();
            verify(dialog.visible);
            tryVerify(function () { return Ui.Navigation.contains(dialog, dialog.Window.window.activeFocusItem); }, 1000, "focus returns into the dialog");
            dismiss(dialog, method);
            findChild(shownDiscard(dialog), "discardDialogDiscard").activate();
            tryCompare(dialog, "visible", false);
        }

        function test_key_dialog_asks_before_dropping_a_typed_key_data() {
            return [{tag: "cancel", method: "cancel"}, {tag: "back", method: "back"}, {tag: "escape", method: "escape"}, {tag: "material only", method: "cancel", material: true}];
        }
        function test_key_dialog_asks_before_dropping_a_typed_key(data) {
            sheet.open("", "p25");
            var dialog = named(sheet, "Key details");
            verify(dialog !== null);
            dialog.openFor(-1);
            waitForRendering(dialog);
            dismiss(dialog, data.method);
            tryCompare(dialog, "visible", false);
            verify(!shownDiscard(sheet), "an untouched key closes without asking");
            verify(sheet.visible, "the profile stays open under it");
            dialog.openFor(-1);
            waitForRendering(dialog);
            if (data.material)
                findChild(dialog, "profileKeyMaterial").text = "0123456789";
            else
                findChild(dialog, "profileKeyName").text = "Fire";
            expectAsk(dialog, data.method);
            compare(sheet.keyRows.length, 0, "a discarded key never reaches the draft");
            verify(sheet.visible);
            verify(!sheet.dirty);
        }
        // Opened for a received key ID, the dialog fills itself in: that is not an edit.
        function test_matching_key_prefill_is_not_an_edit() {
            var uid = profile({label: "Matching", protocol: "p25", mode: "automatic", keySource: "managed"});
            sheet.openMatching(uid, "p25", 0x12, "scalar");
            var dialog = named(sheet, "Key details");
            tryCompare(dialog, "visible", true);
            compare(findChild(dialog, "profileKeyId").text, "12");
            waitForRendering(dialog);
            dismiss(dialog, "cancel");
            tryCompare(dialog, "visible", false);
            verify(!shownDiscard(sheet));
            sheet.openMatching(uid, "p25", 0x12, "scalar");
            tryCompare(dialog, "visible", true);
            waitForRendering(dialog);
            findChild(dialog, "profileKeyId").text = "13";
            expectAsk(dialog, "cancel");
        }
        function test_mapping_dialog_asks_before_dropping_an_override() {
            var uid = profile({label: "Overrides", protocol: "dmr", mode: "automatic", keySource: "managed"});
            sheet.openTalkgroup(uid, "dmr", 100);
            var dialog = named(sheet, "Talkgroup key selection");
            tryCompare(dialog, "visible", true);
            compare(findChild(dialog, "profileMapTalkgroup").text, "100");
            waitForRendering(dialog);
            dismiss(dialog, "back");
            tryCompare(dialog, "visible", false);
            verify(!shownDiscard(sheet), "the talkgroup it opened for is not an edit");
            sheet.openTalkgroup(uid, "dmr", 100);
            tryCompare(dialog, "visible", true);
            waitForRendering(dialog);
            findChild(dialog, "profileMapKeyId").text = "2A";
            expectAsk(dialog, "back");
            compare(sheet.mapRows.length, 0);
            // Cancel asks too once the override was typed.
            dialog.openFor(-1);
            findChild(dialog, "profileMapTalkgroup").text = "200";
            findChild(dialog, "profileMapCancel").activate();
            verify(dialog.visible);
            findChild(shownDiscard(dialog), "discardDialogDiscard").activate();
            tryCompare(dialog, "visible", false);
        }
        // The fallback algorithm commits on editingFinished, which Back beats:
        // the editor reads what the field holds.
        function test_pending_fallback_algorithm_asks() {
            var uid = profile({label: "Fallback", protocol: "dmr", mode: "automatic", keySource: "managed", force: 33});
            sheet.open(uid, "dmr");
            var field = findChild(sheet, "profileFallbackAlgorithm");
            verify(field.visible);
            compare(field.text, "21");
            waitForRendering(sheet);
            dismiss(sheet, "back");
            tryCompare(sheet, "visible", false);
            verify(!shownDiscard(loader.item), "the saved algorithm is not an edit");
            sheet.open(uid, "dmr");
            waitForRendering(sheet);
            // Typed, not assigned: an assignment would break the field's binding
            // to the draft for every later case on this shared editor.
            field = findChild(sheet, "profileFallbackAlgorithm");
            field.input.forceActiveFocus();
            field.input.selectAll();
            keyClick("2");
            keyClick("A");
            compare(field.text, "2A");
            verify(!sheet.dirty, "the edit has not committed yet");
            expectAsk(sheet, "back");
            compare(decryptionProfiles.get(uid).force, 33, "a discarded edit is never saved");
            // The field still follows the draft: reopened, it reads the saved value.
            sheet.open(uid, "dmr");
            compare(findChild(sheet, "profileFallbackAlgorithm").text, "21");
            sheet.visible = false;
        }
        // A question left open goes with the sheet that asked it.
        function test_question_closes_with_its_sheet() {
            sheet.open("", "p25");
            var dialog = named(sheet, "Key details");
            dialog.openFor(-1);
            waitForRendering(dialog);
            findChild(dialog, "profileKeyName").text = "Fire";
            dismiss(dialog, "back");
            verify(shownDiscard(dialog) !== null);
            sheet.visible = false;
            verify(!Ui.Navigation.modals.length, "no modal outlives the editor");
            sheet.open("", "p25");
            dialog.openFor(-1);
            verify(dialog.visible);
            verify(!shownDiscard(sheet), "the old question does not come back");
            compare(Ui.Navigation.topModal, dialog);
        }
        // Issue #718: the editor and both dialogs are form sheets. A tap beside
        // the panel keeps them open and asks nothing, edited or not.
        function test_outside_tap_keeps_every_form() {
            var uid = profile({label: "Beside", protocol: "dmr", mode: "automatic", keySource: "managed"});
            sheet.open(uid, "dmr");
            waitForRendering(sheet);
            tapBeside(sheet);
            wait(50);
            verify(sheet.visible, "the profile editor stays open");
            var key = named(sheet, "Key details");
            key.openFor(-1);
            waitForRendering(key);
            findChild(key, "profileKeyName").text = "Fire";
            tapBeside(key);
            wait(50);
            verify(key.visible, "the key dialog stays open");
            verify(!shownDiscard(key), "and asks nothing");
            compare(findChild(key, "profileKeyName").text, "Fire");
            key.visible = false;
            var mapping = named(sheet, "Talkgroup key selection");
            mapping.openFor(-1);
            waitForRendering(mapping);
            tapBeside(mapping);
            wait(50);
            verify(mapping.visible, "the override dialog stays open");
            verify(!shownDiscard(mapping));
        }
        // The fallback algorithm reaches the draft on editingFinished. Save,
        // tapped while the field still has focus, ends its editing first, so the
        // value typed (or still composing on the keyboard) is the value saved.
        function test_save_keeps_a_focused_fallback_algorithm_data() {
            return [{tag: "typed", composing: false}, {tag: "composing", composing: true}];
        }
        function test_save_keeps_a_focused_fallback_algorithm(data) {
            var uid = profile({label: "Fallback save", protocol: "dmr", mode: "automatic", keySource: "managed", force: 33});
            sheet.open(uid, "dmr");
            waitForRendering(sheet);
            var field = findChild(sheet, "profileFallbackAlgorithm");
            field.input.forceActiveFocus();
            field.input.selectAll();
            if (data.composing) {
                keyClick(Qt.Key_Backspace);
                verify(testContext.composeText("2A"));
                compare(field.text, "", "the keyboard still holds the value");
            } else {
                keyClick("2");
                keyClick("A");
                compare(field.text, "2A");
            }
            verify(field.input.activeFocus);
            var save = findChild(sheet, "profileSave");
            save.activate();
            tryCompare(sheet, "visible", false);
            compare(decryptionProfiles.get(uid).force, 0x2A, "Save stores the algorithm in the field");
        }
        // A 64-digit AES-256 key, built so no literal key sits in the source.
        function hexDigits(count) {
            var digits = "";
            for (var i = 0; i < count; ++i)
                digits += "0123456789ABCDEF"[(i * 7 + 3) % 16];
            return digits;
        }
        function newKeyDialog(protocol) {
            sheet.open("", protocol || "p25");
            var dialog = named(sheet, "Key details");
            verify(dialog !== null);
            dialog.openFor(-1);
            waitForRendering(dialog);
            return dialog;
        }
        // The helper line under a field: guidance while typing, an error once left.
        function helper(field) {
            return field.error.length ? field.error : field.hint;
        }
        // The format is picked from the real list by mouse, as a person would.
        function chooseFormat(dialog, label) {
            var combo = findChild(dialog, "profileKeyKind");
            verify(combo !== null);
            var index = combo.find(label);
            verify(index >= 0, "the format list offers " + label + ": " + JSON.stringify(combo.model));
            mouseClick(combo, combo.width / 2, combo.height / 2);
            tryCompare(combo.popup, "opened", true);
            var list = combo.popup.contentItem;
            list.positionViewAtIndex(index, ListView.Contain);
            var option = null;
            tryVerify(function () {
                option = list.itemAtIndex(index);
                return option !== null && option.visible && option.height > 0;
            });
            mouseClick(option, option.width / 2, option.height / 2);
            tryCompare(combo.popup, "visible", false);
            compare(combo.displayText, label);
        }
        function useKey(dialog) {
            var use = findChild(dialog, "profileUseKey");
            mouseClick(use, use.width / 2, use.height / 2);
        }
        // Each format reads with its width, and the one picked from the popup is
        // the one the draft keeps and the key list shows.
        function test_format_chosen_through_the_popup() {
            var dialog = newKeyDialog("p25");
            chooseFormat(dialog, "AES-256 · 64 hex digits");
            compare(dialog.kindId, "aes256");
            findChild(dialog, "profileKeyName").text = "Dispatch";
            findChild(dialog, "profileKeyId").text = "1A";
            findChild(dialog, "profileKeyMaterial").text = hexDigits(64);
            verify(findChild(dialog, "profileUseKey").inputValid);
            useKey(dialog);
            tryCompare(dialog, "visible", false);
            compare(sheet.keyRows.length, 1);
            compare(sheet.keyRows[0].kind, "aes256");
            compare(sheet.keyRows[0].keyId, 0x1A);
            var row = named(sheet, "Dispatch");
            verify(row !== null);
            compare(row.subtitle, "ID 1A · AES-256 · 64 hex digits", "the list names the format, not its id");
        }
        // "Manage matching key" for an AES-256 call: the received ID and the
        // call's format, and the ID read back in decimal.
        function test_matching_prefill_with_aes256() {
            var uid = profile({label: "Matching AES", protocol: "p25", mode: "automatic", keySource: "managed"});
            sheet.openMatching(uid, "p25", 0x1A2B, "aes256");
            var dialog = named(sheet, "Key details");
            tryCompare(dialog, "visible", true);
            waitForRendering(dialog);
            var keyId = findChild(dialog, "profileKeyId");
            compare(keyId.text, "1A2B");
            compare(keyId.hint, "= 6699 decimal");
            compare(dialog.kindId, "aes256");
            compare(findChild(dialog, "profileKeyKind").displayText, "AES-256 · 64 hex digits");
            verify(!findChild(dialog, "profileUseKey").inputValid, "a new key still needs its material");
            dismiss(dialog, "cancel");
            tryCompare(dialog, "visible", false);
            verify(!shownDiscard(sheet), "the prefill is not an edit");
        }
        // Key material still composing on the keyboard is taken by "Use this key".
        function test_use_this_key_takes_composing_material() {
            var dialog = newKeyDialog("p25");
            chooseFormat(dialog, "AES-256 · 64 hex digits");
            findChild(dialog, "profileKeyId").text = "1";
            var material = findChild(dialog, "profileKeyMaterial");
            material.input.forceActiveFocus();
            verify(testContext.composeText(hexDigits(64)));
            compare(material.text, "", "the keyboard still holds the key");
            var use = findChild(dialog, "profileUseKey");
            verify(!use.inputValid);
            verify(use.enabled, "a composing word keeps the button live");
            useKey(dialog);
            tryCompare(dialog, "visible", false);
            compare(sheet.keyRows.length, 1);
            compare(sheet.keyRows[0].material, hexDigits(64));
            compare(sheet.keyRows[0].kind, "aes256");
        }
        // One digit short keeps the key out and says how many were entered.
        function test_aes256_width_is_counted() {
            var dialog = newKeyDialog("p25");
            chooseFormat(dialog, "AES-256 · 64 hex digits");
            findChild(dialog, "profileKeyId").text = "1";
            var material = findChild(dialog, "profileKeyMaterial");
            var use = findChild(dialog, "profileUseKey");
            material.text = hexDigits(63);
            verify(!use.inputValid);
            verify(!use.enabled);
            compare(helper(material), "AES-256 needs 64 hex digits; 63 entered.");
            compare(material.error, helper(material), "shown as an error while the field is not being typed in");
            material.text = hexDigits(64);
            verify(use.inputValid);
            compare(material.error, "");
            compare(material.hint, "Fits AES-256 · 64 hex digits.", "a masked key says when it fits");
            // Grouped as a key sheet prints it: the same 64 digits.
            material.text = hexDigits(64).replace(/(.{8})/g, "$1 ").trim();
            verify(use.inputValid, "spaced groups are the same key");
        }
        // 64 digits on the default format names the format that takes them.
        function test_scalar_with_64_digits_names_aes256() {
            var dialog = newKeyDialog("p25");
            compare(dialog.kindId, "scalar");
            compare(findChild(dialog, "profileKeyKind").displayText, "DES / RC4 / ADP · up to 16 hex digits");
            findChild(dialog, "profileKeyId").text = "1";
            var material = findChild(dialog, "profileKeyMaterial");
            material.text = hexDigits(64);
            compare(helper(material), "64 hex digits is an AES-256 key. Choose AES-256.");
            verify(!findChild(dialog, "profileUseKey").inputValid);
        }
        // Two AES keys whose cells overlap: the second is refused at the ID.
        function test_aes_collision_is_named_at_the_key_id() {
            var dialog = newKeyDialog("p25");
            chooseFormat(dialog, "AES-256 · 64 hex digits");
            findChild(dialog, "profileKeyName").text = "First";
            findChild(dialog, "profileKeyId").text = "1";
            findChild(dialog, "profileKeyMaterial").text = hexDigits(64);
            useKey(dialog);
            tryCompare(dialog, "visible", false);
            dialog.openFor(-1);
            waitForRendering(dialog);
            chooseFormat(dialog, "AES-256 · 64 hex digits");
            var keyId = findChild(dialog, "profileKeyId");
            keyId.text = "102";
            findChild(dialog, "profileKeyMaterial").text = hexDigits(64);
            verify(!findChild(dialog, "profileUseKey").inputValid);
            verify(helper(keyId).indexOf("Uses the same keyring cells as key \"First\" (ID 1).") === 0, helper(keyId));
            compare(findChild(dialog, "profileKeyMaterial").error, "", "the material itself is fine");
            keyId.text = "400";
            verify(findChild(dialog, "profileUseKey").inputValid);
            compare(keyId.hint, "= 1024 decimal");
        }
        // Show reveals the key in the field only; the dialog reopens masked.
        function test_reveal_toggle_changes_echo_mode() {
            var dialog = newKeyDialog("p25");
            var material = findChild(dialog, "profileKeyMaterial");
            verify(material.mono, "key material is set in the mono face");
            compare(material.input.echoMode, TextInput.Password);
            var reveal = findChild(dialog, "profileKeyReveal");
            mouseClick(reveal, reveal.width / 2, reveal.height / 2);
            compare(material.input.echoMode, TextInput.Normal);
            compare(reveal.text, "Hide");
            dialog.visible = false;
            dialog.openFor(-1);
            compare(material.input.echoMode, TextInput.Password);
        }
        // A new key starts with no ID; the received key ID is typed.
        function test_new_key_has_no_prefilled_id() {
            var dialog = newKeyDialog("dmr");
            var keyId = findChild(dialog, "profileKeyId");
            compare(keyId.text, "");
            compare(helper(keyId), "", "an untouched field shows no error");
            keyId.text = "FFF";
            keyId.input.forceActiveFocus();
            findChild(dialog, "profileKeyName").input.forceActiveFocus();
            compare(keyId.error, "DMR key IDs are hex 0 to FF.");
        }
        // Saved, reopened and edited: blank material keeps the key under its
        // format, and a changed format asks for new material.
        function test_save_reopen_edit() {
            sheet.open("", "p25");
            findChild(sheet, "profileName").text = "Reopened";
            var dialog = named(sheet, "Key details");
            dialog.openFor(-1);
            waitForRendering(dialog);
            chooseFormat(dialog, "AES-256 · 64 hex digits");
            findChild(dialog, "profileKeyName").text = "Ops";
            findChild(dialog, "profileKeyId").text = "1A";
            findChild(dialog, "profileKeyMaterial").text = hexDigits(64);
            useKey(dialog);
            tryCompare(dialog, "visible", false);
            var uid = sheet.draft.uid;
            findChild(sheet, "profileSave").activate();
            tryCompare(sheet, "visible", false);
            created = created.concat([uid]);
            sheet.open(uid, "p25");
            compare(sheet.keyRows.length, 1);
            verify(sheet.keyRows[0].material === undefined, "material never comes back to the dialog");
            dialog.openFor(0);
            waitForRendering(dialog);
            compare(dialog.kindId, "aes256");
            compare(findChild(dialog, "profileKeyId").text, "1A");
            var material = findChild(dialog, "profileKeyMaterial");
            compare(material.text, "");
            var use = findChild(dialog, "profileUseKey");
            verify(use.inputValid, "blank keeps the stored key");
            chooseFormat(dialog, "AES-128 · 32 hex digits");
            verify(!use.inputValid);
            compare(helper(material), "Enter replacement material when changing the key format.");
            chooseFormat(dialog, "AES-256 · 64 hex digits");
            findChild(dialog, "profileKeyName").text = "Ops 2";
            verify(use.inputValid);
            useKey(dialog);
            tryCompare(dialog, "visible", false);
            findChild(sheet, "profileSave").activate();
            tryCompare(sheet, "visible", false);
            var saved = decryptionProfiles.get(uid);
            compare(saved.keys[0].label, "Ops 2");
            compare(saved.keys[0].kind, "aes256");
        }
        // A reason the sheet cannot do what it opened for sits below the form;
        // it is scrolled into view rather than left below the fold.
        function test_error_is_scrolled_into_view() {
            var uid = profile({label: "Direct only", protocol: "p25", mode: "direct", directType: "hex", directValue: hexDigits(64)});
            loader.anchors.fill = undefined;
            loader.height = 360;
            try {
                sheet.openMatching(uid, "p25", 0x12, "aes256");
                var label = findChild(sheet, "profileError");
                verify(label.visible);
                var scroll = findChild(sheet, "modalSheetScroll");
                verify(scroll.contentHeight > scroll.height, "the form is taller than the panel");
                tryVerify(function () {
                    var at = label.mapToItem(scroll, 0, 0).y;
                    return at >= 0 && at + label.height <= scroll.height + 1;
                }, 1000, "the error is on screen");
            } finally {
                sheet.visible = false;
                loader.anchors.fill = loader.parent;
            }
        }
        // "Use this key" is gated on the key ID's committed text. A key ID still
        // composing on the keyboard passes when it is valid and is refused when
        // it is not; either way the word is kept.
        function test_use_this_key_takes_a_composing_key_id_data() {
            return [{tag: "valid", word: "AB", accepted: true}, {tag: "invalid", word: "XZ", accepted: false}];
        }
        function test_use_this_key_takes_a_composing_key_id(data) {
            sheet.open("", "p25");
            var dialog = named(sheet, "Key details");
            dialog.openFor(-1);
            waitForRendering(dialog);
            // Valid material, so the key ID alone decides.
            findChild(dialog, "profileKeyMaterial").text = "0123456789ABCDEF";
            var keyId = findChild(dialog, "profileKeyId");
            keyId.text = "";
            keyId.input.forceActiveFocus();
            verify(testContext.composeText(data.word));
            var use = findChild(dialog, "profileUseKey");
            verify(!use.inputValid, "the committed key ID is empty: the button looks unavailable");
            mouseClick(use, use.width / 2, use.height / 2);
            if (data.accepted) {
                tryCompare(dialog, "visible", false);
                compare(sheet.keyRows.length, 1);
                compare(sheet.keyRows[0].keyId, 0xAB);
            } else {
                verify(dialog.visible, "an invalid key ID is still refused");
                compare(sheet.keyRows.length, 0);
                compare(keyId.text, data.word, "the typed word is kept");
                verify(!use.enabled, "and the button is disabled again");
            }
        }
    }
}
