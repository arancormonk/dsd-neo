// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The received sub-audible tone or code row (#522, #523) and the reserved Tone filter
// row beside it: the received row shows only what was received, whatever the configured
// policy says, and it clears when the session stops or the receiver retunes.
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
        // A received DCS code (rxToneKind 2 is DSD_ANALOG_TONE_KIND_DCS): the canonical
        // normal spelling and the inverted one that sends the same signal.
        function showCode(code, alias, text) {
            testContext.setMetric("rxToneVisible", true);
            testContext.setMetric("rxToneStatus", 3);
            testContext.setMetric("rxToneKind", 2);
            testContext.setMetric("rxToneTenthsHz", 0);
            testContext.setMetric("rxToneDcsCode", code);
            testContext.setMetric("rxToneDcsInverted", false);
            testContext.setMetric("rxToneDcsAliasCode", alias);
            testContext.setMetric("rxToneDcsAliasInverted", true);
            testContext.setMetric("rxToneCarrier", true);
            testContext.setMetric("rxToneText", text);
        }
        function cleanup() {
            testContext.setMetric("rxToneVisible", false);
            testContext.setMetric("rxToneStatus", 0);
            testContext.setMetric("rxToneKind", 0);
            testContext.setMetric("rxToneTenthsHz", 0);
            testContext.setMetric("rxToneDcsCode", 0);
            testContext.setMetric("rxToneDcsInverted", false);
            testContext.setMetric("rxToneDcsAliasCode", 0);
            testContext.setMetric("rxToneDcsAliasInverted", false);
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
        function test_received_row_shows_the_locked_code() {
            testContext.setHostRunning(true);
            // Both spellings of the signal, D023N and D047I (047 octal = 39), canonical first.
            showCode(19, 39, "DCS D023N / D047I");
            var row = item("monitorRxTone");
            tryCompare(row, "visible", true);
            var value = item("monitorRxToneValue");
            compare(value.text, "DCS D023N / D047I");
            // The longer text still fits the phone-width monitor beside its label.
            verify(row.mapToItem(monitor, 0, 0).x + row.width <= monitor.width);
        }
        function test_received_code_is_not_the_configured_value() {
            testContext.setHostRunning(true);
            showCode(19, 39, "DCS D023N / D047I");
            // A configured policy naming the other signal of the same code number, in both
            // of its spellings: the received row still says what was received.
            testContext.setMetric("rxToneConfiguredText", "block D047N/D023I");
            tryCompare(item("monitorToneFilterValue"), "text", "block D047N/D023I");
            compare(item("monitorRxToneValue").text, "DCS D023N / D047I");
            showCode(492, 78, "DCS D754N / D116I");
            tryCompare(item("monitorRxToneValue"), "text", "DCS D754N / D116I");
            compare(item("monitorToneFilterValue").text, "block D047N/D023I");
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
        function test_code_clears_after_retune_and_stop() {
            testContext.setHostRunning(true);
            testContext.setMetric("rxToneConfiguredText", "allow D023N");
            showCode(19, 39, "DCS D023N / D047I");
            tryCompare(item("monitorRxToneValue"), "text", "DCS D023N / D047I");
            // The decoder's retune reset: nothing heard on the new channel yet.
            testContext.setMetric("rxToneStatus", 1);
            testContext.setMetric("rxToneKind", 0);
            testContext.setMetric("rxToneDcsCode", 0);
            testContext.setMetric("rxToneDcsAliasCode", 0);
            testContext.setMetric("rxToneDcsAliasInverted", false);
            testContext.setMetric("rxToneCarrier", false);
            testContext.setMetric("rxToneText", "—");
            tryCompare(item("monitorRxToneValue"), "text", "—");
            verify(item("monitorRxTone").visible);
            compare(item("monitorToneFilterValue").text, "allow D023N");
            // Locked again, then the session stops: MetricsModel::clear().
            showCode(19, 39, "DCS D023N / D047I");
            tryCompare(item("monitorRxToneValue"), "text", "DCS D023N / D047I");
            testContext.setMetric("rxToneVisible", false);
            testContext.setMetric("rxToneStatus", 0);
            testContext.setMetric("rxToneKind", 0);
            testContext.setMetric("rxToneDcsCode", 0);
            testContext.setMetric("rxToneDcsAliasCode", 0);
            testContext.setMetric("rxToneDcsAliasInverted", false);
            testContext.setMetric("rxToneCarrier", false);
            testContext.setMetric("rxToneText", "");
            testContext.setHostRunning(false);
            tryCompare(item("monitorRxTone"), "visible", false);
            compare(item("monitorRxToneValue").text, "");
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
