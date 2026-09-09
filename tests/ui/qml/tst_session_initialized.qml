// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900
    Loader {
        id: appLoader
        source: uiDir + "/Main.qml"
    }
    TestCase {
        name: "SessionInitializedBookkeeping"
        when: windowShown
        function init() {
            testContext.useLifecycleHost(true);
            while (savedSystems.count > 0)
                savedSystems.remove(0);
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            savedSystems.add({
                name: "First",
                sourceType: "rtltcp",
                host: "127.0.0.1",
                port: 1234,
                freqMhz: "851.5"
            });
            savedSystems.add({
                name: "Second",
                sourceType: "rtltcp",
                host: "127.0.0.1",
                port: 1234,
                freqMhz: "852.5"
            });
        }
        function cleanup() {
            testContext.useLifecycleHost(false);
            while (savedSystems.count > 0)
                savedSystems.remove(0);
        }
        function test_invalid_saved_system_error_sentences_data() {
            return [
                {tag: "frequency", fields: {freqMhz: "invalid"},
                 sentence: "“First” has no valid frequency — long-press its card to edit it."},
                {tag: "ppm", fields: {ppm: "invalid"},
                 sentence: "“First” has an invalid PPM correction — long-press its card to edit it."},
                {tag: "basic", fields: {encKeyType: "basic", encKeyValue: "invalid"}},
                {tag: "hex", fields: {encKeyType: "hex", encKeyValue: "invalid"}},
                {tag: "rc4", fields: {encKeyType: "rc4", encKeyValue: "invalid"}},
                {tag: "scrambler", fields: {encKeyType: "scrambler", encKeyValue: "invalid"}},
                {tag: "key-type", fields: {encKeyType: "unknown", encKeyValue: "invalid"}},
                {tag: "key-conflict", fields: {encKeyType: "basic", encKeyValue: "0", keyCsvPath: "keys.csv"}},
                {tag: "force-mode", fields: {encForceKey: 3}}
            ];
        }
        function test_invalid_saved_system_error_sentences(data) {
            savedSystems.update(0, data.fields);
            var uid = savedSystems.get(0).uid;
            appLoader.item.startSystem(0);
            compare(appLoader.item.startError, data.sentence || "“First” has an invalid encryption key — long-press its card to edit it.");
            compare(decoderHost.running, false);
            compare(savedSystems.getByUid(uid).lastHeard, 0);
            compare(prefs.lastStartedUid, "");
        }
        function test_only_initialized_session_updates_recency() {
            var uid = savedSystems.get(1).uid;
            appLoader.item.startSystem(1);
            compare(savedSystems.getByUid(uid).lastHeard, 0);
            compare(prefs.lastStartedKind, "");
            // The engine can still be initializing while the list is edited.
            savedSystems.remove(0);
            testContext.emitSessionInitialized();
            verify(savedSystems.getByUid(uid).lastHeard > 0);
            compare(prefs.lastStartedKind, "saved");
            compare(prefs.lastStartedUid, uid);
        }
        function test_failed_start_does_not_update_recency() {
            appLoader.item.startSystem(0);
            decoderHost.stop();
            compare(savedSystems.get(0).lastHeard, 0);
            compare(prefs.lastStartedUid, "");
        }
    }
}
