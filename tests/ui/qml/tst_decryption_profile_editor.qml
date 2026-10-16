// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The decryption profile editor and its key and talkgroup-override dialogs keep
// what was typed through a tap beside the panel, Back or Escape: each asks
// before dropping a change, and closes at once when nothing changed.
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
                tapBeside(item);
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
            return [{tag: "scrim", method: "scrim"}, {tag: "back", method: "back"}, {tag: "escape", method: "escape"}, {tag: "material only", method: "scrim", material: true}];
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
            tapBeside(dialog);
            tryCompare(dialog, "visible", false);
            verify(!shownDiscard(sheet));
            sheet.openMatching(uid, "p25", 0x12, "scalar");
            tryCompare(dialog, "visible", true);
            waitForRendering(dialog);
            findChild(dialog, "profileKeyId").text = "13";
            expectAsk(dialog, "scrim");
        }
        function test_mapping_dialog_asks_before_dropping_an_override() {
            var uid = profile({label: "Overrides", protocol: "dmr", mode: "automatic", keySource: "managed"});
            sheet.openTalkgroup(uid, "dmr", 100);
            var dialog = named(sheet, "Talkgroup key selection");
            tryCompare(dialog, "visible", true);
            compare(findChild(dialog, "profileMapTalkgroup").text, "100");
            waitForRendering(dialog);
            tapBeside(dialog);
            tryCompare(dialog, "visible", false);
            verify(!shownDiscard(sheet), "the talkgroup it opened for is not an edit");
            sheet.openTalkgroup(uid, "dmr", 100);
            tryCompare(dialog, "visible", true);
            waitForRendering(dialog);
            findChild(dialog, "profileMapKeyId").text = "2A";
            expectAsk(dialog, "scrim");
            compare(sheet.mapRows.length, 0);
            // Cancel asks too once the override was typed.
            dialog.openFor(-1);
            findChild(dialog, "profileMapTalkgroup").text = "200";
            findChild(dialog, "profileMapCancel").activate();
            verify(dialog.visible);
            findChild(shownDiscard(dialog), "discardDialogDiscard").activate();
            tryCompare(dialog, "visible", false);
        }
        // The fallback algorithm commits on editingFinished, which a tap beside
        // the panel beats: the editor reads what the field holds.
        function test_pending_fallback_algorithm_asks() {
            var uid = profile({label: "Fallback", protocol: "dmr", mode: "automatic", keySource: "managed", force: 33});
            sheet.open(uid, "dmr");
            var field = findChild(sheet, "profileFallbackAlgorithm");
            verify(field.visible);
            compare(field.text, "21");
            waitForRendering(sheet);
            tapBeside(sheet);
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
            expectAsk(sheet, "scrim");
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
            tapBeside(dialog);
            verify(shownDiscard(dialog) !== null);
            sheet.visible = false;
            verify(!Ui.Navigation.modals.length, "no modal outlives the editor");
            sheet.open("", "p25");
            dialog.openFor(-1);
            verify(dialog.visible);
            verify(!shownDiscard(sheet), "the old question does not come back");
            compare(Ui.Navigation.topModal, dialog);
        }
    }
}
