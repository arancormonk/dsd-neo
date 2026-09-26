// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    id: testRoot
    width: 420
    height: 900
    property var app: null
    SignalSpy {
        id: recencyWrites
        target: savedSystems
        signalName: "dataChanged"
    }
    SignalSpy {
        id: kindWrites
        target: prefs
        signalName: "lastStartedKindChanged"
    }
    SignalSpy {
        id: uidWrites
        target: prefs
        signalName: "lastStartedUidChanged"
    }
    TestCase {
        name: "SessionInitializedBookkeeping"
        when: windowShown
        function initTestCase() {
            testContext.useLifecycleHost(true, true);
            var mainQml = testContext.androidMainQml();
            verify(mainQml.length > 0,
                   "androidMainQml() requires readable Main.qml with exactly one Qt.platform.os occurrence");
            app = Qt.createQmlObject(mainQml, testRoot, uiDir + "/Main.qml");
            verify(app !== null, "Failed to create the Android Main.qml test fixture");
        }
        function init() {
            testContext.useLifecycleHost(true, true);
            testContext.setNotificationPermissionNeeded(false);
            prefs.backgroundListening = true;
            prefs.notificationExplained = false;
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
            findChild(app, "notificationExplanation").visible = false;
            prefs.notificationExplained = true;
            prefs.backgroundListening = true;
            testContext.setNotificationPermissionNeeded(false);
            testContext.useLifecycleHost(false);
            while (savedSystems.count > 0)
                savedSystems.remove(0);
        }
        function test_notification_explanation_gate_data() {
            return [
                { tag: "permission-needed", needed: true, explained: false, background: true, shown: true },
                { tag: "permission-granted-or-pre-33", needed: false, explained: false, background: true, shown: false },
                { tag: "already-explained", needed: true, explained: true, background: true, shown: false },
                { tag: "background-off", needed: true, explained: false, background: false, shown: false }
            ];
        }
        function test_notification_explanation_gate(data) {
            testContext.setNotificationPermissionNeeded(data.needed);
            compare(decoderHost.notificationPermissionNeeded, data.needed);
            prefs.notificationExplained = data.explained;
            prefs.backgroundListening = data.background;
            app.startSystem(0);
            var explanation = findChild(app, "notificationExplanation");
            verify(explanation !== null);
            verify(!explanation.visible, "wait for successful initialization");
            testContext.emitSessionInitialized();
            compare(explanation.visible, data.shown);
            compare(prefs.notificationExplained, data.explained, "checking permission must not mark it explained");
            verifyRecencyWrites(1);
        }
        function test_permission_granted_before_later_session() {
            testContext.setNotificationPermissionNeeded(true);
            app.startSystem(0);
            testContext.emitSessionInitialized();
            var explanation = findChild(app, "notificationExplanation");
            verify(explanation.visible);
            explanation.visible = false;
            decoderHost.stop();
            testContext.setNotificationPermissionNeeded(false);
            compare(decoderHost.notificationPermissionNeeded, false);
            // Leave the explanation preference false to exercise the permission
            // refresh independently of the sheet's once-explained behaviour.
            compare(prefs.notificationExplained, false);
            app.startSystem(0);
            testContext.emitSessionInitialized();
            verify(!explanation.visible);
            compare(prefs.notificationExplained, false);
        }
        function test_invalid_saved_system_error_sentences_data() {
            return [
                {
                    tag: "frequency",
                    fields: {
                        freqMhz: "invalid"
                    },
                    sentence: "“First” has no valid frequency. Edit the source to correct it."
                },
                {
                    tag: "ppm",
                    fields: {
                        ppm: "invalid"
                    },
                    sentence: "“First” has an invalid PPM correction. Edit the source to correct it."
                },
                {
                    tag: "basic",
                    fields: {
                        encKeyType: "basic",
                        encKeyValue: "invalid"
                    }
                },
                {
                    tag: "hex",
                    fields: {
                        encKeyType: "hex",
                        encKeyValue: "invalid"
                    }
                },
                {
                    tag: "rc4",
                    fields: {
                        encKeyType: "rc4",
                        encKeyValue: "invalid"
                    }
                },
                {
                    tag: "scrambler",
                    fields: {
                        encKeyType: "scrambler",
                        encKeyValue: "invalid"
                    }
                },
                {
                    tag: "key-type",
                    fields: {
                        encKeyType: "unknown",
                        encKeyValue: "invalid"
                    }
                },
                {
                    tag: "key-conflict",
                    fields: {
                        encKeyType: "basic",
                        encKeyValue: "0",
                        keyCsvPath: "keys.csv"
                    }
                },
                {
                    tag: "force-mode",
                    fields: {
                        encForceKey: 3
                    }
                },
                {
                    tag: "am-needs-radio",
                    fields: {
                        sourceType: "tcp",
                        decodeFlag: "-fM"
                    },
                    sentence: "“First” decodes AM, which needs a radio source: network and file audio arrives already demodulated. Edit the source to correct it."
                },
                {
                    tag: "am-bandwidth",
                    fields: {
                        decodeFlag: "-fM",
                        bandwidthKhz: 6
                    },
                    sentence: "“First” decodes AM, whose 6 kHz channel needs a DSP bandwidth of 8 kHz or more. Set the source's bandwidth, or the default bandwidth in Settings, to 8, 12, 16, 24 or 48 kHz."
                }
            ];
        }
        function test_invalid_saved_system_error_sentences(data) {
            savedSystems.update(0, data.fields);
            var uid = savedSystems.get(0).uid;
            app.startSystem(0);
            compare(app.startError, data.sentence || "“First” has an invalid decryption configuration. Edit the source to correct it.");
            compare(decoderHost.running, false);
            compare(savedSystems.getByUid(uid).lastHeard, 0);
            compare(prefs.lastStartedUid, "");
        }
        // Issue #524: an AM system whose own bandwidth is unset runs at the app-wide
        // default, which a change in Settings can move below what AM filters.
        function test_am_bandwidth_from_the_app_default() {
            var saved = prefs.bandwidthKhz;
            prefs.bandwidthKhz = 6;
            savedSystems.update(0, {
                decodeFlag: "-fM"
            });
            app.startSystem(0);
            prefs.bandwidthKhz = saved;
            compare(app.startError, "“First” decodes AM, whose 6 kHz channel needs a DSP bandwidth of 8 kHz or more. Set the source's bandwidth, or the default bandwidth in Settings, to 8, 12, 16, 24 or 48 kHz.");
            compare(decoderHost.running, false);
        }
        function test_only_initialized_session_updates_recency() {
            var uid = savedSystems.get(1).uid;
            app.startSystem(1);
            compare(savedSystems.getByUid(uid).lastHeard, 0);
            compare(prefs.lastStartedKind, "");
            // The engine can still be initializing while the list is edited.
            savedSystems.remove(0);
            testContext.emitSessionInitialized();
            verify(savedSystems.getByUid(uid).lastHeard > 0);
            compare(prefs.lastStartedKind, "saved");
            compare(prefs.lastStartedUid, uid);
        }
        function test_prohibited_extra_option_message_data() {
            return [
                {
                    tag: "key display",
                    token: "--show-keys"
                },
                {
                    tag: "grouped input",
                    token: "-Fiother-input"
                },
                {
                    tag: "grouped trunking",
                    token: "-FTZ"
                }
            ];
        }
        function test_prohibited_extra_option_message(data) {
            var sys = savedSystems.get(0);
            sys.extraArgs = data.token;
            app.startWithMap(sys, 0);
            verify(app.startError.indexOf("prohibited extra option") >= 0);
            verify(app.startError.indexOf("each short option separately") >= 0);
            verify(app.startError.indexOf(sys.extraArgs) < 0);
            verify(app.startError.indexOf("PPM") < 0);
            compare(savedSystems.get(0).lastHeard, 0);
        }
        function test_late_initialization_after_grace_failure() {
            app.startSystem(0);
            verify(decoderHost.running);
            testContext.setLifecyclePhase(4);
            compare(savedSystems.get(0).lastHeard, 0);
            testContext.setLifecyclePhase(2);
            verifyRecencyWrites(0);
            testContext.emitSessionInitialized();
            verifyRecencyWrites(1);
        }
        function runningAtStartCases() {
            return [
                {
                    tag: "running-edge",
                    runningAtStart: false
                },
                {
                    tag: "running-on-return",
                    runningAtStart: true
                }
            ];
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
            app.startSystem(0);
            verify(decoderHost.running);
            verify(app.sessionSystem !== null);
            if (!data.runningAtStart)
                testContext.setLifecyclePhase(2);
        }
        function test_signaling_host_running_before_failed_initialization_data() {
            return runningAtStartCases();
        }
        function test_signaling_host_running_before_failed_initialization(data) {
            startRunningHost(true, data);
            verifyRecencyWrites(0);
            testContext.setLifecyclePhase(4);
            verifyRecencyWrites(0);
            decoderHost.stop();
            verifyRecencyWrites(0);
        }
        function test_signaling_host_waits_for_successful_initialization_data() {
            return runningAtStartCases();
        }
        function test_signaling_host_waits_for_successful_initialization(data) {
            startRunningHost(true, data);
            verifyRecencyWrites(0);
            testContext.emitSessionInitialized();
            verifyRecencyWrites(1);
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            verifyRecencyWrites(1);
        }
        function test_running_host_without_initialization_signal_data() {
            return runningAtStartCases();
        }
        function test_running_host_without_initialization_signal(data) {
            startRunningHost(false, data);
            verifyRecencyWrites(1);
            testContext.setLifecyclePhase(2);
            verifyRecencyWrites(1);
        }
        function test_failed_start_does_not_update_recency() {
            app.startSystem(0);
            verify(decoderHost.running);
            decoderHost.stop();
            compare(savedSystems.get(0).lastHeard, 0);
            compare(prefs.lastStartedUid, "");
        }
    }
}
