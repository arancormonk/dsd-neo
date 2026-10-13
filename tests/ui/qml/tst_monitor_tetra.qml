// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The TETRA rows: hidden until the TETRA preset has decoded a block (tetraVisible,
// app-control's decision), each part only once the decoder has something for it,
// the values as the shared view spells them, and the colours taken from the view's
// flags, never from the words.
Item {
    id: root
    width: 411
    height: 700
    Ui.MonitorScreen { id: monitor; anchors.fill: parent }
    TestCase {
        name: "MonitorTetra"
        when: windowShown
        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function showControlChannel() {
            testContext.setMetric("tetraModeText", "TMO downlink · clear");
            testContext.setMetric("tetraNetworkText", "MCC 262 · MNC 1001 · CC 25");
            testContext.setMetric("tetraControlText", "carrier 321 · 392.0125 MHz");
            testContext.setMetric("tetraVisible", true);
        }
        function cleanup() {
            testContext.setMetric("tetraVisible", false);
            testContext.setMetric("tetraModeText", "");
            testContext.setMetric("tetraEncrypted", false);
            testContext.setMetric("tetraNetworkText", "");
            testContext.setMetric("tetraTrunkText", "");
            testContext.setMetric("tetraOnTraffic", false);
            testContext.setMetric("tetraControlText", "");
            testContext.setMetric("tetraTrafficText", "");
            testContext.setMetric("tetraMmStatusText", "");
            testContext.setMetric("tetraCodecText", "");
            testContext.setMetric("tetraCodecFault", false);
            Ui.Theme.resetFontScale();
            root.width = 411;
            root.height = 700;
        }
        function insideBody(reading) {
            var body = item("monitorBody");
            var left = reading.mapToItem(body, 0, 0).x;
            var right = reading.mapToItem(body, reading.width, 0).x;
            return left >= -0.5 && right <= body.width + 0.5;
        }
        function test_hidden_at_rest() {
            verify(!item("monitorTetra").visible);
            verify(!item("monitorTetraChannels").visible);
            verify(!item("monitorTetraMmStatus").visible);
            verify(!item("monitorTetraCodec").visible);
        }
        // Texts the view filled stay off screen while it says the rows are hidden (another
        // preset, or nothing decoded yet).
        function test_hidden_while_the_view_says_so() {
            showControlChannel();
            testContext.setMetric("tetraCodecText", "ready");
            testContext.setMetric("tetraMmStatusText", "7 · MS frequency bands request");
            tryCompare(item("monitorTetra"), "visible", true);
            testContext.setMetric("tetraVisible", false);
            tryCompare(item("monitorTetra"), "visible", false);
            verify(!item("monitorTetraChannels").visible);
            verify(!item("monitorTetraMmStatus").visible);
            verify(!item("monitorTetraCodec").visible);
        }
        function test_a_control_channel_shows_mode_network_and_carrier() {
            showControlChannel();
            tryCompare(item("monitorTetra"), "visible", true);
            compare(item("monitorTetraMode").text, "TMO downlink · clear");
            compare(item("monitorTetraNetwork").text, "MCC 262 · MNC 1001 · CC 25");
            verify(item("monitorTetraNetwork").visible);
            verify(item("monitorTetraChannels").visible);
            compare(item("monitorTetraControl").text, "carrier 321 · 392.0125 MHz");
            verify(!item("monitorTetraTrunk").visible);
            verify(!item("monitorTetraTraffic").visible);
            verify(!item("monitorTetraMmStatus").visible);
            verify(!item("monitorTetraCodec").visible);
            compare(item("monitorTetraMode").color, Ui.Theme.cyan);
        }
        // Colour follows the view's flags: encryption, the traffic channel, a codec fault.
        function test_colours_follow_the_flags() {
            showControlChannel();
            testContext.setMetric("tetraTrunkText", "traffic channel");
            testContext.setMetric("tetraTrafficText", "carrier 654 · TS 1,3 · 392.1250 MHz");
            testContext.setMetric("tetraCodecText", "ready · 18 frames · 2 errors");
            tryCompare(item("monitorTetraCodec"), "visible", true);
            compare(item("monitorTetraTrunk").color, Ui.Theme.textSubdued);
            compare(item("monitorTetraCodecValue").color, Ui.Theme.cyan);
            testContext.setMetric("tetraEncrypted", true);
            testContext.setMetric("tetraOnTraffic", true);
            testContext.setMetric("tetraCodecFault", true);
            tryCompare(item("monitorTetraMode"), "color", Ui.Theme.magenta);
            compare(item("monitorTetraTrunk").color, Ui.Theme.cyan);
            compare(item("monitorTetraCodecValue").color, Ui.Theme.alert);
            // The words alone change nothing.
            testContext.setMetric("tetraCodecFault", false);
            testContext.setMetric("tetraCodecText", "missing");
            tryCompare(item("monitorTetraCodecValue"), "text", "missing");
            compare(item("monitorTetraCodecValue").color, Ui.Theme.cyan);
        }
        function test_mm_status_row() {
            showControlChannel();
            testContext.setMetric("tetraMmStatusText", "7 · MS frequency bands request");
            tryCompare(item("monitorTetraMmStatus"), "visible", true);
            compare(item("monitorTetraMmStatusValue").text, "7 · MS frequency bands request");
        }
        // A narrow phone with large text: no reading runs off the body.
        function test_rows_fit_a_compact_monitor() {
            root.width = 320;
            root.height = 360;
            Ui.Theme.fontScale = 1.6;
            showControlChannel();
            testContext.setMetric("tetraModeText", "DMO via repeater/gateway · encrypted (TEA2)");
            testContext.setMetric("tetraTrafficText", "carrier 4095 · TS 1,2,3,4 · 1234.56789 MHz");
            testContext.setMetric("tetraCodecText", "decode errors · 4294967295 frames · 4294967295 errors");
            var row = item("monitorTetra");
            tryCompare(row, "visible", true);
            waitForItemPolished(row);
            verify(insideBody(item("monitorTetraMode")));
            verify(insideBody(item("monitorTetraNetwork")));
            verify(insideBody(item("monitorTetraControl")));
            verify(insideBody(item("monitorTetraTraffic")));
            verify(insideBody(item("monitorTetraCodecValue")));
        }
    }
}
