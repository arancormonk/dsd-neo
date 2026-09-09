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
    TestCase {
        name: "MonitorCompactHeight"
        when: windowShown
        function init() {
            testContext.setHostRunning(true);
            testContext.setMetric("leadSlot", 1);
            testContext.setMetric("slot1CallState", 2);
            testContext.setMetric("slot2CallState", 2);
            testContext.setMetric("slot1CallName", "Dispatch");
            testContext.setMetric("slot2CallName", "Fireground");
        }
        function cleanup() {
            Ui.Theme.fontScale = 1;
            sheet.visible = false;
            testContext.setMetric("slot1CallState", 0);
            testContext.setMetric("slot2CallState", 0);
            testContext.setMetric("leadSlot", 0);
            testContext.setHostRunning(false);
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
