// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 420
    height: 900

    Ui.MonitorScreen {
        id: monitor
        anchors.fill: parent
    }

    TestCase {
        name: "MonitorCallActions"
        when: windowShown

        function button(name) {
            var item = findChild(monitor, name);
            verify(item !== null, name + " is missing");
            return item;
        }
        function init() {
            testContext.resetCommands();
            testContext.setHostRunning(true);
            testContext.setMetric("leadSlot", 1);
            testContext.setMetric("heldTg", 0);
            testContext.setMetric("persistTgLockouts", true);
            for (var slot = 1; slot <= 2; ++slot) {
                testContext.setMetric("slot" + slot + "CallState", 2);
                testContext.setMetric("slot" + slot + "CallName", "Dispatch");
                testContext.setMetric("slot" + slot + "TgId", 4000 + slot);
            }
            Ui.Theme.fontScale = 1;
        }
        function cleanup() {
            button("otherSlotMenu").visible = false;
            root.width = 420;
            root.height = 900;
            Ui.Theme.resetFontScale();
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
        function test_skip_targets_hero_data() {
            return [{tag: "slot1", lead: 1}, {tag: "slot2", lead: 2}];
        }
        function test_skip_targets_hero(data) {
            testContext.setMetric("leadSlot", data.lead);
            var skip = button("skipButton");
            compare(skip.text, "Skip");
            compare(skip.accessibleName, "Skip this call");
            verify(skip.enabled);
            waitForRendering(monitor);
            mouseClick(skip);
            compare(testContext.skipSlotCalls(), 1);
            compare(testContext.lastSkipSlot(), data.lead - 1);
            compare(testContext.lockoutSlotCalls(), 0);
            compare(testContext.lastLockoutSlot(), -1);
        }
        function test_avoid_targets_hero_data() {
            return [
                {tag: "slot1-session", lead: 1, persist: false, compact: false},
                {tag: "slot2-saved", lead: 2, persist: true, compact: false},
                {tag: "compact-slot2-session", lead: 2, persist: false, compact: true},
                {tag: "compact-slot1-saved", lead: 1, persist: true, compact: true}
            ];
        }
        function test_avoid_targets_hero(data) {
            testContext.setMetric("leadSlot", data.lead);
            testContext.setMetric("persistTgLockouts", data.persist);
            if (data.compact) {
                root.width = 640;
                root.height = 360;
            }
            var avoid = button(data.compact ? "avoidButtonCompact" : "avoidButton");
            compare(avoid.text, data.persist ? "Lock out" : "Avoid");
            compare(avoid.accessibleName, data.persist ? "Lock out talkgroup and save" : "Avoid talkgroup for this session");
            verify(avoid.enabled && avoid.visible);
            waitForRendering(monitor);
            mouseClick(avoid);
            compare(testContext.lockoutSlotCalls(), 1);
            compare(testContext.lastLockoutSlot(), data.lead - 1);
            compare(testContext.skipSlotCalls(), 0);
            compare(testContext.lastSkipSlot(), -1);
        }
        function test_unavailable_actions_data() {
            return [{tag: "stopped", running: false, lead: 1}, {tag: "no-call", running: true, lead: 0}];
        }
        function test_unavailable_actions(data) {
            testContext.setHostRunning(data.running);
            testContext.setMetric("leadSlot", data.lead);
            for (var name of ["skipButton", "avoidButton", "avoidButtonCompact"]) {
                var action = button(name);
                verify(!action.enabled);
                action.activate();
            }
            compare(testContext.skipSlotCalls(), 0);
            compare(testContext.lockoutSlotCalls(), 0);
        }
        function test_layout_data() {
            return [
                {tag: "portrait", width: 420, height: 900, scale: 1},
                {tag: "portrait-large-font", width: 420, height: 900, scale: 1.6},
                {tag: "landscape", width: 640, height: 360, scale: 1},
                {tag: "landscape-large-font", width: 640, height: 360, scale: 1.6}
            ];
        }
        function test_layout(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            var compact = data.height < 500;
            compare(button("avoidButton").visible, !compact);
            compare(button("talkgroupsButton").visible, !compact);
            compare(button("avoidButtonCompact").visible, compact);
            compare(button("monitorAvoidActions").visible, !compact);
            var actions = button("monitorLiveActions");
            var stop = button("stopListeningButton");
            var body = button("monitorBody");
            tryVerify(function () {
                return body.height > 0 && actions.mapToItem(monitor, 0, actions.height).y <= stop.y;
            });
            for (var name of ["muteButton", "holdTalkgroupButton", "skipButton",
                              compact ? "avoidButtonCompact" : "talkgroupsButton"]) {
                var action = button(name);
                compare(action.width, (actions.width - 30) / 4);
                verify(action.mapToItem(monitor, action.width, action.height).x <= monitor.width);
                verify(action.mapToItem(monitor, 0, action.height).y <= stop.y);
            }
        }
    }
}
