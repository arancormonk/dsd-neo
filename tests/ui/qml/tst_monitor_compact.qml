// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
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
            Ui.Theme.resetFontScale();
            sheet.visible = false;
            testContext.setMetric("slot1CallState", 0);
            testContext.setMetric("slot2CallState", 0);
            testContext.setMetric("leadSlot", 0);
            testContext.setHostRunning(false);
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
        function test_scrolls_rows_below_fixed_hero_data() {
            return [
                {
                    tag: "normal",
                    scale: 1
                },
                {
                    tag: "large fonts",
                    scale: 1.6
                }
            ];
        }
        function test_scrolls_rows_below_fixed_hero(data) {
            Ui.Theme.fontScale = data.scale;
            var body = findChild(monitor, "monitorBody");
            var hero = findChild(monitor, "monitorHero");
            var other = findChild(monitor, "otherSlotPanel");
            var recent = findChild(monitor, "recentCallsPanel");
            verify(body !== null && hero !== null && other !== null && recent !== null);
            tryVerify(function () {
                return body.height > 0 && body.contentHeight > body.height;
            });
            verify(other.visible);
            verify(recent.height >= 120);
            var heroY = hero.mapToItem(monitor, 0, 0).y;
            body.contentY = body.contentHeight - body.height;
            wait(20);
            compare(hero.mapToItem(monitor, 0, 0).y, heroY);
            verify(body.contentY > 0);
            tryVerify(function () {
                body.contentY = body.contentHeight - body.height;
                return recent.mapToItem(body, 0, recent.height).y <= body.height + 1;
            }, 1000, "recent calls remain reachable by scrolling");
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
