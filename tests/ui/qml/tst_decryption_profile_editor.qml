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
