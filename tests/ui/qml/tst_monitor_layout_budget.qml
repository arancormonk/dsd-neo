// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui
import "../../../src/ui/qt/qml/Util.js" as Util

// Issue #718: the Monitor puts Recent Calls first. One list scrolls; its header
// holds the "Signal & site" disclosure. The layout tier is the first that leaves
// two whole call rows on screen below the list header, with the list at its top
// and the disclosure closed, measured at the current size and fonts; where even
// the tightest fixed layout leaves less than a row, only Stop stays put.
Item {
    id: root

    width: 360
    height: 690

    // A writable stand-in for AppPrefs: the fixture's prefs map answers reads only.
    QtObject {
        id: fakePrefs
        property bool monitorDetailsExpanded: false
    }

    Loader {
        id: loader

        anchors.fill: parent
        source: uiDir + "/MonitorScreen.qml"
    }

    TestCase {
        id: tc

        name: "MonitorLayoutBudget"
        when: windowShown

        property var monitor: null
        property var list: null

        function item(name) {
            var found = findChild(tc.monitor, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        function m(key, value) {
            testContext.setMetric(key, value);
        }
        // The session's live-strip parts. The scan-timing line comes and goes with
        // each followed call, so it is set apart (setTiming), as traffic.
        function setStrip(slot, scan) {
            m("twoSlotCarrier", slot);
            m("scanRotationActive", scan);
            m("scanTargetCount", scan ? 3 : 0);
            m("scanTargetOrdinal", scan ? 2 : 0);
        }
        function setTiming(on) {
            m("scanTimingVisible", on);
            m("scanStayPhrase", on ? "Following call" : "");
            m("scanHangMs", on ? 2000 : 0);
        }
        function setCall(slot, state) {
            m("leadSlot", state === 0 ? 0 : slot);
            m("slot" + slot + "CallState", state);
            m("slot" + slot + "CallName", state === 0 ? "" : "Fire Dispatch");
            m("slot" + slot + "TgText", state === 0 ? "" : "4001");
            m("slot" + slot + "TgId", state === 0 ? 0 : 4001);
            m("slot" + slot + "SrcText", state === 0 ? "" : "7001");
            m("slot" + slot + "CallSeconds", 12);
        }
        function callsTop() {
            return tc.list.headerItem.y + tc.list.headerItem.height;
        }
        function atStart() {
            return Math.abs(tc.list.contentY - tc.list.originY) < 1;
        }
        function settle() {
            waitForRendering(tc.monitor);
            wait(20);
        }
        // Rows wholly on screen below the list header.
        function fullRowsVisible() {
            var end = tc.callsTop();
            var rows = 0;
            for (var i = 0; i < tc.list.count; ++i) {
                var row = tc.list.itemAtIndex(i);
                if (!row)
                    continue;
                var top = row.y - tc.list.contentY;
                if (row.y >= end - 0.5 && top >= -0.5 && top + row.height <= tc.list.height + 0.5)
                    ++rows;
            }
            return rows;
        }
        function viewportY(target) {
            return target.mapToItem(tc.list, 0, 0).y;
        }

        function initTestCase() {
            tc.monitor = loader.item;
            verify(tc.monitor !== null, "MonitorScreen.qml failed to load");
            tc.list = item("recentCallsList");
            tc.monitor.prefsStore = fakePrefs;
        }
        function init() {
            // The history clear below lands while the list may still be building a
            // row ahead of the viewport; the clear must not leave it a stale cancel.
            failOnWarning(/DelegateModel::cancel/);
            testContext.resetCommands();
            testContext.setHostRunning(true);
            m("radioInput", true);
            m("snrValid", true);
            m("snrDb", 31.9);
            m("carrierLock", true);
            m("siteLine", "P25 · WACN 45564 · SYS 006 · NAC 006 · RFSS 10 · SITE 10 · LRA 0A");
            m("siteConfirmed", true);
            m("qualityValid", true);
            m("ccFecValid", true);
            m("ccFecOkPct", 100);
            callHistory.clearAll();
            callHistory.pushMany(12, "TODAY");
            fakePrefs.monitorDetailsExpanded = false;
            root.width = 360;
            root.height = 690;
            Ui.Theme.fontScale = 1;
            settle();
            tc.list.positionViewAtBeginning();
            settle();
        }
        function cleanup() {
            // A press and release on a row is a tap, which opens its details.
            item("monitorHistoryDetail").visible = false;
            setStrip(false, false);
            setTiming(false);
            setCall(1, 0);
            setCall(2, 0);
            for (var key of ["snrValid", "carrierLock", "qualityValid", "ccFecValid", "voiceErrsValid", "rsValid", "tetraVisible", "tetraCodecFault"])
                m(key, false);
            m("encLockoutCount", 0);
            m("uiMessage", "");
            m("siteLine", "");
            m("tetraCodecText", "");
            m("siteProtocol", "");
            testContext.setPrefs("monitorDetailsExpanded", false);
            Ui.Theme.resetFontScale();
            item("monitorReceiverSheet").visible = false;
            tc.monitor.system = null;
            tc.monitor.sitesAvailable = false;
            decoderHost.receiverHealth = {state: "ok", source: "", session: 0};
            decoderHost.localDeviceBrokered = false;
            testContext.setHostRunning(false);
        }

        // ---- #647: a lost receiver in tier 5 ----

        // Where the header scrolls with the calls, a lost receiver also shows as
        // one fixed line above Stop, on screen with the header scrolled away, and
        // it opens the pill's sheet. Elsewhere the pill is in view and there is no
        // line. Losing the receiver never changes the tier.
        function test_lost_line_data() {
            return [{tag: "640x360@1.6", width: 640, height: 360, scale: 1.6, line: true},
                    {tag: "640x360@1", width: 640, height: 360, scale: 1, line: true},
                    {tag: "360x690@1.6", width: 360, height: 690, scale: 1.6, line: false},
                    {tag: "411x914@1", width: 411, height: 914, scale: 1, line: false}];
        }
        function test_lost_line(data) {
            decoderHost.localDeviceBrokered = true;
            tc.monitor.system = {name: "County P25", sourceType: "usb"};
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            setCall(1, 2);
            settle();
            var tier = tc.monitor.tier;
            compare(tier === 5, data.line, "tier " + tier);
            var line = item("monitorReceiverLostLine");
            verify(!line.visible, "no line while the receiver is healthy");
            var listBottom = tc.list.mapToItem(tc.monitor, 0, tc.list.height).y;
            decoderHost.receiverHealth = {state: "lost", source: "usb", session: 1};
            tryCompare(item("monitorLiveLabel"), "text", "LOST");
            settle();
            compare(tc.monitor.tier, tier, "the tier held");
            compare(line.visible, data.line);
            if (!data.line) {
                compare(tc.list.mapToItem(tc.monitor, 0, tc.list.height).y, listBottom, "the list kept its place");
                return;
            }
            // The header, and the pill with it, scrolled out of view.
            tc.list.contentY = tc.list.originY + tc.list.contentHeight - tc.list.height;
            settle();
            var pill = item("monitorLiveStatus");
            verify(pill.mapToItem(tc.list, 0, pill.height).y <= 0.5, "the pill scrolled away");
            var at = line.mapToItem(tc.monitor, 0, 0);
            var stop = item("stopListeningButton");
            verify(at.x >= -0.5 && at.y >= -0.5 && at.x + line.width <= tc.monitor.width + 0.5, "the line is inside the screen");
            verify(at.y + line.height <= stop.mapToItem(tc.monitor, 0, 0).y + 0.5, "the line sits above Stop");
            verify(tc.list.mapToItem(tc.monitor, 0, tc.list.height).y <= at.y + 0.5, "the list ends above the line");
            verify(line.height >= Ui.Theme.minimumTouchSize);
            compare(line.Accessible.name, "RTL-SDR lost");
            compare(line.Accessible.role, Accessible.Button);
            mouseClick(line, line.width / 2, line.height / 2);
            var sheet = item("monitorReceiverSheet");
            tryCompare(sheet, "visible", true);
            compare(sheet.confirmText, "Stop now");
            verify(!line.activeFocusOnTab, "no tab stop behind its sheet");
            sheet.cancel();
            tryCompare(sheet, "visible", false);
        }

        // ---- The budget ----

        function test_tier_budget_data() {
            var rows = [];
            var sizes = [[360, 690], [411, 780], [411, 914], [640, 360]];
            for (var size of sizes) {
                for (var scale of [1, 1.6]) {
                    for (var combo of [[false, false], [true, false], [false, true], [true, true]]) {
                        rows.push({
                            tag: size[0] + "x" + size[1] + "@" + scale + (combo[0] ? "+slot" : "") + (combo[1] ? "+scan" : ""),
                            width: size[0], height: size[1], scale: scale, slot: combo[0], scan: combo[1]
                        });
                    }
                }
            }
            // #647 B16: a brokered host with Sites and Spectrum, where the header
            // actions do not fit a 360 dp row and tier 0 keeps the title-line header.
            for (var broker of [[360, 690, 1], [360, 690, 1.6], [411, 914, 1], [411, 914, 1.6]]) {
                rows.push({tag: broker[0] + "x" + broker[1] + "@" + broker[2] + "+brokered", width: broker[0], height: broker[1],
                           scale: broker[2], slot: false, scan: false, brokered: true});
            }
            return rows;
        }
        // Brokered (Android) with Sites and Spectrum in play: the width rule in force.
        function useBrokered(on) {
            decoderHost.localDeviceBrokered = on;
            tc.monitor.sitesAvailable = on;
        }
        // The chosen tier is the first whose measured call space holds two rows, and
        // the screen as laid out agrees with the measurement: at the list's start,
        // the viewport less the list header is the call space the tier promised.
        function test_tier_budget(data) {
            useBrokered(data.brokered === true);
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            setCall(1, 2);
            setStrip(data.slot, data.scan);
            settle();
            tc.list.positionViewAtBeginning();
            settle();
            var tier = tc.monitor.tier;
            var row = tc.monitor.callRowHeight;
            verify(tc.list.itemAtIndex(0) !== null);
            compare(tc.list.itemAtIndex(0).height, row, "the budget's row is the delegate's height");
            for (var t = 0; t < Math.min(tier, 5); ++t)
                verify(tc.monitor.callSpaceFor(t) < 2 * row, "tier " + t + " already fit");
            verify(atStart());
            var measured = tc.list.height - tc.list.headerItem.height;
            if (tier < 5) {
                verify(Math.abs(measured - tc.monitor.callSpaceFor(tier)) < 1,
                       "measured " + measured + " vs budget " + tc.monitor.callSpaceFor(tier));
                if (tc.monitor.callSpaceFor(tier) >= 2 * row)
                    verify(fullRowsVisible() >= 2, "two whole rows under the header");
                else
                    compare(tier, 4, "only tier 4 may run short, and only by less than a row");
            } else {
                verify(tc.monitor.callSpaceFor(4) < row, "tier 5 only when tier 4 leaves less than a row");
            }
            // The header the budget measured is the one drawn: it ends where its
            // title lines or its actions end, stacked or not.
            var header = item("monitorHeader");
            var heading = item("monitorHeaderTitle").parent;
            var pill = item("monitorLiveStatus");
            verify(waitForPolish(pill.parent));
            var drawn = Math.max(heading.mapToItem(header, 0, heading.height).y, pill.mapToItem(header, 0, pill.height).y);
            verify(Math.abs(drawn - header.height) < 0.5, "header " + header.height + " vs drawn " + drawn);
            if (data.brokered && !tc.monitor.headerActionsFit) {
                verify(!tc.monitor.headerStacked, "nothing to stack");
                verify(!item("runningSiteChooserButton").visible && !item("openSpectrumButton").visible);
            }
            // The table for the record.
            console.log("TIER", data.tag, "tier", tier, "row", row,
                        "hero", item("monitorHero").height, "header", item("monitorHeader").height,
                        "strip", tc.monitor.stripHeight, "actions", item("monitorLiveActions").height,
                        "listHeader", tc.list.headerItem.height, "viewport", tc.list.height,
                        "callSpace", measured, "rows", fullRowsVisible());
        }

        // On a 360 dp phone at normal text, every live-strip combination keeps two
        // whole call rows on screen; on the tester's 411x914 phone the roomiest
        // layout holds in every combination.
        function test_phone_portrait_guarantees_data() {
            var rows = [];
            for (var combo of [[false, false], [true, false], [false, true], [true, true]]) {
                rows.push({tag: "360x690" + (combo[0] ? "+slot" : "") + (combo[1] ? "+scan" : ""), width: 360, height: 690, slot: combo[0], scan: combo[1], tier: -1});
                rows.push({tag: "411x914" + (combo[0] ? "+slot" : "") + (combo[1] ? "+scan" : ""), width: 411, height: 914, slot: combo[0], scan: combo[1], tier: 0});
                rows.push({tag: "360x690+brokered" + (combo[0] ? "+slot" : "") + (combo[1] ? "+scan" : ""), width: 360, height: 690, slot: combo[0], scan: combo[1], tier: -1, brokered: true});
                rows.push({tag: "411x914+brokered" + (combo[0] ? "+slot" : "") + (combo[1] ? "+scan" : ""), width: 411, height: 914, slot: combo[0], scan: combo[1], tier: 0, brokered: true});
            }
            return rows;
        }
        function test_phone_portrait_guarantees(data) {
            useBrokered(data.brokered === true);
            root.width = data.width;
            root.height = data.height;
            setCall(1, 2);
            setStrip(data.slot, data.scan);
            settle();
            tc.list.positionViewAtBeginning();
            settle();
            verify(tc.monitor.tier <= 4);
            if (data.tier >= 0)
                compare(tc.monitor.tier, data.tier);
            verify(fullRowsVisible() >= 2, fullRowsVisible() + " rows visible in tier " + tc.monitor.tier);
        }

        // Call traffic never changes the tier, and the hero's box is reserved:
        // active, ended and idle leave the list exactly where it was.
        function test_call_traffic_never_moves_the_list_data() {
            return [{tag: "phone", width: 360, height: 690, scale: 1},
                    {tag: "phone-large", width: 411, height: 914, scale: 1.6}];
        }
        function test_call_traffic_never_moves_the_list(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            setStrip(true, false);
            setCall(1, 2);
            settle();
            tc.list.positionViewAtBeginning();
            settle();
            var tier = tc.monitor.tier;
            var hero = item("monitorHero");
            var heroHeight = hero.height;
            var listTop = tc.list.mapToItem(tc.monitor, 0, 0).y;
            for (var state of [1, 0, 2]) {
                setCall(1, state);
                settle();
                compare(tc.monitor.tier, tier);
                compare(hero.height, heroHeight);
                compare(tc.list.mapToItem(tc.monitor, 0, 0).y, listTop);
            }
        }

        // The scan-timing line is published with each followed call and withdrawn
        // between them; the header keeps its line for the whole rotation, so
        // neither the list nor the hero moves and the tier holds.
        function test_scan_timing_never_moves_the_list_data() {
            return [{tag: "phone", width: 360, height: 690, scale: 1},
                    {tag: "tester", width: 411, height: 914, scale: 1},
                    {tag: "large", width: 411, height: 780, scale: 1.6}];
        }
        function test_scan_timing_never_moves_the_list(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            setStrip(true, true);
            setCall(1, 2);
            setTiming(false);
            settle();
            tc.list.positionViewAtBeginning();
            settle();
            var tier = tc.monitor.tier;
            var hero = item("monitorHero");
            var header = item("monitorHeader");
            var heroY = hero.mapToItem(tc.monitor, 0, 0).y;
            var listTop = tc.list.mapToItem(tc.monitor, 0, 0).y;
            var headerHeight = header.height;
            verify(item("scanTimingRow").visible, "the line is kept for the rotation");
            compare(item("scanTimingRow").text, "");
            for (var on of [true, false, true, false]) {
                setTiming(on);
                settle();
                compare(item("scanTimingRow").text.length > 0, on);
                compare(tc.monitor.tier, tier);
                compare(header.height, headerHeight);
                compare(hero.mapToItem(tc.monitor, 0, 0).y, heroY);
                compare(tc.list.mapToItem(tc.monitor, 0, 0).y, listTop);
            }
        }

        // Issue #782: a scan rotation that mixes two-slot DMR with single-slot
        // NXDN and P25 targets. Once it has visited a two-slot carrier the slot
        // strip's place stays reserved, so the hops move nothing: not the tier,
        // not the header, not the list, and not Hold, Avoid or Next, which sit
        // under the strip. Only the slot's own content follows the carrier.
        function hopTo(i) {
            var protocol = ["DMR", "NXDN", "P25"][i % 3];
            testContext.publishTarget({twoSlotCarrier: protocol === "DMR", siteProtocol: protocol, scanTargetOrdinal: (i % 3) + 1});
        }
        function sceneY(name) {
            return item(name).mapToItem(null, 0, 0).y;
        }
        function test_two_slot_hops_never_move_the_screen_data() {
            return [{tag: "phone", width: 360, height: 690, scale: 1},
                    {tag: "tester", width: 411, height: 914, scale: 1},
                    {tag: "large", width: 411, height: 780, scale: 1.6},
                    {tag: "landscape", width: 640, height: 360, scale: 1}];
        }
        function test_two_slot_hops_never_move_the_screen(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            setStrip(false, true);
            settle();
            hopTo(0);
            settle();
            tc.list.positionViewAtBeginning();
            settle();
            var buttons = ["scanHoldButton", "scanAvoidButton", "scanNextButton"];
            var held = {tier: tc.monitor.tier, wide: tc.monitor.wideHeaderActions, strip: tc.monitor.stripHeight, listY: tc.list.y, listHeight: tc.list.height};
            var scene = buttons.map(function (name) { return item(name).mapToItem(null, 0, 0); });
            var label = item("otherSlotLabel");
            for (var i = 1; i <= 12; ++i) {
                hopTo(i);
                settle();
                var at = "hop " + i + " (" + ["DMR", "NXDN", "P25"][i % 3] + ")";
                compare(tc.monitor.tier, held.tier, at + ": tier");
                compare(tc.monitor.wideHeaderActions, held.wide, at + ": header actions");
                compare(tc.monitor.stripHeight, held.strip, at + ": strip budget");
                compare(tc.list.y, held.listY, at + ": list top");
                compare(tc.list.height, held.listHeight, at + ": list height");
                for (var b = 0; b < buttons.length; ++b) {
                    var now = item(buttons[b]).mapToItem(null, 0, 0);
                    compare(now.x, scene[b].x, at + ": " + buttons[b] + " x");
                    compare(now.y, scene[b].y, at + ": " + buttons[b] + " y");
                }
                // The slot's truth: "SLOT 2 · idle" on a two-slot carrier and
                // nothing on a single-slot one.
                if (i % 3 === 0) {
                    verify(label.visible, at + ": the idle slot shows");
                    compare(label.text, "SLOT 2 · idle");
                } else {
                    verify(!item("otherSlotPanel").visible, at + ": no slot on a single-slot carrier");
                    verify(!label.visible, at + ": no slot label on a single-slot carrier");
                }
            }
        }
        // A second call on the other slot shows whatever the carrier is known to
        // be, in the same reserved place.
        function test_two_slot_hops_show_an_active_other_slot() {
            setStrip(false, true);
            hopTo(0);
            settle();
            var holdY = sceneY("scanHoldButton");
            hopTo(1);
            setCall(2, 2);
            setCall(1, 2);
            settle();
            verify(item("otherSlotPanel").visible, "an active second call shows on a single-slot carrier");
            verify(item("otherSlotSkip").visible);
            compare(item("otherSlotLabel").text, "SLOT 2");
            compare(sceneY("scanHoldButton"), holdY);
            m("slot2CallState", 0);
            settle();
            verify(!item("otherSlotPanel").visible, "the strip goes with the call on a single-slot carrier");
            compare(sceneY("scanHoldButton"), holdY);
        }
        // The reservation lasts for the rotation: once it ends, or before a
        // rotation has seen a two-slot carrier, the strip follows the carrier.
        function test_two_slot_reservation_ends_with_the_rotation() {
            setStrip(false, true);
            settle();
            hopTo(1);
            settle();
            var unreserved = tc.monitor.stripHeight;
            compare(unreserved, item("scanControlsRow").height, "no strip before a two-slot carrier");
            hopTo(0);
            hopTo(2);
            settle();
            var reserved = tc.monitor.stripHeight;
            verify(reserved > unreserved, "a single-slot hop keeps the place after a two-slot one");
            m("scanRotationActive", false);
            settle();
            compare(tc.monitor.stripHeight, 0, "no rotation, no single-slot strip");
            m("scanRotationActive", true);
            settle();
            compare(tc.monitor.stripHeight, unreserved, "a new rotation starts unreserved");
            hopTo(0);
            settle();
            compare(tc.monitor.stripHeight, reserved);
            hopTo(1);
            settle();
            compare(tc.monitor.stripHeight, reserved);
            testContext.setHostRunning(false);
            testContext.setHostRunning(true);
            settle();
            compare(tc.monitor.stripHeight, unreserved, "a new session starts unreserved");
        }
        // Without a rotation nothing is reserved: a single-target session's
        // strip follows its carrier.
        function test_two_slot_reservation_needs_a_rotation() {
            setStrip(true, false);
            settle();
            verify(tc.monitor.stripHeight > 0, "the strip shows on a two-slot carrier");
            m("twoSlotCarrier", false);
            settle();
            compare(tc.monitor.stripHeight, 0, "and goes with it outside a rotation");
            verify(!tc.monitor.rotationReservesStrip);
        }
        // The reservation is the rotation's, not the screen's: hops made while
        // the monitor is hidden keep it, and the screen comes back as it left.
        function test_two_slot_reservation_holds_while_hidden() {
            setStrip(false, true);
            hopTo(0);
            settle();
            var reserved = tc.monitor.stripHeight;
            var holdY = sceneY("scanHoldButton");
            tc.monitor.visible = false;
            try {
                hopTo(1);
                hopTo(2);
                settle();
            } finally {
                tc.monitor.visible = true;
            }
            settle();
            compare(tc.monitor.stripHeight, reserved);
            compare(sceneY("scanHoldButton"), holdY);
            verify(!item("otherSlotPanel").visible, "no slot on the single-slot carrier");
        }

        // An empty summary keeps its line: the disclosure's height, which the
        // budget measures, never follows the readings.
        function test_empty_summary_keeps_the_row_height() {
            var card = item("monitorDetailsCard");
            var height = card.height;
            var tier = tc.monitor.tier;
            for (var key of ["radioInput", "qualityValid"])
                m(key, false);
            m("siteLine", "");
            tryCompare(item("monitorDetailsRow"), "accessibleDescription", "");
            settle();
            compare(card.height, height);
            compare(tc.monitor.tier, tier);
            m("radioInput", true);
        }

        // The hero's lines fit its reserved box in both forms, at large text too,
        // without overlapping one another.
        function test_hero_contents_contained_data() {
            return [{tag: "full", width: 411, height: 914, scale: 1, compact: false},
                    {tag: "full-large", width: 411, height: 914, scale: 1.6, compact: false},
                    {tag: "compact", width: 360, height: 690, scale: 1, compact: true},
                    {tag: "compact-large", width: 360, height: 690, scale: 1.6, compact: true}];
        }
        function test_hero_contents_contained(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            setCall(1, 2);
            m("slot1CallEnc", true);
            m("slot1EncText", "AES-256 K:65535");
            setStrip(data.compact, data.compact);
            try {
                settle();
                compare(tc.monitor.heroCompact, data.compact);
                var hero = item("monitorHero");
                var names = ["heroNameLine", "heroIds", "heroEncTag", "heroAlgorithm", data.compact ? "heroCompactDuration" : "heroDuration"];
                var boxes = [];
                for (var name of names) {
                    var part = item(name);
                    verify(part.visible, name);
                    var at = part.mapToItem(hero, 0, 0);
                    verify(at.x >= -0.5 && at.y >= -0.5 && at.x + part.width <= hero.width + 0.5 && at.y + part.height <= hero.height + 0.5,
                           name + " inside the hero");
                    boxes.push({name: name, x: at.x, y: at.y, w: part.width, h: part.height});
                }
                // The duration either ends the name line (compact) or sits below the subline.
                var duration = boxes[4];
                var subline = item("heroIds").mapToItem(hero, 0, 0).y + item("heroIds").height;
                if (!data.compact)
                    verify(duration.y >= subline - 0.5, "the duration sits under the subline");
                else
                    verify(duration.y + duration.h <= item("heroNameLine").mapToItem(hero, 0, 0).y + item("heroNameLine").height + 0.5);
            } finally {
                m("slot1CallEnc", false);
                m("slot1EncText", "");
            }
        }

        // One scrolling surface: no nested Flickable inside the list, and the old
        // body is gone. At large text every row is reachable by scrolling it.
        function test_one_list_reaches_every_row_data() {
            return [{tag: "phone-large", width: 360, height: 690, scale: 1.6},
                    {tag: "landscape", width: 640, height: 360, scale: 1},
                    {tag: "landscape-large", width: 640, height: 360, scale: 1.6},
                    {tag: "wide-landscape", width: 914, height: 411, scale: 1},
                    {tag: "wide-landscape-large", width: 914, height: 411, scale: 1.6}];
        }
        function test_one_list_reaches_every_row(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            setCall(1, 2);
            setStrip(true, true);
            settle();
            compare(findChild(tc.monitor, "monitorBody"), null);
            var nested = 0;
            var stack = [tc.list.contentItem];
            while (stack.length > 0) {
                var node = stack.pop();
                for (var i = 0; i < node.children.length; ++i) {
                    if (node.children[i] instanceof Flickable)
                        ++nested;
                    stack.push(node.children[i]);
                }
            }
            compare(nested, 0, "a Flickable inside the list");
            // Every control in the list header is reachable, and so is the last row.
            for (var name of ["monitorDetailsRow", "scanNextButton", "otherSlotPanel"]) {
                var control = item(name);
                tc.list.contentY = Math.max(tc.list.originY, Math.min(control.mapToItem(tc.list.contentItem, 0, 0).y, tc.list.originY + tc.list.contentHeight - tc.list.height));
                settle();
                verify(viewportY(control) >= -0.5 && viewportY(control) + Math.min(control.height, tc.list.height) <= tc.list.height + 0.5, name + " reachable");
            }
            tryVerify(function () {
                tc.list.contentY = tc.list.originY + tc.list.contentHeight - tc.list.height;
                var last = tc.list.itemAtIndex(tc.list.count - 1);
                return last !== null && viewportY(last) + last.height <= tc.list.height + 1;
            }, 2000, "the last row is reachable");
        }

        // ---- The disclosure ----

        // Closed by default, the readings hidden; opening it shows them in their
        // usual order. It reads as a checkable control named with its summary.
        function test_disclosure_defaults_closed() {
            var row = item("monitorDetailsRow");
            var details = item("monitorDetails");
            verify(!tc.monitor.detailsExpanded);
            verify(!details.visible);
            verify(!item("siteRow").visible);
            verify(row.Accessible.checkable);
            verify(!row.Accessible.checked);
            compare(row.Accessible.name, "Signal & site");
            compare(row.accessibleDescription, "SNR 31.9 dB · CC FEC 100% · SITE 10");
            mouseClick(row);
            // The tap writes the preference, and the disclosure follows it.
            verify(fakePrefs.monitorDetailsExpanded, "the tap is remembered");
            verify(tc.monitor.detailsExpanded);
            tryCompare(details, "visible", true);
            verify(row.Accessible.checked);
            // Today's rows, in today's order.
            var order = ["siteRow", "openKeySheetButton", "signalStrip", "decodeQualityRow"];
            var last = -1;
            for (var name of order) {
                var y = item(name).mapToItem(details, 0, 0).y;
                verify(y > last, name + " out of order");
                last = y;
            }
            mouseClick(row);
            verify(!fakePrefs.monitorDetailsExpanded);
            verify(!tc.monitor.detailsExpanded);
            // A change to the stored value from elsewhere reaches the screen.
            fakePrefs.monitorDetailsExpanded = true;
            verify(tc.monitor.detailsExpanded);
        }

        // The disclosure is remembered (prefs monitor/detailsExpanded), on a new
        // Monitor as on this one.
        function test_disclosure_remembered() {
            testContext.setPrefs("monitorDetailsExpanded", true);
            var fresh = createTemporaryObject(Qt.createComponent(uiDir + "/MonitorScreen.qml"), root, {width: 360, height: 690, visible: false});
            verify(fresh !== null);
            verify(fresh.detailsExpanded, "a new Monitor opens as the last was left");
            testContext.setPrefs("monitorDetailsExpanded", false);
            verify(!fresh.detailsExpanded);
        }

        function test_site_short_form_data() {
            return [{tag: "p25", line: "P25 · WACN 45564 · SYS 006 · NAC 006 · RFSS 10 · SITE 10 · LRA 0A", want: "SITE 10"},
                    {tag: "edacs", line: "EDACS · SITE 007 [07] · Standard/Networked · No ESK", want: "SITE 007 [07]"},
                    {tag: "dmr", line: "DMR · CC 1 · Rest LSN 2", want: "DMR · CC 1"},
                    {tag: "none", line: "", want: ""}];
        }
        function test_site_short_form(data) {
            compare(Util.siteShortForm(data.line), data.want);
        }
        function test_summary_markup_escapes() {
            var out = Util.summaryMarkup([{text: "A<b>&", color: ""}, {text: "ENC", color: "#ff00ff"}]);
            compare(out.plain, "A<b>& · ENC");
            compare(out.markup, "A&lt;b&gt;&amp; · <font color=\"#ff00ff\">ENC</font>");
        }

        // The summary: SNR, quality (a codec fault ahead of it, in the alert colour),
        // decryption, an ENC LOCKOUT count in magenta, the site. One elided line.
        function test_summary_tokens() {
            var row = item("monitorDetailsRow");
            m("tetraVisible", true);
            m("tetraCodecFault", true);
            m("tetraCodecText", "no codec");
            m("encLockoutCount", 4);
            tryCompare(row, "accessibleDescription", "SNR 31.9 dB · CODEC no codec · CC FEC 100% · ENC LOCKOUT 4 · SITE 10");
            verify(row.subtitle.indexOf("<font color=\"" + String(Ui.Theme.alert) + "\">CODEC no codec</font>") >= 0, row.subtitle);
            verify(row.subtitle.indexOf("<font color=\"" + String(Ui.Theme.magenta) + "\">ENC LOCKOUT 4</font>") >= 0, row.subtitle);
            // A fault is said in the summary, never by opening the readings.
            verify(!tc.monitor.detailsExpanded);
            m("radioInput", false);
            m("ccFecValid", false);
            m("voiceErrsValid", true);
            m("voiceErrsPerFrame", 2.5);
            m("tetraCodecFault", false);
            m("encLockoutCount", 0);
            tryCompare(row, "accessibleDescription", "VOICE 2.5 err/fr · SITE 10");
            m("voiceErrsValid", false);
            m("radioInput", true);
        }

        function test_summary_elides_on_one_line_data() {
            return [{tag: "phone", scale: 1}, {tag: "phone-large", scale: 1.6}];
        }
        function test_summary_elides_on_one_line(data) {
            Ui.Theme.fontScale = data.scale;
            m("encLockoutCount", 12);
            settle();
            var row = item("monitorDetailsRow");
            var summary = null;
            for (var text of findChildrenOf(row)) {
                if (text instanceof Text && text.text === row.subtitle)
                    summary = text;
            }
            verify(summary !== null);
            compare(summary.lineCount, 1);
            verify(summary.truncated, "the summary is longer than a 360 dp row");
            verify(summary.mapToItem(row, summary.width, 0).x <= row.width + 0.5);
            compare(row.height, item("monitorDetailsCard").height);
        }
        function findChildrenOf(parent) {
            var out = [];
            var stack = [parent];
            while (stack.length > 0) {
                var node = stack.pop();
                for (var i = 0; i < node.children.length; ++i) {
                    out.push(node.children[i]);
                    stack.push(node.children[i]);
                }
            }
            return out;
        }

        // ---- Following the calls with a header ----

        // Opening and closing the readings keeps the disclosure row where it is on
        // screen, at the list's start and partly scrolled alike.
        function test_disclosure_holds_its_place_data() {
            return [{tag: "start", offset: 0}, {tag: "scrolled", offset: 30}];
        }
        function test_disclosure_holds_its_place(data) {
            root.width = 411;
            root.height = 914;
            settle();
            tc.list.contentY = tc.list.originY + data.offset;
            settle();
            var row = item("monitorDetailsRow");
            var y = viewportY(row);
            tc.monitor.setDetailsExpanded(true);
            settle();
            compare(viewportY(row), y);
            verify(item("siteRow").visible);
            tc.monitor.setDetailsExpanded(false);
            settle();
            compare(viewportY(row), y);
        }

        // With the open readings in view, a landing call moves nothing on screen;
        // it lands at the top of the calls, directly under the header.
        function test_insert_with_the_header_in_view() {
            root.width = 411;
            root.height = 914;
            tc.monitor.setDetailsExpanded(true);
            settle();
            tc.list.positionViewAtBeginning();
            settle();
            var row = item("monitorDetailsRow");
            var y = viewportY(row);
            var contentY = tc.list.contentY;
            var name = callHistory.push("TODAY");
            tryVerify(function () { return tc.list.itemAtIndex(0) !== null && tc.list.itemAtIndex(0).name === name; });
            settle();
            compare(tc.list.contentY, contentY);
            compare(viewportY(row), y);
            compare(tc.list.itemAtIndex(0).y, tc.callsTop());
        }

        // Keyboard focus reaching the disclosure from the calls, after scrolling and
        // after prepends shifted the list's origin, scrolls it wholly into view.
        function test_keyboard_focus_reveals_the_disclosure_data() {
            return [{tag: "scrolled", prepends: 0}, {tag: "after-prepends", prepends: 3}];
        }
        function test_keyboard_focus_reveals_the_disclosure(data) {
            var row = item("monitorDetailsRow");
            tc.list.contentY = tc.callsTop() + 3 * tc.monitor.callRowHeight;
            settle();
            for (var i = 0; i < data.prepends; ++i)
                callHistory.push("TODAY");
            settle();
            verify(viewportY(row) + row.height < 0, "the disclosure is scrolled away");
            var first = tc.list.itemAtIndex(tc.list.indexAt(10, tc.list.contentY + 10));
            first.forceActiveFocus(Qt.TabFocusReason);
            settle();
            row.forceActiveFocus(Qt.BacktabFocusReason);
            tryVerify(function () { return viewportY(row) >= -0.5 && viewportY(row) + row.height <= tc.list.height + 0.5; },
                      2000, "the focused disclosure is revealed");
            verify(tc.list.contentY >= tc.list.originY - 0.5);
        }

        // Position alone decides: with the header still focused, the list scrolled
        // to the top of the calls follows a landing call there.
        function test_scrolled_to_calls_with_the_header_focused_follows() {
            var row = item("monitorDetailsRow");
            row.forceActiveFocus(Qt.TabFocusReason);
            settle();
            tc.list.contentY = tc.callsTop();
            settle();
            verify(row.activeFocus);
            var name = callHistory.push("TODAY");
            tryVerify(function () { return Math.abs(tc.list.contentY - tc.callsTop()) < 1; });
            settle();
            tryVerify(function () {
                var top = tc.list.itemAtIndex(tc.list.indexAt(10, tc.list.contentY + 5));
                return top !== null && top.name === name;
            }, 2000, "the newest call is the row at the top");
            compare(tc.list.contentY, tc.callsTop());
            // A landing call never takes keyboard focus off the header.
            verify(row.activeFocus);
        }

        // A header resize under a held finger waits for the release before the view
        // moves to keep the disclosure in place.
        function test_header_resize_under_a_held_finger() {
            root.width = 411;
            root.height = 914;
            settle();
            tc.list.contentY = tc.list.originY + 30;
            settle();
            var row = item("monitorDetailsRow");
            var y = viewportY(row);
            var pressY = tc.list.height - 20;
            mousePress(tc.list, tc.list.width / 2, pressY);
            tc.monitor.setDetailsExpanded(true);
            settle();
            verify(Math.abs(viewportY(row) - y) > 1, "not corrected under the finger");
            mouseRelease(tc.list, tc.list.width / 2, pressY);
            tryVerify(function () { return Math.abs(viewportY(row) - y) < 1; }, 2000, "corrected on release");
        }

        // Far down a long history the first row is no longer instantiated. A
        // header resize there (the engine's message line appearing under the
        // disclosure) happens off screen and must not move what the reader sees.
        function test_header_resize_with_the_first_row_virtualized() {
            callHistory.clearAll();
            callHistory.pushMany(200, "TODAY");
            settle();
            tc.list.positionViewAtIndex(100, ListView.Beginning);
            settle();
            verify(tc.list.itemAtIndex(0) === null, "the first row is virtualized");
            var index = tc.list.indexAt(10, tc.list.contentY + 10);
            var row = tc.list.itemAtIndex(index);
            verify(row !== null);
            var y = viewportY(row);
            var contentY = tc.list.contentY;
            m("uiMessage", "Muted");
            settle();
            compare(tc.list.contentY, contentY);
            compare(viewportY(row), y);
            m("uiMessage", "");
            settle();
            compare(tc.list.contentY, contentY);
            compare(viewportY(row), y);
        }
    }
}
