// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The received sub-audible tone row (#522) and the reserved Tone filter row beside it:
// the received row shows only what was received, whatever the configured policy says,
// and it clears when the session stops or the receiver retunes.
Item {
    id: root
    width: 411
    height: 700
    Ui.MonitorScreen { id: monitor; anchors.fill: parent }
    TestCase {
        name: "MonitorRxTone"
        when: windowShown
        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function showLocked(tenths, text) {
            testContext.setMetric("rxToneVisible", true);
            testContext.setMetric("rxToneStatus", 3);
            testContext.setMetric("rxToneKind", 1);
            testContext.setMetric("rxToneTenthsHz", tenths);
            testContext.setMetric("rxToneCarrier", true);
            testContext.setMetric("rxToneText", text);
        }
        function cleanup() {
            testContext.setMetric("rxToneVisible", false);
            testContext.setMetric("rxToneStatus", 0);
            testContext.setMetric("rxToneKind", 0);
            testContext.setMetric("rxToneTenthsHz", 0);
            testContext.setMetric("rxToneCarrier", false);
            testContext.setMetric("rxToneText", "");
            testContext.setMetric("rxToneConfiguredText", "off");
            testContext.setHostRunning(false);
        }
        function test_hidden_outside_the_fm_monitor() {
            verify(!item("monitorRxTone").visible);
            // Reserved until #527: the policy row never shows yet.
            verify(!item("monitorToneFilter").visible);
            compare(item("monitorToneFilterValue").text, "off");
        }
        function test_received_row_shows_the_locked_tone() {
            testContext.setHostRunning(true);
            showLocked(1000, "CTCSS 100.0 Hz");
            var row = item("monitorRxTone");
            tryCompare(row, "visible", true);
            compare(item("monitorRxToneValue").text, "CTCSS 100.0 Hz");
        }
        function test_received_is_not_the_configured_value() {
            testContext.setHostRunning(true);
            showLocked(1000, "CTCSS 100.0 Hz");
            // A configured policy naming a different tone: the received row must still say
            // what was received, and the policy lands only in its own reading.
            testContext.setMetric("rxToneConfiguredText", "allow 67.0/D023N");
            tryCompare(item("monitorToneFilterValue"), "text", "allow 67.0/D023N");
            compare(item("monitorRxToneValue").text, "CTCSS 100.0 Hz");
            // And a received change never reaches the policy reading.
            showLocked(1318, "CTCSS 131.8 Hz");
            tryCompare(item("monitorRxToneValue"), "text", "CTCSS 131.8 Hz");
            compare(item("monitorToneFilterValue").text, "allow 67.0/D023N");
        }
        function test_detecting_none_and_no_carrier() {
            testContext.setHostRunning(true);
            testContext.setMetric("rxToneVisible", true);
            testContext.setMetric("rxToneStatus", 2);
            testContext.setMetric("rxToneText", "detecting");
            tryCompare(item("monitorRxToneValue"), "text", "detecting");
            testContext.setMetric("rxToneStatus", 4);
            testContext.setMetric("rxToneText", "none");
            tryCompare(item("monitorRxToneValue"), "text", "none");
            testContext.setMetric("rxToneStatus", 1);
            testContext.setMetric("rxToneText", "—");
            tryCompare(item("monitorRxToneValue"), "text", "—");
        }
        function test_clears_after_retune() {
            testContext.setHostRunning(true);
            testContext.setMetric("rxToneConfiguredText", "allow 100.0");
            showLocked(1000, "CTCSS 100.0 Hz");
            tryCompare(item("monitorRxToneValue"), "text", "CTCSS 100.0 Hz");
            // What MetricsModel publishes after the decoder's retune reset: the row stays
            // while the monitor runs, with nothing heard on the new channel yet.
            testContext.setMetric("rxToneStatus", 1);
            testContext.setMetric("rxToneKind", 0);
            testContext.setMetric("rxToneTenthsHz", 0);
            testContext.setMetric("rxToneCarrier", false);
            testContext.setMetric("rxToneText", "—");
            tryCompare(item("monitorRxToneValue"), "text", "—");
            verify(item("monitorRxTone").visible);
            compare(item("monitorToneFilterValue").text, "allow 100.0");
        }
        function test_clears_after_stop() {
            testContext.setHostRunning(true);
            showLocked(2541, "CTCSS 254.1 Hz");
            var row = item("monitorRxTone");
            tryCompare(row, "visible", true);
            // What MetricsModel::clear() publishes when the session stops.
            testContext.setMetric("rxToneVisible", false);
            testContext.setMetric("rxToneStatus", 0);
            testContext.setMetric("rxToneKind", 0);
            testContext.setMetric("rxToneTenthsHz", 0);
            testContext.setMetric("rxToneCarrier", false);
            testContext.setMetric("rxToneText", "");
            testContext.setHostRunning(false);
            tryCompare(row, "visible", false);
            compare(item("monitorRxToneValue").text, "");
        }
    }
}
