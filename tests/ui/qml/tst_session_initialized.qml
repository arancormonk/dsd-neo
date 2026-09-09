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
