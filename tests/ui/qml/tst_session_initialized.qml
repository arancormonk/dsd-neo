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
    SignalSpy { id: recencyWrites; target: savedSystems; signalName: "dataChanged" }
    SignalSpy { id: kindWrites; target: prefs; signalName: "lastStartedKindChanged" }
    SignalSpy { id: uidWrites; target: prefs; signalName: "lastStartedUidChanged" }
    TestCase {
        name: "SessionInitializedBookkeeping"
        when: windowShown
        function init() {
            testContext.useLifecycleHost(true, true);
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
            verify(recencyWrites.valid && kindWrites.valid && uidWrites.valid);
            recencyWrites.clear();
            kindWrites.clear();
            uidWrites.clear();
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
        function test_prohibited_extra_option_message() {
            var sys = savedSystems.get(0);
            sys.extraArgs = "--show-keys";
            appLoader.item.startWithMap(sys, 0);
            verify(appLoader.item.startError.indexOf("prohibited extra option") >= 0);
            verify(appLoader.item.startError.indexOf(sys.extraArgs) < 0);
            verify(appLoader.item.startError.indexOf("PPM") < 0);
            compare(savedSystems.get(0).lastHeard, 0);
        }
        function test_late_initialization_after_grace_failure() {
            appLoader.item.startSystem(0);
            verify(decoderHost.running);
            testContext.setLifecyclePhase(4);
            compare(savedSystems.get(0).lastHeard, 0);
            testContext.setLifecyclePhase(2);
            verifyRecencyWrites(0);
            testContext.emitSessionInitialized();
            verifyRecencyWrites(1);
        }
        function runningAtStartCases() {
            return [{tag: "running-edge", runningAtStart: false},
                    {tag: "running-on-return", runningAtStart: true}];
        }
        function verifyRecencyWrites(count) {
            compare(recencyWrites.count, count);
            compare(kindWrites.count, count);
            compare(uidWrites.count, count);
            if (count === 0) {
                compare(savedSystems.get(0).lastHeard, 0);
                compare(prefs.lastStartedKind, "");
                compare(prefs.lastStartedUid, "");
            } else {
                verify(savedSystems.get(0).lastHeard > 0);
                compare(prefs.lastStartedKind, "saved");
                compare(prefs.lastStartedUid, savedSystems.get(0).uid);
            }
        }
        function startRunningHost(signalsInitialized, data) {
            testContext.useLifecycleHost(true, signalsInitialized, data.runningAtStart);
            compare(decoderHost.signalsSessionInitialized, signalsInitialized);
            appLoader.item.startSystem(0);
            verify(decoderHost.running);
            verify(appLoader.item.sessionSystem !== null);
            if (!data.runningAtStart)
                testContext.setLifecyclePhase(2);
        }
        function test_signaling_host_running_before_failed_initialization_data() { return runningAtStartCases(); }
        function test_signaling_host_running_before_failed_initialization(data) {
            startRunningHost(true, data);
            verifyRecencyWrites(0);
            testContext.setLifecyclePhase(4);
            verifyRecencyWrites(0);
            decoderHost.stop();
            verifyRecencyWrites(0);
        }
        function test_signaling_host_waits_for_successful_initialization_data() { return runningAtStartCases(); }
        function test_signaling_host_waits_for_successful_initialization(data) {
            startRunningHost(true, data);
            verifyRecencyWrites(0);
            testContext.emitSessionInitialized();
            verifyRecencyWrites(1);
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            verifyRecencyWrites(1);
        }
        function test_running_host_without_initialization_signal_data() { return runningAtStartCases(); }
        function test_running_host_without_initialization_signal(data) {
            startRunningHost(false, data);
            verifyRecencyWrites(1);
            testContext.setLifecyclePhase(2);
            verifyRecencyWrites(1);
        }
        function test_failed_start_does_not_update_recency() {
            appLoader.item.startSystem(0);
            verify(decoderHost.running);
            decoderHost.stop();
            compare(savedSystems.get(0).lastHeard, 0);
            compare(prefs.lastStartedUid, "");
        }
    }
}
