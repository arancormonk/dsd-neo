// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 411
    height: 700
    Ui.MonitorScreen {
        id: monitor
        anchors.fill: parent
    }
    TestCase {
        name: "MonitorQuality"
        when: windowShown
        function clearQuality() {
            for (var key of ["qualityValid", "ccFecValid", "voiceErrsValid", "rsValid", "lastFrameErrsValid"])
                testContext.setMetric(key, false);
        }
        function init() {
            clearQuality();
            testContext.setHostRunning(true);
        }
        function cleanup() {
            clearQuality();
            Ui.Theme.fontScale = 1;
            root.width = 411;
            root.height = 700;
            testContext.setMetric("radioInput", true);
            testContext.setHostRunning(false);
        }
        function reading(name) {
            var label = findChild(monitor, name);
            verify(label !== null, name + " exists");
            return label;
        }
        function test_invalid_is_hidden() {
            verify(!reading("decodeQualityRow").visible);
        }
        function test_quality_on_pcm_and_clear() {
            testContext.setMetric("radioInput", false);
            testContext.setMetric("qualityValid", true);
            testContext.setMetric("ccFecValid", true);
            testContext.setMetric("ccFecOkPct", 97.7);
            testContext.setMetric("voiceErrsValid", true);
            testContext.setMetric("voiceErrsPerFrame", 2.25);
            testContext.setMetric("rsValid", true);
            testContext.setMetric("rsOkPct", 75);
            var row = reading("decodeQualityRow");
            tryCompare(row, "visible", true);
            compare(reading("ccFecQuality").text, "CC FEC 98%");
            compare(reading("voiceErrsQuality").text, "VOICE 2.3 err/fr");
            compare(reading("rsQuality").text, "RS 75%");
            verify(!reading("lastFrameQuality").visible);
            clearQuality();
            tryCompare(row, "visible", false);
        }
        function test_non_p25_fallback() {
            testContext.setMetric("radioInput", false);
            testContext.setMetric("qualityValid", true);
            testContext.setMetric("lastFrameErrsValid", true);
            testContext.setMetric("lastFrameErrs", 2);
            testContext.setMetric("lastFrameErrs2", 7);
            tryCompare(reading("lastFrameQuality"), "text", "ERR 2/7");
            verify(reading("lastFrameQuality").visible);
            verify(!reading("voiceErrsQuality").visible);
        }
        function test_compact_wrap_remains_reachable() {
            root.width = 320;
            root.height = 360;
            Ui.Theme.fontScale = 1.6;
            testContext.setMetric("qualityValid", true);
            testContext.setMetric("ccFecValid", true);
            testContext.setMetric("ccFecOkPct", 100);
            testContext.setMetric("voiceErrsValid", true);
            testContext.setMetric("voiceErrsPerFrame", 12.5);
            testContext.setMetric("rsValid", true);
            testContext.setMetric("rsOkPct", 100);
            var row = reading("decodeQualityRow");
            var body = reading("monitorBody");
            tryVerify(function() { return row.height > reading("ccFecQuality").height; });
            for (var name of ["ccFecQuality", "voiceErrsQuality", "rsQuality"]) {
                var label = reading(name);
                verify(label.x + label.width <= row.width + 1);
            }
            verify(row.parent.parent === body.contentItem);
            verify(body.height > 0);
            // A wrapped row can be taller than a landscape viewport. Each whole
            // reading must be reachable, even when they cannot all fit at once.
            for (var labelName of ["ccFecQuality", "voiceErrsQuality", "rsQuality"]) {
                var visibleLabel = reading(labelName);
                tryVerify(function() {
                    body.contentY = visibleLabel.mapToItem(body.contentItem, 0, 0).y;
                    return visibleLabel.mapToItem(body, 0, 0).y >= -1
                        && visibleLabel.mapToItem(body, 0, visibleLabel.height).y <= body.height + 1;
                });
            }
        }
    }
}
