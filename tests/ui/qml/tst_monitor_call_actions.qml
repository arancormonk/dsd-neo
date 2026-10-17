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
        // #718: the tap answers at once, before the service reports STOPPING.
        function test_stop_shows_stopping_data() {
            return [{tag: "live", file: false, idle: "Stop listening"}, {tag: "replay", file: true, idle: "Stop replay"}];
        }
        function test_stop_shows_stopping(data) {
            var saved = monitor.system;
            monitor.system = {sourceType: data.file ? "file" : "rtl"};
            testContext.useLifecycleHost(true);
            testContext.setDelayedStop(true);
            testContext.setLifecyclePhase(2);
            try {
                var stop = button("stopListeningButton");
                var stops = decoderHost.stopCalls();
                compare(stop.text, data.idle);
                verify(stop.enabled);
                waitForRendering(monitor);
                mouseClick(stop);
                compare(decoderHost.stopCalls(), stops + 1);
                compare(decoderHost.sessionState, 3);
                compare(stop.text, "Stopping…");
                verify(!stop.enabled);
                mouseClick(stop);
                compare(decoderHost.stopCalls(), stops + 1);
                testContext.setLifecyclePhase(2);
                compare(stop.text, data.idle);
                verify(stop.enabled);
            } finally {
                testContext.setDelayedStop(false);
                testContext.useLifecycleHost(false);
                monitor.system = saved;
            }
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
            compare(avoid.text, data.persist ? "Lock out" : "Avoid TG");
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
                compare(action.width, (actions.width - (actions.columns - 1) * 10) / actions.columns);
                compare(action.height, actions.cellHeight);
                verify(action.mapToItem(monitor, action.width, action.height).x <= monitor.width);
                verify(action.mapToItem(monitor, 0, action.height).y <= stop.y);
            }
        }

        // Every label each cell can show, as the grid measures them.
        FontMetrics {
            id: labelMetrics
            font.family: Ui.Theme.sans
            font.pixelSize: Ui.Theme.fontSize(15)
            font.weight: Font.DemiBold
        }
        function cellLabels() {
            return {
                muteButton: ["Mute", "Unmute"],
                holdTalkgroupButton: ["Hold TG", "Release"],
                skipButton: ["Skip"],
                talkgroupsButton: ["TG list"],
                avoidButton: ["Avoid TG", "Lock out"]
            };
        }
        // The action cells form one deterministic grid: four columns only where a
        // quarter of the row fits the widest label any cell can show ("Unmute",
        // "Release", "Lock out") with the card padding either side, else two. On a
        // 360 px phone that is two at every text scale: four 73 px cells left
        // "Unmute" 50 px to break inside. Every cell has the same height (at least
        // a touch target) and is wide enough for each label it can show on one
        // line, so no label breaks inside a word. The Avoid row spans two cells,
        // which in two columns is the full width.
        function test_phone_grid_data() {
            return [
                {tag: "phone", width: 360, scale: 1, columns: 2},
                {tag: "phone-large-font", width: 360, scale: 1.6, columns: 2},
                {tag: "tablet", width: 640, scale: 1, columns: 4},
                {tag: "tablet-large-font", width: 640, scale: 1.6, columns: 4}
            ];
        }
        function test_phone_grid(data) {
            root.width = data.width;
            root.height = 800;
            Ui.Theme.fontScale = data.scale;
            try {
                waitForRendering(monitor);
                var actions = button("monitorLiveActions");
                compare(actions.columns, data.columns);
                compare(actions.labelLines, 1);
                var labels = cellLabels();
                var height = -1;
                for (var name in labels) {
                    var action = button(name);
                    verify(action.visible, name);
                    verify(action.height >= Ui.Theme.minimumTouchSize, name);
                    if (name !== "avoidButton") {
                        if (height < 0)
                            height = action.height;
                        compare(action.height, height, name);
                    }
                    for (var label of labels[name])
                        verify(action.width - 24 >= labelMetrics.advanceWidth(label), name + " fits " + label);
                }
                compare(button("avoidButton").height, height);
                compare(button("avoidButton").width, 2 * actions.cell + 10);
                if (data.columns === 2)
                    compare(button("avoidButton").width, actions.width);
                var mute = button("muteButton");
                var tg = button("talkgroupsButton");
                compare(tg.y > mute.y, data.columns === 2);
            } finally {
                Ui.Theme.fontScale = 1;
            }
        }
    }
}
