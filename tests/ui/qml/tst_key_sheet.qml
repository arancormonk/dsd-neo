// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

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
            if (sheet)
                sheet.visible = false;
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
            button.clicked();
            var modal = findChild(monitor, "keySheet");
            verify(modal.visible);
            verify(!findChild(monitor, "monitorBody").enabled);
            monitor.visible = false;
            verify(!modal.visible);
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
