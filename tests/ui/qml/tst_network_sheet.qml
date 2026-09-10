// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 800
    Loader {
        id: sheet
        anchors.fill: parent
        source: uiDir + "/NetworkSheet.qml"
    }
    QtObject {
        id: populated
        property bool active: false
        property var neighbours: [
            {
                freqHz: 851000000,
                sysid: 291,
                rfss: 1,
                site: 2,
                wacn: 703710,
                wacnValid: true,
                lraValid: false,
                isCurrentCc: true,
                isCandidate: true,
                cfvaText: "valid"
            }
        ]
        property var patches: [
            {
                sgid: 42,
                isPatch: true,
                groups: [77, 88],
                radios: [12345]
            }
        ]
        property var affiliations: [
            {
                rid: 12345,
                tg: 77
            }
        ]
        property var radios: [
            {
                rid: 12345
            }
        ]
    }
    QtObject {
        id: retained
        property bool active: false
        property var neighbours: []
        property var patches: []
        property var affiliations: []
        property var radios: []
    }
    Loader {
        id: monitor
        anchors.fill: parent
        source: uiDir + "/MonitorScreen.qml"
        visible: false
    }
    TestCase {
        name: "NetworkSheet"
        when: windowShown
        function cleanup() {
            findChild(monitor.item, "siteSheet").visible = false;
            monitor.visible = false;
            testContext.setMetric("siteProtocol", "");
            testContext.setMetric("siteLine", "");
            testContext.setMetric("siteConfirmed", false);
            findChild(monitor.item, "networkSheet").network = p25Network;
            retained.neighbours = [];
            retained.patches = [];
            retained.affiliations = [];
            retained.radios = [];
            testContext.setMetric("syncLabel", "");
        }
        function textCount(item, text) {
            var count = item.text === text ? 1 : 0;
            for (var i = 0; i < item.children.length; ++i)
                count += textCount(item.children[i], text);
            return count;
        }
        function test_populated() {
            sheet.item.network = populated;
            sheet.item.open();
            compare(populated.active, true);
            compare(textCount(sheet.item, "RID:12345 · TG:77"), 1);
            compare(textCount(sheet.item, "RID:12345"), 1);
            compare(textCount(sheet.item, "SG:42 · Patch · TG:77, 88 · RID:12345"), 1);
            compare(textCount(sheet.item, "851.000000 MHz [CC] [C] · SYS:123 RFSS:1 Site:2 WACN:ABCDE CFVA:valid"), 1);
            sheet.item.visible = false;
            compare(populated.active, false);
            sheet.item.network = p25Network;
        }
        function openSiteSheet() {
            var row = findChild(monitor.item, "siteRow");
            verify(row !== null);
            tryCompare(row, "visible", true);
            waitForRendering(row);
            mouseClick(row, row.width / 2, row.height / 2);
            var siteSheet = findChild(monitor.item, "siteSheet");
            tryCompare(siteSheet, "visible", true);
            return siteSheet;
        }
        function test_monitor_entry() {
            monitor.visible = true;
            testContext.setMetric("siteProtocol", "DMR");
            testContext.setMetric("siteLine", "DMR · CC 7");
            var siteSheet = openSiteSheet();
            var button = findChild(siteSheet, "siteNetworkButton");
            verify(button !== null);
            for (var protocol of ["DMR", "NXDN", "IDAS", "EDACS", ""]) {
                testContext.setMetric("siteProtocol", protocol);
                compare(button.visible, false);
            }
            testContext.setMetric("siteProtocol", "P25");
            testContext.setMetric("siteLine", "P25 · NAC 293");
            compare(button.visible, true);
            waitForRendering(siteSheet);
            mouseClick(button);
            compare(siteSheet.visible, true);
            compare(findChild(monitor.item, "networkSheet").visible, true);
            compare(p25Network.active, true);
            monitor.visible = false;
            compare(p25Network.active, false);
        }
        function test_monitor_entry_retains_announcements_data() {
            return ["neighbours", "patches", "affiliations", "radios"].map(function (section) {
                return {
                    tag: section,
                    section: section
                };
            });
        }
        function test_monitor_entry_retains_announcements(data) {
            var networkSheet = findChild(monitor.item, "networkSheet");
            networkSheet.network = retained;
            retained[data.section] = populated[data.section];
            monitor.visible = true;
            testContext.setMetric("syncLabel", "P25p1");
            testContext.setMetric("siteProtocol", "P25");
            testContext.setMetric("siteLine", "P25 · NAC 293");
            testContext.setMetric("siteConfirmed", true);
            var siteSheet = openSiteSheet();
            var button = findChild(siteSheet, "siteNetworkButton");
            verify(button !== null);
            compare(button.visible, true);
            siteSheet.visible = false;
            testContext.setMetric("syncLabel", "");
            testContext.setMetric("siteConfirmed", false);
            // F1 retains identity after sync expires; reopen through the site row.
            verify(retained[data.section].length > 0);
            siteSheet = openSiteSheet();
            compare(findChild(siteSheet, "siteRetained").visible, true);
            compare(button.visible, true);
            waitForRendering(siteSheet);
            mouseClick(button);
            compare(siteSheet.visible, true);
            compare(retained.active, true);
            compare(networkSheet.visible, true);
            networkSheet.visible = false;
            compare(retained.active, false);
            retained[data.section] = [];
            openSiteSheet();
            // Session/target clear removes identity and closes the site sheet.
            testContext.setMetric("siteProtocol", "");
            testContext.setMetric("siteLine", "");
            compare(button.visible, false);
            compare(siteSheet.visible, false);
            compare(findChild(monitor.item, "siteRow").visible, false);
        }

        function test_empty_and_lifecycle() {
            compare(sheet.status, Loader.Ready);
            compare(p25Network.active, false);
            sheet.item.open();
            compare(p25Network.active, true);
            compare(textCount(sheet.item, "(none announced)"), 4);
            for (var title of ["Neighbours", "Patches", "Affiliations", "Radios"])
                compare(textCount(sheet.item, title), 1);
            compare(textCount(sheet.item, "Tune"), 0);
            mouseClick(findChild(sheet.item, "networkDone"));
            compare(p25Network.active, false);
        }
    }
}
