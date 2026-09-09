// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    id: mainRoot
    property bool transitioning: false
    property bool showFailure: false
    width: 420
    height: 900
    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/WizardScreen.qml"
    }
    Loader {
        id: homeLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/HomeScreen.qml"
    }
    TestCase {
        name: "WizardEncryption"
        when: windowShown
        property int savedRow: -1
        readonly property var wizard: loader.item
        function init() {
            verify(wizard !== null);
            wizard.openForAdd(false);
            wizard.nameText = "Encryption test";
            wizard.freqText = "851.375";
            wizard.step = 1;
        }
        function cleanup() {
            homeLoader.active = false;
            if (savedRow >= 0)
                savedSystems.remove(savedRow);
            savedRow = -1;
        }
        // The production builder retrieves the secret by UID through keyValueForUid.
        // Compare only a boolean so a failure never prints the secret argv.
        function verifySavedKey(uid, expected) {
            var sys = savedSystems.getByUid(uid);
            verify(!("encKeyValue" in sys));
            var result = sessionArgs.build(sys);
            verify(result.ok);
            var keyIndex = result.args.indexOf("-b");
            verify(keyIndex >= 0);
            verify(result.args[keyIndex + 1] === expected, "Private saved key round trip");
        }
        function test_roundtrip_and_reset() {
            wizard.encKeyType = "basic";
            wizard.encKeyValue = String(17 * 3);
            wizard.encForceKey = 1;
            verify(wizard.encryptionValid);
            wizard.commit();
            savedRow = savedSystems.count - 1;
            var sys = savedSystems.get(savedRow);
            compare(sys.encKeyType, "basic");
            verify(!("encKeyValue" in sys));
            verify(!("encKeyValue" in savedSystems.getByUid(sys.uid)));
            verify(sys.encKeyConfigured);
            compare(sys.encForceKey, 1);
            wizard.openForAdd(false);
            verify(wizard.encKeyValue.length === 0);
            compare(wizard.encForceKey, 0);
            wizard.openForEdit(savedRow);
            wizard.step = 1;
            verify(wizard.encKeyValue.length === 0);
            var editor = findChild(wizard, "wizardEncryptionEditor");
            compare(editor.keyAction, "keep");
            verify(findChild(editor, "configuredKeyLabel").visible);
            verify(!findChild(editor, "encryptionKeyField").visible);
            verify(wizard.encryptionValid);
            findChild(editor, "encryptionAction_replace").clicked();
            wizard.encKeyType = "rc4";
            wizard.encKeyValue = "AB";
            findChild(editor, "encryptionAction_keep").clicked();
            compare(wizard.encKeyType, "basic");
            verify(wizard.encKeyValue.length === 0);
            wizard.nameText = "Renamed keyed system";
            wizard.commit();
            verifySavedKey(sys.uid, String(17 * 3));
            wizard.openForEdit(savedRow);
            wizard.step = 1;
            wizard.assignCsvPath("keys", "/imports/keys.csv", true);
            verify(!wizard.encryptionValid);
            wizard.assignCsvPath("keys", "", false);
            findChild(editor, "encryptionAction_replace").clicked();
            verify(!wizard.encryptionValid);
            compare(findChild(editor, "encryptionKeyField").input.echoMode, TextInput.Password);
            wizard.encKeyValue = "73";
            verify(wizard.encryptionValid);
            wizard.commit();
            verifySavedKey(sys.uid, "73");
            wizard.openForEdit(savedRow);
            wizard.step = 1;
            compare(editor.keyAction, "keep");
            findChild(editor, "encryptionAction_clear").clicked();
            verify(wizard.encryptionValid);
            wizard.commit();
            var cleared = savedSystems.getByUid(sys.uid);
            compare(cleared.encKeyType, "");
            verify(!cleared.encKeyConfigured);
            verify(!("encKeyValue" in cleared));
            var result = sessionArgs.build(cleared);
            verify(result.ok);
            verify(result.args.indexOf("-b") < 0);
            wizard.openForFound(null, "851.375");
            verify(wizard.encKeyValue.length === 0);
            compare(wizard.encKeyType, "");
        }
        function test_invalid_and_csv_conflict_refuse_commit() {
            var count = savedSystems.count;
            wizard.encKeyType = "hex";
            wizard.encKeyValue = "invalid";
            verify(!wizard.encryptionValid);
            verify(!wizard.stepValid());
            wizard.commit();
            compare(savedSystems.count, count);
            wizard.encKeyType = "basic";
            wizard.encKeyValue = String(17 * 3);
            wizard.assignCsvPath("keys", "/imports/keys.csv", true);
            verify(!wizard.encryptionValid);
            wizard.step = 2;
            verify(!wizard.stepValid());
            wizard.commit();
            compare(savedSystems.count, count);
            wizard.assignCsvPath("keys", "", false);
            verify(wizard.encryptionValid);
        }
        function test_shapes_data() {
            return [
                {
                    tag: "basic-min",
                    type: "basic",
                    value: "0",
                    ok: true
                },
                {
                    tag: "basic-max",
                    type: "basic",
                    value: "255",
                    ok: true
                },
                {
                    tag: "basic-over",
                    type: "basic",
                    value: "256",
                    ok: false
                },
                {
                    tag: "basic-negative",
                    type: "basic",
                    value: "-1",
                    ok: false
                },
                {
                    tag: "basic-empty",
                    type: "basic",
                    value: "",
                    ok: false
                },
                {
                    tag: "hex-10",
                    type: "hex",
                    value: "a".repeat(10),
                    ok: true
                },
                {
                    tag: "hex-32",
                    type: "hex",
                    value: "a".repeat(32),
                    ok: true
                },
                {
                    tag: "hex-64",
                    type: "hex",
                    value: "a".repeat(64),
                    ok: true
                },
                {
                    tag: "hex-normalized",
                    type: "hex",
                    value: " 0x" + "ab ".repeat(5),
                    ok: true
                },
                {
                    tag: "hex-short",
                    type: "hex",
                    value: "a".repeat(9),
                    ok: false
                },
                {
                    tag: "rc4-min",
                    type: "rc4",
                    value: "a",
                    ok: true
                },
                {
                    tag: "rc4-max",
                    type: "rc4",
                    value: "a".repeat(16),
                    ok: true
                },
                {
                    tag: "rc4-over",
                    type: "rc4",
                    value: "a".repeat(17),
                    ok: false
                },
                {
                    tag: "scrambler-min",
                    type: "scrambler",
                    value: "0",
                    ok: true
                },
                {
                    tag: "scrambler-max",
                    type: "scrambler",
                    value: "32767",
                    ok: true
                },
                {
                    tag: "scrambler-over",
                    type: "scrambler",
                    value: "32768",
                    ok: false
                },
                {
                    tag: "none",
                    type: "",
                    value: "",
                    ok: true
                },
                {
                    tag: "unknown",
                    type: "unknown",
                    value: "",
                    ok: false
                }
            ];
        }
        function test_shapes(data) {
            wizard.encKeyType = data.type;
            wizard.encKeyValue = data.value;
            compare(wizard.encryptionValid, data.ok);
        }
        function test_home_saved_marker() {
            wizard.encKeyType = "basic";
            wizard.encKeyValue = String(17 * 3);
            wizard.commit();
            savedRow = savedSystems.count - 1;
            homeLoader.active = true;
            tryCompare(homeLoader, "status", Loader.Ready);
            var meta = findChild(homeLoader.item, "savedSystemMeta");
            verify(meta !== null);
            verify(meta.text.indexOf("key configured") === 0);
            savedSystems.update(savedRow, {
                encKeyType: "",
                encKeyValue: ""
            });
            verify(meta.text.indexOf("key configured") < 0);
            savedSystems.update(savedRow, {
                keyCsvPath: "/imports/keys.csv"
            });
            verify(meta.text.indexOf("key configured") === 0);
        }
        function test_mask_and_eye() {
            var editor = findChild(wizard, "wizardEncryptionEditor");
            verify(editor !== null);
            editor.keyType = "rc4";
            var field = findChild(editor, "encryptionKeyField");
            verify(field !== null);
            compare(field.input.echoMode, TextInput.Password);
            findChild(editor, "encryptionKeyEye").clicked();
            compare(field.input.echoMode, TextInput.Normal);
            wizard.openForAdd(false);
            compare(field.input.echoMode, TextInput.Password);
        }
    }
}
