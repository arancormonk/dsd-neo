// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #782: what a trunk scan costs an accessibility client, end to end through Main.
//
// On Android every ObjectShow and ObjectHide costs the main thread a runnable that makes eight blocking calls into
// the Qt thread. A scan used to raise them on every hop from sheets nobody had open: each hop swung the readings to
// their defaults and back, and a closed sheet whose list model followed a reading rebuilt its delegates, each new
// delegate raising ObjectHide as it was parented under the hidden sheet. The hops here publish each target's
// readings once, as MetricsModel does since the fix (the C++ regression for that is UI_QT_CONTROLLER's
// test_target_change_publishes_once); these cases hold the screens to costing nothing on such hops.
//
// tst_a11y_harness.qml proves first that the counter sees the events these cases assert are absent.
Item {
    width: 420
    height: 900
    Loader {
        id: loader
        source: uiDir + "/Main.qml"
    }
    SignalSpy {
        id: modelSpy
        signalName: "modelChanged"
    }
    TestCase {
        name: "A11yScanChurn"
        when: windowShown
        readonly property var app: loader.item
        readonly property int hops: 30
        // Parts of closed sheets that still churn on a hop, for a reason the root fix does not reach, and the work
        // package that removes it. Only that part is exempt: the rest of its sheet stays under the strict assertion.
        // The part itself is held to an expected failure, which turns into a failure (XPASS) once the package lands,
        // so the package has to take its entry off this list and the strict assertion covers the part from then on.
        // Each entry: {sheet: objectName, part: what churns, wp: the package, find: function (sheet) -> the part}.
        readonly property var pendingFix: []
        property var restore: ({})

        // One target's readings per protocol, as a scan that alternates DMR, NXDN and P25 sites publishes them. The
        // decryption slots stay as the fixture has them: the closed key sheet's churn on those is WP-Q3's, measured
        // in its own case.
        function reading(i) {
            var ordinal = (i % 3) + 1;
            var common = {radioInput: true, optionsKnown: true, scanRotationActive: true, squelchNoiseOffered: true,
                scanTargetCount: 3, scanTargetOrdinal: ordinal, scanTargetId: "target-" + ordinal, syncedHere: true,
                siteLine: "Site " + ordinal, siteConfirmed: true, p25WacnValid: i % 3 === 2, p25Wacn: 0xBEE00,
                p25SysIdValid: i % 3 === 2, p25SysId: 0x3A0, p25Rfss: i % 3 === 2 ? 1 : 0,
                p25Site: i % 3 === 2 ? ordinal : 0, dmrSiteText: i % 3 === 0 ? "Site " + ordinal : "",
                nxdnSysCode: i % 3 === 1 ? 100 + ordinal : 0, nxdnSiteCode: i % 3 === 1 ? ordinal : 0};
            if (i % 3 === 0)
                return Object.assign(common, {siteProtocol: "DMR", twoSlotCarrier: true, syncLabel: "DMR",
                    dmrColorCode: 1, nxdnRan: -1, p25NacValid: false, p25Nac: 0, centerFreqHz: 451012500,
                    channelBandwidthHz: 12500});
            if (i % 3 === 1)
                return Object.assign(common, {siteProtocol: "NXDN", twoSlotCarrier: false, syncLabel: "NXDN48",
                    dmrColorCode: -1, nxdnRan: 5, p25NacValid: false, p25Nac: 0, centerFreqHz: 452500000,
                    channelBandwidthHz: 6250});
            return Object.assign(common, {siteProtocol: "P25", twoSlotCarrier: false, syncLabel: "P25p1",
                dmrColorCode: -1, nxdnRan: -1, p25NacValid: true, p25Nac: 0x293, centerFreqHz: 851012500,
                channelBandwidthHz: 12500});
        }
        // The same readings as MetricsModel::clear() publishes them: what every hop passed through before the fix.
        function cleared() {
            return {radioInput: false, optionsKnown: false, scanRotationActive: false, squelchNoiseOffered: false,
                scanTargetCount: 0, scanTargetOrdinal: 0, scanTargetId: "", syncedHere: false, siteLine: "",
                siteConfirmed: false, p25WacnValid: false, p25Wacn: 0, p25SysIdValid: false, p25SysId: 0, p25Rfss: 0,
                p25Site: 0, dmrSiteText: "", nxdnSysCode: 0, nxdnSiteCode: 0, siteProtocol: "", twoSlotCarrier: false,
                syncLabel: "", dmrColorCode: -1, nxdnRan: -1, p25NacValid: false, p25Nac: 0, centerFreqHz: 0,
                channelBandwidthHz: 0};
        }

        function isSheet(item) {
            return item.closesOnOutsideTap !== undefined && typeof item.requestDismiss === "function";
        }
        function findItem(item, matches) {
            var children = item.children || [];
            for (var i = 0; i < children.length; ++i) {
                if (matches(children[i]))
                    return children[i];
                var found = findItem(children[i], matches);
                if (found !== null)
                    return found;
            }
            return null;
        }
        function isWithin(item, ancestor) {
            for (var at = item; at; at = at.parent) {
                if (at === ancestor)
                    return true;
            }
            return false;
        }
        function collectSheets(item, out) {
            var children = item.children || [];
            for (var i = 0; i < children.length; ++i) {
                var child = children[i];
                if (isSheet(child))
                    out.push(child);
                collectSheets(child, out);
            }
            return out;
        }
        function sheetName(sheet) {
            return sheet.objectName || sheet.panelObjectName || sheet.accessibleName || String(sheet);
        }
        function showHide(scope) {
            return testContext.accessibilityCountWithin(scope, "ObjectShow")
                + testContext.accessibilityCountWithin(scope, "ObjectHide");
        }
        function typed(counts, type) {
            return counts.types[type] ? counts.types[type] : 0;
        }
        function churn(counts) {
            return JSON.stringify(Object.keys(counts.chains).filter(function (chain) {
                return /^(ObjectShow|ObjectHide|NameChanged) /.test(chain);
            }).map(function (chain) {
                return counts.chains[chain] + "x " + chain;
            }));
        }
        function closedSheets() {
            var sheets = collectSheets(app.contentItem, []);
            var closed = sheets.filter(function (sheet) { return !sheet.visible; });
            compare(closed.length, sheets.length, "every sheet starts closed");
            verify(!findChild(app, "talkgroupsScreen").visible, "the talkgroups screen starts closed");
            var names = closed.map(sheetName);
            for (var name of ["sessionMenu", "radioSheet", "squelchSheet", "keySheet"])
                verify(names.indexOf(name) >= 0, name + " must be among the closed sheets: " + JSON.stringify(names));
            return closed;
        }

        function initTestCase() {
            // This file asserts that events are absent, so it must not run with nothing being raised; the harness
            // file's controls guard the directory run, this guards a run of this file alone.
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            var first = reading(0);
            var defaults = cleared();
            for (var key in first) {
                verify(key in defaults, key + " needs its cleared value, or case C would hop past it");
                restore[key] = metrics[key];
            }
        }
        function init() {
            failOnWarning(/Binding loop detected/);
            verify(app !== null);
            testContext.useLifecycleHost(true);
            testContext.setLifecyclePhase(2);
            prefs.onboardingDone = true;
            app.currentTab = 0;
            app.requestActivate();
            tryCompare(app, "active", true);
            testContext.publishTarget(reading(0));
            verify(app.monitorMode, "the scan runs on the monitor");
            var keys = findChild(app, "keySheet");
            tryCompare(keys, "targetName", "Target 1");
        }
        function cleanup() {
            // A case that changed the screen reader's pace puts the app's back, whatever it failed on.
            Ui.Navigation.speechPeriodMs = 5000;
            Ui.Navigation.speechStepFloorMs = 1000;
            testContext.stopAccessibilityCount();
            testContext.publishTarget(restore);
            testContext.setLifecyclePhase(0);
            testContext.useLifecycleHost(false);
        }

        // Issue #901 r4 F3: the other slot's optional label is drawn text with no node of its own. One stable node,
        // the identity line, reads the slot with the call (or the idle state). At 320 dp a short identity left room
        // for the label and a long one did not, so each change hid or showed the label's node; hidden, it still
        // took the label's text as its name, so six lead-slot changes renamed it six times, under the session's
        // Settings too. Now neither a fit change nor a lead-slot change shows, hides or renames anything but the
        // identity line, whose name holds the slot whether the label is drawn or not, and which says nothing while
        // the Monitor is covered.
        function setSlotCall(slot, name) {
            var p = "slot" + slot;
            var v = {};
            v[p + "CallKind"] = 2;
            v[p + "CallName"] = name;
            v[p + "TgText"] = name;
            v[p + "TgId"] = 4000 + slot;
            v[p + "TgLabel"] = name;
            v[p + "SrcText"] = "700" + slot;
            v[p + "ProtocolLabel"] = "DMR";
            v[p + "CallSeconds"] = 12;
            v[p + "CallState"] = 2;
            testContext.setMetrics(v);
        }
        function test_g_other_slot_label_has_no_node_of_its_own() {
            var monitor = findChild(app, "monitorScreen");
            var panel = findChild(monitor, "otherSlotPanel");
            var label = findChild(monitor, "otherSlotLabel");
            var identity = findChild(monitor, "otherSlotNode");
            var drawnIdentity = findChild(monitor, "otherSlotIdentity");
            var width = app.width;
            app.width = 320;
            try {
                setSlotCall(1, "4001");
                setSlotCall(2, "4002");
                testContext.setMetrics({leadSlot: 1});
                waitForRendering(app.contentItem);
                wait(20);
                verify(panel.visible, "the other slot's strip is shown");
                verify(panel.plan.label, "a short identity leaves the label room");
                var node = testContext.accessibleNode(identity);
                verify(node.exists && node.inTree, "the identity line is a node");
                var unread = [];
                var noteRead = function (when) {
                    var name = testContext.accessibleNode(identity).name;
                    if (!/SLOT 2/.test(name))
                        unread.push(when + ": " + name);
                };
                noteRead("label drawn");
                testContext.startAccessibilityCount();
                for (var i = 0; i < 3; ++i) {
                    setSlotCall(2, "Dispatcher North Zone Seven Long");
                    waitForRendering(app.contentItem);
                    verify(!panel.plan.label, "a long identity leaves the label out");
                    noteRead("label left out");
                    setSlotCall(2, "4002");
                    waitForRendering(app.contentItem);
                    verify(panel.plan.label, "a short identity draws it again");
                }
                for (i = 0; i < 6; ++i) {
                    testContext.setMetrics({leadSlot: i % 2 === 0 ? 2 : 1});
                    waitForRendering(app.contentItem);
                }
                var counts = testContext.accessibilityCount();
                testContext.stopAccessibilityCount();
                compare(showHide(label), 0, "the label showed or hid: " + churn(counts));
                compare(testContext.accessibilityCountWithin(label, "NameChanged"), 0, "the label renamed: " + churn(counts));
                compare(showHide(panel), 0, "the strip showed or hid something: " + churn(counts));
                compare(unread.length, 0, "the identity line does not read the slot: " + JSON.stringify(unread));
                // The sweep: every optional item the strip, the hero's subline and a call row draw by fit is drawn
                // text or a drawn tag with no node in the tree, which its node-owning line reads instead, so a fit
                // change raises nothing from it.
                callHistory.push("TODAY");
                var list = findChild(monitor, "recentCallsList");
                tryVerify(function () { return list.itemAtIndex(0) !== null; }, 1000, "a call row");
                var optional = [label, drawnIdentity, findChild(monitor, "otherPrivateTag"), findChild(monitor, "otherEmergencyTag"),
                                findChild(monitor, "otherEncTag")];
                for (var name of ["heroPrivateTag", "heroIds", "heroIdentityNote", "heroEmergencyTag", "heroEncTag",
                                  "heroAlgorithm", "heroChannel"])
                    optional.push(findChild(monitor, name));
                for (name of ["rowEmergencyTag", "rowRecordingGlyph", "rowEncTag", "callRowTime", "callRowName", "callRowMeta"])
                    optional.push(findChild(list.itemAtIndex(0), name));
                for (var item of optional) {
                    verify(item !== null, "an optional item is missing");
                    verify(!testContext.accessibleNode(item).inTree, item.objectName + " has a node of its own");
                }
                // Covered by the session's Settings: the identity says nothing and nothing in the strip renames.
                app.sessionDestination = "settings";
                tryVerify(function () { return !Ui.Navigation.allows(identity); }, 1000, "the Settings cover the Monitor");
                waitForRendering(app.contentItem);
                compare(testContext.accessibleNode(identity).name, "", "covered, the identity is unnamed");
                testContext.startAccessibilityCount();
                for (i = 0; i < 6; ++i) {
                    testContext.setMetrics({leadSlot: i % 2 === 0 ? 2 : 1});
                    setSlotCall(2, i % 2 === 0 ? "Dispatcher North Zone Seven Long" : "4002");
                    waitForRendering(app.contentItem);
                }
                counts = testContext.accessibilityCount();
                testContext.stopAccessibilityCount();
                compare(testContext.accessibilityCountWithin(panel, "NameChanged"), 0, "covered, the strip renamed: " + churn(counts));
                compare(showHide(panel), 0, "covered, the strip showed or hid: " + churn(counts));
            } finally {
                testContext.stopAccessibilityCount();
                app.sessionDestination = "";
                app.width = width;
                testContext.setMetrics({leadSlot: 0, slot1CallState: 0, slot2CallState: 0, slot1CallName: "", slot2CallName: "",
                    slot1TgText: "", slot2TgText: "", slot1TgLabel: "", slot2TgLabel: "", slot1SrcText: "", slot2SrcText: ""});
            }
        }

        // Issue #901 r5 H2: the strip's one node covers what it reads, the slot label, the identity and the tags, and
        // stops at Skip. It started after the drawn label (at 411 dp idle the label was at x 16-127 and the node from
        // x 137), so a touch on the label found no node. Idle at 411 dp and with a call at 320 dp.
        function test_h_a_touch_on_the_slot_label_finds_the_strip() {
            var monitor = findChild(app, "monitorScreen");
            var panel = findChild(monitor, "otherSlotPanel");
            var label = findChild(monitor, "otherSlotLabel");
            var drawnIdentity = findChild(monitor, "otherSlotIdentity");
            var skip = findChild(monitor, "otherSlotSkip");
            var width = app.width;
            var touched = function (item) {
                var hit = testContext.accessibleNodeAt(item, item.width / 2, item.height / 2);
                return hit.exists && hit.object ? hit.object.objectName : "";
            };
            try {
                for (var step of [{width: 411, active: false}, {width: 320, active: true}]) {
                    app.width = step.width;
                    setSlotCall(1, "4001");
                    setSlotCall(2, "4002");
                    testContext.setMetrics({leadSlot: 1, slot2CallState: step.active ? 2 : 0});
                    waitForRendering(app.contentItem);
                    wait(20);
                    verify(panel.visible && label.visible, step.width + ": the label is drawn");
                    compare(touched(label), "otherSlotNode", step.width + ": a touch on the label");
                    if (step.active) {
                        verify(drawnIdentity.visible);
                        compare(touched(drawnIdentity), "otherSlotNode", "a touch on the identity");
                        verify(touched(skip) !== "otherSlotNode", "a touch on Skip finds the line");
                    }
                }
            } finally {
                app.width = width;
                testContext.setMetrics({leadSlot: 0, slot1CallState: 0, slot2CallState: 0, slot1CallName: "", slot2CallName: "",
                    slot1TgText: "", slot2TgText: "", slot1TgLabel: "", slot2TgLabel: "", slot1SrcText: "", slot2SrcText: ""});
            }
        }

        // A tick that changes no reading costs nothing. Qt already keeps it so (an equal model is not reloaded, an
        // equal name not announced); this guards it.
        function test_a_steady_ticks_raise_nothing() {
            closedSheets();
            testContext.startAccessibilityCount();
            testContext.tickMetrics(20);
            var counts = testContext.accessibilityCount();
            compare(typed(counts, "ObjectShow"), 0, churn(counts));
            compare(typed(counts, "ObjectHide"), 0, churn(counts));
            compare(typed(counts, "NameChanged"), 0, churn(counts));
        }

        // Hops between targets of different protocols, one publication each: no closed sheet shows or hides
        // anything, the session menu among them.
        function test_b_hops_leave_closed_sheets_quiet() {
            var closed = closedSheets();
            testContext.startAccessibilityCount();
            for (var i = 1; i <= hops; ++i)
                testContext.publishTarget(reading(i));
            var counts = testContext.accessibilityCount();
            // The hops reached the screens: the key sheet follows the target's name.
            compare(findChild(app, "keySheet").targetName, "Target " + ((hops % 3) + 1));
            var parts = pendingFix.map(function (entry) {
                var sheet = findChild(app, entry.sheet);
                verify(sheet !== null && !sheet.visible, entry.sheet + " must be closed");
                var part = entry.find(sheet);
                verify(part !== null, entry.part + " must be in " + entry.sheet);
                return {entry: entry, item: part};
            });
            var noisy = [];
            for (var sheet of closed) {
                var events = showHide(sheet);
                for (var held of parts) {
                    if (isWithin(held.item, sheet))
                        events -= showHide(held.item);
                }
                if (events !== 0)
                    noisy.push(sheetName(sheet) + ": " + events);
            }
            compare(noisy.length, 0, "closed sheets that showed or hid something: " + JSON.stringify(noisy) + " "
                    + churn(counts));
            for (var pending of parts) {
                expectFailContinue("", "TODO(" + pending.entry.wp + "): " + pending.entry.part + " in "
                                   + pending.entry.sheet + " still churns on a hop");
                compare(showHide(pending.item), 0, pending.entry.part + " in " + pending.entry.sheet + ": "
                        + churn(counts));
            }
        }

        // The closed key sheet sees the targets change without rebuilding its scope choice: the choice is a fixed
        // pair, so its model never changes and its popup never reparents a delegate under the hidden sheet. The name
        // line holds the target the sheet last opened on, the one a target-scope Apply addresses, until it opens again.
        function test_b2_target_names_leave_the_closed_key_sheet_quiet() {
            closedSheets();
            var keys = findChild(app, "keySheet");
            var scope = findChild(keys, "keySheetScope");
            verify(scope !== null, "the key sheet's scope choice is missing");
            var line = findChild(keys, "keySheetTargetName");
            verify(line !== null, "the key sheet's target name line is missing");
            var held = line.text;
            modelSpy.target = scope;
            modelSpy.clear();
            testContext.startAccessibilityCount();
            for (var i = 0; i < 10; ++i) {
                var before = keys.targetName;
                testContext.publishTarget({scanTargetOrdinal: (i % 2) + 2, scanTargetId: "target-" + ((i % 2) + 2)});
                verify(keys.targetName !== before, "change " + (i + 1) + " renamed the target");
                compare(line.text, held, "the closed sheet's line holds its target");
            }
            var counts = testContext.accessibilityCount();
            compare(modelSpy.count, 0, "the scope choice's model changed with the name");
            compare(showHide(keys), 0, churn(counts));
            compare(testContext.accessibilityCountWithin(keys, "NameChanged"), 0, churn(counts));
            testContext.stopAccessibilityCount();
            modelSpy.target = null;
            // Opening captures the scan's current target. (This fixture's lifecycle host is not running a session,
            // so open() takes the target and leaves the sheet closed; tst_key_sheet.qml covers the open sheet.)
            keys.open();
            compare(line.text, "Applies to " + keys.targetName);
            keys.visible = false;
        }

        // A rotation that mixes two-slot DMR with single-slot NXDN and P25 keeps the monitor's layout still once it
        // has visited a two-slot carrier: the slot strip's place stays reserved, so the scan buttons and the call
        // list never move, and no geometry change reaches an accessibility client on a hop. The slot's node comes
        // and goes with the carrier, which is the slot's truth, and is the one thing allowed to show or hide: the
        // strip line's node (`otherSlotNode`), which reads the idle state too (issue #901 r4 F3, r5 H2: the label
        // it replaced as that node is drawn text now).
        function test_d_two_slot_hops_leave_the_monitor_still() {
            var monitor = findChild(app, "monitorScreen");
            verify(monitor !== null && monitor.visible, "the monitor is on screen");
            var label = findChild(monitor, "otherSlotNode");
            var still = ["scanHoldButton", "scanAvoidButton", "scanPrevButton", "scanNextButton", "recentCallsList"].map(function (name) {
                var found = findChild(monitor, name);
                verify(found !== null && found.visible, name + " must be on screen");
                return found;
            });
            // init() published reading(0), a DMR target, so the rotation has already visited a two-slot carrier.
            verify(label.visible, "the strip shows on the two-slot carrier");
            testContext.startAccessibilityCount();
            for (var i = 1; i <= hops; ++i) {
                testContext.publishTarget(reading(i));
                compare(label.visible, i % 3 === 0, "hop " + i + ": the slot's node follows the carrier");
            }
            var counts = testContext.accessibilityCount();
            for (var item of still)
                compare(testContext.accessibilityCountWithin(item, "LocationChanged"), 0, item.objectName + ": "
                        + JSON.stringify(counts.chains));
            var shown = showHide(app.contentItem) - showHide(label);
            compare(shown, 0, "only the slot's node shows or hides: " + churn(counts));
            // The slot's node hides on every hop away from DMR (Qt reports its return as a creation, which Android does not
            // forward), and never more than once per carrier change.
            var changes = 2 * Math.floor(hops / 3);
            verify(showHide(label) >= changes / 2 && showHide(label) <= changes, churn(counts));
        }

        // Calls landing while the monitor is up reach its own list and nothing else: the Home and History screens
        // under it hold no rows while hidden, so no row is created, moved or destroyed there for each call. Before,
        // every call built a row in each of the three hidden lists, each one raising ObjectHide as it was parented
        // under the hidden screen.
        function test_e_calls_leave_hidden_lists_quiet() {
            var monitorList = findChild(app, "recentCallsList");
            var hidden = ["homeScreen", "idleHistoryScreen", "sessionHistoryScreen"].map(function (name) {
                var screen = findChild(app, name);
                verify(screen !== null && !screen.visible, name + " must be hidden under the monitor");
                return screen;
            });
            var before = monitorList.count;
            testContext.startAccessibilityCount();
            // One frame per call, as calls land in a session: an item view lays out its rows when it is polished.
            for (var i = 0; i < 10; ++i) {
                callHistory.push("TODAY");
                waitForRendering(app.contentItem);
            }
            var counts = testContext.accessibilityCount();
            compare(monitorList.count, before + 10, "the calls reached the monitor");
            var noisy = [];
            for (var screen of hidden) {
                var events = 0;
                for (var type of Object.keys(counts.types))
                    events += testContext.accessibilityCountWithin(screen, type);
                if (events !== 0)
                    noisy.push(screen.objectName + ": " + events);
            }
            compare(noisy.length, 0, "hidden screens that raised events: " + JSON.stringify(noisy) + " "
                    + JSON.stringify(counts.chains));
        }

        // The control for case B: hops that pass through the defaults first, as every hop did before the fix, flood
        // the closed sheets the root fix quietens. So case B cannot pass because the hops stopped reaching these
        // sheets. It guards the screens against a publication that passes through the defaults; whether production
        // publishes that way is UI_QT_CONTROLLER's test_target_change_publishes_once to catch, not this file's.
        function test_c_hops_through_defaults_flood_closed_sheets() {
            closedSheets();
            testContext.startAccessibilityCount();
            for (var i = 1; i <= hops; ++i) {
                testContext.publishTarget(cleared());
                testContext.publishTarget(reading(i));
            }
            var counts = testContext.accessibilityCount();
            for (var name of ["sessionMenu", "radioSheet", "squelchSheet"])
                verify(showHide(findChild(app, name)) >= hops, name + ": " + churn(counts));
        }

        // Issue #782 (T9): the monitor's readings are now read by TalkBack, each group of them (a label and its value,
        // a strip of readings) one node named by what it shows at the readings' pace (SpeechPacer, renaming 1 ms after
        // a step). The event loop runs between hops, so those renames are seen: with the details open, each hop
        // renames a changed group once, no reading group shows, hides or moves, and the texts under the closed
        // sheets, which hold no name while hidden, rename nothing. The step floor is off here, so each hop is a step
        // heard on its own (tst_a11y_paced_names.qml holds a flapping step to the floor).
        function test_f_hops_with_details_open_rename_once_each() {
            var monitor = findChild(app, "monitorScreen");
            // The slot's node (r4 F3, r5 H2), as in case D.
            var label = findChild(monitor, "otherSlotNode");
            var closed = closedSheets();
            var floor = Ui.Navigation.speechStepFloorMs;
            Ui.Navigation.speechStepFloorMs = 1;
            monitor.setDetailsExpanded(true);
            waitForRendering(app.contentItem);
            wait(20);
            var strip = findChild(monitor, "signalStrip");
            verify(strip.visible, "the signal strip is on screen");
            verify(/FREQ/.test(testContext.accessibleNode(strip).name), "the strip is read: "
                   + JSON.stringify(testContext.accessibleNode(strip)));
            // Each hop is let through once the strip reads its carrier: a fixed pause between hops can end before the
            // 1 ms rename on a loaded machine, and the next hop then lands in the same step.
            var freq = findChild(monitor, "monitorFreqValue");
            var carrier = freq.text;
            testContext.startAccessibilityCount();
            for (var i = 1; i <= hops; ++i) {
                testContext.publishTarget(reading(i));
                verify(freq.text !== carrier, "hop " + i + " moved the carrier from " + carrier);
                carrier = freq.text;
                tryVerify(function () {
                    return testContext.accessibleNode(strip).name.indexOf("FREQ " + carrier) >= 0;
                }, 5000, "hop " + i + ": the strip reads " + carrier + ": " + testContext.accessibleNode(strip).name);
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            Ui.Navigation.speechStepFloorMs = floor;
            compare(showHide(monitor) - showHide(label), 0, "only the slot's node shows or hides: " + churn(counts));
            compare(testContext.accessibilityCountWithin(monitor, "LocationChanged"), 0, JSON.stringify(counts.chains));
            // Each hop moves to another carrier, so the strip (its FREQ) renames once a hop, and nothing more than once.
            var renames = renamesByOwner(counts);
            compare(renames.signalStrip, hops, "the strip renames once a hop: " + JSON.stringify(renames));
            for (var owner in renames)
                verify(renames[owner] <= hops, owner + " renamed more than once a hop: " + JSON.stringify(renames));
            var renamed = [];
            for (var sheet of closed) {
                if (testContext.accessibilityCountWithin(sheet, "NameChanged") !== 0)
                    renamed.push(sheetName(sheet));
            }
            compare(renamed.length, 0, "closed sheets that renamed something: " + JSON.stringify(renamed) + " "
                    + churn(counts));
            monitor.setDetailsExpanded(false);
        }
        // NameChanged and DescriptionChanged by the object that raised them.
        function renamesByOwner(counts) {
            var out = {};
            for (var chain in counts.chains) {
                var match = /^(NameChanged|DescriptionChanged) (\w+)/.exec(chain);
                if (match)
                    out[match[2]] = (out[match[2]] || 0) + counts.chains[chain];
            }
            return out;
        }

        // Hops between a digital and an analog target hide the readings the other kind does not have: the decode
        // quality group leaving an analog target, the received tone and tone filter (with its Edit button, as before
        // T9) leaving a digital one. Content that differs, one hide each per change, as the open site sheet's field
        // groups; no show (a reading returns as a creation, which Android does not forward).
        function test_f2_mixed_hops_hide_what_differs() {
            var monitor = findChild(app, "monitorScreen");
            var mixed = function (i) {
                var digital = i % 2 === 0;
                return Object.assign(reading(0), {scanTargetOrdinal: (i % 2) + 1, scanTargetId: "target-" + ((i % 2) + 1),
                    syncedHere: digital, siteProtocol: digital ? "P25" : "", syncLabel: digital ? "P25p1" : "",
                    twoSlotCarrier: false, centerFreqHz: digital ? 851012500 : 154430000, qualityValid: digital,
                    ccFecValid: digital, ccFecOkPct: 95, rxToneVisible: !digital,
                    rxToneText: digital ? "" : "CTCSS 100.0", toneFilterVisible: !digital});
            };
            for (var key of ["qualityValid", "ccFecValid", "ccFecOkPct", "rxToneVisible", "rxToneText", "toneFilterVisible"]) {
                if (!(key in restore))
                    restore[key] = metrics[key];
            }
            monitor.setDetailsExpanded(true);
            testContext.publishTarget(mixed(0));
            waitForRendering(app.contentItem);
            wait(20);
            testContext.startAccessibilityCount();
            var changes = 10;
            for (var i = 1; i <= changes; ++i) {
                testContext.publishTarget(mixed(i));
                wait(20);
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            monitor.setDetailsExpanded(false);
            var hides = {};
            for (var chain in counts.chains) {
                var match = /^ObjectHide (\w+)/.exec(chain);
                if (match)
                    hides[match[1]] = (hides[match[1]] || 0) + counts.chains[chain];
            }
            compare(JSON.stringify(hides), JSON.stringify({decodeQualityRow: changes / 2, monitorRxTone: changes / 2,
                monitorToneFilter: changes / 2, monitorToneFilterEdit: changes / 2}), JSON.stringify(counts.chains));
            // No reading group shows. (After case E's calls, the recent calls list's own rows, accessible before T9,
            // come and go as the target changes; they are case E's subject, not this one's.)
            var shows = Object.keys(counts.chains).filter(function (chain) {
                return /^ObjectShow /.test(chain) && !/^ObjectShow recentCallsList /.test(chain);
            });
            compare(shows.length, 0, JSON.stringify(shows));
        }

        // Readings that move on every tick are read at their pace: a short test period, the event loop running
        // between ticks, and the strip renames at most once a period (and once when the count starts), never per
        // tick; nothing shows, hides or moves, whatever the values' widths; ticks that change nothing rename nothing.
        function test_g_moving_readings_rename_at_their_pace() {
            var monitor = findChild(app, "monitorScreen");
            var restoreReadings = {snrValid: metrics.snrValid, snrDb: metrics.snrDb, cfoHz: metrics.cfoHz};
            var period = Ui.Navigation.speechPeriodMs;
            monitor.setDetailsExpanded(true);
            testContext.setMetric("snrValid", true);
            waitForRendering(app.contentItem);
            wait(30);
            testContext.startAccessibilityCount();
            testContext.tickMetrics(20);
            wait(50);
            var steady = testContext.accessibilityCount();
            compare(typed(steady, "NameChanged"), 0, churn(steady));
            testContext.stopAccessibilityCount();
            Ui.Navigation.speechPeriodMs = 100;
            testContext.startAccessibilityCount();
            var changes = 40;
            var start = Date.now();
            for (var t = 0; t < changes; ++t) {
                // From 9.0 dB to 36.3 dB and a CFO from 0 Hz to -1443 Hz: the readings change width as well as value.
                testContext.setMetric("snrDb", 9 + t * 0.7);
                testContext.setMetric("cfoHz", (t % 2 ? 1 : -1) * t * 37);
                wait(10);
            }
            var elapsed = Date.now() - start;
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            var strip = findChild(monitor, "signalStrip");
            var last = Number(9 + (changes - 1) * 0.7).toFixed(1) + " dB";
            tryVerify(function () {
                return testContext.accessibleNode(strip).name.indexOf("SNR " + last) >= 0;
            }, 1000, "the strip catches up: " + testContext.accessibleNode(strip).name);
            Ui.Navigation.speechPeriodMs = period;
            monitor.setDetailsExpanded(false);
            for (var key in restoreReadings)
                testContext.setMetric(key, restoreReadings[key]);
            compare(typed(counts, "ObjectShow") + typed(counts, "ObjectHide"), 0, churn(counts));
            compare(typed(counts, "LocationChanged"), 0, JSON.stringify(counts.chains));
            var renames = renamesByOwner(counts);
            var most = Math.min(Math.ceil(elapsed / 100) + 1, changes / 4);
            verify(renames.signalStrip >= 1 && renames.signalStrip <= most, "the strip renamed " + renames.signalStrip
                   + " times for " + changes + " ticks in " + elapsed + " ms (at most " + most + "): "
                   + JSON.stringify(renames));
            verify(renames.monitorDetailsRow >= 1 && renames.monitorDetailsRow <= most, "the summary redescribed "
                   + renames.monitorDetailsRow + " times: " + JSON.stringify(renames));
        }

        // Closed sheets and the screens hidden under the monitor stay silent over time, not only at once: a short
        // period, moving readings and hops, with the event loop running throughout.
        function test_h_hidden_and_closed_stay_silent_over_time() {
            var closed = closedSheets();
            var hidden = ["homeScreen", "idleHistoryScreen", "sessionHistoryScreen"].map(function (name) {
                var screen = findChild(app, name);
                verify(screen !== null && !screen.visible, name + " must be hidden under the monitor");
                return screen;
            });
            var period = Ui.Navigation.speechPeriodMs;
            Ui.Navigation.speechPeriodMs = 30;
            var restoreReadings = {snrValid: metrics.snrValid, snrDb: metrics.snrDb, squelchAutoStatus: metrics.squelchAutoStatus};
            testContext.startAccessibilityCount();
            for (var i = 1; i <= 20; ++i) {
                testContext.setMetric("snrValid", true);
                testContext.setMetric("snrDb", 9 + i * 0.5);
                testContext.setMetric("squelchAutoStatus", "quieting " + (10 + i) + " dB");
                if (i % 5 === 0)
                    testContext.publishTarget(reading(i));
                wait(15);
            }
            wait(150);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            Ui.Navigation.speechPeriodMs = period;
            for (var key in restoreReadings)
                testContext.setMetric(key, restoreReadings[key]);
            var noisy = [];
            for (var scope of closed.concat(hidden)) {
                var events = 0;
                for (var type of Object.keys(counts.types))
                    events += testContext.accessibilityCountWithin(scope, type);
                if (events !== 0)
                    noisy.push((scope.objectName || sheetName(scope)) + ": " + events);
            }
            compare(noisy.length, 0, "hidden or closed parts that raised events over time: " + JSON.stringify(noisy) + " "
                    + JSON.stringify(counts.chains));
        }

        // Issue #801 [r1-7]: with Home hidden behind the wizard, renaming, removing and unpinning pinned targets raises
        // nothing from the Pinned strip or the closed Pinned sheet; back on Home, the strip shows them as they are.
        // (Home's own saved-system cards follow their model while hidden, as before: a decided limit, not this case's.)
        function test_k_hidden_home_pin_changes_stay_silent() {
            testContext.setLifecyclePhase(0);
            verify(!app.monitorMode);
            testContext.resetPins();
            var uids = [];
            for (var name of ["Alpha", "Bravo", "Charlie"]) {
                verify(savedSystems.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"}));
                uids.push(savedSystems.get(savedSystems.count - 1).uid);
                compare(listenPins.pin("system", uids[uids.length - 1]), "");
            }
            verify(scanLists.add({name: "Patrol", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]}));
            var list = scanLists.get(scanLists.count - 1).uid;
            compare(listenPins.pin("scan", list), "");
            var home = findChild(app, "homeScreen");
            tryCompare(home, "visible", true);
            waitForRendering(home);
            var strip = findChild(home, "pinnedStrip");
            var sheet = findChild(home, "pinnedSheet");
            verify(strip.visible && !sheet.visible);
            app.wizardOpen = true;
            tryCompare(home, "visible", false);
            testContext.startAccessibilityCount();
            verify(savedSystems.update(savedSystems.rowForUid(uids[0]), {name: "Alpha Prime"}));
            verify(scanLists.update(scanLists.rowForUid(list), {name: "Night patrol"}));
            verify(savedSystems.remove(savedSystems.rowForUid(uids[1])));
            listenPins.unpin("system", uids[2]);
            wait(50);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            compare(listenPins.count, 2, "the pins moved on");
            var noisy = [];
            for (var scope of [strip, sheet]) {
                var events = 0;
                for (var type of Object.keys(counts.types))
                    events += testContext.accessibilityCountWithin(scope, type);
                if (events !== 0)
                    noisy.push(scope.objectName + ": " + events);
            }
            app.wizardOpen = false;
            tryCompare(home, "visible", true);
            var shown = [];
            tryVerify(function () {
                shown = [];
                for (var child of strip.children) {
                    if (child.objectName === "pinnedStripPin" && child.opacity > 0)
                        shown.push(child.text);
                }
                return shown.join("|") === "Alpha Prime|Night patrol";
            }, 1000, "the strip shows the pins as they are now");
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
            testContext.resetPins();
            compare(noisy.length, 0, "the hidden strip or closed sheet raised events: " + JSON.stringify(noisy) + " "
                    + JSON.stringify(counts.chains));
        }

        // Listen's saved systems with a screen reader on: the rows are positional. A reorder nobody asked for
        // (a system just listened to, in Last listened order) waits while the cards are on screen and raises nothing;
        // the order the user picks renames each row for the card now at its place, with no delegate made or
        // destroyed and no show or hide (Qt 6.11.3's Android bridge walks the list again for each delegate made or
        // destroyed, and refetches a parent for each show or hide), and the keyboard stays on the Sort pill.
        function test_m_saved_systems_reorder_waits_then_renames_in_place() {
            testContext.setLifecyclePhase(0);
            verify(!app.monitorMode);
            verify(screenReader.active, "this run has a screen reader on");
            var home = findChild(app, "homeScreen");
            var shownTitles = function () {
                var section = findChild(home, "listenSystemsSection");
                section.forceLayout();
                var entries = [];
                for (var child of section.children) {
                    if (child.objectName === "savedSystemEntry" && child.visible && !child.held)
                        entries.push(child);
                }
                entries.sort(function (a, b) {
                    return a.y - b.y;
                });
                return entries.map(function (entry) {
                    return entry.title;
                }).join("|");
            };
            var repeater = null;
            var made = 0;
            var gone = 0;
            var onMade = function () {
                ++made;
            };
            var onGone = function () {
                ++gone;
            };
            try {
                while (savedSystems.count)
                    savedSystems.remove(0);
                var heard = {Charlie: 300, Alpha: 200, Bravo: 100};
                for (var name of ["Charlie", "Alpha", "Bravo"])
                    verify(savedSystems.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5",
                        lastHeard: heard[name]}));
                prefs.systemsSort = "recent";
                tryCompare(home, "visible", true);
                home.showSection("systems");
                tryCompare(savedSystemCards, "totalCount", 3);
                waitForRendering(home);
                verify(savedSystemCards.screenReaderActive && savedSystemCards.holdReorders);
                compare(shownTitles(), "Charlie|Alpha|Bravo");
                var section = findChild(home, "listenSystemsSection");
                repeater = findChild(home, "savedSystemCardsRepeater");
                repeater.itemAdded.connect(onMade);
                repeater.itemRemoved.connect(onGone);

                testContext.startAccessibilityCount();
                verify(savedSystems.update(2, {lastHeard: 5000}));
                wait(50);
                var counts = testContext.accessibilityCount();
                testContext.stopAccessibilityCount();
                compare(showHide(section) + testContext.accessibilityCountWithin(section, "LocationChanged"), 0,
                        "a reorder nobody asked for raised events: " + JSON.stringify(counts.chains));
                compare(shownTitles(), "Charlie|Alpha|Bravo", "held while on screen");

                var pill = findChild(home, "systemsSortPill");
                pill.forceActiveFocus();
                testContext.startAccessibilityCount();
                findChild(home, "savedSystemsControls").chooseSort("name");
                wait(50);
                counts = testContext.accessibilityCount();
                testContext.stopAccessibilityCount();
                compare(shownTitles(), "Alpha|Bravo|Charlie");
                compare(made + gone, 0, "no delegate made or destroyed");
                compare(showHide(section), 0, "the chosen order shows or hides nothing: " + churn(counts));
                verify(testContext.accessibilityCountWithin(section, "NameChanged") > 0, "its rows are renamed");
                verify(pill.activeFocus, "the keyboard stays on the pill");
            } finally {
                testContext.stopAccessibilityCount();
                if (repeater) {
                    repeater.itemAdded.disconnect(onMade);
                    repeater.itemRemoved.disconnect(onGone);
                }
                testContext.resetSystemsSort();
                while (savedSystems.count)
                    savedSystems.remove(0);
            }
        }

        // At 300 cards of mixed shapes: site groups of two (a site button, no Play), single sites (a site button
        // and Play), rows outside a group (Play alone), placed and not, trunked rows from a RadioReference import that
        // named no site (the refresh hint) and one system on two receivers (a receiver line). A sort (Name, Order
        // added, Place with its section headings), a search to 5 and back, a sort to Nearest, a Nearest reorder from a
        // location the user asked for, and back to Name make and destroy no delegate and raise no show or hide in the
        // section, the lines over the cards included: a part one card has and the row's next card lacks is held
        // (transparent, unnamed, out of the tree), never hidden. On Android each show or hide made the bridge fetch the
        // scroll body, with every card under it, again: ~140 a sort at 300 cards left TalkBack silent for over a
        // minute. Each change instead raises one show or hide, of Home's tree refresh node under the window, which has
        // the bridge read its copy of the window again. Counts and timings are printed.
        function test_m2_saved_systems_300_rows_stay() {
            // Nearest needs the cards' location support; setting it re-sets decoderHost to the plain map, so the
            // lifecycle host Main runs on is put back after it.
            testContext.setLocationSupported(true);
            testContext.useLifecycleHost(true);
            testContext.setLifecyclePhase(0);
            verify(screenReader.active, "this run has a screen reader on");
            var home = findChild(app, "homeScreen");
            var repeater = null;
            var made = 0;
            var gone = 0;
            var onMade = function () {
                ++made;
            };
            var onGone = function () {
                ++gone;
            };
            var report = [];
            var imported = null;
            try {
                while (savedSystems.count)
                    savedSystems.remove(0);
                var csv = testContext.writeFixtureCsv("churn-provenance.csv", "Decimal,Mode,Alpha Tag\n123,A,Dispatch\n");
                imported = importedFiles.importGeneratedFile(csv, "Churn provenance.csv", "group",
                    {origin: "radioreference", rrSid: 12, rrSiteIds: "16863", rrKind: "group"});
                verify(imported.ok, "the RadioReference import for the refresh hint");
                var rows = [];
                var names = [];
                for (var i = 0; i < 300; ++i) {
                    // Card i + 1 of every 30th pair is card i's system on a USB dongle: two cards of one title.
                    var twin = i % 30 === 7;
                    var name = twin ? names[i - 1] : (i % 60 === 0 ? "Harbor " : "Bench ") + String(299 - i).padStart(3, "0");
                    names.push(name);
                    var row = {name: name, sourceType: twin ? "usb" : "rtltcp", host: "127.0.0.1", port: 1234,
                        freqMhz: "851.5"};
                    if (i % 4 === 0)
                        row.place = {states: [{stid: 19, name: "Iowa"}], counties: [], candidateStates: [19]};
                    var kind = i % 6;
                    if (kind === 1 || kind === 2 || kind === 4 || kind === 5)
                        Object.assign(row, {rrSid: 1000 + i, rrSiteId: 1, siteName: "S", siteLat: 41 + i / 100,
                            siteLon: -93.6, hasSitePos: true});
                    else if (kind === 3)
                        Object.assign(row, {trunking: true, groupCsvPath: imported.path});
                    rows.push(row);
                    // A second site of the same system: one card of two sites, nothing to play alone.
                    if (kind === 5)
                        rows.push(Object.assign({}, row, {rrSiteId: 2, siteName: "T", siteLat: 41.005 + i / 100}));
                }
                verify(savedSystems.addAll(rows, "") >= 0);
                prefs.systemsSort = "added";
                tryCompare(home, "visible", true);
                home.showSection("systems");
                tryCompare(savedSystemCards, "totalCount", 300);
                waitForRendering(home);
                var section = findChild(home, "listenSystemsSection");
                repeater = findChild(home, "savedSystemCardsRepeater");
                compare(repeater.count, 300);
                // Every shape the rows trade is there.
                var shapes = {play: 0, noPlay: 0, sites: 0, noSites: 0, hint: 0, receiver: 0, place: 0, noPlace: 0};
                for (i = 0; i < 300; ++i) {
                    var entry = repeater.itemAt(i);
                    ++shapes[entry.playUid.length > 0 ? "play" : "noPlay"];
                    ++shapes[entry.siteCount > 0 ? "sites" : "noSites"];
                    shapes.hint += entry.refreshHint ? 1 : 0;
                    shapes.receiver += entry.receiverLabel.length > 0 ? 1 : 0;
                    ++shapes[entry.placeLabel.length > 0 ? "place" : "noPlace"];
                }
                for (var shape in shapes)
                    verify(shapes[shape] > 0, "a card with " + shape + ": " + JSON.stringify(shapes));
                wait(50); // the tree refresh the cards' arrival owes
                repeater.itemAdded.connect(onMade);
                repeater.itemRemoved.connect(onGone);
                var controls = findChild(home, "savedSystemsControls");
                var step = function (label, change) {
                    testContext.startAccessibilityCount();
                    var started = Date.now();
                    change();
                    wait(0);
                    section.forceLayout();
                    var ms = Date.now() - started;
                    wait(50);
                    var counts = testContext.accessibilityCount();
                    testContext.stopAccessibilityCount();
                    verify(home.visible && home.section === "systems", label + ": Home shows Systems");
                    compare(made + gone, 0, label + ": no delegate made or destroyed");
                    compare(repeater.count, 300, label + ": 300 rows");
                    var shows = testContext.accessibilityCountWithin(section, "ObjectShow");
                    var hides = testContext.accessibilityCountWithin(section, "ObjectHide");
                    var refresh = findChild(app, "savedSystemsTreeRefresh");
                    var refreshed = refresh ? showHide(refresh) : 0;
                    var anywhere = typed(counts, "ObjectShow") + typed(counts, "ObjectHide");
                    report.push(label + " " + ms + " ms, " + shows + " shows, " + hides + " hides, " + refreshed
                        + " tree refresh");
                    compare(shows + hides, 0, label + ": " + shows + " shows, " + hides + " hides: " + churn(counts));
                    // Then one show or hide under the window has Android read its copy again, and nothing else.
                    compare(refreshed, 1, label + ": the tree refresh");
                    compare(anywhere, 1, label + ": shows and hides anywhere: " + churn(counts));
                };
                step("sort Name", function () {
                    controls.chooseSort("name");
                });
                step("sort Order added", function () {
                    controls.chooseSort("added");
                });
                step("sort Place", function () {
                    controls.chooseSort("place");
                });
                verify(findChild(repeater.itemAt(0), "savedSystemSection").opacity === 1, "Place order heads its sections");
                step("search 300 to 5", function () {
                    savedSystemCards.query = "harbor";
                });
                compare(savedSystemCards.shownCount, 5);
                step("search cleared", function () {
                    savedSystemCards.query = "";
                });
                prefs.setLocationFix(41.0, -93.6, savedSystems.realtimeNowMs() - 60000, 25);
                step("sort Nearest", function () {
                    controls.chooseSort("nearest");
                });
                compare(savedSystemCards.effectiveSort, "nearest");
                var nearestFirst = repeater.itemAt(0).title;
                step("Nearest, a location asked for", function () {
                    savedSystemCards.markLocationRequested();
                    prefs.setLocationFix(44.0, -93.6, savedSystems.realtimeNowMs(), 25);
                });
                verify(repeater.itemAt(0).title !== nearestFirst, "the location reordered the rows");
                // Leaving Nearest takes its location line and the distances down.
                step("sort Name, from Nearest", function () {
                    controls.chooseSort("name");
                });
                console.log("a11y counts with a screen reader, 300 cards: " + report.join("; "));
            } finally {
                testContext.stopAccessibilityCount();
                if (repeater) {
                    repeater.itemAdded.disconnect(onMade);
                    repeater.itemRemoved.disconnect(onGone);
                }
                testContext.setLocationSupported(false);
                prefs.setLocationFix(0, 0, 0, 0);
                testContext.resetSystemsSort();
                savedSystemCards.query = "";
                while (savedSystems.count)
                    savedSystems.remove(0);
                if (imported && imported.ok)
                    importedFiles.remove(importedFiles.rowForPath(imported.path));
            }
        }

        // r6 1: a card that gains or loses a part in place, with no other change to the list (an automatic place-name
        // lookup naming a place in Order added, which keeps the order), holds or shows that part, which raises no show
        // or hide; Android's bridge would keep its copy without the place line, which TalkBack could then not reach.
        // So a change to which parts a card shows has Home refresh the tree once, as a reorder does. A change of an
        // already shown part's words only is a rename, which the bridge sends on, and refreshes nothing.
        function test_m4_saved_systems_part_shown_in_place_refreshes_the_tree() {
            testContext.setLifecyclePhase(0);
            verify(screenReader.active, "this run has a screen reader on");
            var home = findChild(app, "homeScreen");
            try {
                while (savedSystems.count)
                    savedSystems.remove(0);
                for (var name of ["Charlie", "Alpha", "Bravo"])
                    verify(savedSystems.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"}));
                prefs.systemsSort = "added";
                tryCompare(home, "visible", true);
                home.showSection("systems");
                tryCompare(savedSystemCards, "totalCount", 3);
                waitForRendering(home);
                wait(50); // the tree refresh the cards' arrival owes
                var section = findChild(home, "listenSystemsSection");
                var repeater = findChild(home, "savedSystemCardsRepeater");
                var entry = repeater.itemAt(1);
                compare(entry.title, "Alpha");
                var place = findChild(entry, "savedSystemPlace");
                verify(place.Accessible.ignored, "no place yet");
                var placed = function (counties) {
                    var row = savedSystems.rowForUid(entry.leadUid);
                    verify(savedSystems.update(row, Object.assign(savedSystems.get(row), {place: {states: [{stid: 19,
                        name: "Iowa"}], counties: counties, candidateStates: [19]}})));
                };
                var step = function (label, change, refreshes) {
                    testContext.startAccessibilityCount();
                    change();
                    wait(50);
                    var counts = testContext.accessibilityCount();
                    testContext.stopAccessibilityCount();
                    compare(repeater.itemAt(1), entry, label + ": the same row");
                    compare(entry.title, "Alpha", label + ": the same card");
                    compare(showHide(section), 0, label + ": no show or hide in the list: " + churn(counts));
                    var refresh = findChild(app, "savedSystemsTreeRefresh");
                    compare(refresh ? showHide(refresh) : 0, refreshes, label + ": the tree refresh: " + churn(counts));
                    compare(typed(counts, "ObjectShow") + typed(counts, "ObjectHide"), refreshes,
                            label + ": shows and hides anywhere: " + churn(counts));
                };
                step("a place named", function () {
                    placed([]);
                }, 1);
                verify(!place.Accessible.ignored && place.opacity === 1, "the place line is shown");
                compare(place.Accessible.name, "Iowa");
                step("the place renamed", function () {
                    placed([{ctid: 85, stid: 19, name: "Story"}]);
                }, 0);
                compare(place.Accessible.name, "Story, Iowa");
            } finally {
                testContext.stopAccessibilityCount();
                testContext.resetSystemsSort();
                while (savedSystems.count)
                    savedSystems.remove(0);
            }
        }

        // Issue #901 (D8): a change of Compact view holds or shows lines and site buttons in every card, in place, with
        // no rebuild of the cards. At 300 mixed cards it makes and destroys no delegate and raises no show or hide in
        // the section; while Systems is shown and in reach it has Home refresh Android's copy of the tree once. Made
        // where it is made (Settings, the session menu: Home hidden, on another tab, or under a sheet) it refreshes
        // nothing there and owes the refresh until Systems is shown and in reach again, through a rebuild of the cards
        // meanwhile, and then gives it once. Counts are printed.
        function test_m6_density_change_refreshes_the_tree_once() {
            testContext.setLifecyclePhase(0);
            verify(screenReader.active, "this run has a screen reader on");
            var home = findChild(app, "homeScreen");
            var repeater = null;
            var made = 0;
            var gone = 0;
            var onMade = function () {
                ++made;
            };
            var onGone = function () {
                ++gone;
            };
            var report = [];
            var imported = null;
            var rebuilt = Qt.createQmlObject("import QtTest; SignalSpy { signalName: \"rebuilt\" }", app);
            try {
                while (savedSystems.count)
                    savedSystems.remove(0);
                prefs.compactView = false;
                var csv = testContext.writeFixtureCsv("density-provenance.csv", "Decimal,Mode,Alpha Tag\n123,A,Dispatch\n");
                imported = importedFiles.importGeneratedFile(csv, "Density provenance.csv", "group",
                    {origin: "radioreference", rrSid: 12, rrSiteIds: "16863", rrKind: "group"});
                verify(imported.ok);
                var rows = [];
                for (var i = 0; i < 300; ++i) {
                    var twin = i % 30 === 7;
                    var row = {name: twin ? "Bench " + String(300 - i).padStart(3, "0") : "Bench " + String(299 - i).padStart(3, "0"),
                        sourceType: twin ? "usb" : "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"};
                    if (i % 4 === 0)
                        row.place = {states: [{stid: 90, name: "Westmark"}], counties: [], candidateStates: [90]};
                    var kind = i % 6;
                    if (kind === 1 || kind === 2 || kind === 4 || kind === 5)
                        Object.assign(row, {rrSid: 1000 + i, rrSiteId: 1, siteName: "S"});
                    else if (kind === 3)
                        Object.assign(row, {trunking: true, groupCsvPath: imported.path});
                    rows.push(row);
                    if (kind === 5)
                        rows.push(Object.assign({}, row, {rrSiteId: 2, siteName: "T"}));
                }
                verify(savedSystems.addAll(rows, "") >= 0);
                prefs.systemsSort = "added";
                tryCompare(home, "visible", true);
                home.showSection("systems");
                tryCompare(savedSystemCards, "totalCount", 300);
                waitForRendering(home);
                wait(50); // the tree refresh the cards' arrival owes
                var section = findChild(home, "listenSystemsSection");
                repeater = findChild(home, "savedSystemCardsRepeater");
                compare(repeater.count, 300);
                repeater.itemAdded.connect(onMade);
                repeater.itemRemoved.connect(onGone);
                rebuilt.target = savedSystemCards;
                var refreshes = function () {
                    var refresh = findChild(app, "savedSystemsTreeRefresh");
                    return refresh ? showHide(refresh) : 0;
                };
                // Counts what @a change raises, in the section and of the tree refresh.
                var step = function (label, change, wantRefresh, quietSection) {
                    testContext.startAccessibilityCount();
                    var started = Date.now();
                    change();
                    wait(0);
                    section.forceLayout();
                    var ms = Date.now() - started;
                    wait(50);
                    var counts = testContext.accessibilityCount();
                    testContext.stopAccessibilityCount();
                    var inSection = showHide(section);
                    var refreshed = refreshes();
                    report.push(label + " " + ms + " ms, " + inSection + " shows or hides in the section, " + refreshed
                        + " tree refresh, " + (typed(counts, "ObjectShow") + typed(counts, "ObjectHide")) + " anywhere");
                    compare(made + gone, 0, label + ": no delegate made or destroyed");
                    compare(repeater.count, 300, label + ": 300 rows");
                    if (quietSection)
                        compare(inSection, 0, label + ": no show or hide in the section: " + churn(counts));
                    compare(refreshed, wantRefresh, label + ": the tree refresh: " + churn(counts));
                };
                var flip = function () {
                    prefs.compactView = !prefs.compactView;
                };
                step("compact, Systems shown", flip, 1, true);
                verify(home.compactView);
                var first = repeater.itemAt(0);
                verify(findChild(first, "savedSystemPlace").visible && findChild(first, "savedSystemPlace").opacity === 0,
                    "the place line is held, not hidden");
                step("full, Systems shown", flip, 1, true);

                app.currentTab = 1;
                tryCompare(home, "visible", false);
                wait(50);
                step("compact, Home hidden", flip, 0, true);
                step("Home shown again", function () {
                    app.currentTab = 0;
                    tryCompare(home, "visible", true);
                }, 1, false);

                rebuilt.clear();
                app.currentTab = 1;
                tryCompare(home, "visible", false);
                wait(50);
                step("full, Home hidden, the cards rebuilt", function () {
                    flip();
                    verify(savedSystems.update(0, Object.assign(savedSystems.get(0), {freqMhz: "852.5"})));
                    tryVerify(function () {
                        return rebuilt.count > 0;
                    }, 1000, "the cards rebuilt while hidden");
                }, 0, true);
                step("Home shown again after the rebuild", function () {
                    app.currentTab = 0;
                    tryCompare(home, "visible", true);
                }, 1, false);

                home.showSection("explore");
                wait(50);
                step("compact, on Explore", flip, 0, true);
                step("back on Systems", function () {
                    home.showSection("systems");
                }, 1, false);

                var sheet = findChild(home, "systemsSortSheet");
                sheet.open("Sort systems", ["Name"], null);
                tryCompare(sheet, "visible", true);
                wait(50);
                step("full, under a sheet", flip, 0, true);
                step("the sheet closed", function () {
                    sheet.visible = false;
                }, 1, false);
                verify(!home.compactView);
                console.log("a11y counts for a density change, 300 cards: " + report.join("; "));
            } finally {
                testContext.stopAccessibilityCount();
                if (repeater) {
                    repeater.itemAdded.disconnect(onMade);
                    repeater.itemRemoved.disconnect(onGone);
                }
                rebuilt.destroy();
                for (var modal of Ui.Navigation.modals.slice())
                    modal.visible = false;
                app.currentTab = 0;
                prefs.compactView = false;
                home.showSection("systems");
                testContext.resetSystemsSort();
                while (savedSystems.count)
                    savedSystems.remove(0);
                if (imported && imported.ok)
                    importedFiles.remove(importedFiles.rowForPath(imported.path));
            }
        }

        // Issue #901: Compact view flipped with the session menu closed (from Settings, or the menu's own item, which
        // closes it first) changes nothing in the closed menu: no row is rebuilt, so nothing shows or hides, and
        // nothing in it is renamed. The item's On/Off is the menu's describe(), which the row calls in its own
        // binding, so the menu's action list neither reads the preference nor holds a function, and its re-runs (each
        // metrics tick or hop) give the Repeater an equal model. Opened, the row reads the state and follows a change
        // in place.
        function test_m8_closed_session_menu_ignores_a_density_change() {
            testContext.setLifecyclePhase(0);
            var menu = findChild(app, "sessionMenu");
            verify(!menu.visible, "the session menu starts closed");
            var repeater = findItem(menu, function (item) {
                return item.itemAdded !== undefined && item.count === menu.actions.length && item.count > 0
                    && item.itemAt(0).modelData.key === menu.actions[0].key;
            });
            verify(repeater !== null, "the menu's rows");
            var rowsMade = 0;
            var onRow = function () {
                ++rowsMade;
            };
            var indexOfCompact = function () {
                for (var i = 0; i < menu.actions.length; ++i) {
                    if (menu.actions[i].key === "compactView")
                        return i;
                }
                return -1;
            };
            repeater.itemAdded.connect(onRow);
            repeater.itemRemoved.connect(onRow);
            try {
                prefs.compactView = false;
                wait(50);
                var at = indexOfCompact();
                verify(at >= 0, "the menu offers Compact view");
                for (var on of [true, false, true]) {
                    rowsMade = 0;
                    testContext.startAccessibilityCount();
                    prefs.compactView = on;
                    wait(50);
                    var counts = testContext.accessibilityCount();
                    testContext.stopAccessibilityCount();
                    compare(rowsMade, 0, "the flip rebuilt the closed menu's rows");
                    compare(showHide(menu), 0, "shows or hides in the closed menu: " + churn(counts));
                    compare(testContext.accessibilityCountWithin(menu, "NameChanged"), 0, "renames in the closed menu: "
                        + churn(counts));
                    compare(menu.actionDescription(indexOfCompact()), on ? "On" : "Off");
                }
                compare(menu.actions[at].description, undefined, "the action list holds the state");
                compare(typeof menu.describe, "function", "the row's state comes from the menu's describe()");
                compare(menu.actionDescription(at), "On");
                // Metrics ticks and hops re-run the action list: an equal list, no row rebuilt.
                rowsMade = 0;
                testContext.tickMetrics(5);
                for (var i = 1; i <= 3; ++i)
                    testContext.publishTarget(reading(i));
                wait(50);
                compare(rowsMade, 0, "a tick or a hop rebuilt the menu's rows");
                // Open, the row says the state and follows a change in place.
                menu.open();
                tryCompare(menu, "visible", true);
                waitForRendering(menu);
                var line = findChild(menu, "actionMenuDescription" + indexOfCompact());
                var button = findChild(menu, "sessionMenuCompactView");
                compare(line.text, "On");
                compare(button.accessibleName, "Compact view. On");
                rowsMade = 0;
                prefs.compactView = false;
                tryCompare(line, "text", "Off");
                compare(button.accessibleName, "Compact view. Off");
                compare(rowsMade, 0, "the open menu rebuilt its rows");
            } finally {
                testContext.stopAccessibilityCount();
                repeater.itemAdded.disconnect(onRow);
                repeater.itemRemoved.disconnect(onRow);
                menu.visible = false;
                prefs.compactView = false;
            }
        }

        // Issue #901 (r2 D1): Compact view flipped in the session's Settings, which covers the Monitor without hiding
        // it, re-laid out and scrolled the Monitor's calls behind it: at 320 dp and text 1.6 with 250 calls and the
        // list at row 160, eight window-level hides, each a blocking query of Android's bridge. The Monitor keeps its
        // model but takes the new density only once it is in reach again: while covered a flip raises no show or
        // hide anywhere and moves nothing; uncovered, the rows take the new height once and the list goes to its
        // start. A flip and a flip back under the cover leave nothing to apply. The session's History and Home are
        // hidden meanwhile (their lists detached), so they have nothing to lay out either.
        function test_m9_covered_monitor_takes_the_density_when_shown() {
            var monitor = findChild(app, "monitorScreen");
            var list = findChild(monitor, "recentCallsList");
            var width = app.width;
            prefs.compactView = false;
            app.width = 320;
            Ui.Theme.fontScale = 1.6;
            try {
                callHistory.clearAll();
                callHistory.pushMany(250, "TODAY");
                tryCompare(list, "count", 250);
                verify(monitor.visible && !monitor.compactView);
                var fullRow = monitor.callRowHeight;
                list.positionViewAtIndex(160, ListView.Beginning);
                waitForRendering(app.contentItem);
                var deep = list.contentY;
                verify(deep - list.originY > 100 * fullRow, "the list is deep in the calls: " + deep);
                app.sessionDestination = "settings";
                tryVerify(function () { return !Ui.Navigation.allows(list); }, 1000, "the session's Settings cover the Monitor");
                verify(monitor.visible, "covered, not hidden");
                for (var name of ["homeScreen", "idleHistoryScreen", "sessionHistoryScreen"])
                    verify(!findChild(app, name).visible, name + " is hidden under the session's Settings");
                waitForRendering(app.contentItem);
                wait(50);

                for (var on of [true, false, true]) {
                    testContext.startAccessibilityCount();
                    prefs.compactView = on;
                    wait(0);
                    waitForRendering(app.contentItem);
                    wait(50);
                    var counts = testContext.accessibilityCount();
                    testContext.stopAccessibilityCount();
                    compare(typed(counts, "ObjectShow") + typed(counts, "ObjectHide"), 0,
                            "covered, a flip to " + on + " raised shows or hides: " + churn(counts));
                    verify(!monitor.compactView, "covered, the Monitor keeps the density it shows");
                    compare(monitor.callRowHeight, fullRow);
                    compare(list.contentY, deep, "covered, the list did not move");
                }

                // Uncovered: the new density once, and the list at its start.
                app.sessionDestination = "";
                tryVerify(function () { return Ui.Navigation.allows(list); }, 1000, "the Monitor in reach again");
                tryVerify(function () { return monitor.compactView; }, 1000, "uncovered, the Monitor takes compact view");
                verify(monitor.callRowHeight < fullRow, "compact rows are shorter");
                tryVerify(function () { return Math.abs(list.contentY - list.originY) < 1; }, 1000,
                          "the list at its start: " + list.contentY + " vs " + list.originY);
                waitForRendering(app.contentItem);
                var row = list.itemAtIndex(0);
                verify(row !== null && Math.abs(row.height - monitor.callRowHeight) < 0.5, "the rows at the compact height");

                // A flip and a flip back under the cover: nothing to apply, the list stays where it is.
                list.positionViewAtIndex(160, ListView.Beginning);
                waitForRendering(app.contentItem);
                deep = list.contentY;
                app.sessionDestination = "settings";
                tryVerify(function () { return !Ui.Navigation.allows(list); }, 1000);
                prefs.compactView = false;
                prefs.compactView = true;
                app.sessionDestination = "";
                tryVerify(function () { return Ui.Navigation.allows(list); }, 1000);
                waitForRendering(app.contentItem);
                wait(50);
                verify(monitor.compactView);
                compare(list.contentY, deep, "no change to apply: the list keeps its place");
            } finally {
                testContext.stopAccessibilityCount();
                app.sessionDestination = "";
                prefs.compactView = false;
                Ui.Theme.resetFontScale();
                app.width = width;
                callHistory.clearAll();
            }
        }

        // B11 (issue #901): a card's heading and controls (the section heading, the site button, Play, More, a scan
        // list's Play and More) carry no name or description while covered by a sheet or hidden with Home, as while
        // held: Android keeps the nodes it already fetched, and reads a covered node's cached interface by what it says
        // now. Their interfaces are made first, while shown; a late press on them under the sheet does nothing.
        function test_m7_covered_cards_say_nothing() {
            testContext.setLifecyclePhase(0);
            verify(screenReader.active, "this run has a screen reader on");
            var home = findChild(app, "homeScreen");
            var played = Qt.createQmlObject("import QtTest; SignalSpy { signalName: \"playSystem\" }", app);
            var chosen = Qt.createQmlObject("import QtTest; SignalSpy { signalName: \"chooseSites\" }", app);
            var listPlayed = Qt.createQmlObject("import QtTest; SignalSpy { signalName: \"playScanList\" }", app);
            played.target = home;
            chosen.target = home;
            listPlayed.target = home;
            try {
                while (savedSystems.count)
                    savedSystems.remove(0);
                while (scanLists.count)
                    scanLists.remove(0);
                verify(savedSystems.add({name: "Ridge", sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5",
                    rrSid: 41, rrSiteId: 410, siteName: "North", place: {states: [{stid: 90, name: "Westmark"}], counties: [],
                    candidateStates: [90]}}));
                verify(savedSystems.add({name: "Valley", sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "852.5"}));
                verify(scanLists.add({name: "Coast loop", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                    entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]}));
                prefs.systemsSort = "place";
                tryCompare(home, "visible", true);
                home.showSection("systems");
                tryCompare(savedSystemCards, "totalCount", 2);
                waitForRendering(home);
                var entry = null;
                var other = null;
                var repeater = findChild(home, "savedSystemCardsRepeater");
                for (var i = 0; i < repeater.count; ++i) {
                    var row = repeater.itemAt(i);
                    if (row.held)
                        continue;
                    if (row.title === "Ridge")
                        entry = row;
                    else if (row.title === "Valley")
                        other = row;
                }
                verify(entry !== null && other !== null);
                var parts = ["savedSystemSection", "homeSiteChooserButton", "savedSystemPlay", "savedSystemManageButton"].map(function (name) {
                    var item = findChild(entry, name);
                    verify(item !== null, name);
                    return [name, item];
                });
                var expectSaid = function (label, said) {
                    for (var pair of parts) {
                        var node = testContext.accessibleNode(pair[1]);
                        verify(node.exists, label + ": " + pair[0] + " has its interface");
                        if (said) {
                            verify(node.name.length > 0, label + ": " + pair[0] + " named");
                        } else {
                            compare(node.name, "", label + ": " + pair[0] + " unnamed");
                            compare(node.description, "", label + ": " + pair[0] + " no description");
                        }
                    }
                };
                expectSaid("shown", true);

                var menu = findChild(home, "savedSystemManageSheet");
                home.manageCard(other.leadUid, "Valley", false, false);
                tryCompare(menu, "visible", true);
                var opened = menu.systemUid;
                expectSaid("under a sheet", false);
                for (var pair of parts)
                    testContext.pressAccessible(pair[1]);
                wait(50);
                compare(played.count, 0, "a late Play under the sheet plays nothing");
                compare(chosen.count, 0, "a late site button under the sheet opens no chooser");
                compare(menu.systemUid, opened, "a late More under the sheet opens no other menu");
                menu.visible = false;
                tryCompare(menu, "visible", false);
                expectSaid("the sheet closed", true);

                app.currentTab = 1;
                tryCompare(home, "visible", false);
                expectSaid("Home hidden", false);
                app.currentTab = 0;
                tryCompare(home, "visible", true);

                home.showSection("scanLists");
                var list = findChild(findChild(home, "listenScanListsSection"), "scanListCard");
                tryCompare(list, "visible", true);
                waitForRendering(home);
                parts = ["scanListPlay", "scanListManageButton"].map(function (name) {
                    return [name, findChild(list, name)];
                });
                expectSaid("scan list shown", true);
                var scanMenu = findChild(home, "scanListManageSheet");
                scanMenu.openFor(0);
                tryCompare(scanMenu, "visible", true);
                expectSaid("scan list under a sheet", false);
                verify(testContext.pressAccessible(parts[0][1]), "Play offers its press");
                wait(50);
                compare(listPlayed.count, 0, "a late scan-list Play under the sheet plays nothing");
                scanMenu.visible = false;
                tryCompare(scanMenu, "visible", false);
                expectSaid("scan list, the sheet closed", true);
            } finally {
                for (var modal of Ui.Navigation.modals.slice())
                    modal.visible = false;
                app.currentTab = 0;
                home.showSection("systems");
                played.destroy();
                chosen.destroy();
                listPlayed.destroy();
                while (scanLists.count)
                    scanLists.remove(0);
                testContext.resetSystemsSort();
                while (savedSystems.count)
                    savedSystems.remove(0);
            }
        }

        // a11y2 B: a choice made in a sheet as TalkBack makes it (Qt's bridge carries TalkBack's focus out as the
        // set-focus action, and its double tap as the press) hands the keyboard focus, which TalkBack's follows, back
        // to the control that opened the sheet: one Focus event, the opener's. Home disables its body while the Sort
        // sheet shows, the Sort pill with it, so the pill had lost the focus when the sheet read its opener, and the
        // focus went back to Home itself: TalkBack started again from the top ("Listen"). History's filter sheet, whose
        // pill stays enabled, is the control.
        function focusEvents(counts) {
            return counts.sequence.filter(function (event) {
                return /^Focus /.test(event);
            });
        }
        function chooseAsTalkBack(pill, sheetName, choiceText) {
            verify(testContext.focusAccessible(pill), "the pill takes the set-focus action");
            tryVerify(function () {
                return pill.activeFocus;
            });
            verify(testContext.pressAccessible(pill), "and the press");
            var sheet = null;
            tryVerify(function () {
                sheet = Ui.Navigation.topModal;
                return sheet !== null && sheet.visible;
            }, 2000, sheetName + " opens");
            waitForRendering(sheet);
            var choice = textControl(sheet, choiceText);
            verify(choice !== null, choiceText + " is offered");
            verify(testContext.focusAccessible(choice));
            tryVerify(function () {
                return choice.activeFocus;
            });
            testContext.startAccessibilityCount();
            verify(testContext.pressAccessible(choice));
            tryVerify(function () {
                return !sheet.visible;
            });
            wait(100);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            return counts;
        }
        function textControl(root, text) {
            if (!root)
                return null;
            if (root.text === text && root.activate !== undefined)
                return root;
            for (var child of root.children || []) {
                var hit = textControl(child, text);
                if (hit)
                    return hit;
            }
            return null;
        }
        function test_m3_sort_sheet_gives_the_focus_back_to_its_pill() {
            testContext.setLifecyclePhase(0);
            var home = findChild(app, "homeScreen");
            try {
                while (savedSystems.count)
                    savedSystems.remove(0);
                for (var name of ["Charlie", "Alpha", "Bravo"])
                    verify(savedSystems.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"}));
                prefs.systemsSort = "added";
                tryCompare(home, "visible", true);
                home.showSection("systems");
                tryCompare(savedSystemCards, "totalCount", 3);
                waitForRendering(home);
                var pill = findChild(home, "systemsSortPill");
                var counts = chooseAsTalkBack(pill, "the Sort sheet", "Name");
                compare(savedSystemCards.effectiveSort, "name");
                verify(pill.activeFocus, "the keyboard is back on the pill, not on " + app.activeFocusItem);
                var focused = focusEvents(counts);
                compare(focused.length, 1, "one Focus event: " + JSON.stringify(focused));
                verify(/^Focus systemsSortPill /.test(focused[0]), "the pill's: " + focused[0]);
            } finally {
                testContext.stopAccessibilityCount();
                testContext.resetSystemsSort();
                while (savedSystems.count)
                    savedSystems.remove(0);
            }
        }
        function test_m3_history_filter_sheet_gives_the_focus_back_to_its_pill() {
            testContext.setLifecyclePhase(0);
            app.currentTab = 1;
            var history = findChild(app, "idleHistoryScreen");
            var pill = null;
            try {
                tryCompare(history, "visible", true);
                waitForRendering(history);
                pill = textControl(history, "All activity");
                verify(pill !== null, "the activity pill");
                var counts = chooseAsTalkBack(pill, "the activity sheet", "Encrypted");
                verify(pill.activeFocus, "the keyboard is back on the pill, not on " + app.activeFocusItem);
                var focused = focusEvents(counts);
                compare(focused.length, 1, "one Focus event: " + JSON.stringify(focused));
            } finally {
                testContext.stopAccessibilityCount();
                if (pill && pill.text !== "All activity") {
                    pill.activate();
                    var sheet = Ui.Navigation.topModal;
                    if (sheet) {
                        var all = textControl(sheet, "All activity");
                        if (all)
                            all.activate();
                    }
                }
                app.currentTab = 0;
            }
        }

        // TalkBack's keyboard commands hold Alt: Alt+Enter clicks the item TalkBack has focused, Alt+Shift+Enter
        // long-presses it. TalkBack takes the key and makes the click itself, but Android hands the app a key that an
        // accessibility service has not answered within 500 ms as well. On the emulator (300 cards) the click on a
        // Sort choice blocked TalkBack for 860 ms while Home re-sorted, and the same Alt+Enter press (not a repeat)
        // reached the app after the click had closed the sheet and handed the keyboard back to the pill, and opened
        // the sheet again; a slow TalkBack passes the key on before its click instead. With a screen reader on, a
        // control takes Space, Return or Enter held with Alt and does nothing: the screen reader's click is the one
        // action. The press, a repeat and the release, as Android delivered them.
        function screenReaderCommandKeys(item, key, modifiers) {
            var taken = testContext.sendKey(item, key, modifiers, true, false);
            taken = testContext.sendKey(item, key, modifiers, true, true) && taken;
            testContext.sendKey(item, key, modifiers, false, false);
            return taken;
        }
        function openAsTalkBack(pill, what) {
            verify(testContext.pressAccessible(pill), "TalkBack's click on " + what);
            var sheet = null;
            tryVerify(function () {
                sheet = Ui.Navigation.topModal;
                return sheet !== null && sheet.visible;
            }, 2000, what + " opens its sheet");
            waitForRendering(sheet);
            return sheet;
        }
        function lateKeysLeaveTheSheetClosed(pill, data, what) {
            verify(pill.activeFocus, "the keyboard is back on " + what + ", not on " + app.activeFocusItem);
            var taken = screenReaderCommandKeys(pill, data.key, data.modifiers);
            wait(50);
            compare(Ui.Navigation.topModal, null, "the sheet stays closed after " + what + "'s late " + data.tag);
            verify(taken, "the late keys are taken");
            verify(pill.activeFocus, "the keyboard stays on " + what);
        }
        // Then the click on another choice: the key, passed on first, did nothing on it.
        function keyFirstThenClick(sheet, choiceText, data) {
            var choice = textControl(sheet, choiceText);
            verify(choice !== null, choiceText + " is offered");
            verify(testContext.focusAccessible(choice));
            tryVerify(function () {
                return choice.activeFocus;
            });
            var taken = screenReaderCommandKeys(choice, data.key, data.modifiers);
            wait(50);
            verify(sheet.visible, "the early " + data.tag + " chose nothing");
            verify(taken, "the early keys are taken");
            verify(testContext.pressAccessible(choice));
            tryVerify(function () {
                return !sheet.visible;
            });
        }
        function cancelAsTalkBack(sheet) {
            var cancel = textControl(sheet, "Cancel");
            verify(cancel !== null, "the sheet has its Cancel");
            verify(testContext.focusAccessible(cancel));
            tryVerify(function () {
                return cancel.activeFocus;
            });
            verify(testContext.pressAccessible(cancel));
            tryVerify(function () {
                return !sheet.visible;
            });
        }
        function test_m5_late_screen_reader_keys_leave_the_sort_sheet_closed_data() {
            return [{tag: "Alt+Return", key: Qt.Key_Return, modifiers: Qt.AltModifier},
                {tag: "Alt+keypad Enter", key: Qt.Key_Enter, modifiers: Qt.AltModifier | Qt.KeypadModifier},
                {tag: "Alt+Shift+Return", key: Qt.Key_Return, modifiers: Qt.AltModifier | Qt.ShiftModifier}];
        }
        function test_m5_late_screen_reader_keys_leave_the_sort_sheet_closed(data) {
            verify(screenReader.active, "this run has a screen reader on");
            testContext.setLifecyclePhase(0);
            var home = findChild(app, "homeScreen");
            try {
                while (savedSystems.count)
                    savedSystems.remove(0);
                for (var name of ["Charlie", "Alpha", "Bravo"])
                    verify(savedSystems.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"}));
                prefs.systemsSort = "added";
                tryCompare(home, "visible", true);
                home.showSection("systems");
                tryCompare(savedSystemCards, "totalCount", 3);
                waitForRendering(home);
                var pill = findChild(home, "systemsSortPill");
                chooseAsTalkBack(pill, "the Sort sheet", "Name");
                compare(savedSystemCards.effectiveSort, "name");
                lateKeysLeaveTheSheetClosed(pill, data, "the Sort pill");
                compare(savedSystemCards.effectiveSort, "name");

                cancelAsTalkBack(openAsTalkBack(pill, "the Sort pill"));
                lateKeysLeaveTheSheetClosed(pill, data, "the Sort pill, after Cancel");

                keyFirstThenClick(openAsTalkBack(pill, "the Sort pill"), "Order added", data);
                tryCompare(savedSystemCards, "effectiveSort", "added");
                lateKeysLeaveTheSheetClosed(pill, data, "the Sort pill, after the click");

                // TalkBack passes a plain Return on: it still presses the pill.
                testContext.sendKey(pill, Qt.Key_Return, Qt.NoModifier, true, false);
                testContext.sendKey(pill, Qt.Key_Return, Qt.NoModifier, false, false);
                tryVerify(function () {
                    return Ui.Navigation.topModal !== null && Ui.Navigation.topModal.visible;
                }, 2000, "a plain Return opens the sheet");
                cancelAsTalkBack(Ui.Navigation.topModal);
            } finally {
                testContext.stopAccessibilityCount();
                if (Ui.Navigation.topModal)
                    Ui.Navigation.topModal.visible = false;
                testContext.resetSystemsSort();
                while (savedSystems.count)
                    savedSystems.remove(0);
            }
        }
        function test_m5_late_screen_reader_keys_leave_the_history_filter_sheet_closed_data() {
            return test_m5_late_screen_reader_keys_leave_the_sort_sheet_closed_data();
        }
        function test_m5_late_screen_reader_keys_leave_the_history_filter_sheet_closed(data) {
            verify(screenReader.active, "this run has a screen reader on");
            testContext.setLifecyclePhase(0);
            app.currentTab = 1;
            var history = findChild(app, "idleHistoryScreen");
            var pill = null;
            try {
                tryCompare(history, "visible", true);
                waitForRendering(history);
                pill = textControl(history, "All activity");
                verify(pill !== null, "the activity pill");
                chooseAsTalkBack(pill, "the activity sheet", "Encrypted");
                compare(pill.text, "Encrypted");
                lateKeysLeaveTheSheetClosed(pill, data, "the activity pill");

                cancelAsTalkBack(openAsTalkBack(pill, "the activity pill"));
                lateKeysLeaveTheSheetClosed(pill, data, "the activity pill, after Cancel");

                keyFirstThenClick(openAsTalkBack(pill, "the activity pill"), "All activity", data);
                compare(pill.text, "All activity");
                lateKeysLeaveTheSheetClosed(pill, data, "the activity pill, after the click");
            } finally {
                testContext.stopAccessibilityCount();
                if (Ui.Navigation.topModal)
                    Ui.Navigation.topModal.visible = false;
                if (pill && pill.text !== "All activity") {
                    pill.activate();
                    var sheet = Ui.Navigation.topModal;
                    if (sheet) {
                        var all = textControl(sheet, "All activity");
                        if (all)
                            all.activate();
                    }
                }
                app.currentTab = 0;
            }
        }

        // Issue #801 B3: Explore's face carries its frequency in its name. Edited in Explore's setup, Home hidden under
        // it, the face renames nothing; shown again, it reads the new frequency.
        function test_l_hidden_explore_face_stays_silent() {
            testContext.setLifecyclePhase(0);
            verify(!app.monitorMode);
            var saved = {source: prefs.exploreSourceType, freq: prefs.exploreFreqMhz};
            try {
                prefs.exploreSourceType = "rtltcp";
                prefs.exploreFreqMhz = "851.5";
                var home = findChild(app, "homeScreen");
                tryCompare(home, "visible", true);
                // Issue #646: on the Explore tab, where the face is shown.
                home.showSection("explore");
                waitForRendering(home);
                var face = findChild(home, "exploreStart");
                compare(face.Accessible.name, "Explore the band, start on RTL-TCP from 851.5 MHz");
                app.exploreSetupOpen = true;
                tryVerify(function () { return face.Accessible.ignored; }, 1000, "Home's face leaves the tree");
                testContext.startAccessibilityCount();
                prefs.exploreFreqMhz = "852.0125";
                wait(50);
                var counts = testContext.accessibilityCount();
                testContext.stopAccessibilityCount();
                var events = 0;
                for (var type of Object.keys(counts.types))
                    events += testContext.accessibilityCountWithin(face, type);
                compare(events, 0, "the hidden face raised events: " + JSON.stringify(counts.chains));
                app.exploreSetupOpen = false;
                tryVerify(function () { return !face.Accessible.ignored; }, 1000);
                compare(face.Accessible.name, "Explore the band, start on RTL-TCP from 852.0125 MHz");
            } finally {
                testContext.stopAccessibilityCount();
                app.exploreSetupOpen = false;
                findChild(app, "homeScreen").showSection("systems");
                prefs.exploreSourceType = saved.source;
                prefs.exploreFreqMhz = saved.freq;
            }
        }

        // Issue #646: Home shown on another Listen tab hides the Explore section, and the face in it renames nothing
        // on a frequency change either; back on Explore, it reads the new frequency.
        function test_l2_explore_face_on_another_tab_stays_silent() {
            testContext.setLifecyclePhase(0);
            verify(!app.monitorMode);
            var saved = {source: prefs.exploreSourceType, freq: prefs.exploreFreqMhz};
            var home = findChild(app, "homeScreen");
            try {
                prefs.exploreSourceType = "rtltcp";
                prefs.exploreFreqMhz = "851.5";
                tryCompare(home, "visible", true);
                home.showSection("explore");
                waitForRendering(home);
                var face = findChild(home, "exploreStart");
                compare(face.Accessible.name, "Explore the band, start on RTL-TCP from 851.5 MHz");
                home.showSection("systems");
                tryVerify(function () { return face.Accessible.ignored; }, 1000, "the face leaves the tree with its section");
                verify(home.visible, "Home itself is still shown");
                testContext.startAccessibilityCount();
                prefs.exploreFreqMhz = "852.0125";
                wait(50);
                var counts = testContext.accessibilityCount();
                testContext.stopAccessibilityCount();
                var events = 0;
                for (var type of Object.keys(counts.types))
                    events += testContext.accessibilityCountWithin(face, type);
                compare(events, 0, "the face on a hidden tab raised events: " + JSON.stringify(counts.chains));
                home.showSection("explore");
                tryVerify(function () { return !face.Accessible.ignored; }, 1000);
                compare(face.Accessible.name, "Explore the band, start on RTL-TCP from 852.0125 MHz");
            } finally {
                testContext.stopAccessibilityCount();
                home.showSection("systems");
                prefs.exploreSourceType = saved.source;
                prefs.exploreFreqMhz = saved.freq;
            }
        }

        // Issue #782: the Spectrum is built at once on its first open, screen reader or not. Building it raises a hide
        // for each part created hidden, and Qt's Android bridge answers each with calls the main thread waits on, so
        // the first open under TalkBack stalls (a Qt limit, android/README.md "Known limits"). Building it a slice at a
        // time did not shorten the stall on a device, so it is not: open, it is built and shown in the same turn, and
        // what it puts in the tree is small.
        function test_j_first_spectrum_open_builds_at_once() {
            var loader = findChild(app, "spectrumLoader");
            verify(loader !== null, "the spectrum's loader");
            compare(loader.active, false, "the spectrum is not built yet");
            verify(!loader.asynchronous, "it is built at once with a screen reader on");
            testContext.startAccessibilityCount();
            findChild(app, "monitorScreen").openSpectrum();
            compare(loader.status, Loader.Ready, "built in the same turn as the open");
            wait(50);
            var built = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            // Its sheets are built closed: none may raise a show or hide under the window, which the bridge reads as a
            // change to the whole window (each sheet's refresh node is made only when it first takes the top).
            var windowLevel = [];
            for (var chain in built.chains) {
                if ((chain.indexOf("ObjectShow") === 0 || chain.indexOf("ObjectHide") === 0)
                        && (chain.indexOf("TreeRefresh") >= 0 || chain.indexOf("<unnamed>") >= 0
                            || chain.indexOf(" tst_a11y_scan_churn") === chain.indexOf(" ")))
                    windowLevel.push(chain + " x" + built.chains[chain]);
            }
            compare(windowLevel.length, 0, "shows or hides under the window on the first open: " + JSON.stringify(windowLevel));
            compare(loader.opacity, 1, "and shown at once");
            wait(50);
            var nodes = testContext.accessibleNodesUnder(loader.item).filter(function (node) { return node.inTree; });
            verify(nodes.length > 0 && nodes.length <= 20, nodes.length + " nodes in the tree when it shows");
            app.spectrumOpen = false;
            wait(20);
        }
    }
}
