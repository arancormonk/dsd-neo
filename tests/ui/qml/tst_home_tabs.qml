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
            testContext.resetListenSection();
            testContext.resetBackup();
            testContext.setRadioReference("available", false);
            home.failure = ({});
            home.completionMessage = "";
            root.width = 390;
            root.height = 844;
            Ui.Theme.resetFontScale();
            body().contentY = body().originY;
            for (var spy of [systemAdded, scanAdded, searchOpened, exploreStarted, exploreSetUp, systemPlayed, listPlayed])
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
            home.visible = true;
            testContext.resetListenSection();
            testContext.resetPins();
            testContext.resetBackup();
            testContext.setRadioReference("available", false);
            testContext.setDongleStatus(false, 0, "");
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

        // D4: the old in-body headings are gone; the tab's label is each section's heading.
        function test_tab_labels_are_the_headings() {
            addSystems(1);
            addLists(1);
            var headings = ["Saved systems", "Scan lists", "Or go looking"];
            for (var id of ids) {
                choose(id);
                compare(shownTexts(body().contentItem, headings).length, 0, "no heading in the body on the " + id + " tab");
            }
            compare(findChild(home, "savedSystemsHeading"), null);
            compare(findChild(home, "scanListsHeading"), null);
            compare(ids.map(function (id) {
                return tab(id).Accessible.name;
            }).join(), "Systems,Scan lists,Explore");
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
    }
}
