// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

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
    // The P25 network the Network sheet reads, as P25NetworkModel publishes it: a new list on every refresh.
    QtObject {
        id: fakeNet
        property bool active: false
        property var neighbours: []
        property var patches: []
        property var affiliations: []
        property var radios: []
    }
    Loader {
        id: networkLoader
        anchors.fill: parent
        source: uiDir + "/NetworkSheet.qml"
        onLoaded: item.network = fakeNet
    }
    // The same sheet on the app's own P25NetworkModel, which testContext.hopNetwork() publishes a scan hop to.
    Loader {
        id: liveNetworkLoader
        anchors.fill: parent
        source: uiDir + "/NetworkSheet.qml"
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
            // A case that changed the screen reader's pace puts the app's back, whatever it failed on.
            Ui.Navigation.speechPeriodMs = 5000;
            Ui.Navigation.speechStepFloorMs = 1000;
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
            // Each hop is a step heard on its own here (the step floor off); tst_a11y_paced_names.qml holds a
            // flapping step to the floor.
            var floor = Ui.Navigation.speechStepFloorMs;
            Ui.Navigation.speechStepFloorMs = 1;
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
            Ui.Navigation.speechStepFloorMs = floor;
            compare(hides, changes, "one field group leaves per protocol change: " + JSON.stringify(mixed.chains));
            compare(testContext.accessibilityCountWithin(siteSheet, "ObjectShow"), 0, churn());
        }

        // Issue #782: an open Network sheet reads each entry as its own row, and a refresh that hands a new list with
        // the same entries (the engine's last-seen times moved) rebuilds nothing: no row shows or hides, and none is
        // renamed. A changed entry renames its row only; a new entry adds one row.
        function neighbours(n, seen, site) {
            var rows = [];
            for (var i = 0; i < n; ++i)
                rows.push({freqHz: 851000000 + i * 12500, sysid: 291, rfss: 1, site: i === 3 ? site : i, wacn: 1,
                    wacnValid: true, lra: 0, lraValid: false, isCurrentCc: i === 0, isCandidate: false, cfvaText: "valid",
                    lastSeen: seen});
            return rows;
        }
        // The neighbour rows @p sheet shows, in order.
        function neighbourRows(sheet) {
            var rows = [];
            (function collect(item) {
                for (var k = 0; k < item.children.length; ++k) {
                    var child = item.children[k];
                    // A neighbour row: a text that starts with its frequency.
                    if (child.textFormat !== undefined && /^[0-9.]+ MHz/.test(child.text) && child.visible)
                        rows.push(child);
                    collect(child);
                }
            })(sheet);
            return rows;
        }
        function test_open_network_sheet_refreshes() {
            var sheet = networkLoader.item;
            verify(sheet !== null);
            fakeNet.neighbours = neighbours(10, 0, 3);
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(20);
            var rows = neighbourRows(sheet);
            compare(rows.length, 10, "a row per neighbour");
            for (var r of rows)
                compare(testContext.accessibleNode(r).name, r.text, "each row is read as itself");
            testContext.startAccessibilityCount();
            for (var refresh = 1; refresh <= 10; ++refresh) {
                fakeNet.neighbours = neighbours(10, refresh, 3);
                wait(20);
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(showHide(sheet), 0, "last-seen refreshes rebuild no row: " + JSON.stringify(counts.chains));
            compare(testContext.accessibilityCountWithin(sheet, "NameChanged"), 0, JSON.stringify(counts.chains));
            // A neighbour's site changes: that row is renamed, in place, at the readings' pace (no entry came or went).
            Ui.Navigation.speechPeriodMs = 100;
            testContext.startAccessibilityCount();
            fakeNet.neighbours = neighbours(10, 11, 42);
            wait(200);
            counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(showHide(sheet), 0, JSON.stringify(counts.chains));
            compare(testContext.accessibilityCountWithin(sheet, "NameChanged"), 1, JSON.stringify(counts.chains));
            verify(testContext.accessibleNode(rows[3]).name.indexOf("Site:42") >= 0, testContext.accessibleNode(rows[3]).name);
            // A new neighbour adds its row and touches no other.
            testContext.startAccessibilityCount();
            fakeNet.neighbours = neighbours(11, 12, 42);
            wait(20);
            counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(testContext.accessibilityCountWithin(sheet, "ObjectHide"), 0, JSON.stringify(counts.chains));
            verify(testContext.accessibilityCountWithin(sheet, "ObjectShow") <= 1, JSON.stringify(counts.chains));
            verify(testContext.accessibilityCountWithin(sheet, "NameChanged") <= 1, "only the new row is named: "
                   + JSON.stringify(counts.chains));
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.neighbours = [];
        }

        // Issue #782: an open Network sheet lists each section most recent first, so a neighbour heard again moves to
        // the top and every row above its old place shows another entry. A row is renamed at the readings' pace for
        // that, not on each refresh: a listener can stay on a row and hear it out. It ends on what it shows.
        function test_open_network_sheet_reorders_at_the_pace() {
            var sheet = networkLoader.item;
            var period = 500;
            var order = [0, 1, 2, 3, 4, 5, 6, 7];
            var listed = function () {
                return order.map(function (i) {
                    return {freqHz: 851000000 + i * 12500, sysid: 291, rfss: 1, site: i, wacn: 1, wacnValid: true, lra: 0,
                        lraValid: false, isCurrentCc: i === 0, isCandidate: false, cfvaText: "valid"};
                });
            };
            Ui.Navigation.speechPeriodMs = period;
            fakeNet.neighbours = listed();
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(50);
            var rows = neighbourRows(sheet);
            compare(rows.length, 8);
            var row = rows[3];
            compare(testContext.accessibleNode(row).name, row.text);
            testContext.startAccessibilityCount();
            var renames = 0;
            var said = testContext.accessibleNode(row).name;
            var start = Date.now();
            for (var refresh = 1; refresh <= 16; ++refresh) {
                // The last neighbour in the list is heard again and moves to the top.
                order = [order[order.length - 1]].concat(order.slice(0, order.length - 1));
                fakeNet.neighbours = listed();
                wait(100);
                var now = testContext.accessibleNode(row).name;
                if (now !== said)
                    ++renames;
                said = now;
            }
            var elapsed = Date.now() - start;
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(showHide(sheet), 0, JSON.stringify(counts.chains));
            var most = Math.ceil(elapsed / period) + 1;
            verify(renames >= 1 && renames <= most, "row 3 renamed " + renames + " times for 16 refreshes in " + elapsed
                   + " ms (at most " + most + ")");
            verify(row.text.indexOf("Site:" + order[3] + " ") >= 0, "the text on screen follows every refresh: " + row.text);
            tryVerify(function () { return testContext.accessibleNode(row).name === row.text; }, period + 500,
                      testContext.accessibleNode(row).name);
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.neighbours = [];
        }

        // Issue #782: a trunk scan's hops with the Network sheet open. Each hop publishes the new target's network
        // as UiController does, and the sheet keeps every row both targets list: only a row for an entry count that
        // came or went shows or hides, the row a listener is on stays, and each row ends on the new target's entry.
        function test_open_network_sheet_on_hops() {
            var sheet = liveNetworkLoader.item;
            verify(sheet !== null);
            var targets = [[0, 1, 2, 3, 4, 5, 6, 7], [3, 4, 5, 6, 7, 8]];
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            testContext.hopNetwork(targets[0]);
            waitForRendering(sheet);
            wait(50);
            var rows = neighbourRows(sheet);
            compare(rows.length, 8);
            var row = rows[3];
            testContext.startAccessibilityCount();
            var hops = 10;
            for (var hop = 1; hop <= hops; ++hop) {
                testContext.hopNetwork(targets[hop % 2]);
                wait(20);
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            // Two rows go on each hop to the six-neighbour target and come back on the next.
            var changed = 2 * hops;
            var shows = testContext.accessibilityCountWithin(sheet, "ObjectShow");
            var hides = testContext.accessibilityCountWithin(sheet, "ObjectHide");
            verify(shows <= changed / 2 && hides <= changed / 2, shows + " shows and " + hides + " hides for " + hops
                   + " hops: " + JSON.stringify(counts.chains));
            rows = neighbourRows(sheet);
            compare(rows.length, targets[0].length);
            verify(rows[3] === row, "the row a listener is on stays across the hops");
            verify(testContext.accessibleNode(row).inTree);
            verify(row.text.indexOf("Site:" + targets[0][3] + " ") >= 0, row.text);
            tryVerify(function () {
                return neighbourRows(sheet).every(function (shown) {
                    return testContext.accessibleNode(shown).name === shown.text;
                });
            }, 2000, "each row is read as the new target's entry");
            sheet.visible = false;
            Ui.Navigation.modals = [];
            testContext.hopNetwork([]);
        }

        // Issue #782: the open key sheet keeps a row per slot while the count holds; each call publishes a new list,
        // and the rows read it in place rather than being rebuilt with their buttons.
        function test_open_key_sheet_keeps_its_slot_rows() {
            testContext.publishTarget({decryptionSlots: slots(0)});
            keySheet.open();
            keySheet.visible = true;
            Ui.Navigation.modals = [keySheet];
            var rows = findChild(keySheet, "keySheetSlots");
            waitForRendering(keySheet);
            wait(20);
            compare(rows.count, 2);
            testContext.startAccessibilityCount();
            for (var i = 2; i <= 2 * changes; i += 2) {
                testContext.publishTarget({decryptionSlots: slots(i)});
                wait(20);
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(rows.count, 2);
            compare(showHide(keySheet), 0, "new slot lists rebuild no row: " + JSON.stringify(counts.chains));
            var text = rows.itemAt(0).children[0].text;
            verify(text.indexOf("Call key ID: " + slots(2 * changes)[0].keyId) >= 0, text);
            compare(testContext.accessibleNode(rows.itemAt(0).children[0]).name, text);
            Ui.Navigation.modals = [];
        }
    }
}
