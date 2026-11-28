// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #782: the Monitor's toggles as TalkBack presses and reads them.
//
// The scan's Hold, Hold TG and Mute read back a command the decoder applies later: `checked` follows the engine's
// state, which reaches the screen on a later refresh. Qt's Android bridge builds the click event TalkBack reads out as
// soon as the press returns, so each toggle reports the state asked for from the press on, until the engine's state
// arrives or a few seconds pass without it.
Item {
    id: root
    width: 420
    height: 900
    Loader {
        id: monitorLoader
        anchors.fill: parent
        source: uiDir + "/MonitorScreen.qml"
    }
    Loader {
        id: siteLoader
        anchors.fill: parent
        source: uiDir + "/SiteSheet.qml"
    }
    // What a sheet over the screen leaves reachable: only itself.
    Item {
        id: cover
    }
    TestCase {
        name: "A11yMonitorControls"
        when: windowShown
        readonly property var monitor: monitorLoader.item
        property var restore: ({})

        function set(values) {
            for (var key in values) {
                if (!(key in restore))
                    restore[key] = metrics[key];
                testContext.setMetric(key, values[key]);
            }
        }
        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            verify(monitor !== null);
        }
        function init() {
            testContext.setHostRunning(true);
            set({scanTargetCount: 3, scanTargetOrdinal: 1, scanTargetId: "target-1", scanRotationActive: true,
                scanHold: false});
            monitor.visible = true;
            waitForRendering(root);
            wait(20);
        }
        function cleanup() {
            if (siteLoader.item)
                siteLoader.item.visible = false;
            Ui.Navigation.modals = [];
            for (var key in restore)
                testContext.setMetric(key, restore[key]);
            restore = {};
            testContext.setHostRunning(false);
        }

        function test_hold_scan_reports_the_state_asked_for() {
            var hold = findChild(monitor, "scanHoldButton");
            verify(hold !== null);
            compare(testContext.accessibleNode(hold).checked, false);
            var calls = testContext.scanHoldCalls();
            verify(testContext.pressAccessible(hold), "TalkBack's press");
            compare(testContext.scanHoldCalls(), calls + 1, "the hold is asked for");
            compare(testContext.accessibleNode(hold).checked, true, "and read as held from the press on");
            // The engine holds: it stays held.
            set({scanHold: true});
            wait(20);
            compare(testContext.accessibleNode(hold).checked, true);
            verify(testContext.pressAccessible(hold));
            compare(testContext.accessibleNode(hold).checked, false, "released from the press on");
            set({scanHold: false});
            wait(20);
            compare(testContext.accessibleNode(hold).checked, false);
            // A press the engine never applies goes back to the engine's state.
            verify(testContext.pressAccessible(hold));
            compare(testContext.accessibleNode(hold).checked, true);
            tryCompare(hold, "shownChecked", false, 3000, "back to the engine's state when the hold never comes");
            compare(testContext.accessibleNode(hold).checked, false);
        }

        // Issue #666 (B9): each scan control says what it acts on. On a screen whose call actions also hold and avoid
        // (a talkgroup), "Avoid" and "Next" alone did not say they act on the scan; Prev is named alike. Each name
        // starts with the word on the button, or ("Prev") the word it shortens, so a reader hears what the eye sees.
        function test_scan_controls_are_named_for_the_scan() {
            var names = {scanHoldButton: "Hold the scan here", scanAvoidButton: "Avoid this scan channel",
                         scanPrevButton: "Previous scan channel", scanNextButton: "Next scan channel"};
            for (var objectName in names) {
                var button = findChild(monitor, objectName);
                verify(button !== null, objectName + " is missing");
                var node = testContext.accessibleNode(button);
                verify(node.inTree && node.focusable, objectName + " is a swipe stop");
                compare(node.name, names[objectName], objectName);
            }
        }

        // Issue #672 (B-Qt2): the SCAN AVOIDS row's buttons say what they act on, each name starting with the word on
        // the button. Clear was read as "Clear" alone, which could as well be the call log or the squelch.
        function test_scan_avoid_buttons_are_named_for_the_avoids() {
            testContext.setPrefs("monitorDetailsExpanded", true);
            set({scanAvoidsManageable: true, scanAvoidCount: 2});
            var row = findChild(monitor, "scanAvoidRow");
            tryVerify(function () { return row.visible });
            wait(20);
            var names = {scanAvoidShowButton: "Show avoided scan channels", scanAvoidClearButton: "Clear scan avoids"};
            for (var objectName in names) {
                var button = findChild(monitor, objectName);
                verify(button !== null, objectName + " is missing");
                var node = testContext.accessibleNode(button);
                verify(node.inTree && node.focusable, objectName + " is a swipe stop");
                compare(node.name, names[objectName], objectName);
            }
            testContext.setPrefs("monitorDetailsExpanded", false);
        }

        // Hold TG sends the hold or release the press asks for, not one read from the engine's state: pressed twice
        // before the engine's state arrives, it is read as held and then released, and the decoder is asked for the
        // hold and then the release (twice "hold" read the engine's unchanged state).
        function test_hold_tg_sends_the_state_asked_for() {
            set({leadSlot: 1, slot1CallState: 2, slot1CallKind: 0, slot1TgText: "42", slot1TgId: 42, heldTg: 0});
            var hold = findChild(monitor, "holdTalkgroupButton");
            verify(hold !== null && hold.enabled);
            var calls = testContext.holdCalls();
            verify(testContext.pressAccessible(hold));
            compare(testContext.accessibleNode(hold).checked, true);
            compare(testContext.holdCalls(), calls + 1);
            compare(testContext.lastHoldTg(), 42, "the hold is asked for");
            verify(testContext.pressAccessible(hold));
            compare(testContext.accessibleNode(hold).checked, false);
            compare(testContext.holdCalls(), calls + 2);
            compare(testContext.lastHoldTg(), 0, "and the release, as read out");
        }

        // The recent calls pane holds the Monitor's list header ("Signal & site", the site row, the strip, the "Recent
        // calls" heading) above the calls. It is a container TalkBack may land on and pass through, never a node that
        // takes Qt's keyboard focus: a pane that took it (an earlier attempt to let TalkBack swipe back from it) trapped
        // a backward swipe on itself, since the list is a focus scope that already holds the focus while a header part
        // has it, and made TalkBack skip the header going forward. So the pane is not focusable and offers no set-focus
        // action (the bridge lands TalkBack's focus on it without asking Qt), and its header parts are swipe stops.
        function test_recent_calls_pane_is_no_focus_target() {
            testContext.setPrefs("monitorDetailsExpanded", true);
            // The site row shows only with a site to name (issue #902: hidden, it is no stop).
            set({siteLine: "P25 · NAC 293"});
            wait(50);
            var pane = findChild(monitor, "recentCallsList");
            var details = findChild(monitor, "monitorDetailsRow");
            verify(pane !== null && details !== null);
            var node = testContext.accessibleNode(pane);
            verify(node.inTree, "the pane is in the tree");
            compare(node.focusable, false, "the pane is not focusable");
            for (var part of [details, findChild(monitor, "siteRow"), findChild(monitor, "recentCallsLabel")]) {
                var partNode = testContext.accessibleNode(part);
                verify(partNode.inTree && partNode.focusable, part.objectName + " is a swipe stop in the header");
            }
            // TalkBack's focus on the header, then on the pane: the pane takes nothing, so the header keeps the focus.
            verify(testContext.focusAccessible(details), "the header row offers the set-focus action");
            wait(20);
            verify(!testContext.focusAccessible(pane), "the pane offers no set-focus action");
            wait(20);
            verify(details.activeFocus, "the header row keeps the keyboard focus");
            testContext.setPrefs("monitorDetailsExpanded", false);
        }

        // Issue #902: a group of readings read as one node (the Monitor's ReadingFlow and ReadingRow, the site sheet's
        // fields) is gated as each single reading is. Qt keeps a static text a stop with SetFocus whatever its flags,
        // so out of reach (hidden, or under a sheet) the group is a plain pane, no stop, and a group a screen reader's
        // SetFocus gave the keyboard gives it back.
        function groupGone(group, why) {
            tryVerify(function () { return !group.activeFocus && !group.focus; }, 1000, why + ": the keyboard is given back");
            var node = testContext.accessibleNode(group);
            verify(!node.inTree, why + ": out of the tree");
            compare(node.name, "", why + ": nameless");
            verify(!node.focusable && node.actions.indexOf(testContext.setFocusAction) < 0, why + ": no stop, " + JSON.stringify(node.actions));
            testContext.accessibleDoAction(group, testContext.setFocusAction);
            verify(!group.activeFocus, why + ": a cached SetFocus takes no keyboard");
        }
        function grouped_data() {
            return [
                {tag: "ReadingFlow", name: "decodeQualityRow", shown: {qualityValid: true, ccFecValid: true, ccFecOkPct: 97},
                    hidden: {qualityValid: false}},
                {tag: "ReadingRow", name: "encLockoutRow", shown: {encLockoutCount: 3}, hidden: {encLockoutCount: 0}}
            ];
        }
        function test_grouped_readings_follow_their_gate_data() {
            return grouped_data();
        }
        function test_grouped_readings_follow_their_gate(data) {
            testContext.setPrefs("monitorDetailsExpanded", true);
            set(data.shown);
            var group = findChild(monitor, data.name);
            verify(group !== null);
            tryVerify(function () { return group.visible; });
            wait(50);
            for (var how of ["hidden", "covered"]) {
                var node = testContext.accessibleNode(group);
                verify(node.inTree && node.focusable && node.name.length > 0, how + ": shown, a named stop");
                testContext.accessibleDoAction(group, testContext.setFocusAction);
                verify(group.activeFocus, how + ": SetFocus gives the group the keyboard");
                if (how === "hidden")
                    set(data.hidden);
                else
                    Ui.Navigation.modals = [cover];
                groupGone(group, how);
                if (how === "hidden")
                    set(data.shown);
                else
                    Ui.Navigation.modals = [];
                wait(50);
            }
            testContext.setPrefs("monitorDetailsExpanded", false);
        }
        // Issue #902 with discussion #900 F3 and issue #901: the Tier III channels row, the NO FREQ reading and the
        // other slot's strip line are gated as any control or reading is. Hidden or under a sheet each is nameless, out
        // of the tree and no stop, gives the keyboard back, and a press a screen reader still holding it sends opens
        // nothing.
        function nodes_data() {
            return [
                {tag: "channelsRow", name: "monitorChannelsRow", expanded: true, shown: {dmrUserChannels: true, noFreqCount: 0},
                    hidden: {dmrUserChannels: false}, opens: true},
                {tag: "noFreqRow", name: "noFreqRow", expanded: true, shown: {noFreqCount: 2}, hidden: {noFreqCount: 0}, opens: true},
                {tag: "otherSlotNode", name: "otherSlotNode", shown: {twoSlotCarrier: true}, hidden: {twoSlotCarrier: false}}
            ];
        }
        function test_monitor_nodes_follow_their_gate_data() {
            return nodes_data();
        }
        function test_monitor_nodes_follow_their_gate(data) {
            testContext.setPrefs("monitorDetailsExpanded", data.expanded === true);
            set(data.shown);
            var item = findChild(monitor, data.name);
            var channels = findChild(monitor, "channelsSheet");
            verify(item !== null && channels !== null);
            tryVerify(function () { return Ui.Navigation.presented(item); });
            var node = null;
            tryVerify(function () { node = testContext.accessibleNode(item); return node.name.length > 0; }, 7000);
            for (var how of ["hidden", "covered"]) {
                node = testContext.accessibleNode(item);
                verify(node.inTree && node.focusable && node.name.length > 0, how + ": shown, a named stop");
                testContext.accessibleDoAction(item, testContext.setFocusAction);
                verify(item.activeFocus, how + ": SetFocus gives it the keyboard");
                if (how === "hidden")
                    set(data.hidden);
                else
                    Ui.Navigation.modals = [cover];
                groupGone(item, how);
                if (data.opens) {
                    testContext.accessibleClick(item);
                    item.activate();
                    verify(!channels.visible, how + ": a press opens nothing");
                }
                if (how === "hidden")
                    set(data.shown);
                else
                    Ui.Navigation.modals = [];
                tryVerify(function () { return testContext.accessibleNode(item).name.length > 0; }, 7000);
            }
            testContext.setPrefs("monitorDetailsExpanded", false);
        }
        function test_site_fields_follow_their_gate() {
            var sheet = siteLoader.item;
            verify(sheet !== null);
            set({siteLine: "Site 1", siteConfirmed: true, siteProtocol: "P25", p25NacValid: true, p25Nac: 0x291});
            sheet.visible = true;
            waitForRendering(sheet);
            wait(50);
            var fields = null;
            (function walk(item) {
                for (var child of item.children) {
                    if (child.objectName === "siteFields" && child.visible && fields === null)
                        fields = child;
                    walk(child);
                }
            })(sheet);
            verify(fields !== null, "the P25 fields are shown");
            for (var how of ["hidden", "covered"]) {
                var node = testContext.accessibleNode(fields);
                verify(node.inTree && node.focusable && node.name.length > 0, how + ": shown, a named stop");
                testContext.accessibleDoAction(fields, testContext.setFocusAction);
                verify(fields.activeFocus, how + ": SetFocus gives the fields the keyboard");
                if (how === "hidden")
                    set({siteProtocol: "DMR"});
                else
                    Ui.Navigation.modals = Ui.Navigation.modals.concat([cover]);
                groupGone(fields, how);
                if (how === "hidden")
                    set({siteProtocol: "P25"});
                else
                    Ui.Navigation.modals = Ui.Navigation.modals.filter(function (m) { return m !== cover; });
                wait(50);
            }
            sheet.visible = false;
        }
    }
}
