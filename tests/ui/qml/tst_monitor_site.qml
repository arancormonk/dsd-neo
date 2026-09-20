// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 411
    height: 700
    Ui.MonitorScreen { id: monitor; anchors.fill: parent }
    TestCase {
        name: "MonitorSite"
        when: windowShown
        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function cleanup() {
            testContext.setMetric("siteLine", "");
            testContext.setMetric("siteConfirmed", false);
            testContext.setMetric("siteProtocol", "");
            testContext.setMetric("p25NacValid", false);
            testContext.setMetric("dmrColorCode", -1);
            testContext.setMetric("dmrSiteText", "");
            testContext.setMetric("dmrRestLsn", 0);
            testContext.setMetric("ccFreqHz", 0);
            testContext.setMetric("vcFreqHz", 0);
            testContext.setMetric("nxdnRan", -1);
            testContext.setMetric("nxdnSiteCode", 0);
            testContext.setMetric("nxdnSysCode", 0);
            testContext.setMetric("edacsSiteText", "");
            Ui.Theme.resetFontScale();
            root.width = 411;
            root.height = 700;
        }
        function test_cleanup_restores_platform_font_binding() {
            var previousFont = testContext.applicationFont();
            try {
                // Qt.application.font is CONSTANT: change the font before cleanup
                // so the restored binding evaluates the platform's current value.
                testContext.setApplicationFont(Qt.font({pixelSize: 24}));
                Ui.Theme.fontScale = 1.6;
                cleanup();
                compare(Ui.Theme.fontScale, 1.5);
            } finally {
                testContext.setApplicationFont(previousFont);
                Ui.Theme.resetFontScale();
            }
        }
        function test_site_row_and_sheet() {
            verify(!item("siteRow").visible);
            testContext.setMetric("siteProtocol", "P25");
            testContext.setMetric("siteLine", "P25 · NAC 293");
            testContext.setMetric("p25NacValid", true);
            testContext.setMetric("p25Nac", 659);
            testContext.setMetric("siteConfirmed", true);
            var row = item("siteRow");
            tryCompare(row, "visible", true);
            compare(row.opacity, 1);
            waitForRendering(row);
            mouseClick(row, row.width / 2, row.height / 2);
            tryCompare(item("siteSheet"), "visible", true);
            compare(item("siteNac").text, "293");
            verify(!item("siteWacn").visible);
            testContext.setMetric("siteConfirmed", false);
            tryVerify(function() { return row.opacity < 1; });
            verify(item("siteRetained").visible);
            testContext.setMetric("siteLine", "");
            tryCompare(row, "visible", false);
            tryCompare(item("siteSheet"), "visible", false);
        }
        function test_protocol_grids_and_frequencies() {
            testContext.setMetric("siteLine", "DMR · CC 7");
            testContext.setMetric("siteProtocol", "DMR");
            testContext.setMetric("dmrColorCode", 7);
            testContext.setMetric("dmrSiteText", "<b>Net 12 Site 3</b> ");
            testContext.setMetric("dmrRestLsn", 4);
            testContext.setMetric("ccFreqHz", 851012500);
            testContext.setMetric("vcFreqHz", 852000000);
            item("siteSheet").visible = true;
            tryCompare(item("siteDmrColor"), "visible", true);
            compare(item("siteDmrColor").text, "7");
            compare(item("siteDmrText").text, "<b>Net 12 Site 3</b> ");
            compare(item("siteDmrText").textFormat, Text.PlainText);
            compare(item("siteCcFreq").text, "851.012500 MHz");
            compare(item("siteVcFreq").text, "852.000000 MHz");
            testContext.setMetric("siteProtocol", "IDAS");
            testContext.setMetric("nxdnRan", -1);
            verify(!item("siteDmrColor").visible);
            verify(!item("siteRan").visible);
            testContext.setMetric("nxdnRan", 0);
            testContext.setMetric("nxdnSiteCode", 3);
            testContext.setMetric("nxdnSysCode", 12);
            tryCompare(item("siteRan"), "visible", true);
            compare(item("siteRan").text, "0");
            compare(item("siteNxdnSys").text, "12");
            testContext.setMetric("siteProtocol", "EDACS");
            testContext.setMetric("edacsSiteText", "SITE 007 [07]");
            tryCompare(item("siteEdacs"), "visible", true);
            compare(item("siteEdacs").text, "SITE 007 [07]");
        }
        function test_compact_site_reachable() {
            root.width = 320;
            root.height = 360;
            Ui.Theme.fontScale = 1.6;
            testContext.setMetric("siteLine", "P25 · WACN BEE00 · SYS 123 · NAC 293 · RFSS 2 · SITE 3");
            var row = item("siteRow");
            var body = item("monitorBody");
            tryCompare(row, "visible", true);
            compare(row.parent.parent, body.contentItem);
            compare(row.y, 0);
            verify(row.height >= 44);
            waitForRendering(row);
            mouseClick(row, row.width / 2, Math.min(20, row.height / 2));
            tryCompare(item("siteSheet"), "visible", true);
            item("siteSheet").visible = false;
        }
    }
}
