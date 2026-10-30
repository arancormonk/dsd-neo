// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

// Issue #782: a reading that moves every tick is not read out every tick.
//
// Qt's Android bridge speaks every rename of the node TalkBack is focused on (QtAccessibilityDelegate's
// notifyDescriptionOrNameChanged sends an announcement for the focused node). The harness cannot focus a node the
// way TalkBack does, so these cases hold every node to the rule a focused one needs: a moving reading's name catches
// up at most every 5 s (SpeechPacer), and at once on a meaningful step, while the text on screen stays live.
Item {
    id: root
    width: 420
    height: 900
    Loader {
        id: monitorLoader
        anchors.fill: parent
        source: uiDir + "/MonitorScreen.qml"
    }
    Loader {
        id: spectrumLoader
        anchors.fill: parent
        source: uiDir + "/SpectrumScreen.qml"
    }
    TestCase {
        name: "A11yPacedNames"
        when: windowShown
        readonly property var monitor: monitorLoader.item
        readonly property var spectrum: spectrumLoader.item
        readonly property int period: 5000
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
        function nameOf(target) {
            return testContext.accessibleNode(target).name;
        }
        function set(values) {
            for (var key in values) {
                if (!(key in restore))
                    restore[key] = metrics[key];
                testContext.setMetric(key, values[key]);
            }
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            verify(monitor !== null && spectrum !== null);
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
            testContext.setPrefs("monitorDetailsExpanded", true);
            waitForRendering(root);
            wait(50);
        }
        function cleanup() {
            testContext.stopAccessibilityCount();
            testContext.setPrefs("monitorDetailsExpanded", false);
            for (var key in restore)
                testContext.setMetric(key, restore[key]);
            restore = {};
            testContext.setHostRunning(false);
        }

        // Every moving reading the monitor and spectrum show, at 4 Hz for 10 s: each node renames a few times at
        // most, and ends on the value on screen. The text on screen follows every change.
        function test_a_four_hertz_for_ten_seconds() {
            var strip = item(monitor, "signalStrip");
            var timing = item(monitor, "scanTimingRow");
            var duration = item(monitor, "heroDuration");
            var summary = item(monitor, "monitorDetailsRow");
            var status = item(spectrum, "spectrumStatusLine");
            var snrText = item(monitor, "monitorSnrValue");
            testContext.startAccessibilityCount();
            for (var i = 1; i <= 40; ++i) {
                set({snrDb: 9 + i * 0.3, cfoHz: (i % 2 ? -1 : 1) * i * 11, scanTimerRemainingDs: 100 - i * 2,
                    slot1CallSeconds: 1 + Math.floor(i / 4)});
                compare(snrText.text, Number(9 + i * 0.3).toFixed(1) + " dB", "the strip's text follows every tick");
                wait(250);
            }
            var counts = testContext.accessibilityCount();
            var report = JSON.stringify(counts.chains);
            for (var named of ["signalStrip", "scanTimingRow", "heroDuration", "spectrumStatusLine"]) {
                var renames = raised(counts, "NameChanged", named);
                verify(renames >= 1 && renames <= 3, named + " renamed " + renames + " times in 10 s: " + report);
            }
            var described = raised(counts, "DescriptionChanged", "monitorDetailsRow");
            verify(described >= 1 && described <= 3, "the summary row redescribed " + described + " times: " + report);
            compare(typed(counts, "ObjectShow") + typed(counts, "ObjectHide") + typed(counts, "LocationChanged"), 0, report);
            // A focused node is never left behind for longer than the period: each catches up with the screen.
            var snr = Number(9 + 40 * 0.3).toFixed(1) + " dB";
            tryVerify(function () { return nameOf(strip).indexOf("SNR " + snr) >= 0; }, period + 1000, nameOf(strip));
            tryVerify(function () { return nameOf(timing) === timing.text; }, period + 1000, nameOf(timing));
            tryVerify(function () { return nameOf(duration) === duration.text; }, period + 1000, nameOf(duration));
            tryVerify(function () {
                return testContext.accessibleNode(summary).description === summary.accessibleDescription;
            }, period + 1000, testContext.accessibleNode(summary).description);
            tryVerify(function () { return nameOf(status).indexOf(snr) >= 0; }, period + 1000, nameOf(status));
        }
        function typed(counts, type) {
            return counts.types[type] ? counts.types[type] : 0;
        }

        // A meaningful step renames at once, with every reading as it then is: a lost lock, a new scan target, a call
        // ending, a sync change on the spectrum.
        function test_b_steps_rename_at_once() {
            var strip = item(monitor, "signalStrip");
            var timing = item(monitor, "scanTimingRow");
            var status = item(spectrum, "spectrumStatusLine");
            // Let the period start, then move a reading without a step: the names hold.
            wait(100);
            set({snrDb: 21.4});
            wait(100);
            verify(nameOf(strip).indexOf("SNR 21.4 dB") < 0, "a tick alone renames at the readings' pace: " + nameOf(strip));
            set({carrierLock: false});
            tryVerify(function () {
                return nameOf(strip).indexOf("No carrier lock") >= 0 && nameOf(strip).indexOf("SNR 21.4 dB") >= 0;
            }, 300, "a lost lock is said at once, with the SNR as it is: " + nameOf(strip));
            tryVerify(function () { return nameOf(status).indexOf("21.4") >= 0; }, 300, nameOf(status));
            set({scanTimerRemainingDs: 37});
            wait(100);
            verify(nameOf(timing) !== timing.text, "the countdown alone waits: " + nameOf(timing));
            set({scanTargetOrdinal: 2, scanTargetId: "target-2"});
            tryVerify(function () { return nameOf(timing) === timing.text; }, 300, "a new target is said at once");
            set({syncLabel: "DMR"});
            tryVerify(function () { return nameOf(status).indexOf("DMR") >= 0; }, 300, nameOf(status));
            // The call ends: its duration leaves the tree with it.
            var duration = item(monitor, "heroDuration");
            set({slot1CallState: 0, leadSlot: 0});
            tryVerify(function () { return !duration.visible && duration.Accessible.name === ""; }, 300);
        }
    }
}
