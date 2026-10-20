// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 411
    height: 700
    QtObject {
        id: fake
        property int scanRowEditSet: 1
        property int scanRowEditInherit: 2
        property int scanRowEditReset: 3
        property var row: ({})
        property var sent: null
        function receiveContext() {
            return {filters: [{enabled: true, allow: "1", block: "2"}], names: []};
        }
        function scanRowContext() { return row; }
        function receiveError(kind, enabled, allow, block, names) {
            if (allow.indexOf(",") >= 0)
                return "use / between identifiers";
            for (var i = 0; i < names.length; i++) {
                if (!names[i].name || !names[i].value)
                    return "expected identifier=name";
            }
            return "";
        }
        function setReceive(kind, enabled, allow, block, names, destination, action) {
            sent = {kind: kind, enabled: enabled, allow: allow, block: block, names: names,
                    destination: destination, action: action};
            return true;
        }
    }
    Ui.ReceivePolicySheet { id: sheet; anchors.fill: parent; bridge: fake }
    TestCase {
        name: "ReceivePolicySheet"
        when: windowShown
        function item(name) { return findChild(sheet, name); }
        function init() {
            sheet.visible = false;
            sheet.namesEditor = false;
            sheet.kind = 0;
            fake.sent = null;
            fake.row = {active: true, editable: 0x130, label: "Hospital", row: 4, session: 7,
                        receive: {filters: [{enabled: false, allow: "", block: ""}], names: []}};
        }
        function cleanup() {
            sheet.visible = false;
            var discard = findChild(sheet, "discardDialog");
            if (discard) discard.visible = false;
            Ui.Theme.resetFontScale();
            root.width = 411;
            root.height = 700;
        }
        function test_defaults_and_atomic_apply() {
            sheet.openEditor();
            compare(item("receiveAllow").text, "1");
            compare(item("receiveBlock").text, "2");
            item("receiveFilterEnabled").selected(0);
            item("receiveApply").activate();
            verify(!sheet.visible);
            compare(fake.sent.enabled, false);
            compare(fake.sent.allow, "1");
            compare(fake.sent.block, "2");
        }
        function test_inline_error_stays_open() {
            sheet.openEditor();
            item("receiveAllow").text = "1,2";
            verify(!item("receiveApply").enabled);
            verify(item("receiveValidation").text.length > 0);
            sheet.submit(1);
            verify(sheet.visible);
            compare(fake.sent, null);
        }
        function test_captured_row_and_inherit() {
            sheet.openEditor();
            fake.row = {active: true, editable: 0x130, row: 5, session: 8};
            sheet.chooseScope(true);
            compare(item("receiveAllow").text, "");
            sheet.submit(2);
            compare(fake.sent.destination.row, 4);
            compare(fake.sent.destination.session, 7);
            compare(fake.sent.action, 2);
        }
        function test_kind_picker_follows_sheet_kind() {
            sheet.openEditor();
            var picker = item("receiveFilterKind");
            picker.currentIndex = 3;
            picker.activated(3);
            compare(sheet.kind, 3);
            compare(picker.currentIndex, 3);
            // A later change of the sheet's kind (a fresh open) still reaches the picker.
            sheet.kind = 1;
            compare(picker.currentIndex, 1);
            // An activation the unsaved-edit guard holds keeps showing the kind in force.
            item("receiveAllow").text = "9";
            picker.currentIndex = 2;
            picker.activated(2);
            compare(sheet.kind, 1);
            compare(picker.currentIndex, 1);
            item("discardDialogKeep").activate();
        }
        function test_back_protects_unsaved_edits() {
            sheet.openEditor();
            item("receiveAllow").text = "2";
            sheet.requestDismiss();
            verify(sheet.visible);
            verify(item("discardDialogQuestion").visible);
            item("discardDialogKeep").activate();
            verify(sheet.visible);
            sheet.requestDismiss();
            item("discardDialogDiscard").clicked();
            verify(!sheet.visible);
            compare(fake.sent, null);
        }
        function test_names_empty_add_remove_and_errors() {
            sheet.namesEditor = true;
            sheet.openEditor();
            compare(sheet.namesValue().length, 0);
            item("receiveAddName").clicked();
            compare(sheet.namesValue().length, 1);
            verify(!item("receiveApply").enabled);
            sheet.changeRule(0, "value", "7");
            sheet.changeRule(0, "name", "Hospital Emergency Department");
            verify(item("receiveApply").enabled);
            sheet.submit(1);
            compare(fake.sent.names[0].name, "Hospital Emergency Department");
        }
        function test_large_text_phone_and_tablet_data() {
            return [{tag: "phone", width: 360, height: 640}, {tag: "tablet", width: 1024, height: 700}];
        }
        function test_large_text_phone_and_tablet(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = 1.3;
            sheet.openEditor();
            waitForRendering(sheet);
            var apply = item("receiveApply");
            verify(apply.height >= Ui.Theme.minimumTouchSize);
            verify(apply.width <= root.width);
            verify(item("receiveAllow").width <= root.width);
            sheet.requestDismiss();
            verify(!sheet.visible);
        }
    }
}
