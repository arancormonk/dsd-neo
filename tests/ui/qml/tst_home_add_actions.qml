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
        // Issue #646: each add action heads its own Listen tab's section.
        // Systems' search field and Sort come between its action and its cards.
        readonly property var actions: [{tab: "systems", button: "addSystemButton", card: "savedSystemTitle",
                between: ["systemsSearch", "systemsSortPill"]},
            {tab: "scanLists", button: "addScanListButton", card: "scanListCard", between: []}]
        function show(tab) {
            home.showSection(tab);
            settle();
        }
        // The card a section's first saved item draws.
        function firstCard(action) {
            var found = item(action.card);
            return action.card === "savedSystemTitle" ? found.parent.parent : found;
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
            home.showSection("systems");
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
            home.showSection("systems");
            Ui.Theme.resetFontScale();
        }
        // Each tab's add action is its section's first control: in view at the top with 20 items, above the first
        // card, and where it was with none.
        function test_many_saved_items_do_not_move_actions_data() {
            return actions.map(function (action) {
                return {tag: action.tab, action: action};
            });
        }
        function test_many_saved_items_do_not_move_actions(data) {
            show(data.action.tab);
            var button = item(data.action.button);
            var buttonY = point(button).y;
            populate(20);
            settle();
            item("dongleScrollBody").contentY = 0;
            withinViewport(button);
            fuzzyCompare(point(button).y, buttonY, 1);
            verify(point(button).y + button.height <= point(firstCard(data.action)).y, "above the first card");
            mouseClick(button);
            compare(data.action.tab === "systems" ? systemAdded.count : scanAdded.count, 1);
            compare(data.action.tab === "systems" ? scanAdded.count : systemAdded.count, 0);
        }
        // Issue #646 D4: no section heading in the body, whatever the counts; the tab's label carries it.
        function test_no_section_headings() {
            compare(findChild(home, "savedSystemsHeading"), null);
            compare(findChild(home, "scanListsHeading"), null);
            for (var count of [0, 1]) {
                populate(count);
                for (var tab of ["systems", "scanLists", "explore"]) {
                    show(tab);
                    (function look(node) {
                        for (var child of node.children) {
                            if (!child.visible)
                                continue;
                            verify(["Saved systems", "Scan lists", "Or go looking"].indexOf(child.text) < 0,
                                "a heading \"" + child.text + "\" on the " + tab + " tab");
                            look(child);
                        }
                    })(item("dongleScrollBody").contentItem);
                    compare(findChild(home, "listenTab_" + tab).Accessible.name,
                        {systems: "Systems", scanLists: "Scan lists", explore: "Explore"}[tab]);
                }
                for (var action of actions) {
                    show(action.tab);
                    withinViewport(item(action.button));
                }
            }
        }
        function test_responsive_actions_data() {
            var rows = [];
            for (var size of [[320, 568], [360, 690], [390, 844], [640, 360], [899, 900], [900, 900], [1100, 900]]) {
                for (var scale of [1, 1.3, 1.6])
                    rows.push({tag: size[0] + "x" + size[1] + "@" + scale, w: size[0], h: size[1], scale: scale});
            }
            return rows;
        }
        // Each tab's action, shown on its own tab: in view, a touch target, its whole label, the column's width;
        // Add a system keeps its source hint under it, and the scan action its empty-list line while there is none.
        function checkLayout() {
            for (var action of actions) {
                show(action.tab);
                var button = item(action.button);
                withinViewport(button);
                verify(button.height >= Ui.Theme.minimumTouchSize);
                fuzzyCompare(button.width, home.columnWidth, 1, "the action fills the column");
                var label = buttonLabel(button);
                verify(label.contentWidth <= label.width + 1, "complete label fits its width");
                verify(label.contentHeight <= button.height - 24 + 1, "complete label fits its height");
                var hint = item(action.tab === "systems" ? "addSystemHint" : "scanListsEmptyHint");
                if (!hint.visible)
                    continue;
                var b = point(button);
                var h = point(hint);
                verify(h.y >= b.y + button.height, "the hint under its action");
                fuzzyCompare(h.x, b.x, 1);
                verify(hint.contentHeight <= hint.height + 1);
            }
        }
        function test_responsive_actions(data) {
            populate(1);
            root.width = data.w;
            root.height = data.h;
            Ui.Theme.fontScale = data.scale;
            checkLayout();
            compare(home.supportingPane, data.w >= 900);
            // The label stays on one line wherever it fits whole on one at normal text.
            if (data.scale < 1.1) {
                for (var action of actions) {
                    show(action.tab);
                    compare(buttonLabel(item(action.button)).lineCount, 1, action.button + " on one line");
                }
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
            for (var action of actions) {
                var button = item(action.button);
                var original = button.text;
                try {
                    button.text = "+ Add another saved radio system with a longer translated name";
                    checkLayout();
                    show(action.tab);
                    verify(buttonLabel(button).lineCount > 1);
                } finally {
                    button.text = original;
                }
            }
        }
        // Issue #646: Tab goes from the current tab to its section's action, then (on Systems past the search field
        // and Sort) to the section's first card; Backtab returns; Return, Enter and Space each activate it.
        function test_keyboard_order_and_activation() {
            populate(1);
            for (var scale of [1, 1.6]) {
                Ui.Theme.fontScale = scale;
                for (var action of actions) {
                    show(action.tab);
                    var tab = findChild(home, "listenTab_" + action.tab);
                    var button = item(action.button);
                    tab.forceActiveFocus();
                    keyClick(Qt.Key_Tab);
                    verify(button.activeFocus, "Tab from the tab reaches " + action.button);
                    keyClick(Qt.Key_Tab);
                    for (var stop of action.between) {
                        verify(Ui.Navigation.contains(item(stop), root.Window.window.activeFocusItem), "then " + stop);
                        keyClick(Qt.Key_Tab);
                    }
                    var next = root.Window.window.activeFocusItem;
                    verify(Ui.Navigation.contains(firstCard(action), next), "then the first card: " + next);
                    for (var back = 0; back <= action.between.length; ++back)
                        keyClick(Qt.Key_Backtab);
                    verify(button.activeFocus);
                    for (var key of [Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space])
                        keyClick(key);
                }
            }
            compare(scanAdded.count, 6);
            compare(systemAdded.count, 6);
        }
        function test_menu_routes_and_blocks_background_data() {
            return [{tag: "manual", index: 0}, {tag: "import", index: 1}];
        }
        function test_menu_routes_and_blocks_background(data) {
            testContext.setRadioReference("available", true);
            settle();
            var system = item("addSystemButton");
            system.forceActiveFocus();
            mouseClick(system);
            var menu = item("addSystemMenu");
            tryCompare(menu, "visible", true);
            verify(!item("dongleScrollBody").enabled);
            verify(!system.activeFocusOnTab);
            system.Accessible.pressAction();
            compare(systemAdded.count, 0);
            menu.activate(data.index);
            tryCompare(menu, "visible", false);
            compare(systemAdded.count, data.index === 0 ? 1 : 0);
            compare(systemImported.count, data.index === 1 ? 1 : 0);
            verify(item("dongleScrollBody").enabled);
        }
        // Issue #646: Add a scan list, on its own tab, is held by an open Home sheet as Add a system is.
        function test_scan_action_blocked_under_a_sheet() {
            show("scanLists");
            var scan = item("addScanListButton");
            scan.forceActiveFocus();
            verify(scan.activeFocus);
            var menu = item("scanListManageSheet");
            scanLists.add({name: "First list", sourceType: "rtltcp", entries: []});
            menu.openFor(0);
            tryCompare(menu, "visible", true);
            verify(!item("dongleScrollBody").enabled);
            verify(!scan.activeFocusOnTab);
            scan.Accessible.pressAction();
            mouseClick(scan);
            compare(scanAdded.count, 0);
            menu.cancel();
            tryCompare(menu, "visible", false);
            verify(item("dongleScrollBody").enabled);
        }
        // On a short screen at large text the status cards come first on each tab and push the action below the
        // fold; the body scrolls to it.
        function test_status_cards_precede_actions_and_scroll_data() {
            return actions.map(function (action) {
                return {tag: action.tab, action: action};
            });
        }
        function test_status_cards_precede_actions_and_scroll(data) {
            root.width = 320;
            root.height = 360;
            Ui.Theme.fontScale = 1.6;
            home.failure = {message: "Connection failed. Check the receiver and retry.", canRetry: true};
            home.completionMessage = "Replay complete";
            testContext.setBackup("startupOutcome", {kind: "applied", systems: 20, scanLists: 20});
            show(data.action.tab);
            var button = item(data.action.button);
            var scroll = item("dongleScrollBody");
            var backupCard = item("backupResultCard");
            verify(point(backupCard).y + backupCard.height < point(button).y);
            verify(point(button).y > scroll.height, "status messages retain priority on a short screen");
            scroll.contentY = Math.min(button.mapToItem(scroll.contentItem, 0, 0).y, scroll.contentHeight - scroll.height);
            withinViewport(button);
            mouseClick(button);
            compare(data.action.tab === "systems" ? systemAdded.count : scanAdded.count, 1);
        }
    }
}
