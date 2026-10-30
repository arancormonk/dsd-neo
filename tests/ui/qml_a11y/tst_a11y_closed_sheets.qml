// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

// Issue #782: a closed sheet whose list follows a live reading must not build that list while nobody can see it.
//
// A delegate created under a hidden parent raises ObjectHide, and on Android each such event costs the main thread
// eight blocking calls into the Qt thread. The key sheet's decryption slots and the site sheet's identity fields
// follow readings that change with every call and every scan hop, so while closed they leave their lists empty and
// fill them when opened, with the readings as they are then.
Item {
    id: root
    width: 420
    height: 900
    Loader {
        id: keyLoader
        anchors.fill: parent
        source: uiDir + "/KeySheet.qml"
    }
    Loader {
        id: siteLoader
        anchors.fill: parent
        source: uiDir + "/SiteSheet.qml"
    }
    Component {
        id: spyComponent
        SignalSpy {
            signalName: "itemAdded"
        }
    }
    TestCase {
        name: "A11yClosedSheets"
        when: windowShown
        readonly property var keySheet: keyLoader.item
        readonly property var siteSheet: siteLoader.item
        readonly property int changes: 10
        property var restore: ({})
        property var spies: []

        // One call per slot, both with a key ID so each row carries its "Manage matching key" button.
        function slots(i) {
            var slot = function (n) {
                return {slot: n, status: "Encrypted", protocol: "dmr", lastObserved: false, algorithm: "21",
                    keyId: (i * 2 + n).toString(16).toUpperCase(), effectiveId: "", source: "Call " + i,
                    availability: "No matching key", fallback: "", blockReason: "", materialKind: "", targetId: String(100 + i),
                    group: true, privateCall: false, dmr: true, profileRef: ""};
            };
            return i % 2 === 0 ? [slot(1), slot(2)] : [slot(1)];
        }
        // A P25 site whose identity moves with each change.
        function site(i) {
            return {siteLine: "Site " + i, siteConfirmed: true, siteProtocol: "P25", p25NacValid: true, p25Nac: 0x290 + i,
                p25WacnValid: true, p25Wacn: 0xBEE00 + i, p25SysIdValid: true, p25SysId: 0x3A0 + i, p25Rfss: 1,
                p25Site: i + 1, ccFreqHz: 851012500 + i * 12500, vcFreqHz: 0};
        }
        function hex(value, width) {
            return Number(value).toString(16).toUpperCase().padStart(width, "0");
        }
        function showHide(scope) {
            return testContext.accessibilityCountWithin(scope, "ObjectShow")
                + testContext.accessibilityCountWithin(scope, "ObjectHide");
        }
        function churn() {
            var counts = testContext.accessibilityCount();
            return JSON.stringify(Object.keys(counts.chains).filter(function (chain) {
                return /^(ObjectShow|ObjectHide) /.test(chain);
            }).map(function (chain) {
                return counts.chains[chain] + "x " + chain;
            }));
        }
        function repeaters(item, out) {
            var children = item.children || [];
            for (var i = 0; i < children.length; ++i) {
                var child = children[i];
                if (typeof child.itemAt === "function" && child.delegate !== undefined)
                    out.push(child);
                repeaters(child, out);
            }
            return out;
        }
        // Spy on every Repeater in @p sheet; returns how many items they have added since.
        function spyOn(sheet) {
            var found = repeaters(sheet, []);
            verify(found.length > 0, "no Repeater in " + sheet.objectName);
            for (var repeater of found)
                spies.push(spyComponent.createObject(root, {target: repeater}));
            return function () {
                var added = 0;
                for (var spy of spies)
                    added += spy.count;
                return added;
            };
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            verify(keySheet !== null && siteSheet !== null);
            restore.decryptionSlots = metrics.decryptionSlots;
            for (var key in site(0))
                restore[key] = metrics[key];
        }
        function init() {
            testContext.setHostRunning(true);
            keySheet.visible = false;
            siteSheet.visible = false;
        }
        function cleanup() {
            testContext.stopAccessibilityCount();
            for (var spy of spies)
                spy.destroy();
            spies = [];
            keySheet.visible = false;
            siteSheet.visible = false;
            testContext.publishTarget(restore);
            testContext.setHostRunning(false);
        }

        function test_closed_key_sheet_leaves_its_slots_unbuilt() {
            var rows = findChild(keySheet, "keySheetSlots");
            verify(rows !== null, "the decryption slots are missing");
            var added = spyOn(keySheet);
            testContext.startAccessibilityCount();
            for (var i = 0; i < changes; ++i)
                testContext.publishTarget({decryptionSlots: slots(i)});
            compare(added(), 0, "the closed sheet built slot rows");
            compare(showHide(keySheet), 0, churn());
            testContext.stopAccessibilityCount();
            // Opened, it shows the slots as they are now.
            keySheet.open();
            verify(keySheet.visible);
            var last = slots(changes - 1);
            compare(rows.count, last.length);
            for (var n = 0; n < last.length; ++n) {
                var row = rows.itemAt(n);
                verify(row !== null);
                var text = row.children[0].text;
                verify(text.indexOf("Slot " + last[n].slot) === 0, text);
                verify(text.indexOf("Call key ID: " + last[n].keyId) >= 0, text);
                var manage = row.children[1];
                compare(manage.text, "Manage matching key");
                verify(manage.visible, "the key ID gives the row its matching-key button");
            }
            // And it follows a change while open.
            testContext.publishTarget({decryptionSlots: slots(0)});
            compare(rows.count, slots(0).length);
            // Closed again, it lets the rows go.
            keySheet.visible = false;
            compare(rows.count, 0);
        }

        function test_closed_site_sheet_leaves_its_fields_unbuilt() {
            testContext.publishTarget(site(0));
            var added = spyOn(siteSheet);
            testContext.startAccessibilityCount();
            for (var i = 1; i <= changes; ++i)
                testContext.publishTarget(site(i));
            compare(added(), 0, "the closed sheet built field rows");
            compare(showHide(siteSheet), 0, churn());
            testContext.stopAccessibilityCount();
            // Opened, it shows the identity as it is now.
            siteSheet.visible = true;
            var now = site(changes);
            var nac = findChild(siteSheet, "siteNac");
            verify(nac !== null, "the NAC row is missing");
            compare(nac.text, hex(now.p25Nac, 3));
            compare(findChild(siteSheet, "siteWacn").text, hex(now.p25Wacn, 5));
            compare(findChild(siteSheet, "siteSys").text, hex(now.p25SysId, 3));
            compare(String(findChild(siteSheet, "siteId").text), String(now.p25Site));
            compare(findChild(siteSheet, "siteCcFreq").text, (now.ccFreqHz / 1000000).toFixed(6) + " MHz");
            // And it follows a change while open.
            testContext.publishTarget(site(1));
            compare(findChild(siteSheet, "siteNac").text, hex(site(1).p25Nac, 3));
        }

        // Issue #782: open, the site sheet reads each protocol's identity as one node per field group. Hops between
        // targets of one protocol show and hide nothing and rename the group once a hop, after its rows are rebuilt,
        // not once per row. A hop to another protocol hides the group of fields that protocol does not have, one per
        // change, as it hid the P25-only network button before: content that differs, not churn.
        function test_open_site_sheet_on_hops() {
            testContext.publishTarget(site(0));
            siteSheet.visible = true;
            waitForRendering(siteSheet);
            wait(20);
            testContext.startAccessibilityCount();
            for (var i = 1; i <= changes; ++i) {
                testContext.publishTarget(site(i));
                wait(20);
            }
            var same = testContext.accessibilityCount();
            compare(showHide(siteSheet), 0, churn());
            var renames = 0;
            for (var chain in same.chains) {
                if (/^NameChanged siteFields /.test(chain))
                    renames += same.chains[chain];
            }
            var groups = 0;
            (function count(item) {
                for (var k = 0; k < item.children.length; ++k) {
                    var child = item.children[k];
                    if (child.objectName === "siteFields" && child.visible)
                        ++groups;
                    count(child);
                }
            })(siteSheet);
            verify(groups > 0, "the P25 fields are shown");
            // Each shown group renamed at most once a hop (before the pacing, the P25 groups renamed 500 times here).
            verify(renames >= 1 && renames <= groups * changes, groups + " P25 groups renamed " + renames + " times in "
                   + changes + " hops: " + JSON.stringify(same.chains));
            testContext.stopAccessibilityCount();
            testContext.startAccessibilityCount();
            for (var j = 1; j <= changes; ++j) {
                testContext.publishTarget(j % 2 ? {siteLine: "DMR " + j, siteProtocol: "DMR", dmrColorCode: j % 16}
                                                : site(j));
                wait(20);
            }
            var mixed = testContext.accessibilityCount();
            var hides = 0;
            for (var key in mixed.chains) {
                if (/^ObjectHide siteFields /.test(key))
                    hides += mixed.chains[key];
            }
            compare(hides, changes, "one field group leaves per protocol change: " + JSON.stringify(mixed.chains));
            compare(testContext.accessibilityCountWithin(siteSheet, "ObjectShow"), 0, churn());
        }
    }
}
