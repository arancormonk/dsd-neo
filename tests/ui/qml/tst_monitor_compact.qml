// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 640
    height: 360
    Ui.MonitorScreen {
        id: monitor
        anchors.fill: parent
    }
    Ui.ModalSheet {
        id: sheet
        panelObjectName: "compactSheetPanel"
        Repeater {
            model: 8
            Ui.PlexTextField {
                width: parent.width
                text: "Row " + index
            }
        }
        Ui.OutlineButton {
            objectName: "sheetLastAction"
            width: parent.width
            text: "Done"
        }
    }
    Ui.MicroLabel { id: scaledLabel; text: "Label" }
    Ui.RadioReferenceScreen { id: rr; visible: false; width: 640; height: 360 }
    Ui.HomeScreen { id: home; visible: false; width: 420; height: 900 }
    TestCase {
        name: "MonitorCompactHeight"
        when: windowShown
        property string originalExploreSource: ""
        function init() {
            originalExploreSource = prefs.exploreSourceType;
            testContext.setHostRunning(true);
            testContext.setMetric("leadSlot", 1);
            testContext.setMetric("slot1CallState", 2);
            testContext.setMetric("slot2CallState", 2);
            testContext.setMetric("slot1CallName", "Dispatch");
            testContext.setMetric("slot2CallName", "Fireground");
        }
        function cleanup() {
            testContext.setPrefs("exploreSourceType", originalExploreSource);
            root.width = 640;
            root.height = 360;
            Ui.Theme.resetFontScale();
            sheet.visible = false;
            testContext.setMetric("slot1CallState", 0);
            testContext.setMetric("slot2CallState", 0);
            testContext.setMetric("leadSlot", 0);
            testContext.setHostRunning(false);
        }
        // The call actions are one row of four on a 360 dp phone (#718): the
        // talkgroup list left the row for the session menu, and the toggles keep
        // short labels, so the longest word is "Avoid" and it fits its cell.
        function test_quick_row_at_360_dp() {
            root.width = 360;
            root.height = 800;
            Ui.Theme.fontScale = 1;
            compare(findChild(monitor, "talkgroupsButton"), null);
            var actions = findChild(monitor, "monitorLiveActions");
            compare(actions.columns, 4);
            for (var name of ["muteButton", "holdTalkgroupButton", "skipButton", "avoidButton"]) {
                var button = findChild(monitor, name);
                verify(button.visible, name);
                var label = null;
                for (var child of button.children) {
                    if (child instanceof Text && child.text === button.text)
                        label = child;
                }
                verify(label !== null, name);
                compare(label.wrapMode, Text.WordWrap);
                // Broken at a space, never inside a word.
                verify(label.lineCount <= actions.labelLines, name);
                verify(!label.truncated && label.contentWidth <= label.width + 0.5, name + " fits its cell");
                verify(button.mapToItem(monitor, button.width, 0).x <= 360);
            }
            root.height = 360;
            compare(findChild(monitor, "talkgroupsButton"), null, "no size brings the talkgroup list back");
        }

        function test_explore_subtitle_scales_data() {
            return [{tag: "unconfigured", sourceType: "", pixels: 13},
                    {tag: "configured", sourceType: "usb", pixels: 12}];
        }
        function test_explore_subtitle_scales(data) {
            testContext.setPrefs("exploreSourceType", data.sourceType);
            var subtitle = findChild(home, "homeExploreSubtitle");
            verify(subtitle !== null);
            Ui.Theme.fontScale = 1;
            compare(subtitle.font.pixelSize, data.pixels);
            Ui.Theme.fontScale = 1.6;
            compare(subtitle.font.pixelSize, Math.round(Ui.Theme.fontSize(data.pixels)));
        }
        function test_platform_font_default() {
            compare(Ui.Theme.fontScale, Math.min(1.6, Math.max(1, Qt.application.font.pixelSize / 16)));
            Ui.Theme.fontScale = 1.6;
            // QFont stores absolute spacing as a truncated 1/64-pixel fixed point value.
            compare(scaledLabel.font.letterSpacing, Math.floor(Ui.Theme.fontSize(11) * 0.18 * 64) / 64);
        }
        function test_browse_sheet_last_row_reachable() {
            var rows = [];
            for (var i = 0; i < 12; ++i) rows.push({name: "Country " + i, coid: i});
            testContext.setRadioReference("countries", rows);
            rr.visible = true;
            var browse = findChild(rr, "radioReferenceCountrySheet");
            browse.keyboardTop = 200;
            browse.visible = true;
            var scroll = findChild(browse, "modalSheetScroll");
            var last = findChild(browse, "browseRow11");
            verify(last !== null);
            scroll.contentY = scroll.contentHeight - scroll.height;
            tryVerify(function() {
                scroll.contentY = scroll.contentHeight - scroll.height;
                return last.mapToItem(scroll, 0, last.height).y <= scroll.height + 1;
            });
            browse.visible = false;
            rr.visible = false;
        }
        // The recent calls are always reachable by scrolling the one list (#718).
        // Where a fixed hero fits, it holds still while they scroll; on a
        // landscape phone nothing fixed fits but Stop, so the hero scrolls away
        // with the rest.
        function test_scrolls_rows_below_fixed_hero_data() {
            return [
                {tag: "landscape", width: 640, height: 360, scale: 1, fixed: false},
                {tag: "landscape large fonts", width: 640, height: 360, scale: 1.6, fixed: false},
                {tag: "portrait", width: 411, height: 780, scale: 1, fixed: true},
                {tag: "portrait large fonts", width: 411, height: 780, scale: 1.6, fixed: true}
            ];
        }
        function test_scrolls_rows_below_fixed_hero(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            callHistory.clearAll();
            callHistory.pushMany(12, "TODAY");
            var list = findChild(monitor, "recentCallsList");
            var hero = findChild(monitor, "monitorHero");
            var other = findChild(monitor, "otherSlotPanel");
            var recent = findChild(monitor, "recentCallsPanel");
            verify(list !== null && hero !== null && other !== null && recent !== null);
            tryVerify(function () {
                return list.height > 0 && list.contentHeight > list.height;
            });
            verify(other.visible);
            compare(monitor.tier === 5, !data.fixed);
            waitForRendering(monitor);
            var heroY = hero.mapToItem(monitor, 0, 0).y;
            list.contentY = list.originY + list.contentHeight - list.height;
            wait(20);
            if (data.fixed)
                compare(hero.mapToItem(monitor, 0, 0).y, heroY);
            else
                verify(hero.mapToItem(monitor, 0, 0).y < heroY, "the hero scrolls with the calls");
            verify(list.contentY > list.originY);
            tryVerify(function () {
                list.contentY = list.originY + list.contentHeight - list.height;
                return recent.mapToItem(list, 0, recent.height).y <= list.height + 1;
            }, 1000, "recent calls remain reachable by scrolling");
            callHistory.clearAll();
        }
        function test_keyboard_visible_sheet_scrolls() {
            Ui.Theme.fontScale = 1.6;
            sheet.keyboardTop = 200;
            sheet.visible = true;
            var panel = findChild(sheet, "compactSheetPanel");
            var scroll = findChild(sheet, "modalSheetScroll");
            var action = findChild(sheet, "sheetLastAction");
            verify(scroll !== null);
            tryVerify(function () {
                return panel.y + panel.height <= 200;
            });
            verify(scroll.height > 0 && scroll.contentHeight > scroll.height);
            scroll.contentY = scroll.contentHeight - scroll.height;
            tryVerify(function () {
                scroll.contentY = scroll.contentHeight - scroll.height;
                return action.mapToItem(scroll, 0, action.height).y <= scroll.height + 1;
            }, 1000, "sheet's last action must clear keyboard");
        }
    }
}
