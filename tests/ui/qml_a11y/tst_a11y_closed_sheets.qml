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
        // A P25 site whose identity moves with each change, tuned to its control channel (a retune each change).
        function site(i) {
            return {siteLine: "Site " + i, siteConfirmed: true, siteProtocol: "P25", p25NacValid: true, p25Nac: 0x290 + i,
                p25WacnValid: true, p25Wacn: 0xBEE00 + i, p25SysIdValid: true, p25SysId: 0x3A0 + i, p25Rfss: 1,
                p25Site: i + 1, ccFreqHz: 851012500 + i * 12500, centerFreqHz: 851012500 + i * 12500, vcFreqHz: 0};
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
            // Closed again, it lets the rows go, at the end of the turn (StableRows).
            keySheet.visible = false;
            tryCompare(rows, "count", 0);
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
        // Issue #782: during a scan the hops are no steps, so the open Site sheet's field groups are read at the
        // readings' pace even though each hop's new NAC and site rebuild their rows (each rebuild changes the group's
        // children for an instant, which is no step). Timings scaled down by ten: a period of 500 ms, a floor of 100 ms.
        function test_open_site_sheet_scan_hops_are_paced() {
            if (!("scanRotationActive" in restore))
                restore.scanRotationActive = metrics.scanRotationActive;
            testContext.publishTarget({scanRotationActive: true});
            testContext.publishTarget(site(0));
            Ui.Navigation.speechPeriodMs = 500;
            Ui.Navigation.speechStepFloorMs = 100;
            siteSheet.visible = true;
            waitForRendering(siteSheet);
            wait(600);
            testContext.startAccessibilityCount();
            var start = Date.now();
            for (var i = 1; i <= 50; ++i) {
                testContext.publishTarget(site(i));
                wait(30);
            }
            var elapsed = Date.now() - start;
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            var groups = 0;
            (function count(item) {
                for (var k = 0; k < item.children.length; ++k) {
                    if (item.children[k].objectName === "siteFields" && item.children[k].visible)
                        ++groups;
                    count(item.children[k]);
                }
            })(siteSheet);
            verify(groups > 0, "the P25 fields are shown");
            var renames = 0;
            for (var chain in counts.chains) {
                if (/^NameChanged siteFields /.test(chain))
                    renames += counts.chains[chain];
            }
            var most = groups * (Math.ceil(elapsed / 500) + 1);
            verify(renames <= most, groups + " groups renamed " + renames + " times in " + elapsed + " ms (at most "
                   + most + "): " + JSON.stringify(counts.chains));
        }

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
            // The heading names the protocol, and is read at the readings' pace, not on each protocol change.
            var heading = null;
            (function find(item) {
                for (var k = 0; k < item.children.length && heading === null; ++k) {
                    if (String(item.children[k].text).indexOf("Site identity") === 0)
                        heading = item.children[k];
                    else
                        find(item.children[k]);
                }
            })(siteSheet);
            verify(heading !== null, "the site sheet's heading");
            compare(testContext.accessibilityCountWithin(heading, "NameChanged"), 0,
                    "the heading renamed on protocol hops: " + JSON.stringify(mixed.chains));
            var hides = 0;
            for (var key in mixed.chains) {
                if (/^ObjectHide siteFields /.test(key))
                    hides += mixed.chains[key];
            }
            Ui.Navigation.speechStepFloorMs = floor;
            compare(hides, changes, "one field group leaves per protocol change: " + JSON.stringify(mixed.chains));
            compare(testContext.accessibilityCountWithin(siteSheet, "ObjectShow"), 0, churn());
        }

        // Issue #782: a sheet opened over another (Site → Network), closed with Back and opened again. Covered, the Site
        // sheet is out of the tree, all of it; and because leaving the tree raises nothing Android's bridge passes on,
        // the bridge is told to read the window's tree again each time a sheet takes or leaves the top (a show or hide
        // under the window), so TalkBack does not keep a copy of the covered sheet to swipe through. What it may still
        // ask of a node it learned of earlier reads nothing it could stop on: no heading or control is focusable, the
        // sheet's panel is nameless, and text is unnamed.
        function test_a_covered_sheet_leaves_the_tree() {
            var network = liveNetworkLoader.item;
            testContext.hopNetwork([0, 1]);
            testContext.publishTarget(site(1));
            siteSheet.visible = true;
            waitForRendering(siteSheet);
            wait(20);
            compare(Ui.Navigation.topModal, siteSheet);
            var open = testContext.accessibleNodesUnder(siteSheet).filter(function (node) { return node.inTree; });
            verify(open.length > 3, "the open site sheet is in the tree: " + open.length);
            network.open();
            wait(20);
            compare(Ui.Navigation.topModal, network);
            network.visible = false;
            wait(20);
            compare(Ui.Navigation.topModal, siteSheet, "Back closes only the Network sheet");
            var refresh = function (sheet) {
                var found = null;
                (function find(item) {
                    for (var k = 0; k < item.children.length && found === null; ++k) {
                        if (item.children[k].objectName === sheet.objectName + "TreeRefresh")
                            found = item.children[k];
                        else
                            find(item.children[k]);
                    }
                })(sheet.Window.contentItem);
                return found;
            };
            testContext.startAccessibilityCount();
            network.open();
            wait(20);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(Ui.Navigation.topModal, network);
            var stops = [];
            for (var node of testContext.accessibleNodesUnder(siteSheet)) {
                if (node.inTree || (node.focusable && node.role !== Accessible.StaticText && node.role !== Accessible.EditableText)
                        || ((node.role === Accessible.StaticText || node.role === Accessible.Heading
                             || node.role === Accessible.Dialog) && node.name !== ""))
                    stops.push(node.file + "#" + node.objectName + " role " + node.role + " " + JSON.stringify(node.name)
                               + (node.inTree ? " in tree" : "") + (node.focusable ? " focusable" : ""));
            }
            compare(stops.length, 0, "the covered site sheet still offers: " + JSON.stringify(stops));
            // Each sheet that took or left the top told the bridge to read the window's tree again.
            var siteRefresh = refresh(siteSheet);
            var networkRefresh = refresh(network);
            verify(siteRefresh !== null && networkRefresh !== null, "each sheet has its refresh node");
            compare(testContext.accessibleParentRole(siteRefresh), Accessible.Window, "its parent is the window");
            var raised = function (chain) {
                var total = 0;
                for (var key in counts.chains) {
                    if (key.indexOf(chain + " ") === 0 || key === chain)
                        total += counts.chains[key];
                }
                return total;
            };
            compare(raised("ObjectHide siteSheetTreeRefresh"), 1, JSON.stringify(counts.chains));
            compare(raised("ObjectShow networkSheetTreeRefresh"), 1, JSON.stringify(counts.chains));
            verify(testContext.accessibleNode(networkRefresh).invisible, "and nothing reads or reaches it");
            network.visible = false;
            siteSheet.visible = false;
            wait(20);
            testContext.hopNetwork([]);
        }

        // Issue #782: an open Network sheet reads each entry as its own row, and a refresh that hands a new list with
        // the same entries (the engine's last-seen times moved) rebuilds nothing: no row shows or hides, and none is
        // renamed. A changed entry renames its row only, at once; a new entry adds one row. A hop to a site that lists
        // the same neighbours with other fields renames the rows whose words differ, at once.
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
                    // A neighbour row: a text that starts with its frequency, drawn (not held for a later list).
                    if (child.textFormat !== undefined && /^[0-9.]+ MHz/.test(child.text) && drawn(child))
                        rows.push(child);
                    collect(child);
                }
            })(sheet);
            return rows;
        }
        // Whether @p item is drawn: shown, and neither it nor its list transparent.
        function drawn(item) {
            return item.visible && item.opacity > 0 && item.parent.opacity > 0;
        }
        // Every neighbour row @p sheet has, drawn or held.
        function allNeighbourRows(sheet) {
            var rows = [];
            (function collect(item) {
                for (var k = 0; k < item.children.length; ++k) {
                    var child = item.children[k];
                    if (child.textFormat !== undefined && /^[0-9.]+ MHz/.test(child.text))
                        rows.push(child);
                    collect(child);
                }
            })(sheet);
            return rows;
        }
        // Each section's "(none announced)" line in @p sheet, in order.
        function noneLines(sheet) {
            var lines = [];
            (function collect(item) {
                for (var k = 0; k < item.children.length; ++k) {
                    var child = item.children[k];
                    if (child.text === "(none announced)")
                        lines.push(child);
                    collect(child);
                }
            })(sheet);
            return lines;
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
            // A neighbour's site changes: that row is renamed, in place.
            testContext.startAccessibilityCount();
            fakeNet.neighbours = neighbours(10, 11, 42);
            wait(20);
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
            // The same neighbours on another system: each row's words change, and each is renamed once, at once.
            var other = neighbours(11, 13, 42).map(function (row) {
                return Object.assign({}, row, {sysid: 0x2A0});
            });
            rows = neighbourRows(sheet);
            testContext.startAccessibilityCount();
            fakeNet.neighbours = other;
            wait(20);
            counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(showHide(sheet), 0, JSON.stringify(counts.chains));
            compare(testContext.accessibilityCountWithin(sheet, "NameChanged"), 11, JSON.stringify(counts.chains));
            compare(neighbourRows(sheet), rows, "each row stays with its neighbour");
            for (r of rows) {
                verify(r.text.indexOf("SYS:2A0 ") >= 0, r.text);
                compare(testContext.accessibleNode(r).name, r.text);
            }
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.neighbours = [];
        }

        // Issue #782: P25NetworkModel lists each section most recent first, so a neighbour heard again moves to the
        // top of its list. With a screen reader on (this run) the sheet keeps each section in frequency order
        // instead: Qt's bridge tells Android nothing of a row that moves, so TalkBack would swipe the rows in an
        // order they had left. Every row stays where it is and is read as what it shows, and nothing is raised.
        function test_open_network_sheet_keeps_its_order_for_a_screen_reader() {
            verify(screenReader.active, "this run has a screen reader on");
            var sheet = networkLoader.item;
            var order = [5, 2, 7, 0, 3, 6, 1, 4];
            var listed = function () {
                return order.map(function (i) {
                    return {freqHz: 851000000 + i * 12500, sysid: 291, rfss: 1, site: i, wacn: 1, wacnValid: true, lra: 0,
                        lraValid: false, isCurrentCc: i === 0, isCandidate: false, cfvaText: "valid"};
                });
            };
            fakeNet.neighbours = listed();
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(50);
            var rows = neighbourRows(sheet);
            compare(rows.length, 8);
            for (var k = 0; k < rows.length; ++k)
                verify(rows[k].text.indexOf(" Site:" + k + " ") >= 0, "row " + k + " is the " + k + "th by frequency: "
                       + rows[k].text);
            var list = rows[0].parent;
            var texts = rows.map(function (r) { return r.text; });
            testContext.startAccessibilityCount();
            var wrong = [];
            for (var refresh = 1; refresh <= 16; ++refresh) {
                // The last neighbour in the list is heard again and moves to the top of the model's list.
                order = [order[order.length - 1]].concat(order.slice(0, order.length - 1));
                fakeNet.neighbours = listed();
                wait(50);
                var now = neighbourRows(sheet);
                compare(now, rows, "no row moves");
                for (k = 0; k < now.length; ++k) {
                    if (now[k].text !== texts[k] || testContext.accessibleNode(now[k]).name !== now[k].text)
                        wrong.push(refresh + ":" + k);
                }
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(wrong.length, 0, "rows changed or read as another neighbour than they show: " + JSON.stringify(wrong));
            for (var type of ["ObjectShow", "ObjectHide", "NameChanged", "LocationChanged"])
                compare(testContext.accessibilityCountWithin(list, type), 0, type + ": " + JSON.stringify(counts.chains));
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.neighbours = [];
        }

        // Issue #782: two neighbours may announce one frequency (the decoder keeps them apart by system, RFSS, site and
        // WACN). Each keeps its own row and place as the model's most-recent-first order swaps them: nothing is raised.
        function test_open_network_sheet_tells_shared_frequencies_apart() {
            var sheet = networkLoader.item;
            var neighbour = function (rfss) {
                return {freqHz: 851500000, sysid: 291, rfss: rfss, site: 4, wacn: 1, wacnValid: true, lra: 0,
                    lraValid: false, isCurrentCc: false, isCandidate: false, cfvaText: "valid"};
            };
            fakeNet.neighbours = [neighbour(1), neighbour(2)];
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(50);
            var rows = neighbourRows(sheet);
            compare(rows.length, 2);
            var texts = rows.map(function (r) { return r.text; });
            var list = rows[0].parent;
            testContext.startAccessibilityCount();
            for (var refresh = 1; refresh <= 10; ++refresh) {
                fakeNet.neighbours = refresh % 2 ? [neighbour(2), neighbour(1)] : [neighbour(1), neighbour(2)];
                wait(20);
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            for (var type of ["ObjectShow", "ObjectHide", "NameChanged", "LocationChanged"])
                compare(testContext.accessibilityCountWithin(list, type), 0, type + ": " + JSON.stringify(counts.chains));
            compare(neighbourRows(sheet), rows);
            compare(rows.map(function (r) { return r.text; }), texts, "each row keeps its neighbour");
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.neighbours = [];
        }

        // Issue #782: the affiliations and radios are the 100 heard last, so on a busy system a radio that registers
        // again enters the list and the 100th heard leaves. The row of the one that left is renamed for the new one,
        // and no other row: a listener on a radio that stays keeps its row and its name. (With a screen reader on, as
        // here, the radios are in RID order, so the new radios come last.)
        function test_open_network_sheet_capped_arrivals() {
            var sheet = networkLoader.item;
            var rids = [];
            for (var i = 0; i < 100; ++i)
                rids.push(1000 + i);
            var listed = function () {
                return rids.map(function (rid) { return {rid: rid}; });
            };
            var radioRows = function () {
                var found = [];
                (function collect(item) {
                    for (var k = 0; k < item.children.length; ++k) {
                        var child = item.children[k];
                        if (child.textFormat !== undefined && /^RID:[0-9]+$/.test(child.text) && drawn(child))
                            found.push(child);
                        collect(child);
                    }
                })(sheet);
                return found;
            };
            fakeNet.radios = listed();
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(50);
            var rows = radioRows();
            compare(rows.length, 100);
            var row = rows[50];
            var name = testContext.accessibleNode(row).name;
            compare(name, "RID:1050");
            var wrong = [];
            var arrivals = 20;
            for (var arrival = 1; arrival <= arrivals; ++arrival) {
                testContext.startAccessibilityCount();
                rids = [5000 + arrival].concat(rids.slice(0, 99));
                fakeNet.radios = listed();
                wait(20);
                var counts = testContext.accessibilityCount();
                testContext.stopAccessibilityCount();
                compare(showHide(sheet), 0, JSON.stringify(counts.chains));
                verify(testContext.accessibilityCountWithin(sheet, "NameChanged") <= 1, "arrival " + arrival + " renamed "
                       + testContext.accessibilityCountWithin(sheet, "NameChanged") + " rows");
                rows = radioRows();
                compare(rows.length, 100);
                for (var k = 0; k < rows.length; ++k) {
                    if (testContext.accessibleNode(rows[k]).name !== rows[k].text)
                        wrong.push(arrival + ":" + k);
                }
                compare(rows[50], row, "the radio keeps its row");
            }
            compare(wrong.length, 0, "rows read as another radio than they show: " + JSON.stringify(wrong));
            compare(testContext.accessibleNode(row).name, name);
            compare(rows[99].text, "RID:" + (5000 + arrivals));
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.radios = [];
        }

        // Issue #782: a hop to a target with no P25 network holds the rows, and Android's bridge must not keep reading
        // them: Qt sends it no event as they leave the tree, so its cached copy of the sheet is refreshed only by a
        // show or hide under it. Each held row reports itself invisible and unnamed, so a cached or focused copy
        // reads nothing; "(none announced)" stays in the tree while hidden, so it shows (refreshing the cache) as the
        // section empties, and a swipe reaches it. Filled again, the rows are read as themselves and the line hides.
        function test_open_network_sheet_hold_reads_nothing() {
            var sheet = networkLoader.item;
            var listed = neighbours(8, 0, 3);
            fakeNet.neighbours = listed;
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(50);
            var rows = neighbourRows(sheet);
            compare(rows.length, 8);
            var none = noneLines(sheet)[0];
            testContext.startAccessibilityCount();
            fakeNet.neighbours = [];
            wait(20);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            for (var held of rows) {
                var node = testContext.accessibleNode(held);
                verify(node.invisible, "a held row reports itself invisible: " + held.text);
                compare(node.name, "", "and reads nothing");
                verify(!node.inTree);
            }
            var line = testContext.accessibleNode(none);
            verify(line.inTree && !line.invisible, "the section's \"(none announced)\" is reachable");
            compare(line.name, "(none announced)");
            compare(testContext.accessibilityCountWithin(none, "ObjectShow"), 1, "and shows, once: "
                    + JSON.stringify(counts.chains));
            compare(showHide(sheet), 1, "nothing else shows or hides: " + JSON.stringify(counts.chains));
            testContext.startAccessibilityCount();
            fakeNet.neighbours = listed;
            wait(20);
            counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(testContext.accessibilityCountWithin(none, "ObjectHide"), 1, JSON.stringify(counts.chains));
            compare(showHide(sheet), 1, JSON.stringify(counts.chains));
            compare(neighbourRows(sheet), rows, "the same rows");
            for (var r of rows) {
                node = testContext.accessibleNode(r);
                verify(node.inTree && !node.invisible);
                compare(node.name, r.text);
            }
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.neighbours = [];
        }

        // Issue #782: a shrinking list's departed rows are held, not removed: from the moment the list shrinks each
        // is out of the tree, reports itself invisible and unnamed, and takes no room, and none shows or hides. A
        // longer list later reuses them, read as their new entries, and nothing shows or hides then either.
        function test_open_network_sheet_shrink_holds_departed_rows() {
            var sheet = networkLoader.item;
            fakeNet.neighbours = neighbours(8, 0, 3);
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(50);
            var rows = neighbourRows(sheet);
            compare(rows.length, 8);
            var list = rows[0].parent;
            var sixHigh = rows[5].y + rows[5].height;
            var held = allNeighbourRows(sheet).length;
            testContext.startAccessibilityCount();
            fakeNet.neighbours = neighbours(6, 0, 3);
            // Before the event loop runs again.
            compare(neighbourRows(sheet), rows.slice(0, 6), "only the six rows left are drawn");
            for (var k = 6; k < 8; ++k) {
                var node = testContext.accessibleNode(rows[k]);
                verify(node.invisible, "a departed row is not drawn: " + rows[k].text);
                compare(node.name, "", "nor read");
                verify(!node.inTree, "nor in the tree");
            }
            wait(20);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            var type;
            compare(allNeighbourRows(sheet).length, held, "the departed rows are held");
            compare(list.height, sixHigh, "below the rows drawn, taking no room");
            for (type of ["LocationChanged", "DescriptionChanged"])
                compare(testContext.accessibilityCountWithin(list, type), 0, type + ": " + JSON.stringify(counts.chains));
            compare(counts.types.ObjectHide || 0, 0, JSON.stringify(counts.chains));
            compare(counts.types.ObjectShow || 0, 0, JSON.stringify(counts.chains));
            // Unnamed once as it leaves, and not named again: a row TalkBack is on would read it out.
            compare(counts.types.NameChanged || 0, 2, "one rename a departed row: " + JSON.stringify(counts.chains));
            testContext.startAccessibilityCount();
            fakeNet.neighbours = neighbours(8, 1, 3);
            wait(20);
            counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(neighbourRows(sheet), rows, "the held rows come back");
            compare(counts.types.ObjectHide || 0, 0, JSON.stringify(counts.chains));
            compare(counts.types.ObjectShow || 0, 0, JSON.stringify(counts.chains));
            for (var r of rows) {
                node = testContext.accessibleNode(r);
                verify(node.inTree && !node.invisible);
                compare(node.name, r.text);
            }
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.neighbours = [];
        }

        // Issue #782: hops between P25 targets of very different sizes (100 radios and 1), the Network sheet open. The
        // rows the small target does not need are held, and the big target's next list takes them back, so no hop
        // shows or hides a row, however many radios the targets list, and every drawn row reads its own radio.
        function test_open_network_sheet_unequal_hops_hold_rows() {
            var sheet = networkLoader.item;
            var radios = function (n, base) {
                var listed = [];
                for (var i = 0; i < n; ++i)
                    listed.push({rid: base + i});
                return listed;
            };
            var radioRows = function (all) {
                var found = [];
                (function collect(item) {
                    for (var k = 0; k < item.children.length; ++k) {
                        var child = item.children[k];
                        if (child.textFormat !== undefined && /^RID:[0-9]+$/.test(child.text) && (all || drawn(child)))
                            found.push(child);
                        collect(child);
                    }
                })(sheet);
                return found;
            };
            fakeNet.radios = radios(100, 1000);
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(50);
            compare(radioRows(false).length, 100);
            testContext.startAccessibilityCount();
            var hops = 6;
            for (var hop = 1; hop <= hops; ++hop) {
                fakeNet.radios = hop % 2 === 1 ? radios(1, 7000 + hop) : radios(100, 1000 + hop);
                wait(20);
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(counts.types.ObjectShow || 0, 0, JSON.stringify(counts.chains));
            compare(counts.types.ObjectHide || 0, 0, JSON.stringify(counts.chains));
            compare(radioRows(true).length, 100, "no row is added or removed");
            var rows = radioRows(false);
            compare(rows.length, 100);
            var wrong = [];
            for (var k = 0; k < rows.length; ++k) {
                if (testContext.accessibleNode(rows[k]).name !== rows[k].text)
                    wrong.push(k);
            }
            compare(wrong.length, 0, "rows read as another radio than they show: " + JSON.stringify(wrong));
            fakeNet.radios = radios(1, 9000);
            wait(20);
            rows = radioRows(true);
            var inTree = rows.filter(function (row) {
                return testContext.accessibleNode(row).inTree;
            });
            compare(inTree.length, 1, "only the one radio's row is in the tree");
            compare(inTree[0].text, "RID:9000");
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.radios = [];
        }

        // Issue #782: a list emptied and filled again in one turn (a publisher that clears first) keeps every row:
        // nothing shows, hides or is renamed. Filled again a turn later, the rows are kept too, out of the tree in
        // between, and only the "(none announced)" line comes and goes.
        function test_open_network_sheet_clear_and_refill() {
            var sheet = networkLoader.item;
            var listed = neighbours(8, 0, 3);
            fakeNet.neighbours = listed;
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            waitForRendering(sheet);
            wait(50);
            var rows = neighbourRows(sheet);
            compare(rows.length, 8);
            testContext.startAccessibilityCount();
            for (var cycle = 1; cycle <= 10; ++cycle) {
                fakeNet.neighbours = [];
                fakeNet.neighbours = listed;
                wait(20);
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(showHide(sheet), 0, JSON.stringify(counts.chains));
            compare(testContext.accessibilityCountWithin(sheet, "NameChanged"), 0, JSON.stringify(counts.chains));
            compare(neighbourRows(sheet), rows, "the same rows");
            testContext.startAccessibilityCount();
            for (cycle = 1; cycle <= 10; ++cycle) {
                fakeNet.neighbours = [];
                wait(20);
                compare(neighbourRows(sheet).length, 0, "an empty list shows no row");
                verify(!testContext.accessibleNode(rows[3]).inTree, "nor reads one");
                fakeNet.neighbours = listed;
                wait(20);
            }
            counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            var none = noneLines(sheet)[0];
            compare(testContext.accessibilityCountWithin(none, "ObjectShow"), 10, JSON.stringify(counts.chains));
            compare(testContext.accessibilityCountWithin(none, "ObjectHide"), 10, JSON.stringify(counts.chains));
            compare(showHide(sheet), 20, "the \"(none announced)\" line alone shows and hides: "
                    + JSON.stringify(counts.chains));
            compare(neighbourRows(sheet), rows, "the same rows");
            for (var r of rows)
                compare(testContext.accessibleNode(r).name, r.text);
            sheet.visible = false;
            Ui.Navigation.modals = [];
            fakeNet.neighbours = [];
        }

        // Issue #782: a trunk scan's hops with the Network sheet open. Each hop publishes the new target's network
        // as UiController does, and the sheet keeps every row both targets list: no row shows or hides, the row a
        // listener is on stays, and each row ends on the new target's entry.
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
            // Two rows are held on each hop to the six-neighbour target and taken back on the next: none shows or
            // hides.
            compare(counts.types.ObjectHide || 0, 0, JSON.stringify(counts.chains));
            compare(counts.types.ObjectShow || 0, 0, JSON.stringify(counts.chains));
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

        // Issue #782: a scan list that mixes a P25 system with DMR or analog targets, the Network sheet open on the
        // P25 one. A hop to a target with no P25 network leaves the rows out of the tree and out of sight, and the hop
        // back brings the same rows back, so a hop costs the "(none announced)" line and the sheet's layout, the same
        // for 30 neighbours as for 8, and no row shows, hides or is renamed. No P25 row is shown on the other targets.
        function mixedHops(sites) {
            var sheet = liveNetworkLoader.item;
            sheet.visible = true;
            Ui.Navigation.modals = [sheet];
            testContext.hopNetwork(sites);
            waitForRendering(sheet);
            wait(50);
            var rows = neighbourRows(sheet);
            compare(rows.length, sites.length);
            var list = rows[0].parent;
            testContext.startAccessibilityCount();
            var hops = 10;
            for (var hop = 1; hop <= hops; ++hop) {
                testContext.hopNetwork(hop % 2 ? [] : sites);
                wait(20);
                if (hop % 2) {
                    compare(neighbourRows(sheet).length, 0, "no P25 row on the other target");
                    for (var held of rows) {
                        var node = testContext.accessibleNode(held);
                        verify(!node.inTree && node.invisible && node.name === "", "nor is one read");
                    }
                    compare(textsIn(sheet, "(none announced)"), 4);
                }
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            // What Android's bridge turns into main-thread work: the same whatever the number of rows.
            var forwarded = 0;
            for (var type of ["ObjectShow", "ObjectHide", "DescriptionChanged", "LocationChanged"]) {
                compare(testContext.accessibilityCountWithin(list, type), 0, type + " from the rows: "
                        + JSON.stringify(counts.chains));
                forwarded += testContext.accessibilityCountWithin(sheet, type);
            }
            // Each row is unnamed as it is held and named again as it comes back: one call into Java each, which
            // returns at once for a row TalkBack is not on (QtAccessibilityDelegate.notifyDescriptionOrNameChanged).
            compare(testContext.accessibilityCountWithin(list, "NameChanged"), rows.length * hops,
                    JSON.stringify(counts.chains));
            // The neighbours' "(none announced)" line shows going and hides coming back; no other section changes.
            var none = noneLines(sheet)[0];
            compare(testContext.accessibilityCountWithin(none, "ObjectShow"), hops / 2, JSON.stringify(counts.chains));
            compare(testContext.accessibilityCountWithin(none, "ObjectHide"), hops / 2, JSON.stringify(counts.chains));
            compare(showHide(sheet), hops, showHide(sheet) + " shows and hides for " + hops + " hops: "
                    + JSON.stringify(counts.chains));
            compare(neighbourRows(sheet), rows, "the same rows come back");
            for (var r of rows)
                compare(testContext.accessibleNode(r).name, r.text);
            sheet.visible = false;
            Ui.Navigation.modals = [];
            testContext.hopNetwork([]);
            return forwarded;
        }
        function textsIn(item, text) {
            var found = item.text === text && item.visible ? 1 : 0;
            for (var k = 0; k < item.children.length; ++k)
                found += textsIn(item.children[k], text);
            return found;
        }
        function test_open_network_sheet_on_mixed_hops() {
            var eight = mixedHops([0, 1, 2, 3, 4, 5, 6, 7]);
            var thirty = [];
            for (var site = 0; site < 30; ++site)
                thirty.push(site);
            compare(mixedHops(thirty), eight, "a hop costs the same whatever the number of rows");
        }

        // Issue #782: a call on one slot after a call on two: the second slot's row goes at the end of the turn, and
        // until then it is hidden, still reading its last slot, never an empty one.
        function test_open_key_sheet_drops_a_slot_at_once() {
            testContext.publishTarget({decryptionSlots: slots(0)});
            keySheet.open();
            keySheet.visible = true;
            Ui.Navigation.modals = [keySheet];
            var rows = findChild(keySheet, "keySheetSlots");
            waitForRendering(keySheet);
            wait(20);
            compare(rows.count, 2);
            var second = rows.itemAt(1);
            var text = second.children[0];
            var said = text.text;
            verify(said.indexOf("Slot 2") === 0, said);
            testContext.startAccessibilityCount();
            testContext.publishTarget({decryptionSlots: slots(1)});
            // Before the event loop runs again.
            verify(!second.visible, "the row of the slot that left is hidden at once");
            compare(text.text, said, "and does not read an empty slot");
            tryCompare(rows, "count", 1);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            // Its text and its button hide once, as it hides; it is not shown again as it goes.
            compare(counts.types.ObjectHide || 0, 2, JSON.stringify(counts.chains));
            Ui.Navigation.modals = [];
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
