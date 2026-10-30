// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

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
            testContext.stopAccessibilityCount();
            testContext.publishTarget(restore);
            testContext.setLifecyclePhase(0);
            testContext.useLifecycleHost(false);
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
        // list never move, and no geometry change reaches an accessibility client on a hop. The slot label itself
        // comes and goes with the carrier, which is the slot's truth, and is the one thing allowed to show or hide.
        function test_d_two_slot_hops_leave_the_monitor_still() {
            var monitor = findChild(app, "monitorScreen");
            verify(monitor !== null && monitor.visible, "the monitor is on screen");
            var label = findChild(monitor, "otherSlotLabel");
            var still = ["scanHoldButton", "scanAvoidButton", "scanNextButton", "recentCallsList"].map(function (name) {
                var found = findChild(monitor, name);
                verify(found !== null && found.visible, name + " must be on screen");
                return found;
            });
            // init() published reading(0), a DMR target, so the rotation has already visited a two-slot carrier.
            verify(label.visible, "the strip shows on the two-slot carrier");
            testContext.startAccessibilityCount();
            for (var i = 1; i <= hops; ++i) {
                testContext.publishTarget(reading(i));
                compare(label.visible, i % 3 === 0, "hop " + i + ": the slot label follows the carrier");
            }
            var counts = testContext.accessibilityCount();
            for (var item of still)
                compare(testContext.accessibilityCountWithin(item, "LocationChanged"), 0, item.objectName + ": "
                        + JSON.stringify(counts.chains));
            var shown = showHide(app.contentItem) - showHide(label);
            compare(shown, 0, "only the slot label shows or hides: " + churn(counts));
            // The label hides on every hop away from DMR (Qt reports its return as a creation, which Android does not
            // forward), and never more than once per carrier change.
            var changes = 2 * Math.floor(hops / 3);
            verify(showHide(label) >= changes / 2 && showHide(label) <= changes, churn(counts));
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
    }
}
