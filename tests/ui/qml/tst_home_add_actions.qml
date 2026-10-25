// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 390
    height: 844

    Ui.HomeScreen { id: home; anchors.fill: parent; localDeviceRelevant: false }
    SignalSpy { id: systemAdded; target: home; signalName: "addSystem" }
    SignalSpy { id: systemImported; target: home; signalName: "importSystem" }
    SignalSpy { id: scanAdded; target: home; signalName: "addScanList" }

    TestCase {
        name: "HomeAddActions"
        when: windowShown

        function item(name) {
            var result = findChild(home, name);
            verify(result !== null, name + " exists");
            return result;
        }
        function clearModels() {
            while (savedSystems.count) savedSystems.remove(0);
            while (scanLists.count) scanLists.remove(0);
        }
        function populate(count) {
            for (var i = 0; i < count; ++i) {
                savedSystems.add({name: "System " + i, sourceType: "rtltcp", freqMhz: "851.5"});
                scanLists.add({name: "List " + i, sourceType: "rtltcp", entries: []});
            }
        }
        function settle() {
            verify(waitForPolish(home));
            waitForRendering(home);
        }
        function point(control) {
            return control.mapToItem(item("dongleScrollBody"), 0, 0);
        }
        function withinViewport(control) {
            var p = point(control);
            var scroll = item("dongleScrollBody");
            verify(control.visible && control.enabled);
            verify(p.x >= -1 && p.x + control.width <= scroll.width + 1, "action fits horizontally");
            verify(p.y >= -1 && p.y + control.height <= scroll.height + 1, "action fits at the top without scrolling");
        }
        function buttonLabel(control) {
            for (var child of control.children) {
                if (child.text === control.text) return child;
            }
            fail("button label exists");
        }
        function init() {
            clearModels();
            testContext.resetBackup();
            testContext.setRadioReference("available", false);
            home.failure = ({});
            home.completionMessage = "";
            home.localDeviceRelevant = false;
            root.width = 390;
            root.height = 844;
            Ui.Theme.fontScale = 1;
            item("dongleScrollBody").contentY = 0;
            systemAdded.clear();
            systemImported.clear();
            scanAdded.clear();
            failOnWarning(/HomeScreen.qml.*(Binding loop|Unable to assign)/);
        }
        function cleanup() {
            for (var modal of Ui.Navigation.modals.slice()) modal.visible = false;
            testContext.resetBackup();
            testContext.setRadioReference("available", false);
            home.failure = ({});
            home.completionMessage = "";
            clearModels();
            Ui.Theme.resetFontScale();
        }
        function test_many_saved_items_do_not_move_actions() {
            settle();
            var system = item("addSystemButton");
            var scan = item("addScanListButton");
            var systemY = point(system).y;
            var scanY = point(scan).y;
            populate(20);
            settle();
            item("dongleScrollBody").contentY = 0;
            withinViewport(system);
            withinViewport(scan);
            fuzzyCompare(point(system).y, systemY, 1);
            fuzzyCompare(point(scan).y, scanY, 1);
            var firstSystem = item("savedSystemTitle").parent.parent;
            var firstScan = item("scanListCard");
            for (var button of [system, scan]) {
                verify(point(button).y + button.height <= point(firstSystem).y);
                verify(point(button).y + button.height <= point(firstScan).y);
            }
            mouseClick(scan);
            compare(scanAdded.count, 1);
            mouseClick(system);
            compare(systemAdded.count, 1);
        }
        function test_empty_section_headings_follow_counts() {
            var systems = item("savedSystemsHeading");
            var lists = item("scanListsHeading");
            verify(!systems.visible && !lists.visible);
            savedSystems.add({name: "First", sourceType: "rtltcp", freqMhz: "851.5"});
            tryCompare(systems, "visible", true);
            verify(!lists.visible);
            scanLists.add({name: "First list", sourceType: "rtltcp", entries: []});
            tryCompare(lists, "visible", true);
            savedSystems.remove(0);
            tryCompare(systems, "visible", false);
            verify(lists.visible);
            scanLists.remove(0);
            tryCompare(lists, "visible", false);
            settle();
            withinViewport(item("addSystemButton"));
            withinViewport(item("addScanListButton"));
        }
        function test_responsive_actions_data() {
            var rows = [];
            for (var size of [[320, 568], [360, 690], [390, 844], [640, 360], [899, 900], [900, 900], [1100, 900]]) {
                for (var scale of [1, 1.3, 1.6])
                    rows.push({tag: size[0] + "x" + size[1] + "@" + scale, w: size[0], h: size[1], scale: scale});
            }
            return rows;
        }
        function checkLayout() {
            settle();
            var system = item("addSystemButton");
            var scan = item("addScanListButton");
            var hint = item("addSystemHint");
            for (var button of [system, scan]) {
                withinViewport(button);
                verify(button.height >= Ui.Theme.minimumTouchSize);
                var label = buttonLabel(button);
                verify(label.contentWidth <= label.width + 1, "complete label fits its width");
                verify(label.contentHeight <= button.height - 24 + 1, "complete label fits its height");
            }
            fuzzyCompare(system.width, scan.width, 1);
            var s = point(system);
            var l = point(scan);
            var h = point(hint);
            verify(h.y >= s.y + system.height);
            fuzzyCompare(h.x, s.x, 1);
            verify(hint.contentHeight <= hint.height + 1);
            if (Math.abs(s.y - l.y) <= 1) {
                verify(s.x + system.width + 8 <= l.x);
                compare(buttonLabel(system).lineCount, 1);
                compare(buttonLabel(scan).lineCount, 1);
            } else {
                fuzzyCompare(s.x, l.x, 1);
                verify(h.y + hint.height + 8 <= l.y, "hint stays with the system action when stacked");
            }
        }
        function test_responsive_actions(data) {
            populate(1);
            root.width = data.w;
            root.height = data.h;
            Ui.Theme.fontScale = data.scale;
            checkLayout();
            compare(home.supportingPane, data.w >= 900);
            if (data.w === 390) {
                if (data.scale < 1.1)
                    fuzzyCompare(point(item("addSystemButton")).y, point(item("addScanListButton")).y, 1);
                else if (data.scale > 1.5)
                    verify(point(item("addScanListButton")).y > point(item("addSystemButton")).y + 1);
            }
        }
        function test_live_resize_font_and_long_labels() {
            for (var width of [390, 899, 900, 320]) {
                root.width = width;
                for (var scale of [1, 1.6, 1]) {
                    Ui.Theme.fontScale = scale;
                    checkLayout();
                }
            }
            var system = item("addSystemButton");
            var original = system.text;
            try {
                system.text = "+ Add another saved radio system with a longer translated name";
                checkLayout();
                verify(buttonLabel(system).lineCount > 1);
            } finally {
                system.text = original;
            }
        }
        function test_keyboard_order_and_activation() {
            populate(1);
            for (var scale of [1, 1.6]) {
                Ui.Theme.fontScale = scale;
                settle();
                var system = item("addSystemButton");
                var scan = item("addScanListButton");
                system.forceActiveFocus();
                keyClick(Qt.Key_Tab);
                verify(scan.activeFocus, "Tab follows visual action order");
                keyClick(Qt.Key_Space);
                keyClick(Qt.Key_Backtab);
                verify(system.activeFocus);
                keyClick(Qt.Key_Return);
            }
            compare(scanAdded.count, 2);
            compare(systemAdded.count, 2);
        }
        function test_menu_routes_and_blocks_background_data() {
            return [{tag: "manual", index: 0}, {tag: "import", index: 1}];
        }
        function test_menu_routes_and_blocks_background(data) {
            testContext.setRadioReference("available", true);
            settle();
            var system = item("addSystemButton");
            var scan = item("addScanListButton");
            system.forceActiveFocus();
            mouseClick(system);
            var menu = item("addSystemMenu");
            tryCompare(menu, "visible", true);
            verify(!item("dongleScrollBody").enabled);
            verify(!system.activeFocusOnTab && !scan.activeFocusOnTab);
            system.Accessible.pressAction();
            scan.Accessible.pressAction();
            compare(systemAdded.count, 0);
            compare(scanAdded.count, 0);
            menu.activate(data.index);
            tryCompare(menu, "visible", false);
            compare(systemAdded.count, data.index === 0 ? 1 : 0);
            compare(systemImported.count, data.index === 1 ? 1 : 0);
            verify(item("dongleScrollBody").enabled);
        }
        function test_status_cards_precede_actions_and_scroll() {
            root.width = 320;
            root.height = 360;
            Ui.Theme.fontScale = 1.6;
            home.failure = {message: "Connection failed. Check the receiver and retry.", canRetry: true};
            home.completionMessage = "Replay complete";
            testContext.setBackup("startupOutcome", {kind: "applied", systems: 20, scanLists: 20});
            settle();
            var system = item("addSystemButton");
            var scroll = item("dongleScrollBody");
            var backupCard = item("backupResultCard");
            verify(point(backupCard).y + backupCard.height < point(system).y);
            verify(point(system).y > scroll.height, "status messages retain priority on a short screen");
            for (var button of [system, item("addScanListButton")]) {
                scroll.contentY = Math.min(button.mapToItem(scroll.contentItem, 0, 0).y, scroll.contentHeight - scroll.height);
                withinViewport(button);
                mouseClick(button);
            }
            compare(systemAdded.count, 1);
            compare(scanAdded.count, 1);
        }
    }
}
