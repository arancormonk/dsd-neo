// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #646: the Listen tabs on Home. Systems, Scan lists and Explore each show their own section under the status
// cards, the tab Listen opens on is the one the device remembers, and a hidden section takes no focus, key or press.
Item {
    id: root
    width: 390
    height: 844

    Ui.HomeScreen {
        id: home
        anchors.fill: parent
        localDeviceRelevant: false
    }
    // A Home made for a case, on whatever `prefs` the case publishes, to see what it opens on.
    Component {
        id: homeComponent
        Ui.HomeScreen {
            anchors.fill: parent
            localDeviceRelevant: false
        }
    }
    // Changed once per frame wanted, so the window draws while Home is hidden.
    Rectangle {
        id: ticker
        width: 1
        height: 1
        z: 10
        color: "black"
    }
    // A store that keeps what Home writes, as the app's AppPrefs does.
    QtObject {
        id: keepingStore
        property string listenSection: "systems"
    }
    SignalSpy { id: systemAdded; target: home; signalName: "addSystem" }
    SignalSpy { id: scanAdded; target: home; signalName: "addScanList" }
    SignalSpy { id: searchOpened; target: home; signalName: "signalSearch" }
    SignalSpy { id: exploreStarted; target: home; signalName: "explore" }
    SignalSpy { id: exploreSetUp; target: home; signalName: "exploreSetup" }
    SignalSpy { id: systemPlayed; target: home; signalName: "playSystem" }
    SignalSpy { id: listPlayed; target: home; signalName: "playScanList" }
    SignalSpy { id: placementFlips; target: home; signalName: "tabsFixedChanged" }

    TestCase {
        name: "HomeTabs"
        when: windowShown

        readonly property var ids: ["systems", "scanLists", "explore"]
        readonly property var sectionNames: ({systems: "listenSystemsSection", scanLists: "listenScanListsSection",
            explore: "listenExploreSection"})

        function item(name, under) {
            var result = findChild(under || home, name);
            verify(result !== null, name + " exists");
            return result;
        }
        function body(screen) {
            return item("dongleScrollBody", screen);
        }
        function tab(id, screen) {
            return item("listenTab_" + id, screen);
        }
        function section(id, screen) {
            return item(sectionNames[id], screen);
        }
        function emptyModels() {
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
        }
        function addSystems(count) {
            for (var i = 0; i < count; ++i)
                verify(savedSystems.add({name: "System " + i, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"}));
        }
        function addLists(count) {
            for (var i = 0; i < count; ++i) {
                verify(scanLists.add({name: "List " + i, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                    entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]}));
            }
        }
        function settle(screen) {
            verify(waitForPolish(screen || home));
            waitForRendering(screen || home);
        }
        // A tap on the tab, as a finger chooses it. A frame is awaited only when the tap changes something to draw.
        function choose(id, screen) {
            var target = tab(id, screen);
            verify(waitForPolish(screen || home));
            var changes = (screen || home).section !== id;
            mouseClick(target, target.width / 2, target.height / 2);
            compare((screen || home).section, id, "the tap chose " + id);
            if (changes)
                settle(screen);
            else
                verify(waitForPolish(screen || home));
        }
        function rectIn(control, ancestor) {
            var p = control.mapToItem(ancestor, 0, 0);
            return {x: p.x, y: p.y, w: control.width, h: control.height};
        }
        function inView(control, screen) {
            var scroll = body(screen);
            var r = rectIn(control, scroll);
            return control.visible && r.x >= -0.5 && r.x + r.w <= scroll.width + 0.5 && r.y >= -0.5 && r.y + r.h <= scroll.height + 0.5;
        }
        // Draws @a count frames while Home stays as it is: every polish a hidden Home would see runs in them.
        function waitFrames(count) {
            for (var i = 0; i < count; ++i) {
                ticker.color = ticker.color === Qt.color("black") ? "white" : "black";
                verify(waitForRendering(ticker), "frame " + i);
            }
        }
        function focused() {
            return root.Window.window.activeFocusItem;
        }
        // The tabs the keyboard's Tab can land on.
        function tabStops(screen) {
            return ids.filter(function (id) {
                return tab(id, screen).activeFocusOnTab;
            });
        }
        // Every shown text in the body that reads @a words.
        function shownTexts(under, words) {
            var out = [];
            (function look(node) {
                for (var child of node.children) {
                    if (!child.visible)
                        continue;
                    if (typeof child.text === "string" && words.indexOf(child.text) >= 0)
                        out.push(child);
                    look(child);
                }
            })(under);
            return out;
        }

        function init() {
            emptyModels();
            home.visible = true;
            home.showSection("systems");
            home.resetScrollMemory();
            testContext.resetListenSection();
            testContext.resetBackup();
            testContext.setRadioReference("available", false);
            home.failure = ({});
            home.completionMessage = "";
            root.width = 390;
            root.height = 844;
            Ui.Theme.resetFontScale();
            body().contentY = body().originY;
            for (var spy of [systemAdded, scanAdded, searchOpened, exploreStarted, exploreSetUp, systemPlayed, listPlayed, placementFlips])
                spy.clear();
            failOnWarning(/(HomeScreen|ListenTabs)\.qml.*(Binding loop|Unable to assign|TypeError)/);
        }
        function cleanup() {
            root.forceActiveFocus();
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            home.prefsStore = Qt.binding(function () {
                return prefs;
            });
            home.showSection("systems");
            home.resetScrollMemory();
            home.visible = true;
            testContext.resetListenSection();
            testContext.resetPins();
            testContext.resetBackup();
            testContext.setRadioReference("available", false);
            testContext.setDongleStatus(false, 0, "");
            testContext.setLocalDeviceRecord("usb", false, "");
            testContext.setAttachedDeviceSource("");
            testContext.useLifecycleHost(false);
            home.failure = ({});
            home.completionMessage = "";
            emptyModels();
            Ui.Theme.resetFontScale();
        }

        // The issue's own case: with long lists, Explore is one tap away, Signal search and then the Explore card in
        // view at once, and Signal search opens from there.
        function test_explore_one_tap_away_with_long_lists() {
            addSystems(30);
            addLists(10);
            settle();
            verify(body().contentHeight > 3 * body().height, "the lists are long");
            choose("explore");
            var search = item("homeSignalSearch");
            var card = item("exploreCard");
            verify(inView(search), "Signal search in view");
            verify(inView(card), "the Explore card in view");
            verify(rectIn(search, body()).y + search.height <= rectIn(card, body()).y, "Signal search comes first");
            mouseClick(search, search.width / 2, search.height / 2);
            compare(searchOpened.count, 1, "the tap opens Signal search");
        }

        // Each tab shows its own section and nothing of the others, whose cards still follow their models.
        function test_each_tab_shows_only_its_section() {
            addSystems(2);
            addLists(2);
            var shownBy = {systems: ["addSystemButton", "savedSystemTitle"], scanLists: ["addScanListButton", "scanListCard"],
                explore: ["homeSignalSearch", "exploreCard"]};
            for (var id of ids) {
                choose(id);
                for (var other of ids) {
                    var wrapper = section(other);
                    compare(wrapper.visible, other === id, other + " shown on the " + id + " tab");
                    compare(wrapper.enabled, other === id, other + " enabled on the " + id + " tab");
                    for (var name of shownBy[other]) {
                        var control = item(name);
                        compare(control.visible, other === id, name + " on the " + id + " tab");
                    }
                }
                compare(tab(id).Accessible.checked, true, id + " reads as the checked tab");
            }
            // Added while another tab shows, a system is there on its own tab.
            verify(savedSystems.add({name: "Late arrival", sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"}));
            choose("systems");
            var titles = shownTexts(section("systems"), ["Late arrival"]);
            compare(titles.length, 1, "the system added behind another tab shows");
        }

        // D4: each section starts with its creation action (Systems with its hint under it), right under the status
        // cards and in view however long the lists are; Explore with Signal search, then the Explore card.
        function test_sections_start_with_their_action() {
            addSystems(20);
            addLists(20);
            var firsts = {systems: "addSystemButton", scanLists: "addScanListButton", explore: "homeSignalSearch"};
            var seconds = {systems: "addSystemHint", scanLists: "scanListCard", explore: "exploreCard"};
            for (var id of ids) {
                choose(id);
                body().contentY = body().originY;
                var wrapper = section(id);
                var first = item(firsts[id]);
                var next = item(seconds[id]);
                compare(rectIn(first, wrapper).y, 0, firsts[id] + " starts the " + id + " section");
                compare(wrapper.y, 0, "with no status card the section starts the body");
                verify(inView(first), firsts[id] + " in view at the top");
                verify(rectIn(first, wrapper).y + first.height <= rectIn(next, wrapper).y, seconds[id] + " follows it");
                if (id !== "explore") {
                    verify(first.height >= Ui.Theme.minimumTouchSize, "touch size");
                    fuzzyCompare(first.width, home.columnWidth, 0.5, "the action fills the column");
                }
            }
            // Systems: the hint, then the first card.
            choose("systems");
            var hint = item("addSystemHint");
            var firstCard = shownTexts(section("systems"), ["System 0"])[0];
            verify(rectIn(hint, section("systems")).y + hint.height <= rectIn(firstCard, section("systems")).y, "the hint before the first card");
        }

        // D4: with no scan list, one line under Add a scan list says what a list is for; a list replaces it.
        function test_empty_scan_lists_line() {
            choose("scanLists");
            var hint = item("scanListsEmptyHint");
            var add = item("addScanListButton");
            verify(hint.visible);
            compare(hint.text, "A scan list steps through saved systems and frequencies one at a time.");
            verify(rectIn(add, section("scanLists")).y + add.height <= rectIn(hint, section("scanLists")).y, "under the action");
            verify(inView(hint), "in view");
            verify(testContext.accessibleNode(hint).inTree, "a screen reader reads it");
            for (var scale of [1.0, 2.0]) {
                root.width = 320;
                Ui.Theme.fontScale = scale;
                settle();
                compare(hint.height, hint.implicitHeight, "every line shows at " + scale);
                verify(hint.contentWidth <= hint.width + 0.5, "inside the column at " + scale);
            }
            addLists(1);
            tryCompare(hint, "visible", false);
            var card = item("scanListCard");
            settle();
            verify(card.visible);
            choose("systems");
            verify(!testContext.accessibleNode(hint).inTree || !hint.visible, "nothing of it on another tab");
            scanLists.remove(0);
            choose("scanLists");
            tryCompare(hint, "visible", true);
        }

        // D3: the backup result, the failure and the replay completion head the body on every tab.
        function test_status_cards_on_every_tab() {
            addSystems(3);
            addLists(3);
            home.failure = {message: "Connection failed. Check the receiver and retry.", canRetry: true};
            home.completionMessage = "Replay complete";
            testContext.setBackup("startupOutcome", {kind: "applied", systems: 3, scanLists: 3});
            settle();
            var firsts = {systems: "addSystemButton", scanLists: "addScanListButton", explore: "homeSignalSearch"};
            for (var id of ids) {
                choose(id);
                var bottom = 0;
                for (var name of ["backupResultCard", "homeFailureCard", "homeCompletionCard"]) {
                    var card = item(name);
                    verify(card.visible, name + " on the " + id + " tab");
                    var r = rectIn(card, body().contentItem);
                    verify(r.y >= bottom, name + " in order");
                    bottom = r.y + r.h;
                }
                verify(rectIn(item(firsts[id]), body().contentItem).y >= bottom, "the " + id + " section under them");
            }
        }

        // D5: a Home opens on the tab the device remembers, read once (later changes to the stored value move nothing),
        // and anything stored that is not a tab opens on Systems.
        function test_opens_on_the_remembered_tab() {
            testContext.useLifecycleHost(true);
            home.visible = false;
            compare(prefs.listenSection, "systems");
            var first = createTemporaryObject(homeComponent, root);
            verify(first !== null);
            compare(first.section, "systems", "a fresh install opens on Systems");
            settle(first);
            choose("explore", first);
            compare(prefs.listenSection, "explore", "the choice is stored");
            prefs.listenSection = "scanLists";
            compare(first.section, "explore", "read once: a stored change later moves nothing");
            first.destroy();
            wait(0);
            var second = createTemporaryObject(homeComponent, root);
            compare(second.section, "scanLists", "the next Home opens on what is stored");
            verify(section("scanLists", second).visible);
            compare(tab("scanLists", second).Accessible.checked, true);
            second.destroy();
            wait(0);
            for (var stored of ["bogus", "Explore", 2]) {
                testContext.storeRawPref("listen/section", stored);
                var damaged = createTemporaryObject(homeComponent, root);
                compare(damaged.section, "systems", JSON.stringify(stored) + " opens on Systems");
                verify(section("systems", damaged).visible);
                verify(!section("explore", damaged).visible);
                damaged.destroy();
                wait(0);
            }
        }

        // D5: Home's own choice stands whatever the store does with it: the test map keeps nothing, and the app's
        // store keeps it.
        function test_choice_stands_without_the_store() {
            compare(prefs.listenSection, "systems");
            choose("explore");
            compare(prefs.listenSection, "systems", "the plain map kept nothing");
            verify(section("explore").visible, "and Explore shows all the same");
            choose("scanLists");
            verify(section("scanLists").visible);
            home.prefsStore = keepingStore;
            choose("explore");
            compare(keepingStore.listenSection, "explore", "a store that keeps it is written");
            home.showSection("nowhere");
            compare(home.section, "explore", "an unknown id does nothing");
            compare(keepingStore.listenSection, "explore");
        }

        // A site group's card keeps its layout while its section is hidden: the site button stays where it was, clear
        // of Play, through the hidden layout passes (tst_home_layout_fixes.qml holds it with Home hidden).
        function test_hidden_card_keeps_its_layout() {
            verify(savedSystems.add({name: "County", siteName: "North", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "851.5", rrSid: 12, rrSiteId: 100}));
            settle();
            var sites = item("homeSiteChooserButton");
            var play = item("savedSystemPlay");
            verify(sites.visible && play.visible);
            var x = sites.x;
            verify(x + sites.width <= play.x, "the site button left of Play");
            choose("explore");
            waitFrames(3);
            compare(sites.x, x, "the site button kept its place with its section hidden");
            choose("systems");
            compare(sites.x, x);
        }

        // A hidden section is inert: what had the keyboard there gives it up to the current tab when a tap changes
        // the tab, and Return, Enter or a screen reader's late press on its controls do nothing.
        function test_hidden_section_is_inert_data() {
            return [
                {tag: "Add a system", name: "addSystemButton", section: "systems", spy: "systemAdded"},
                {tag: "a system's Play", name: "savedSystemPlay", section: "systems", spy: "systemPlayed"},
                {tag: "a system's More", name: "savedSystemManageButton", section: "systems", menu: "savedSystemManageSheet"},
                {tag: "Add a scan list", name: "addScanListButton", section: "scanLists", spy: "scanAdded"},
                {tag: "a list's Play", name: "scanListPlay", section: "scanLists", spy: "listPlayed"},
                {tag: "a list's More", name: "scanListManageButton", section: "scanLists", menu: "scanListManageSheet"},
                {tag: "Signal search", name: "homeSignalSearch", section: "explore", spy: "searchOpened"},
                {tag: "Signal search More", name: "signalSearchManageButton", section: "explore", menu: "signalSearchManageSheet"},
                {tag: "the Explore card", name: "exploreStart", section: "explore", spy: "exploreSetUp"},
                {tag: "Explore More", name: "exploreManageButton", section: "explore", menu: "exploreManageSheet"}
            ];
        }
        function test_hidden_section_is_inert(data) {
            addSystems(1);
            addLists(1);
            var spies = {systemAdded: systemAdded, systemPlayed: systemPlayed, scanAdded: scanAdded, listPlayed: listPlayed,
                searchOpened: searchOpened, exploreSetUp: exploreSetUp};
            choose(data.section);
            var control = item(data.name);
            control.forceActiveFocus();
            verify(control.activeFocus, "it takes the focus while shown");
            var away = data.section === "explore" ? "systems" : "explore";
            choose(away);
            verify(!control.activeFocus, "the hidden control gave up the focus");
            verify(tab(away).activeFocus, "to the current tab");
            compare(JSON.stringify(tabStops()), JSON.stringify([away]), "one Tab stop, the current tab");
            control.forceActiveFocus();
            verify(!control.activeFocus, "a hidden control takes no focus");
            verify(!control.activeFocusOnTab || !control.enabled, "nor is it a Tab stop");
            verify(!testContext.accessibleNode(control).inTree, "out of the tree");

            // The tab changed under the focus without the tabs' hand-over: still nothing hidden acts.
            choose(data.section);
            control.forceActiveFocus();
            home.section = away;
            for (var key of [Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space])
                keyClick(key);
            control.Accessible.pressAction();
            testContext.pressAccessible(control);
            if (data.spy)
                compare(spies[data.spy].count, 0, data.tag + " did nothing while hidden");
            else
                verify(!item(data.menu).visible, data.tag + " opened nothing while hidden");
            verify(!home.managementSheetOpen);
        }

        // Keyboard focus inside a section moves to the new current tab when a tap changes the tab, and when Home is
        // told to show another section.
        function test_focus_follows_to_the_current_tab() {
            addSystems(3);
            settle();
            var add = item("addSystemButton");
            add.forceActiveFocus();
            choose("explore");
            verify(tab("explore").activeFocus, "a tap hands the focus to the tab it chose");
            item("homeSignalSearch").forceActiveFocus();
            home.showSection("scanLists");
            verify(tab("scanLists").activeFocus, "Home's own switch hands it on too");
            compare(JSON.stringify(tabStops()), JSON.stringify(["scanLists"]));
            // Focus outside the sections stays where it is.
            root.forceActiveFocus();
            home.showSection("systems");
            verify(root.activeFocus, "focus outside Home's sections is left alone");
        }

        // The tabs are disabled under a management sheet, as the Pinned strip is: no tap, key or press changes tab.
        function test_tabs_disabled_under_a_sheet() {
            testContext.setRadioReference("available", true);
            settle();
            var add = item("addSystemButton");
            mouseClick(add, add.width / 2, add.height / 2);
            var menu = item("addSystemMenu");
            tryCompare(menu, "visible", true);
            verify(home.managementSheetOpen);
            var strip = item("listenTabs");
            verify(!strip.enabled, "the strip is disabled");
            var explore = tab("explore");
            explore.Accessible.pressAction();
            testContext.pressAccessible(explore);
            explore.forceActiveFocus();
            verify(!explore.activeFocus, "a covered tab takes no focus");
            compare(home.section, "systems", "no press under the sheet changed the tab");
            // A tap there lands outside the sheet, which closes it and goes no further.
            mouseClick(explore, explore.width / 2, explore.height / 2);
            tryCompare(menu, "visible", false);
            compare(home.section, "systems", "the tap that closed the sheet chose nothing");
            verify(strip.enabled);
            choose("explore");
        }

        // §6: Tab runs pill, pins, More, the current tab, then the body; Backtab from the body returns to the tab.
        function test_keyboard_order_data() {
            return [{tag: "brokered with pins", brokered: true, pins: true, start: "systems"},
                {tag: "desktop with pins", brokered: false, pins: true, start: "systems"},
                {tag: "desktop", brokered: false, pins: false, start: "systems"},
                {tag: "desktop on Explore", brokered: false, pins: false, start: "explore"}];
        }
        function test_keyboard_order(data) {
            if (data.brokered) {
                // The brokered capability is CONSTANT: set it before publishing the host (see tst_home_dongle_status).
                testContext.setDongleStatus(false, 0, "");
                testContext.useLifecycleHost(true);
            }
            addSystems(2);
            if (data.pins) {
                compare(listenPins.pin("system", savedSystems.get(0).uid), "");
                compare(listenPins.pin("system", savedSystems.get(1).uid), "");
            }
            home.showSection(data.start);
            settle();
            var order = [];
            if (data.brokered)
                order.push(item("dongleStatusPill"));
            if (data.pins) {
                var pins = [];
                for (var child of item("pinnedStrip").children) {
                    if (child.objectName === "pinnedStripPin" && child.opacity > 0)
                        pins.push(child);
                }
                pins.sort(function (a, b) {
                    return a.x - b.x;
                });
                compare(pins.length, 2);
                order = order.concat(pins, [item("pinnedStripMore")]);
            }
            order.push(tab(data.start));
            order.push(item(data.start === "explore" ? "homeSignalSearch" : "addSystemButton"));
            root.forceActiveFocus();
            for (var i = 0; i < order.length; ++i) {
                keyClick(Qt.Key_Tab);
                verify(order[i].activeFocus, "Tab " + (i + 1) + " reaches " + (order[i].objectName || order[i].text) + ", not " + focused());
            }
            keyClick(Qt.Key_Backtab);
            verify(tab(data.start).activeFocus, "Backtab from the body lands on the current tab");
        }

        // At every phone width and text size, and beside the Recent activity pane, the tabs sit on the content column
        // under the title, their labels whole, and the body starts under them with each section's first control in
        // view.
        function test_layout_data() {
            var rows = [];
            for (var width of [320, 360, 411]) {
                for (var scale of [1.0, 1.6, 2.0])
                    rows.push({tag: width + " px at " + scale, width: width, height: 740, scale: scale});
            }
            for (var wide of [900, 1000]) {
                for (var wideScale of [1.0, 2.0])
                    rows.push({tag: "Home " + wide + " px at " + wideScale + " with Recent activity", width: wide, height: 900, scale: wideScale});
            }
            return rows;
        }
        function test_layout(data) {
            addSystems(6);
            addLists(6);
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            settle();
            compare(home.width, data.width);
            compare(home.supportingPane, data.width >= 900);
            var recent = item("homeRecentList");
            compare(recent.parent.visible, data.width >= 900, "the Recent activity pane");
            var strip = item("listenTabs");
            var header = item("listenHeader");
            var s = rectIn(strip, home);
            fuzzyCompare(s.x, home.columnX, 0.5, "on the column's x");
            fuzzyCompare(s.w, home.columnWidth, 0.5, "the column's width");
            if (data.width >= 900)
                verify(s.x + s.w <= rectIn(recent.parent, home).x - Ui.Theme.screenPadding + 0.5, "left of Recent activity");
            verify(s.y >= rectIn(header, home).y + header.height, "under the title");
            verify(s.h >= Ui.Theme.minimumTouchSize, "touch height");
            fuzzyCompare(rectIn(body(), home).y, s.y + s.h, 0.5, "the body starts at the tabs' hairline");
            for (var id of ids) {
                var t = tab(id);
                var label = item("listenTabLabel", t);
                verify(!label.elided, id + " label whole");
                verify(label.contentWidth <= label.width + 0.5, id + " label inside its tab");
                var l = rectIn(label, t);
                verify(l.y >= -0.5 && l.y + label.height <= t.height + 0.5, id + " label inside the strip");
            }
            var firsts = {systems: "addSystemButton", scanLists: "addScanListButton", explore: "homeSignalSearch"};
            for (id of ids) {
                choose(id);
                body().contentY = body().originY;
                verify(inView(item(firsts[id])), firsts[id] + " in view");
            }
        }

        // While the tabs are fixed the title is in the fixed header on both hosts, once.
        function test_fixed_title_on_both_hosts_data() {
            return [{tag: "desktop", brokered: false}, {tag: "brokered", brokered: true}];
        }
        function test_fixed_title_on_both_hosts(data) {
            if (data.brokered) {
                testContext.setDongleStatus(false, 0, "");
                testContext.useLifecycleHost(true);
            }
            settle();
            compare(home.pinnedHeader, data.brokered);
            verify(home.tabsFixed);
            var header = item("listenHeader");
            verify(header.visible && item("listenHeaderTitle").visible, "the title is fixed above the tabs");
            compare(header.y, Ui.Theme.screenPadding);
            verify(!item("listenTitle").visible, "and not in the body as well");
            compare(item("dongleStatusPill").visible, data.brokered);
            verify(rectIn(item("listenTabs"), home).y >= header.y + header.height);
            var titles = testContext.accessibleNodesUnder(home).filter(function (node) {
                return node.inTree && node.role === Accessible.Heading && node.name === "Listen";
            });
            compare(titles.length, 1, "the title read once");
        }

        // --- Short screens (§2): the tabs are fixed while fixedExtent <= height / 2, else they lead the body. ---

        function useBrokeredHost() {
            // The brokered capability is CONSTANT: set it before publishing the host (see tst_home_dongle_status).
            testContext.setDongleStatus(false, 0, "");
            testContext.useLifecycleHost(true);
        }
        function pinSystems(count) {
            for (var i = 0; i < count; ++i)
                compare(listenPins.pin("system", savedSystems.get(i).uid), "");
        }
        // Where the body starts, below everything that stays put.
        function fixedRegion() {
            return rectIn(body(), home).y;
        }
        // The one title shown and read, wherever it is.
        function checkOneTitle(why) {
            var shown = [item("listenHeaderTitle"), item("listenTitle")].filter(function (title) {
                for (var at = title; at; at = at.parent) {
                    if (!at.visible)
                        return false;
                }
                return true;
            });
            compare(shown.length, 1, "one title shown " + why);
            var read = testContext.accessibleNodesUnder(home).filter(function (node) {
                return node.inTree && node.role === Accessible.Heading && node.name === "Listen";
            });
            compare(read.length, 1, "one title read " + why);
        }
        // The placement the rule gives, laid out: fixed tabs above the body with the region at most half of Home, or
        // the tabs in the body (just where fixed ones would be when it is at its top).
        function checkPlacement(why) {
            settle();
            compare(home.tabsFixed, home.fixedExtent <= home.height / 2, "the rule " + why);
            var strip = item("listenTabs");
            var inBody = false;
            for (var at = strip; at; at = at.parent)
                inBody = inBody || at === body();
            compare(inBody, !home.tabsFixed, "the tabs in the body " + why);
            if (home.tabsFixed) {
                fuzzyCompare(fixedRegion(), home.fixedExtent, 0.5, "the fixed region is what the rule measured " + why);
                verify(fixedRegion() <= home.height / 2 + 0.5, "the fixed region at most half of Home " + why);
                fuzzyCompare(rectIn(strip, home).y + strip.height, fixedRegion(), 0.5, "the body under the tabs " + why);
            } else {
                verify(fixedRegion() < home.fixedExtent, "the body starts above where fixed tabs would end " + why);
                if (home.fixedTitle && body().contentY <= body().originY + 0.5)
                    fuzzyCompare(rectIn(strip, home).y + strip.height, home.fixedExtent, 0.5, "at rest the tabs sit where fixed ones would " + why);
            }
            checkOneTitle(why);
        }

        // The extent is prospective: it is the same in both placements, with Home hidden and on every tab.
        function test_fixed_extent_independent_of_placement_data() {
            return [{tag: "brokered with pins", brokered: true, pins: true},
                {tag: "brokered", brokered: true, pins: false},
                {tag: "desktop with pins", brokered: false, pins: true},
                {tag: "desktop", brokered: false, pins: false}];
        }
        function test_fixed_extent_independent_of_placement(data) {
            if (data.brokered)
                useBrokeredHost();
            addSystems(3);
            if (data.pins)
                pinSystems(2);
            root.width = 360;
            root.height = 740;
            Ui.Theme.fontScale = 1.6;
            settle();
            verify(home.tabsFixed, "fixed at 360 x 740");
            var extent = home.fixedExtent;
            verify(extent > 0);
            checkPlacement("at 360 x 740");
            root.height = Math.floor(2 * extent) - 2;
            checkPlacement("shorter");
            verify(!home.tabsFixed, "in the body when shorter");
            compare(home.fixedExtent, extent, "the same extent with the tabs in the body");
            for (var id of ids) {
                choose(id);
                compare(home.fixedExtent, extent, "the same extent on " + id);
            }
            home.visible = false;
            waitFrames(3);
            compare(home.fixedExtent, extent, "the same extent with Home hidden");
            compare(home.tabsFixed, false, "and the placement held");
            home.visible = true;
            root.height = 740;
            checkPlacement("taller again");
            compare(home.fixedExtent, extent);
        }

        // A tie is fixed; a pixel less moves the tabs into the body, a pixel more brings them back, each flip once.
        function test_tie_counts_as_fixed() {
            useBrokeredHost();
            addSystems(3);
            pinSystems(1);
            settle();
            var extent = home.fixedExtent;
            root.height = 2 * extent;
            checkPlacement("at the tie");
            verify(home.tabsFixed, "a tie is fixed");
            placementFlips.clear();
            root.height = 2 * extent - 1;
            checkPlacement("a pixel short");
            verify(!home.tabsFixed);
            compare(placementFlips.count, 1, "one flip");
            root.height = 2 * extent;
            checkPlacement("back at the tie");
            verify(home.tabsFixed);
            compare(placementFlips.count, 2, "one flip back");
        }

        // The keyboard focus stays on the current tab when the tabs move into the body and back, still one Tab stop.
        function test_focus_kept_across_a_move() {
            addSystems(3);
            settle();
            root.forceActiveFocus();
            for (var i = 0; i < 4 && !tab("systems").activeFocus; ++i)
                keyClick(Qt.Key_Tab);
            verify(tab("systems").activeFocus);
            root.height = Math.floor(2 * home.fixedExtent) - 2;
            settle();
            verify(!home.tabsFixed);
            verify(tab("systems").activeFocus, "kept moving into the body");
            compare(JSON.stringify(tabStops()), JSON.stringify(["systems"]));
            keyClick(Qt.Key_Right);
            compare(home.section, "scanLists", "the keys still work there");
            root.height = 844;
            settle();
            verify(home.tabsFixed);
            verify(tab("scanLists").activeFocus, "kept moving back");
            keyClick(Qt.Key_Tab);
            verify(item("addScanListButton").activeFocus, "Tab goes on into the body");
        }

        // With no height (not laid out yet, or hidden at zero size) the last placement stands, either way.
        function test_no_height_keeps_the_last_placement() {
            addSystems(3);
            settle();
            verify(home.tabsFixed);
            root.height = 0;
            settle();
            verify(home.tabsFixed, "fixed stays fixed at zero height");
            root.height = Math.floor(2 * home.fixedExtent) - 2;
            settle();
            verify(!home.tabsFixed);
            root.height = 0;
            settle();
            verify(!home.tabsFixed, "in the body stays in the body at zero height");
            root.height = 844;
            checkPlacement("laid out again");
            verify(home.tabsFixed);
        }

        // The phones the spec names: landscape at 1.6 with pins puts the tabs in the body, portrait keeps them fixed.
        // In either, one title, and Tab runs pill, pins, More, the current tab, then the body.
        function test_short_screens_data() {
            return [{tag: "landscape 640 x 360 at 1.6 with pins", width: 640, height: 360, fixed: false, brokered: true},
                {tag: "landscape 640 x 360 at 1.6 with pins on the desktop", width: 640, height: 360, fixed: false, brokered: false},
                {tag: "portrait 360 x 740 at 1.6 with pins", width: 360, height: 740, fixed: true, brokered: true}];
        }
        function test_short_screens(data) {
            if (data.brokered)
                useBrokeredHost();
            addSystems(6);
            pinSystems(2);
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = 1.6;
            checkPlacement(data.tag);
            compare(home.tabsFixed, data.fixed, "the tabs fixed");
            var order = [];
            if (data.brokered)
                order.push(item("dongleStatusPill"));
            var pins = [];
            for (var child of item("pinnedStrip").children) {
                if (child.objectName === "pinnedStripPin" && child.opacity > 0)
                    pins.push(child);
            }
            pins.sort(function (a, b) {
                return a.x - b.x;
            });
            verify(pins.length >= 1);
            order = order.concat(pins, [item("pinnedStripMore"), tab("systems"), item("addSystemButton")]);
            root.forceActiveFocus();
            for (var i = 0; i < order.length; ++i) {
                keyClick(Qt.Key_Tab);
                verify(order[i].activeFocus, "Tab " + (i + 1) + " reaches " + (order[i].objectName || order[i].text) + ", not " + focused());
            }
            keyClick(Qt.Key_Backtab);
            verify(tab("systems").activeFocus, "Backtab from the body lands on the current tab");
            settle();
            verify(inView(tab("systems")) || home.tabsFixed, "the tab in view");
        }

        // One title in both placements, on both hosts, with and without pins.
        function test_one_title_in_both_placements_data() {
            return [{tag: "desktop", brokered: false, pins: false}, {tag: "desktop with pins", brokered: false, pins: true},
                {tag: "brokered", brokered: true, pins: false}, {tag: "brokered with pins", brokered: true, pins: true}];
        }
        function test_one_title_in_both_placements(data) {
            if (data.brokered)
                useBrokeredHost();
            addSystems(3);
            if (data.pins)
                pinSystems(1);
            settle();
            checkPlacement("fixed");
            verify(home.tabsFixed);
            verify(item("listenHeaderTitle").visible && item("listenHeader").visible, "the fixed title, as fixed tabs need");
            root.height = Math.floor(2 * home.fixedExtent) - 2;
            checkPlacement("in the body");
            verify(!home.tabsFixed);
            // The title stays where it is without the tabs: fixed with the pill or pins, else leading the body.
            compare(item("listenHeader").visible, data.brokered || data.pins, "the title fixed with the pill or pins");
            compare(item("listenTitle").visible, !data.brokered && !data.pins, "else the body's own title row");
            if (!data.brokered && !data.pins) {
                var title = item("listenTitle");
                verify(rectIn(title, body().contentItem).y + title.height <= rectIn(item("listenTabs"), body().contentItem).y,
                    "the title above the tabs in the body");
            }
        }

        // Live resize, font change and the widest receiver label just above and just below the threshold: each flips
        // the placement once, and the rendered fixed region is at most half of Home whenever the tabs are fixed.
        function test_threshold_crossings_data() {
            return [{tag: "live resize"}, {tag: "font change"}, {tag: "widest receiver label"}];
        }
        function test_threshold_crossings(data) {
            useBrokeredHost();
            addSystems(6);
            pinSystems(1);
            root.width = 320;
            // At 2.0 the widest label wraps inside the pill, which then stands taller than its minimum.
            Ui.Theme.fontScale = data.tag === "widest receiver label" ? 2.0 : 1.0;
            settle();
            var low = home.fixedExtent;
            var high;
            var change;
            if (data.tag === "live resize") {
                high = low + 2;
                change = function (up) {
                    root.height = up ? 2 * low - 2 : 2 * low;
                };
            } else if (data.tag === "font change") {
                Ui.Theme.fontScale = 1.3;
                settle();
                high = home.fixedExtent;
                change = function (up) {
                    Ui.Theme.fontScale = up ? 1.3 : 1.0;
                };
            } else {
                testContext.setAttachedDeviceSource("usb");
                testContext.setDongleStatus(false, 4, "USB permission denied");
                testContext.setLocalDeviceRecord("", false, "usb");
                tryCompare(item("dongleStatusLabel"), "text", "RTL-SDR NO ACCESS");
                settle();
                var pill = item("dongleStatusPill");
                compare(item("dongleStatusLabel").lineCount, 2, "the widest label wraps");
                verify(pill.height > pill.implicitHeight + 0.5, "so the pill is taller than its minimum");
                high = home.fixedExtent;
                // One input, so one label change: access refused, or the same receiver waiting to connect.
                change = function (up) {
                    testContext.setDongleStatus(false, up ? 4 : 0, up ? "USB permission denied" : "");
                    tryCompare(item("dongleStatusLabel"), "text", up ? "RTL-SDR NO ACCESS" : "CONNECT RTL-SDR");
                };
                change(false);
                settle();
                low = home.fixedExtent;
            }
            verify(high > low + 0.5, "the change moves the extent: " + low + " to " + high);
            if (data.tag !== "live resize")
                root.height = low + high;
            change(false);
            checkPlacement("below the threshold");
            verify(home.tabsFixed, "fixed below the threshold");
            for (var round = 0; round < 2; ++round) {
                placementFlips.clear();
                change(true);
                checkPlacement("above the threshold");
                verify(!home.tabsFixed, "in the body above it");
                compare(placementFlips.count, 1, "one flip up");
                change(false);
                checkPlacement("below it again");
                verify(home.tabsFixed);
                compare(placementFlips.count, 2, "one flip back");
            }
        }

        // The single scroll divider under the fixed region: under fixed tabs their own hairline is the only line;
        // with the tabs in the body under a fixed title, the divider shows once the body scrolls, and stands aside
        // while the tabs' own hairline passes the edge; with nothing fixed there is none.
        function test_one_line_under_the_fixed_region_data() {
            return [{tag: "fixed tabs", pins: true, fixed: true}, {tag: "tabs in the body under pins", pins: true, fixed: false},
                {tag: "nothing fixed", pins: false, fixed: false}];
        }
        function test_one_line_under_the_fixed_region(data) {
            addSystems(20);
            if (data.pins)
                pinSystems(1);
            settle();
            if (!data.fixed)
                root.height = Math.floor(2 * home.fixedExtent) - 2;
            settle();
            compare(home.tabsFixed, data.fixed);
            compare(item("listenHeader").visible, data.pins, "the title fixed with pins");
            var divider = item("listenScrollDivider");
            var strip = item("listenTabs");
            verify(!divider.visible, "no divider at rest");
            body().contentY = body().originY + 200;
            settle();
            compare(divider.visible, !data.fixed && data.pins, "one line under the fixed region when scrolled");
            if (divider.visible)
                fuzzyCompare(rectIn(divider, home).y + 1, fixedRegion(), 0.5, "on the body's top edge");
            if (!data.fixed && data.pins) {
                // The tabs' own hairline on the body's top row: the divider stands aside for it.
                var line = rectIn(strip, body().contentItem).y + strip.height - 1;
                body().contentY = line;
                settle();
                verify(!divider.visible, "the tabs' hairline is the line at the edge");
                body().contentY = line + 2;
                settle();
                verify(divider.visible, "and the divider once it has gone");
            }
        }

        // --- Each tab keeps its place in the body. ---

        function deepPlace() {
            var range = body().contentHeight - body().height;
            verify(range > 400, "the body scrolls far");
            return Math.round(range / 2);
        }
        function test_each_tab_keeps_its_place() {
            addSystems(30);
            addLists(20);
            settle();
            var deep = deepPlace();
            body().contentY = deep;
            choose("explore");
            compare(body().contentY, body().originY, "Explore opens at its top, not at a clamp past it");
            choose("systems");
            compare(body().contentY, deep, "back on Systems, where it was left");
            choose("scanLists");
            compare(body().contentY, body().originY, "Scan lists at its top the first time");
            var listPlace = Math.round((body().contentHeight - body().height) / 3);
            body().contentY = listPlace;
            choose("systems");
            compare(body().contentY, deep);
            choose("scanLists");
            compare(body().contentY, listPlace, "each tab its own place");
        }

        // Switches in one turn remember no place a restore still owed: the body's range was the wrong section's.
        function test_rapid_switches_remember_no_clamp() {
            addSystems(30);
            settle();
            var deep = deepPlace();
            body().contentY = deep;
            home.showSection("explore");
            home.showSection("scanLists");
            home.showSection("explore");
            home.showSection("systems");
            settle();
            compare(body().contentY, deep, "Systems where it was left");
            choose("explore");
            compare(body().contentY, body().originY, "Explore kept no clamped place");
            choose("scanLists");
            compare(body().contentY, body().originY, "nor Scan lists");
            // Laid out between the switches, but with the restore still owed.
            choose("systems");
            home.showSection("explore");
            verify(waitForPolish(home));
            home.showSection("systems");
            settle();
            compare(body().contentY, deep);
            choose("explore");
            compare(body().contentY, body().originY);
        }

        // A switch during a fling stops it: the new tab opens still at its top, and the old one keeps where the fling
        // was when the tab changed.
        function test_switch_during_a_fling() {
            addSystems(30);
            settle();
            var deep = deepPlace();
            body().contentY = deep;
            body().flick(0, -2500);
            tryVerify(function () {
                return body().flicking && body().contentY > deep + 20;
            }, 2000, "the body is flinging down");
            var at = body().contentY;
            home.showSection("explore");
            verify(!body().flicking && !body().moving, "the fling stopped with the tab");
            settle();
            compare(body().contentY, body().originY, "Explore at its top");
            wait(100);
            compare(body().contentY, body().originY, "and still there: no fling carried over");
            choose("systems");
            verify(Math.abs(body().contentY - at) <= 1, "Systems where the fling was: " + body().contentY + " vs " + at);
            verify(body().contentY <= body().originY + body().contentHeight - body().height + 0.5, "inside the range");
        }

        // The section's range can still grow after the place is put back, in the same turn (here the text size
        // changes as the tab is chosen, and the cards' label columns lay out again only on the next polish): a deep
        // place clamped short of the old range is put back once more when the range has grown.
        function test_place_kept_when_the_range_grows_late() {
            addSystems(30);
            Ui.Theme.fontScale = 1.6;
            settle();
            body().contentY = body().originY + body().contentHeight - body().height;
            var deep = body().contentY;
            choose("explore");
            Ui.Theme.fontScale = 1.0;
            settle();
            Ui.Theme.fontScale = 1.6;
            home.showSection("systems");
            settle();
            waitFrames(2);
            compare(body().contentY, deep, "Systems back at its place once the range has grown back");
        }

        // A tab chosen while Home is hidden is put back where it was once Home shows again, not before.
        function test_restore_waits_while_hidden() {
            addSystems(30);
            settle();
            var deep = deepPlace();
            body().contentY = deep;
            choose("explore");
            home.visible = false;
            home.showSection("systems");
            waitFrames(3);
            verify(home.restorePending, "the place is still owed while Home is hidden");
            verify(Math.abs(body().contentY - deep) > 0.5, "and not put back yet");
            home.visible = true;
            settle();
            verify(!home.restorePending);
            compare(body().contentY, deep, "put back once shown");
        }

        // With the tabs in the body, a tab chosen from the keyboard keeps them in view: the place it puts back yields
        // to the focused tab. A tap with the focus elsewhere puts the whole place back.
        function test_keyboard_choice_keeps_the_tabs_in_view() {
            addSystems(30);
            addLists(30);
            root.width = 640;
            root.height = 250;
            Ui.Theme.fontScale = 1.6;
            settle();
            verify(!home.tabsFixed, "the tabs in the body");
            choose("scanLists");
            var deep = deepPlace();
            body().contentY = deep;
            // Scrolled that far the tabs are out of reach of a finger; a screen reader still presses them.
            tab("systems").Accessible.pressAction();
            compare(home.section, "systems");
            settle();
            compare(body().contentY, body().originY);
            root.forceActiveFocus();
            for (var i = 0; i < 6 && !tab("systems").activeFocus; ++i)
                keyClick(Qt.Key_Tab);
            verify(tab("systems").activeFocus, "the keyboard is on the tabs");
            settle();
            verify(inView(tab("systems")), "in view");
            keyClick(Qt.Key_Right);
            compare(home.section, "scanLists");
            settle();
            verify(tab("scanLists").activeFocus);
            verify(inView(tab("scanLists")), "the focused tab stays in view");
            verify(body().contentY > body().originY, "the place yields only as far as the tabs need");
            // A press with the focus outside the tabs: the place comes back whole, the tabs scrolling away with it.
            body().contentY = deep;
            tab("systems").Accessible.pressAction();
            settle();
            root.forceActiveFocus();
            choose("scanLists");
            compare(body().contentY, deep, "a tap puts the whole place back");
            verify(!inView(tab("scanLists")), "and the tabs scroll away with it");
        }

        // --- revealTop() is the strongest scroll writer (B3 precedence). ---

        // It drops a place still owed and keeps the top as the shown tab's place.
        function test_reveal_top_cancels_a_pending_restore() {
            addSystems(30);
            settle();
            var deep = deepPlace();
            body().contentY = deep;
            choose("explore");
            home.showSection("systems");
            verify(home.restorePending);
            home.revealTop();
            verify(!home.restorePending, "the owed place is dropped");
            settle();
            wait(50);
            compare(body().contentY, body().originY, "the top stands");
            choose("explore");
            choose("systems");
            compare(body().contentY, body().originY, "the top is Systems' place now");
        }

        // A focus reveal still pending, or one a sheet's late focus hand-back starts, does not scroll the top away;
        // the user's next key move or scroll reveals again.
        function test_reveal_top_holds_off_focus_reveals() {
            addSystems(30);
            settle();
            var plays = [];
            (function look(node) {
                for (var child of node.children) {
                    if (child.objectName === "savedSystemPlay")
                        plays.push(child);
                    look(child);
                }
            })(section("systems"));
            verify(plays.length >= 20);
            var deepPlay = plays[20];
            deepPlay.forceActiveFocus();
            home.revealTop();
            settle();
            compare(body().contentY, body().originY, "a reveal pending before revealTop() was dropped");
            plays[25].forceActiveFocus();
            settle();
            wait(50);
            compare(body().contentY, body().originY, "a focus handed over after it scrolls nothing");
            keyClick(Qt.Key_Tab);
            settle();
            verify(!plays[25].activeFocus);
            verify(inView(focused()), "a key move reveals again");
            // A scroll by the user lifts the hold too.
            home.revealTop();
            settle();
            mouseDrag(body(), body().width / 2, body().height - 20, 0, -120);
            settle();
            plays[28].forceActiveFocus();
            settle();
            verify(inView(plays[28]), "after the user scrolls, focus moves are revealed again");
        }

    }
}
