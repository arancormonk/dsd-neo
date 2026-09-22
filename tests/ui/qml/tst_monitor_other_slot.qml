// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    width: 420
    height: 900

    Ui.MonitorScreen {
        id: monitor
        anchors.fill: parent
    }

    TestCase {
        name: "MonitorOtherSlot"
        when: windowShown

        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        function openMenu() {
            var more = item("otherSlotMore");
            verify(more.visible && more.enabled);
            waitForRendering(monitor);
            mouseClick(more);
            var menu = item("otherSlotMenu");
            tryCompare(menu, "visible", true);
            compare(menu.menuSlot, monitor.otherSlot);
            compare(menu.menuTgId, monitor.otherTgId);
            waitForRendering(monitor);
            return menu;
        }
        function init() {
            testContext.resetCommands();
            testContext.setHostRunning(true);
            testContext.setMetric("leadSlot", 1);
            testContext.setMetric("slot1CallState", 2);
            testContext.setMetric("slot2CallState", 2);
            testContext.setMetric("slot1CallName", "Dispatch");
            testContext.setMetric("slot2CallName", "Fireground");
            testContext.setMetric("slot1TgId", 456);
            testContext.setMetric("slot2TgId", 1234567);
            testContext.setMetric("heldTg", 0);
            testContext.setMetric("persistTgLockouts", true);
        }
        function cleanup() {
            item("otherSlotMenu").visible = false;
            monitor.visible = true;
            for (var slot = 1; slot <= 2; ++slot) {
                testContext.setMetric("slot" + slot + "CallState", 0);
                testContext.setMetric("slot" + slot + "CallName", "");
                testContext.setMetric("slot" + slot + "TgId", 0);
            }
            testContext.setMetric("leadSlot", 0);
            testContext.setMetric("heldTg", 0);
            testContext.setMetric("persistTgLockouts", true);
            testContext.setHostRunning(false);
            testContext.resetCommands();
        }
        function test_skip_targets_other_slot_data() {
            return [{tag: "slot2", lead: 1}, {tag: "slot1", lead: 2}];
        }
        function test_skip_targets_other_slot(data) {
            testContext.setMetric("leadSlot", data.lead);
            var skip = item("otherSlotSkip");
            verify(skip.visible && skip.enabled);
            verify(skip.height >= 48);
            verify(item("otherSlotMore").height >= 48);
            compare(skip.accessibleName, "Skip this call on slot " + (3 - data.lead));
            waitForRendering(monitor);
            mouseClick(skip);
            compare(testContext.skipSlotCalls(), 1);
            compare(testContext.lastSkipSlot(), 2 - data.lead);
            compare(testContext.lockoutSlotCalls(), 0);
        }
        function test_hold_and_release() {
            var menu = openMenu();
            var hold = item("otherSlotHold");
            compare(hold.text, "Hold TG 1234567");
            var description = item("actionMenuDescription0");
            verify(description.visible);
            compare(description.text, "Only this talkgroup is heard; the other slot goes quiet");
            verify(hold.enabled);
            mouseClick(hold);
            compare(testContext.holdCalls(), 1);
            compare(testContext.lastHoldTg(), 1234567);
            verify(!menu.visible);
            testContext.setMetric("heldTg", 1234567);
            openMenu();
            hold = item("otherSlotHold");
            compare(hold.text, "Release hold");
            mouseClick(hold);
            compare(testContext.holdCalls(), 2);
            compare(testContext.lastHoldTg(), 0);
            verify(!menu.visible);
        }
        function test_hold_requires_numeric_talkgroup() {
            testContext.setMetric("slot2TgId", 0);
            testContext.setMetric("heldTg", 456);
            var menu = openMenu();
            verify(!item("otherSlotHold").enabled);
            menu.activate(0);
            compare(testContext.holdCalls(), 0);
            verify(menu.visible);
        }
        function test_avoid_targets_captured_slot_data() {
            return [
                {tag: "slot2-session", lead: 1, persist: false},
                {tag: "slot2-saved", lead: 1, persist: true},
                {tag: "slot1-session", lead: 2, persist: false},
                {tag: "slot1-saved", lead: 2, persist: true}
            ];
        }
        function test_avoid_targets_captured_slot(data) {
            testContext.setMetric("leadSlot", data.lead);
            testContext.setMetric("persistTgLockouts", data.persist);
            var menu = openMenu();
            var avoid = item("otherSlotAvoid");
            compare(avoid.text, data.persist ? "Lock out" : "Avoid TG");
            var description = item("actionMenuDescription1");
            verify(description.visible);
            compare(description.text, data.persist ? "Lock out talkgroup and save" : "Avoid talkgroup for this session");
            mouseClick(avoid);
            compare(testContext.lockoutSlotCalls(), 1);
            compare(testContext.lastLockoutSlot(), 2 - data.lead);
            compare(testContext.skipSlotCalls(), 0);
            verify(!menu.visible);
        }
        function test_avoid_label_is_captured_on_open_data() {
            return [{tag: "session", persist: false}, {tag: "saved", persist: true}];
        }
        function test_avoid_label_is_captured_on_open(data) {
            testContext.setMetric("persistTgLockouts", data.persist);
            var menu = openMenu();
            var expectedLabel = data.persist ? "Lock out" : "Avoid TG";
            var expectedDescription = data.persist ? "Lock out talkgroup and save" : "Avoid talkgroup for this session";
            compare(item("otherSlotAvoid").text, expectedLabel);
            testContext.setMetric("persistTgLockouts", !data.persist);
            tryCompare(menu, "visible", false);
            compare(item("otherSlotAvoid").text, expectedLabel);
            compare(item("actionMenuDescription1").text, expectedDescription);
            menu.activate(0);
            menu.activate(1);
            compare(testContext.holdCalls(), 0);
            compare(testContext.lockoutSlotCalls(), 0);
            compare(testContext.skipSlotCalls(), 0);
            openMenu();
            compare(item("otherSlotAvoid").text, data.persist ? "Avoid TG" : "Lock out");
            compare(item("actionMenuDescription1").text, data.persist ? "Avoid talkgroup for this session" : "Lock out talkgroup and save");
            mouseClick(item("otherSlotAvoid"));
            compare(testContext.lockoutSlotCalls(), 1);
            compare(testContext.lastLockoutSlot(), 1);
            verify(!menu.visible);
        }
        function test_menu_closes_when_call_changes_data() {
            return [{tag: "hero-ended"}, {tag: "talkgroup-changed"}, {tag: "other-ended"}];
        }
        function test_menu_closes_when_call_changes(data) {
            var menu = openMenu();
            if (data.tag === "hero-ended") {
                testContext.setMetric("slot1CallState", 1);
                testContext.setMetric("leadSlot", 2);
                compare(monitor.otherSlot, 1);
            } else if (data.tag === "talkgroup-changed") {
                testContext.setMetric("slot2TgId", 7654321);
                compare(monitor.otherSlot, 2);
            } else {
                testContext.setMetric("slot2CallState", 1);
            }
            tryCompare(menu, "visible", false);
            compare(menu.menuSlot, 2);
            compare(menu.menuTgId, 1234567);
            // A pending activation cannot act on a newly headlined or replaced call.
            menu.activate(0);
            menu.activate(1);
            compare(testContext.holdCalls(), 0);
            compare(testContext.lockoutSlotCalls(), 0);
            compare(testContext.skipSlotCalls(), 0);
        }
        function test_hiding_monitor_closes_menu() {
            var menu = openMenu();
            monitor.visible = false;
            compare(menu.visible, false);
        }
    }
}
