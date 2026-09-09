// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
Item {
    width: 420; height: 800
    Loader { id: sheet; anchors.fill: parent; source: uiDir + "/NetworkSheet.qml" }
    QtObject {
        id: populated
        property bool active: false
        property var neighbours: [{freqHz: 851000000, sysid: 291, rfss: 1, site: 2,
            wacn: 703710, wacnValid: true, lraValid: false, isCurrentCc: true, isCandidate: true, cfvaText: "valid"}]
        property var patches: [{sgid: 42, isPatch: true, groups: [77, 88], radios: [12345]}]
        property var affiliations: [{rid: 12345, tg: 77}]
        property var radios: [{rid: 12345}]
    }
    Loader { id: monitor; anchors.fill: parent; source: uiDir + "/MonitorScreen.qml"; visible: false }
    TestCase {
        name: "NetworkSheet"
        when: windowShown
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
        function test_monitor_entry() {
            monitor.visible = true;
            testContext.setMetric("syncLabel", "DMR");
            var button = findChild(monitor.item, "siteNetworkButton");
            verify(button !== null);
            compare(button.visible, false);
            testContext.setMetric("syncLabel", "P25p1");
            compare(button.visible, true);
            waitForRendering(monitor.item);
            mouseClick(button);
            compare(p25Network.active, true);
            monitor.visible = false;
            compare(p25Network.active, false);
            testContext.setMetric("syncLabel", "");
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
