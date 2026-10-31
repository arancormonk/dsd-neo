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
            var label = findChild(monitor, "otherSlotLabel");
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
            testContext.startAccessibilityCount();
            for (var i = 1; i <= hops; ++i) {
                testContext.publishTarget(reading(i));
                wait(20);
            }
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            Ui.Navigation.speechStepFloorMs = floor;
            compare(showHide(monitor) - showHide(label), 0, "only the slot label shows or hides: " + churn(counts));
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

        // Issue #782: the Spectrum's first open under TalkBack during a scan held Android's main thread for over two
        // seconds. Building the screen raises a hide for each accessible item created hidden (Qt makes an item
        // accessible as its Accessible properties are set, before its gate is evaluated), and the bridge answers each
        // with calls it makes into the Qt thread and waits on from the main thread, while the Qt thread is still
        // building. With a screen reader on, the screen is built over several event-loop turns (the window's
        // incubation, a third of a frame at a time), so each of those calls waits a slice of the build, not all of it;
        // it is out of sight until it is built, and showing it then raises next to nothing.
        function test_j_first_spectrum_open_builds_over_turns() {
            var loader = findChild(app, "spectrumLoader");
            verify(loader !== null, "the spectrum's loader");
            compare(loader.active, false, "the spectrum is not built yet");
            verify(loader.asynchronous, "with a screen reader on it builds over several turns");
            findChild(app, "monitorScreen").openSpectrum();
            verify(loader.status !== Loader.Ready, "not built in one go");
            compare(loader.opacity, 0, "and out of sight while it builds");
            tryCompare(loader, "status", Loader.Ready, 5000);
            testContext.startAccessibilityCount();
            tryCompare(loader, "opacity", 1);
            wait(50);
            var counts = testContext.accessibilityCount();
            testContext.stopAccessibilityCount();
            var nodes = testContext.accessibleNodesUnder(loader.item).filter(function (node) { return node.inTree; });
            verify(nodes.length > 0 && nodes.length <= 20, nodes.length + " nodes in the tree when it shows");
            verify(typed(counts, "ObjectShow") + typed(counts, "ObjectHide") <= 2, "showing it raised: " + churn(counts));
            app.spectrumOpen = false;
            wait(20);
        }
    }
}
