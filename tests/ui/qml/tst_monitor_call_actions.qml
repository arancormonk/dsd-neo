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
            // A session map without a name used to bind undefined to the monitor header.
            failOnWarning(/MonitorScreen.qml.*Unable to assign/);
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
        // A saved system removed while it plays reads back as an empty map: the
        // header names the session as it does with no map, never a blank.
        function test_unnamed_system_map_falls_back() {
            var saved = monitor.system;
            var label = callHistory.sessionLabel;
            try {
                monitor.system = {};
                callHistory.sessionLabel = "County P25";
                compare(monitor.systemName, "County P25");
                callHistory.sessionLabel = "";
                compare(monitor.systemName, "Listening");
                monitor.system = {name: "Fire Dispatch", sourceType: "usb"};
                compare(monitor.systemName, "Fire Dispatch");
            } finally {
                monitor.system = saved;
                callHistory.sessionLabel = label;
            }
        }

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
                {tag: "slot1-session", lead: 1, persist: false, landscape: false},
                {tag: "slot2-saved", lead: 2, persist: true, landscape: false},
                {tag: "landscape-slot2-session", lead: 2, persist: false, landscape: true},
                {tag: "landscape-slot1-saved", lead: 1, persist: true, landscape: true}
            ];
        }
        // One Avoid action at every size (#718); landscape scrolls it with the calls.
        function test_avoid_targets_hero(data) {
            testContext.setMetric("leadSlot", data.lead);
            testContext.setMetric("persistTgLockouts", data.persist);
            if (data.landscape) {
                root.width = 640;
                root.height = 360;
            }
            var avoid = button("avoidButton");
            compare(avoid.text, data.persist ? "Lock out" : "Avoid TG");
            compare(avoid.accessibleName, data.persist ? "Lock out talkgroup and save" : "Avoid talkgroup for this session");
            verify(avoid.enabled && avoid.visible);
            waitForRendering(monitor);
            if (data.landscape) {
                compare(monitor.tier, 5);
                var list = button("recentCallsList");
                var top = avoid.mapToItem(list.contentItem, 0, 0).y;
                list.contentY = Math.max(list.originY, Math.min(top, list.originY + list.contentHeight - list.height));
                waitForRendering(monitor);
            }
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
            for (var name of ["skipButton", "avoidButton"]) {
                var action = button(name);
                verify(!action.enabled);
                action.activate();
            }
            compare(testContext.skipSlotCalls(), 0);
            compare(testContext.lockoutSlotCalls(), 0);
        }
        // The toggles keep one label each and show their state as checked (#718):
        // "Unmute" and "Release" did not fit a quarter of a phone's row.
        function test_toggles_show_checked_data() {
            return [{tag: "mute", name: "muteButton", label: "Mute", accessible: "Mute audio"},
                    {tag: "hold", name: "holdTalkgroupButton", label: "Hold TG", accessible: "Hold this talkgroup"}];
        }
        function test_toggles_show_checked(data) {
            var toggle = button(data.name);
            var plain = button("skipButton");
            compare(toggle.text, data.label);
            compare(toggle.accessibleName, data.accessible);
            verify(toggle.Accessible.checkable);
            verify(!toggle.checked && !toggle.Accessible.checked);
            verify(!plain.Accessible.checkable);
            compare(String(toggle.border.color), String(plain.border.color));
            if (data.tag === "mute")
                testContext.setMetric("audioMuted", true);
            else
                testContext.setMetric("heldTg", 4001);
            try {
                tryCompare(toggle, "checked", true);
                compare(toggle.text, data.label);
                verify(toggle.Accessible.checked);
                compare(String(toggle.border.color), String(Ui.Theme.cyan));
                tryVerify(function () { return String(toggle.color) === String(Qt.alpha(Ui.Theme.cyan, 0.08)); });
                if (data.tag === "hold") {
                    // A tap on the checked toggle releases, as "Release" did.
                    waitForRendering(monitor);
                    mouseClick(toggle);
                    compare(testContext.holdCalls(), 1);
                    compare(testContext.lastHoldTg(), 0);
                }
            } finally {
                testContext.setMetric("audioMuted", false);
                testContext.setMetric("heldTg", 0);
            }
            tryCompare(toggle, "checked", false);
        }
        function test_layout_data() {
            return [
                {tag: "portrait", width: 420, height: 900, scale: 1},
                {tag: "portrait-large-font", width: 420, height: 900, scale: 1.6},
                {tag: "landscape", width: 640, height: 360, scale: 1},
                {tag: "landscape-large-font", width: 640, height: 360, scale: 1.6}
            ];
        }
        // Mute · Hold TG · Skip · Avoid at every size; the talkgroup list is in the
        // session menu. Above Stop, or scrolled with the calls where nothing else
        // fits (tier 5), and reachable there.
        function test_layout(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            compare(findChild(monitor, "talkgroupsButton"), null);
            compare(findChild(monitor, "avoidButtonCompact"), null);
            compare(findChild(monitor, "monitorAvoidActions"), null);
            var actions = button("monitorLiveActions");
            var stop = button("stopListeningButton");
            var list = button("recentCallsList");
            tryVerify(function () { return list.height > 0; });
            waitForRendering(monitor);
            var scrolls = monitor.tier === 5;
            compare(scrolls, data.height < 500);
            for (var name of ["muteButton", "holdTalkgroupButton", "skipButton", "avoidButton"]) {
                var action = button(name);
                verify(action.visible, name);
                compare(action.width, actions.cell);
                compare(action.height, actions.cellHeight);
                verify(action.mapToItem(monitor, action.width, action.height).x <= monitor.width);
                if (!scrolls) {
                    verify(action.mapToItem(monitor, 0, action.height).y <= stop.y);
                } else {
                    var top = action.mapToItem(list.contentItem, 0, 0).y;
                    list.contentY = Math.max(list.originY, Math.min(top, list.originY + list.contentHeight - list.height));
                    verify(action.mapToItem(list, 0, 0).y >= -0.5 && action.mapToItem(list, 0, action.height).y <= list.height + 0.5,
                           name + " scrolls into view");
                    list.positionViewAtBeginning();
                }
            }
        }

        // Every label each cell shows, as the grid measures them.
        FontMetrics {
            id: labelMetrics
            font.family: Ui.Theme.sans
            font.pixelSize: Ui.Theme.fontSize(15)
            font.weight: Font.DemiBold
        }
        function cellLabels() {
            return {
                muteButton: ["Mute"],
                holdTalkgroupButton: ["Hold TG"],
                skipButton: ["Skip"],
                avoidButton: ["Avoid TG", "Lock out"]
            };
        }
        // The action cells form one deterministic grid (#718): four columns where a
        // quarter of the row, less the 12 px each side of a label, fits the longest
        // word of any label, else two. On a 360 px phone at normal text that is
        // four 73 px cells whose 49 px of label room fit "Avoid"; "Hold TG" and
        // "Lock out" break at their space, so every cell takes two lines. At 1.6x
        // "Avoid" no longer fits and the grid falls back to two columns of one
        // line. Every cell has one height, at least a touch target.
        function test_phone_grid_data() {
            return [
                {tag: "phone", width: 360, scale: 1, columns: 4, lines: 2},
                {tag: "phone-large-font", width: 360, scale: 1.6, columns: 2, lines: 1},
                {tag: "tablet", width: 640, scale: 1, columns: 4, lines: 1},
                {tag: "tablet-large-font", width: 640, scale: 1.6, columns: 4, lines: 1}
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
                compare(actions.labelLines, data.lines);
                var labels = cellLabels();
                var height = -1;
                for (var name in labels) {
                    var action = button(name);
                    verify(action.visible, name);
                    verify(action.height >= Ui.Theme.minimumTouchSize, name);
                    if (height < 0)
                        height = action.height;
                    compare(action.height, height, name);
                    compare(action.width, actions.cell, name);
                    for (var label of labels[name]) {
                        for (var word of label.split(" "))
                            verify(action.width - 24 >= labelMetrics.advanceWidth(word), name + " fits " + word);
                        if (data.lines === 1)
                            verify(action.width - 24 >= labelMetrics.advanceWidth(label), name + " fits " + label + " on one line");
                    }
                }
                var mute = button("muteButton");
                var avoid = button("avoidButton");
                compare(avoid.y > mute.y, data.columns === 2);
                verify(avoid.mapToItem(monitor, avoid.width, 0).x <= data.width - Ui.Theme.screenPadding + 0.5);
            } finally {
                Ui.Theme.fontScale = 1;
            }
        }
    }
}
