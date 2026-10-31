// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #782: a reading that moves every tick is not read out every tick.
//
// Qt's Android bridge speaks every rename of the node TalkBack is focused on (QtAccessibilityDelegate's
// notifyDescriptionOrNameChanged sends an announcement for the focused node). The harness cannot focus a node the
// way TalkBack does, so these cases hold every node to the rule a focused one needs: a moving reading's name catches
// up at most once a period (SpeechPacer), and at once on a meaningful step, while the text on screen stays live.
//
// The app's period is 5 s (Navigation.speechPeriodMs, checked first). The updates run at a 100 ms period, 40 of
// them a few milliseconds apart, as 4 Hz for 10 s is 40 updates over two 5 s periods: a node may rename once a period
// (and once when the count starts), whatever the timer's jitter makes of the elapsed time, and far fewer times than
// it is updated. The steps run at a 2 s period with the step floor off, so a rename within 100 ms is the step's and
// not the period's, and a check that an update alone waits runs just after a step, which starts a period. A step
// that flaps is held to the floor (case F, at a 200 ms floor instead of the app's 1 s).
Item {
    id: root
    width: 420
    height: 900
    readonly property string pathA: "/rec/County (12345678)/A.wav"
    // The search model the screen reads, as tst_signal_search.qml fakes it.
    QtObject {
        id: searchFake
        property var ranges: [{name: "Local UHF", startMHz: "450.000000", endMHz: "460.000000", enabled: true,
            stepNum: 12500, stepDen: 1, bandwidth: 16000, am: false, digital: 1, gainSet: false, gainAuto: true,
            gain: 0, sensitivity: 15, minimumSet: false, minimum: -60, squelchMode: 2, squelchMargin: 10,
            squelchLevel: -60, probe: 2, visit: 30}]
        property var history: []
        property var avoids: []
        property var sessionAvoids: []
        property var status: ({phase: 0})
        property string error: ""
        function control(action, session, hit) { return true; }
        function avoidFrequency(session, frequency) { return true; }
        function avoidPermanently(session, frequency) { return true; }
        function removeSessionAvoid(frequency) { return true; }
        function removeAvoid(frequency) {}
        function arm() { return ""; }
        function saveRange(row, range) { return ""; }
        function defaultRange() { return Object.assign({}, ranges[0]); }
    }
    Loader {
        id: monitorLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/MonitorScreen.qml"
    }
    Loader {
        id: spectrumLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/SpectrumScreen.qml"
    }
    Loader {
        id: historyLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/HistoryDetailSheet.qml"
    }
    Loader {
        id: searchLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/SignalSearchScreen.qml"
        onLoaded: item.searchModel = searchFake
    }
    Loader {
        id: radioLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/RadioSheet.qml"
    }
    Loader {
        id: squelchLoader
        anchors.fill: parent
        active: false
        source: uiDir + "/SquelchSheet.qml"
    }
    TestCase {
        name: "A11yPacedNames"
        when: windowShown
        readonly property var monitor: monitorLoader.item
        readonly property var spectrum: spectrumLoader.item
        readonly property int period: 100
        readonly property int stepPeriod: 2000
        readonly property int updates: 40
        readonly property int tick: 5
        property int appPeriod: 0
        property var restore: ({})

        function item(scope, name) {
            var found = findChild(scope, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        // How often the item named @p name raised @p type during the count, by its own chain.
        function raised(counts, type, name) {
            var total = 0;
            for (var chain in counts.chains) {
                if (chain.indexOf(type + " " + name + " ") === 0 || chain === type + " " + name)
                    total += counts.chains[chain];
            }
            return total;
        }
        function typed(counts, type) {
            return counts.types[type] ? counts.types[type] : 0;
        }
        function nameOf(target) {
            return testContext.accessibleNode(target).name;
        }
        function descriptionOf(target) {
            return testContext.accessibleNode(target).description;
        }
        function set(values) {
            for (var key in values) {
                if (!(key in restore))
                    restore[key] = metrics[key];
                testContext.setMetric(key, values[key]);
            }
        }
        // Runs @p update(i) for i = 1..updates, a tick apart, at the short period; returns the milliseconds it took.
        function run(update) {
            Ui.Navigation.speechPeriodMs = period;
            var start = Date.now();
            for (var i = 1; i <= updates; ++i) {
                update(i);
                wait(tick);
            }
            return Date.now() - start;
        }
        // Each of @p names raised @p type at least once, at most once a period over @p elapsed ms and once more, and
        // far fewer times than it was updated.
        function verifyPaced(counts, type, names, elapsed) {
            var report = JSON.stringify(counts.chains);
            var most = Math.min(Math.ceil(elapsed / period) + 1, updates / 4);
            for (var named of names) {
                var renames = raised(counts, type, named);
                verify(renames >= 1 && renames <= most, named + " raised " + type + " " + renames + " times for "
                       + updates + " updates in " + elapsed + " ms (at most " + most + "): " + report);
            }
        }
        // The long period the step checks run at, each step heard on its own.
        function stepPace() {
            Ui.Navigation.speechPeriodMs = stepPeriod;
            Ui.Navigation.speechStepFloorMs = 1;
            wait(20);
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            appPeriod = Ui.Navigation.speechPeriodMs;
            compare(appPeriod, 5000, "the app paces names at 5 s");
            compare(Ui.Navigation.speechStepFloorMs, 1000, "and hears a flapping step once a second at most");
            Ui.Navigation.speechPeriodMs = period;
            for (var loader of [monitorLoader, spectrumLoader, historyLoader, searchLoader, radioLoader, squelchLoader])
                loader.active = true;
            verify(monitor !== null && spectrum !== null && historyLoader.item !== null && searchLoader.item !== null
                   && radioLoader.item !== null && squelchLoader.item !== null);
        }
        function cleanupTestCase() {
            for (var loader of [monitorLoader, spectrumLoader, historyLoader, searchLoader, radioLoader, squelchLoader])
                loader.active = false;
            Ui.Navigation.speechPeriodMs = appPeriod;
            Ui.Navigation.speechStepFloorMs = 1000;
        }
        function init() {
            testContext.setHostRunning(true);
            set({radioInput: true, carrierLock: true, syncedHere: true, syncLabel: "P25p1", snrValid: true, snrDb: 9,
                cfoHz: 0, centerFreqHz: 851012500, scanTargetCount: 3, scanTargetOrdinal: 1, scanTargetId: "target-1",
                scanRotationActive: true, scanTimingVisible: true, scanStayPhrase: "Dwell", scanTimerLive: true,
                scanTimerRemainingDs: 100, scanTimerSpanMs: 10000, leadSlot: 1, slot1CallState: 2,
                slot1CallName: "Dispatch", slot1TgId: 456, slot1TgText: "456", slot1CallSeconds: 1, qualityValid: true,
                ccFecValid: true, ccFecOkPct: 90});
            monitor.visible = true;
            spectrum.visible = true;
            // Each case shows only the screens it reads.
            searchLoader.item.visible = false;
            testContext.setPrefs("monitorDetailsExpanded", true);
            waitForRendering(root);
            wait(50);
        }
        function cleanup() {
            testContext.stopAccessibilityCount();
            Ui.Navigation.speechStepFloorMs = 1000;
            monitor.visible = true;
            testContext.setPrefs("monitorDetailsExpanded", false);
            historyLoader.item.visible = false;
            radioLoader.item.visible = false;
            squelchLoader.item.visible = false;
            searchFake.status = {phase: 0};
            searchLoader.item.sessionActive = false;
            Ui.Navigation.modals = [];
            for (var key in restore)
                testContext.setMetric(key, restore[key]);
            restore = {};
            testContext.resetHostPlayback();
            recordings.reset();
            callHistory.clearAll();
            testContext.setHostRunning(false);
        }

        // The monitor's and spectrum's moving readings: each node renames a few times at most and ends on the value
        // on screen. The text on screen follows every update.
        function test_a_monitor_and_spectrum() {
            var strip = item(monitor, "signalStrip");
            var timing = item(monitor, "scanTimingRow");
            var duration = item(monitor, "heroDuration");
            var summary = item(monitor, "monitorDetailsRow");
            var status = item(spectrum, "spectrumStatusLine");
            var snrText = item(monitor, "monitorSnrValue");
            testContext.startAccessibilityCount();
            var elapsed = run(function (i) {
                set({snrDb: 9 + i * 0.3, cfoHz: (i % 2 ? -1 : 1) * i * 11, scanTimerRemainingDs: 100 - i * 2,
                    slot1CallSeconds: 1 + Math.floor(i / 4)});
                compare(snrText.text, Number(9 + i * 0.3).toFixed(1) + " dB", "the strip's text follows every update");
            });
            var counts = testContext.accessibilityCount();
            verifyPaced(counts, "NameChanged", ["signalStrip", "scanTimingRow", "heroDuration", "spectrumStatusLine"],
                        elapsed);
            verifyPaced(counts, "DescriptionChanged", ["monitorDetailsRow"], elapsed);
            compare(typed(counts, "ObjectShow") + typed(counts, "ObjectHide") + typed(counts, "LocationChanged"), 0,
                    JSON.stringify(counts.chains));
            // A focused node is never left behind for longer than the period: each catches up with the screen.
            var snr = Number(9 + updates * 0.3).toFixed(1) + " dB";
            tryVerify(function () { return nameOf(strip).indexOf("SNR " + snr) >= 0; }, period + 500, nameOf(strip));
            tryVerify(function () { return nameOf(timing) === timing.text; }, period + 500, nameOf(timing));
            tryVerify(function () { return nameOf(duration) === duration.text; }, period + 500, nameOf(duration));
            tryVerify(function () { return descriptionOf(summary) === summary.accessibleDescription; }, period + 500,
                      descriptionOf(summary));
            tryVerify(function () { return nameOf(status).indexOf(snr) >= 0; }, period + 500, nameOf(status));
        }

        // A meaningful step renames at once, with every reading as it then is: a lost lock, a new scan target, a call
        // ending, a sync change on the spectrum.
        function test_b_monitor_steps_rename_at_once() {
            var strip = item(monitor, "signalStrip");
            var timing = item(monitor, "scanTimingRow");
            var status = item(spectrum, "spectrumStatusLine");
            stepPace();
            // A step starts a period; a reading moved just after it, without a step, waits for the next.
            set({scanTargetOrdinal: 1, scanTargetId: "target-1b"});
            wait(30);
            set({snrDb: 21.4});
            wait(50);
            verify(nameOf(strip).indexOf("SNR 21.4 dB") < 0, "an update alone renames at the readings' pace: "
                   + nameOf(strip));
            set({carrierLock: false});
            tryVerify(function () {
                return nameOf(strip).indexOf("No carrier lock") >= 0 && nameOf(strip).indexOf("SNR 21.4 dB") >= 0;
            }, 100, "a lost lock is said at once, with the SNR as it is: " + nameOf(strip));
            tryVerify(function () { return nameOf(status).indexOf("21.4") >= 0; }, 100, nameOf(status));
            set({scanTimerRemainingDs: 37});
            wait(50);
            verify(nameOf(timing) !== timing.text, "the countdown alone waits: " + nameOf(timing));
            // A scan's hop is not a step (case K); holding the scan is.
            set({scanHold: true});
            tryVerify(function () { return nameOf(timing) === timing.text; }, 100, "a held scan is said at once");
            set({syncLabel: "DMR"});
            tryVerify(function () { return nameOf(status).indexOf("DMR") >= 0; }, 100, nameOf(status));
            // The call ends: its duration leaves the tree with it.
            var duration = item(monitor, "heroDuration");
            set({slot1CallState: 0, leadSlot: 0});
            tryVerify(function () { return !duration.visible && duration.Accessible.name === ""; }, 100);
        }

        // A recording's position, while it plays: the time's name, the seek bar's description and value move at the
        // readings' pace; play, pause, a seek once it lands, and the end of the part are heard at once.
        function test_c_recording_position() {
            monitor.visible = false;
            spectrum.visible = false;
            var sheet = historyLoader.item;
            var uid = callHistory.pushWithRecordings([{id: "t-1", path: root.pathA, fileName: "A.wav", folderUid: "u1",
                start: 1700000000, durationMs: 60000, rate: 8000, bytes: 96000, failed: false, available: true}]);
            sheet.open({uid: uid, name: "Dispatch", tg: 1001, when: 1700000000, systemUid: "test-system"});
            waitForRendering(sheet);
            var time = item(sheet, "recordingTime");
            var bar = item(sheet, "recordingSeekBar");
            stepPace();
            testContext.publishPlayback(root.pathA, 2, 1000, 60000, "");
            tryVerify(function () { return nameOf(time) === "0:01 of 1:00"; }, 100, "playing is heard at once: "
                      + nameOf(time));
            testContext.startAccessibilityCount();
            var elapsed = run(function (i) {
                testContext.publishPlayback(root.pathA, 2, 1000 + i * 250, 60000, "");
            });
            var counts = testContext.accessibilityCount();
            compare(time.text, "0:11 / 1:00", "the time on screen follows every update");
            verifyPaced(counts, "NameChanged", ["recordingTime"], elapsed);
            verifyPaced(counts, "DescriptionChanged", ["recordingSeekBar"], elapsed);
            tryVerify(function () { return nameOf(time) === "0:11 of 1:00"; }, period + 500, nameOf(time));
            tryVerify(function () { return descriptionOf(bar) === "0:11 of 1:00"; }, period + 500, descriptionOf(bar));
            stepPace();
            // Pause: at once.
            testContext.publishPlayback(root.pathA, 2, 12000, 60000, "");
            testContext.publishPlayback(root.pathA, 3, 12400, 60000, "");
            tryVerify(function () { return nameOf(time) === "0:12 of 1:00"; }, 100, "a pause is heard at once: "
                      + nameOf(time));
            // A seek, once the host reports where it landed: at once.
            testContext.publishPlayback(root.pathA, 2, 13000, 60000, "");
            wait(50);
            bar.seekTo(40000);
            testContext.publishPlayback(root.pathA, 2, 40000, 60000, "");
            tryVerify(function () { return nameOf(time) === "0:40 of 1:00"; }, 100, "a seek is heard at once: "
                      + nameOf(time));
            // The end of the part: at once.
            testContext.publishPlayback(root.pathA, 2, 41000, 60000, "");
            wait(50);
            testContext.publishPlayback(root.pathA, 0, 60000, 60000, "");
            tryVerify(function () { return nameOf(time) === "1:00 of 1:00"; }, 100, "the end is heard at once: "
                      + nameOf(time));
        }

        // A seek the host never applies (it clamps or ignores it) is forgotten when playback stops: a later position
        // that happens to pass where the seek asked to land is not heard at once as if the seek had landed.
        function test_c2_an_unlanded_seek_is_forgotten() {
            monitor.visible = false;
            spectrum.visible = false;
            var sheet = historyLoader.item;
            var uid = callHistory.pushWithRecordings([{id: "t-1", path: root.pathA, fileName: "A.wav", folderUid: "u1",
                start: 1700000000, durationMs: 60000, rate: 8000, bytes: 96000, failed: false, available: true}]);
            sheet.open({uid: uid, name: "Dispatch", tg: 1001, when: 1700000000, systemUid: "test-system"});
            waitForRendering(sheet);
            var time = item(sheet, "recordingTime");
            var bar = item(sheet, "recordingSeekBar");
            stepPace();
            testContext.publishPlayback(root.pathA, 2, 13000, 60000, "");
            wait(50);
            // The host leaves the seek unapplied, and playback stops.
            testContext.setSeekApplied(false);
            bar.seekTo(50000);
            var calls = testContext.hostCalls();
            compare(calls[calls.length - 1].name, "seekPlayback", "the seek was asked for");
            testContext.setSeekApplied(true);
            testContext.publishPlayback(root.pathA, 2, 13250, 60000, "");
            testContext.publishPlayback(root.pathA, 0, 13500, 60000, "");
            wait(50);
            // Played again, from further on.
            testContext.publishPlayback(root.pathA, 2, 45000, 60000, "");
            tryVerify(function () { return nameOf(time) === "0:45 of 1:00"; }, 100, "playing is heard at once: "
                      + nameOf(time));
            testContext.startAccessibilityCount();
            testContext.publishPlayback(root.pathA, 2, 50000, 60000, "");
            wait(50);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(raised(counts, "NameChanged", "recordingTime"), 0, "playing past where the seek asked to land "
                    + "waits for the period: " + JSON.stringify(counts.chains));
            compare(nameOf(time), "0:45 of 1:00");
        }

        // Signal search, while it scans and while it receives: the channel count, the live level and a monitored
        // call's details move at the readings' pace; a new hit or a change of state is heard at once.
        function test_d_signal_search_lines() {
            monitor.visible = false;
            spectrum.visible = false;
            var screen = searchLoader.item;
            screen.visible = true;
            screen.sessionActive = true;
            var line = item(screen, "searchStatusLine");
            var strength = item(screen, "searchStrength");
            var detail = item(screen, "searchDetail");
            var scanning = function (channel) {
                return {phase: 1, pass: 1, channel: channel, count: 800, rangeName: "Local UHF", rangeStartMHz: "450.0",
                    rangeEndMHz: "460.0"};
            };
            searchFake.status = scanning(0);
            waitForRendering(screen);
            wait(50);
            testContext.startAccessibilityCount();
            var elapsed = run(function (i) {
                searchFake.status = scanning(i);
            });
            var counts = testContext.accessibilityCount();
            verifyPaced(counts, "NameChanged", ["searchStatusLine"], elapsed);
            tryVerify(function () { return nameOf(line) === line.text; }, period + 500, nameOf(line));
            stepPace();
            // A hit: the search stops scanning and receives, heard at once.
            var receiving = function (power, target) {
                return {phase: 4, pass: 1, channel: 41, count: 800, rangeName: "Local UHF", rangeStartMHz: "450.0",
                    rangeEndMHz: "460.0", livePowerValid: true, livePower: power,
                    hit: {id: "h1", freqMHz: "451.0125", power: -40, range: "Local UHF", confirmed: true, protocol: "P25",
                        targetText: "TG " + target, sourceText: "SRC 2001"}};
            };
            searchFake.status = receiving(-41, 100);
            tryVerify(function () { return nameOf(strength).indexOf("-41.0") >= 0; }, 100, "a new hit is heard at once: "
                      + nameOf(strength));
            tryVerify(function () { return nameOf(detail).indexOf("TG 100") >= 0; }, 100, nameOf(detail));
            testContext.stopAccessibilityCount();
            testContext.startAccessibilityCount();
            elapsed = run(function (j) {
                searchFake.status = receiving(-41 - j * 0.5, 100 + Math.floor(j / 4));
            });
            counts = testContext.accessibilityCount();
            verify(strength.text.indexOf(Number(-41 - updates * 0.5).toFixed(1)) >= 0, "the level on screen follows: "
                   + strength.text);
            verifyPaced(counts, "NameChanged", ["searchStrength", "searchDetail"], elapsed);
            tryVerify(function () { return nameOf(strength).indexOf(Number(-41 - updates * 0.5).toFixed(1)) >= 0; },
                      period + 500, nameOf(strength));
            tryVerify(function () { return nameOf(detail).indexOf("TG " + (100 + updates / 4)) >= 0; }, period + 500,
                      nameOf(detail));
        }

        // The squelch readouts in the radio and squelch sheets, while an auto or noise squelch moves: at the readings'
        // pace; the gate opening or closing, or a change of the setting, at once.
        function test_e_squelch_readouts_data() {
            return [{tag: "radio sheet", loader: "radio", value: "radioSquelchValue", status: "radioSquelchAutoStatus"},
                    {tag: "squelch sheet", loader: "squelch", value: "squelchValue", status: "squelchStatus"}];
        }
        function test_e_squelch_readouts(data) {
            monitor.visible = false;
            spectrum.visible = false;
            var sheet = data.loader === "radio" ? radioLoader.item : squelchLoader.item;
            set({squelchAudioInput: data.loader === "squelch", effectiveSquelchNoise: true, configuredSquelchNoise: true,
                squelchNoiseOffered: true, effectiveSquelchMarginDb: 12, configuredSquelchMarginDb: 12,
                squelchAutoStatus: "quieting 10 dB", squelchGateOpen: false});
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(50);
            var value = item(sheet, data.value);
            var status = item(sheet, data.status);
            verify(status.visible, "the status line shows");
            testContext.startAccessibilityCount();
            var elapsed = run(function (i) {
                set({squelchAutoStatus: "quieting " + (10 + i % 15) + " dB"});
            });
            var counts = testContext.accessibilityCount();
            compare(status.text, "quieting " + (10 + updates % 15) + " dB", "the status on screen follows");
            verifyPaced(counts, "NameChanged", [data.value, data.status], elapsed);
            tryVerify(function () { return nameOf(value).indexOf(status.text) >= 0; }, period + 500, nameOf(value));
            tryVerify(function () { return nameOf(status) === status.text; }, period + 500, nameOf(status));
            stepPace();
            // The gate opens: at once. Just after a step (the setting changing) an update alone waits.
            set({effectiveSquelchMarginDb: 13, configuredSquelchMarginDb: 13});
            wait(30);
            set({squelchAutoStatus: "quieting 31 dB"});
            wait(30);
            verify(nameOf(value).indexOf("quieting 31 dB") < 0, "an update alone waits: " + nameOf(value));
            set({squelchGateOpen: true});
            tryVerify(function () { return nameOf(value).indexOf("quieting 31 dB") >= 0; }, 100,
                      "the gate opening is heard at once: " + nameOf(value));
            tryVerify(function () { return nameOf(status) === "quieting 31 dB"; }, 100, nameOf(status));
            // The setting changes: at once.
            set({squelchAutoStatus: "quieting 33 dB"});
            set({effectiveSquelchMarginDb: 14, configuredSquelchMarginDb: 14});
            tryVerify(function () { return nameOf(value).indexOf("quieting 33 dB") >= 0; }, 100,
                      "a new setting is heard at once: " + nameOf(value));
        }

        // A step input that flaps (a marginal lock, a decode-quality reading coming and going) is heard once a floor
        // at most, and the last state within a floor of the last flap. The monitor's strip, its hero (whose waiting
        // line follows the lock) and its summary row each.
        function test_f_flapping_steps_are_held_to_the_floor() {
            var floor = 200;
            Ui.Navigation.speechPeriodMs = stepPeriod;
            Ui.Navigation.speechStepFloorMs = floor;
            set({slot1CallState: 0, leadSlot: 0});
            wait(50);
            var strip = item(monitor, "signalStrip");
            var hero = item(monitor, "monitorHero");
            testContext.startAccessibilityCount();
            var flaps = 40;
            var start = Date.now();
            for (var i = 1; i <= flaps; ++i) {
                set({carrierLock: i % 2 === 0, ccFecValid: i % 3 !== 0, snrDb: 10 + i * 0.25});
                wait(tick);
            }
            var elapsed = Date.now() - start;
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            var most = Math.ceil(elapsed / floor) + 1;
            for (var named of ["signalStrip", "monitorHero"]) {
                var renames = raised(counts, "NameChanged", named);
                verify(renames >= 1 && renames <= most, named + " renamed " + renames + " times for " + flaps
                       + " flaps in " + elapsed + " ms (at most " + most + "): " + JSON.stringify(counts.chains));
            }
            var described = raised(counts, "DescriptionChanged", "monitorDetailsRow");
            verify(described >= 1 && described <= most, "the summary redescribed " + described + " times: "
                   + JSON.stringify(counts.chains));
            // The last state, the lock held after an even flap, within a floor.
            tryVerify(function () {
                return nameOf(strip).indexOf("Carrier lock") >= 0 && nameOf(strip).indexOf("No carrier lock") < 0
                    && nameOf(strip).indexOf("SNR " + Number(10 + flaps * 0.25).toFixed(1) + " dB") >= 0;
            }, floor + 200, nameOf(strip));
            tryVerify(function () { return nameOf(hero) === "waiting for a call…"; }, floor + 200, nameOf(hero));
        }

        // A step whose inputs arrive in several notifications (a hop: the target, its frequency and its sync each
        // change on their own) is heard once, with all of them, and the readings moving after it wait for the period:
        // the floor holds back a later step, not the rest of this one.
        function test_f2_a_step_in_parts_is_heard_once() {
            var floor = 300;
            Ui.Navigation.speechPeriodMs = appPeriod;
            Ui.Navigation.speechStepFloorMs = floor;
            wait(floor + 100);
            var strip = item(monitor, "signalStrip");
            testContext.startAccessibilityCount();
            testContext.publishTarget({scanTargetId: "target-2", scanTargetOrdinal: 2, centerFreqHz: 852012500,
                syncLabel: "P25p2"});
            var start = Date.now();
            for (var i = 1; i <= 40; ++i) {
                set({snrDb: 9 + i * 0.25});
                wait(15);
            }
            var elapsed = Date.now() - start;
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            verify(elapsed >= 2 * floor, "the readings moved for " + elapsed + " ms, past two floors");
            compare(raised(counts, "NameChanged", "signalStrip"), 1, "one hop, one rename: " + JSON.stringify(counts.chains));
            verify(nameOf(strip).indexOf("852.01") >= 0, "the hop is heard with its frequency: " + nameOf(strip));
        }

        // The scan header names the target the scan is on, which a 300 ms dwell changes three times a second. Hops
        // are read at the readings' pace; a hold is heard at once. Run at a fifth of the app's times (a 1 s period,
        // hops 60 ms apart), 50 hops.
        function test_h_scan_header_hops_are_paced() {
            var period = 1000;
            Ui.Navigation.speechPeriodMs = period;
            Ui.Navigation.speechStepFloorMs = 200;
            set({scanTargetCount: 50, scanTargetOrdinal: 1, scanHold: false});
            var header = item(monitor, "scanTargetHeader");
            wait(250);
            testContext.startAccessibilityCount();
            var hops = 50;
            var start = Date.now();
            for (var i = 1; i <= hops; ++i) {
                testContext.publishTarget({scanTargetOrdinal: i, scanTargetId: "target-" + i,
                    centerFreqHz: 851012500 + i * 12500});
                wait(60);
            }
            var elapsed = Date.now() - start;
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            var renames = raised(counts, "NameChanged", "scanTargetHeader");
            var most = Math.ceil(elapsed / period) + 1;
            verify(renames <= most, "the header renamed " + renames + " times for " + hops + " hops in " + elapsed
                   + " ms (at most " + most + "): " + JSON.stringify(counts.chains));
            compare(header.text.indexOf("SCANNING · 50/50"), 0, "the text on screen follows every hop: " + header.text);
            tryVerify(function () { return nameOf(header) === header.text; }, period + 500, nameOf(header));
            // Held: at once.
            set({scanHold: true});
            tryVerify(function () { return nameOf(header).indexOf("HOLD") >= 0; }, 300, "a hold is heard at once: "
                      + nameOf(header));
        }

        // TalkBack's headings are swipe stops and its heading navigation's list, and Qt's bridge reads out each
        // rename of the one it is on. A scan's hops, each a retune, rename none of the Monitor's or the Spectrum's
        // headings at the app's pace: the Spectrum's scale ticks, which move with every retune, are not headings (not
        // in the tree at all), and its title, which names what is tuned, is paced.
        function test_i_scan_hops_rename_no_heading() {
            Ui.Navigation.speechPeriodMs = appPeriod;
            var screens = [monitor, spectrum];
            var before = [];
            for (var screen of screens)
                before = before.concat(testContext.focusableHeadings(screen));
            verify(before.length > 0, "the screens have headings");
            var renamed = [];
            for (var i = 1; i <= 10; ++i) {
                var hz = 851012500 + i * 250000;
                testContext.setSpectrumCenterHz(hz);
                testContext.publishTarget({scanTargetOrdinal: (i % 3) + 1, scanTargetId: "target-" + i, centerFreqHz: hz});
                wait(100);
                for (var h of before) {
                    var now = testContext.accessibleNode(h.item).name;
                    if (now !== h.name)
                        renamed.push(i + ": " + JSON.stringify(h.name) + " -> " + JSON.stringify(now));
                    h.name = now;
                }
            }
            testContext.setSpectrumCenterHz(0);
            compare(renamed.length, 0, "headings renamed by hops: " + JSON.stringify(renamed));
            var ticks = [];
            (function collect(item) {
                for (var k = 0; k < item.children.length; ++k) {
                    if (item.children[k].objectName === "spectrumScaleLabel" && item.children[k].visible)
                        ticks.push(item.children[k]);
                    collect(item.children[k]);
                }
            })(spectrum);
            verify(ticks.length > 0, "the scale is drawn");
            for (var tick of ticks)
                verify(!testContext.accessibleNode(tick).inTree, "a scale tick is not in the tree: " + tick.text);
        }

        // The Spectrum's system identity line has nothing to say until a carrier names its system: then it is out of
        // the tree, not a swipe stop that reads nothing. Named, it is read.
        function test_j_empty_identity_line_is_no_swipe_stop() {
            var line = item(spectrum, "spectrumIdentityLabel");
            set({carrierIdentityText: "", carrierIdentityTokens: [], carrierIdentityLive: false});
            wait(20);
            verify(!testContext.accessibleNode(line).inTree, "an empty identity line is out of the tree: "
                   + JSON.stringify(testContext.accessibleNode(line)));
            set({carrierIdentityText: "SYS 3A0", carrierIdentityTokens: ["SYS 3A0"], carrierIdentityLive: true});
            tryVerify(function () {
                var node = testContext.accessibleNode(line);
                return node.inTree && node.name.indexOf("SYS 3A0") >= 0;
            }, 500, JSON.stringify(testContext.accessibleNode(line)));
        }

        // A fast scan's hops are not steps: Local84 hops every 300 ms, and with each hop a step every reading paced by
        // one was read at the step floor, once a second, for as long as the scan ran (the strip 21 times in 22 s on the
        // emulator). 50 hops at a 300 ms dwell, with the app's period, floor and dwell scaled down by ten (500 ms,
        // 100 ms, 30 ms): each reading renames once a period at most and once more, not at the floor, and ends on the
        // value on screen. The steps that stay steps are said at once: a hold, then a lock.
        function test_k_fast_scan_hops_are_not_steps() {
            var dwell = 30;
            var hops = 50;
            var paced = ["signalStrip", "scanTimingRow", "monitorHero", "spectrumStatusLine", "spectrumCenterReadout"];
            var strip = item(monitor, "signalStrip");
            var timing = item(monitor, "scanTimingRow");
            var summary = item(monitor, "monitorDetailsRow");
            set({slot1CallState: 0, leadSlot: 0, carrierLock: false, syncedHere: false, syncLabel: "", scanHold: false,
                scanTimerRemainingDs: 3, scanTimerSpanMs: 300});
            Ui.Navigation.speechPeriodMs = 500;
            Ui.Navigation.speechStepFloorMs = 100;
            wait(250);
            testContext.startAccessibilityCount();
            var start = Date.now();
            var freqs = [769393750, 151000000, 162400000, 460000000, 851012500, 154015000];
            for (var i = 1; i <= hops; ++i) {
                var ordinal = (i % 3) + 1;
                set({scanTargetOrdinal: ordinal, scanTargetId: "target-" + ordinal, centerFreqHz: freqs[i % freqs.length],
                    snrDb: 1.5 + (i % 7) * 0.3, scanTimerRemainingDs: 3});
                wait(dwell / 2);
                set({scanTimerRemainingDs: 1});
                wait(dwell / 2);
            }
            var elapsed = Date.now() - start;
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            var most = Math.ceil(elapsed / Ui.Navigation.speechPeriodMs) + 1;
            for (var named of paced) {
                var renames = raised(counts, "NameChanged", named);
                verify(renames <= most, named + " renamed " + renames + " times for " + hops + " hops in " + elapsed
                       + " ms (at most " + most + "): " + JSON.stringify(counts.chains));
            }
            var described = raised(counts, "DescriptionChanged", "monitorDetailsRow");
            verify(described <= most, "Signal & site redescribed " + described + " times (at most " + most + ")");
            // And it catches up with the target the scan is on.
            var snr = "SNR " + Number(1.5 + (hops % 7) * 0.3).toFixed(1) + " dB";
            tryVerify(function () { return nameOf(strip).indexOf(snr) >= 0; }, 1000, nameOf(strip) + " vs " + snr);
            tryVerify(function () { return nameOf(timing) === timing.text; }, 1000, nameOf(timing));
            tryVerify(function () { return descriptionOf(summary) === summary.accessibleDescription; }, 1000,
                      descriptionOf(summary));
            // What stays a step is still said at once.
            wait(150);
            set({scanHold: true, snrDb: 12.7});
            tryVerify(function () { return nameOf(strip).indexOf("SNR 12.7 dB") >= 0; }, 100,
                      "a hold is said at once: " + nameOf(strip));
            wait(150);
            set({carrierLock: true, snrDb: 13.1});
            tryVerify(function () { return nameOf(strip).indexOf("SNR 13.1 dB") >= 0; }, 100,
                      "a lock is said at once: " + nameOf(strip));
        }

        // A hidden screen's pacers stay silent over time: the monitor hidden at a short period, its readings moving.
        function test_g_hidden_monitor_stays_silent() {
            Ui.Navigation.speechPeriodMs = 30;
            monitor.visible = false;
            spectrum.visible = false;
            wait(100);
            testContext.startAccessibilityCount();
            for (var i = 1; i <= 20; ++i) {
                set({snrDb: 30 + i * 0.4, scanTimerRemainingDs: 100 - i, carrierLock: i % 2 === 0});
                wait(15);
            }
            wait(200);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(testContext.accessibilityCountWithin(monitor, "NameChanged")
                    + testContext.accessibilityCountWithin(monitor, "DescriptionChanged")
                    + testContext.accessibilityCountWithin(spectrum, "NameChanged"), 0, JSON.stringify(counts.chains));
        }
    }
}
