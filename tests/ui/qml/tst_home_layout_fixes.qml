// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #646: Listen's own layout on its single-column body. B1: hidden, Home keeps its scroll range and place. B8: a
// saved-system card is as tall as what it holds, at every text size. B3: keyboard focus moving down the body scrolls
// the focused control into view.
Item {
    id: root
    QtObject {
        id: mainRoot
        property bool transitioning: false
        property bool showFailure: false
        property string failureText: ""
        property string dismissedFailure: ""
    }
    width: 360
    height: 700
    Ui.HomeScreen {
        id: home
        anchors.fill: parent
    }
    // Changed once per frame wanted, so the window draws while Home is hidden.
    Rectangle {
        id: ticker
        width: 1
        height: 1
        z: 10
        color: "black"
    }
    // Theme.revealFocus on its own: a body whose content column starts below the top.
    Flickable {
        id: offsetScroll
        width: 200
        height: 100
        visible: false
        contentHeight: offsetColumn.y + offsetColumn.height
        Column {
            id: offsetColumn
            y: 30
            width: parent.width
            Repeater {
                id: offsetRows
                model: 5
                Rectangle {
                    width: offsetColumn.width
                    height: 50
                }
            }
        }
    }
    TestCase {
        name: "HomeLayoutFixes"
        when: windowShown

        property string libraryPath: ""

        function init() {
            emptyModels();
            home.showSection("systems");
        }
        function cleanup() {
            root.forceActiveFocus();
            home.showSection("systems");
            home.visible = true;
            emptyModels();
            if (libraryPath.length)
                importedFiles.remove(importedFiles.rowForPath(libraryPath));
            libraryPath = "";
            testContext.setDongleStatus(false, 0, "");
            testContext.useLifecycleHost(false);
            root.width = 360;
            root.height = 700;
            Ui.Theme.resetFontScale();
            body().contentY = body().originY;
        }
        function emptyModels() {
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
        }
        function addSystem(name, fields) {
            verify(savedSystems.add(Object.assign({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "851.5"}, fields || {})), "could not add " + name);
            // The cards follow on the next turn.
            wait(0);
        }
        function addList(name) {
            verify(scanLists.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]}));
        }
        // A trunked system whose talkgroups came from a RadioReference import that named no site: its card asks
        // for a refresh (savedSystemCards' refreshHint).
        function addRefreshHintSystem(name) {
            var path = testContext.writeFixtureCsv("layout-provenance.csv", "Decimal,Mode,Alpha Tag\n123,A,Dispatch\n");
            var result = importedFiles.importGeneratedFile(path, "Layout provenance.csv", "group",
                {origin: "radioreference", rrSid: 12, rrSiteIds: "16863", rrKind: "group"});
            verify(result.ok);
            libraryPath = result.path;
            addSystem(name, {trunking: true, groupCsvPath: libraryPath});
        }
        function body() {
            return findChild(home, "dongleScrollBody");
        }
        function cards() {
            var out = [];
            // Issue #646: on the Systems tab's section.
            for (var child of findChild(home, "listenSystemsSection").children) {
                if (child.objectName === "savedSystemEntry")
                    out.push(findChild(child, "savedSystemCard"));
            }
            return out;
        }
        function cardNamed(name) {
            for (var card of cards()) {
                if (card.name === name)
                    return card;
            }
            fail("no card for " + name);
        }
        // Draws @a count frames while Home stays as it is: every polish a hidden Home would see runs in them.
        function waitFrames(count) {
            for (var i = 0; i < count; ++i) {
                ticker.color = ticker.color === Qt.color("black") ? "white" : "black";
                verify(waitForRendering(ticker), "frame " + i);
            }
        }
        function rectIn(item, ancestor) {
            var p = item.mapToItem(ancestor, 0, 0);
            return {x: p.x, y: p.y, w: item.width, h: item.height};
        }
        function overlaps(a, b) {
            return a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 && b.y < a.y + a.h - 0.5;
        }
        function inside(a, card) {
            return a.x >= -0.5 && a.y >= -0.5 && a.x + a.w <= card.width + 0.5 && a.y + a.h <= card.height + 0.5;
        }
        function describe(r) {
            return "[" + Math.round(r.x) + "," + Math.round(r.y) + " " + Math.round(r.w) + "x" + Math.round(r.h) + "]";
        }

        // B1: Main hides Home on another tab, the wizard, an editor, Signal search or a session. Hidden, the body's
        // cards keep their heights through every polish, so the scroll range and the place the user scrolled to
        // are there when Home shows again.
        function test_scroll_kept_while_hidden() {
            for (var i = 0; i < 15; ++i)
                addSystem("System " + i);
            for (i = 0; i < 5; ++i)
                addList("List " + i);
            verify(waitForPolish(root.Window.window));
            waitForRendering(home);
            var scrolled = body().contentHeight - body().height;
            verify(scrolled > 1000, "the body scrolls a long way: " + scrolled);
            body().contentY = scrolled;
            var range = body().contentHeight;
            var cardHeight = cards()[0].height;
            verify(cardHeight > 0);
            home.visible = false;
            waitFrames(3);
            compare(cards()[0].height, cardHeight, "a hidden card keeps its height");
            compare(body().contentHeight, range, "the hidden body keeps its scroll range");
            compare(body().contentY, scrolled, "the hidden body keeps its place");
            home.visible = true;
            verify(waitForPolish(home));
            waitFrames(3);
            compare(body().contentHeight, range, "the scroll range when shown again");
            compare(body().contentY, scrolled, "the place when shown again");
        }

        // B1 too: a site group's card keeps its site button where it was, clear of Play, through the hidden body's
        // layout passes. It used to line up against Play's `visible`, which goes false with Home.
        function test_site_button_kept_while_hidden() {
            addSystem("County", {siteName: "North", rrSid: 12, rrSiteId: 100});
            verify(waitForPolish(home));
            waitForRendering(home);
            var card = cardNamed("County");
            var sites = findChild(card, "homeSiteChooserButton");
            var play = findChild(card, "savedSystemPlay");
            verify(sites.visible && play.visible);
            var x = sites.x;
            verify(x + sites.width <= play.x, "the site button left of Play");
            home.visible = false;
            waitFrames(3);
            compare(sites.x, x, "the hidden card's site button kept its place");
            home.visible = true;
            verify(waitForPolish(home));
            compare(sites.x, x, "and shown again");
        }

        // #832: the hint follows the library, a file replaced in place included. A user's own file over the
        // RadioReference channel map is not RadioReference's any more, so the card stops asking for a refresh, though
        // the replace leaves the library's size as it was.
        function test_replaced_map_stops_offering_the_refresh() {
            var generated = testContext.writeFixtureCsv("layout-rr-map.csv", "channel,frequency\n1,851012500\n");
            var result = importedFiles.importGeneratedFile(generated, "Layout RR map.csv", "chan",
                {origin: "radioreference", rrSid: 12, rrSiteIds: "16863", rrKind: "chan"});
            verify(result.ok);
            libraryPath = result.path;
            addSystem("County map system", {trunking: true, chanCsvPath: libraryPath});
            var card = cardNamed("County map system");
            verify(card.refreshHint, "a RadioReference map asks for a refresh");
            var spy = Qt.createQmlObject("import QtTest; SignalSpy { signalName: \"importFinished\" }", root);
            spy.target = importedFiles;
            var count = importedFiles.count;
            var own = testContext.writeFixtureCsv("layout-own-map.csv", "channel,frequency\n1,851012500\n2,852012500\n");
            importedFiles.beginImport(own, "layout-own-map.csv", "chan", {}, libraryPath);
            tryCompare(spy, "count", 1);
            verify(spy.signalArguments[0][1].ok, "the replace committed");
            compare(importedFiles.count, count, "the replace keeps the library's size");
            compare(importedFiles.get(importedFiles.rowForPath(libraryPath)).origin, "", "the file is the user's now");
            tryVerify(function () {
                return !card.refreshHint;
            }, 1000, "the card no longer offers the refresh");
            spy.destroy();
        }

        // B8: a card with the refresh hint holds its labels, its actions and the hint without any two meeting, at
        // every text size, on the narrowest phone.
        function test_refresh_hint_card_holds_its_parts_data() {
            return [{tag: "320 px at 1.0", scale: 1.0}, {tag: "320 px at 1.6", scale: 1.6},
                {tag: "320 px at 2.0", scale: 2.0}];
        }
        function test_refresh_hint_card_holds_its_parts(data) {
            root.width = 320;
            Ui.Theme.fontScale = data.scale;
            addRefreshHintSystem("County trunked system");
            verify(waitForPolish(home));
            waitForRendering(home);
            var card = cardNamed("County trunked system");
            verify(card.refreshHint);
            var hint = findChild(card, "savedSystemRefreshHint");
            verify(hint.visible);
            var hintRect = rectIn(hint, card);
            verify(inside(hintRect, card), "the hint " + describe(hintRect) + " inside the card, " + card.height + " tall");
            compare(hint.height, hint.implicitHeight, "every line of the hint is shown");
            var parts = [["labels", findChild(card, "savedSystemTitle").parent], ["Play", findChild(card, "savedSystemPlay")],
                ["More", findChild(card, "savedSystemManageButton")], ["sites", findChild(card, "homeSiteChooserButton")]];
            for (var part of parts) {
                if (!part[1].visible)
                    continue;
                var r = rectIn(part[1], card);
                verify(inside(r, card), part[0] + " " + describe(r) + " inside the card, " + card.height + " tall");
                verify(!overlaps(r, hintRect), part[0] + " " + describe(r) + " clear of the hint " + describe(hintRect));
            }
        }

        // B8: a card without the hint keeps its 122 px and the place of its labels at normal text, and at large text
        // grows to hold them rather than letting them spill over its top and bottom edges. A card with one site
        // holds its site button the same way where it draws it.
        function test_plain_card_holds_its_parts_data() {
            return [{tag: "1.0", scale: 1.0}, {tag: "1.6", scale: 1.6}, {tag: "2.0", scale: 2.0}];
        }
        function test_plain_card_holds_its_parts(data) {
            root.width = 320;
            Ui.Theme.fontScale = data.scale;
            addSystem("Plain system");
            addSystem("Grouped system", {rrSid: 12, rrSiteId: 100});
            verify(waitForPolish(home));
            waitForRendering(home);
            for (var name of ["Plain system", "Grouped system"]) {
                var card = cardNamed(name);
                verify(!card.refreshHint);
                verify(!findChild(card, "savedSystemRefreshHint").visible);
                var labels = findChild(card, "savedSystemTitle").parent;
                if (data.scale === 1.0) {
                    fuzzyCompare(card.height, 122, 0.01, name + ": the height at normal text");
                    // Centred 10 px above the card's middle, where they always sat.
                    fuzzyCompare(labels.y, (122 - labels.height) / 2 - 10, 0.01, name + ": the labels' place at normal text");
                }
                var parts = [["labels", labels], ["Play", findChild(card, "savedSystemPlay")],
                    ["More", findChild(card, "savedSystemManageButton")], ["sites", findChild(card, "homeSiteChooserButton")]];
                for (var part of parts) {
                    if (!part[1].visible)
                        continue;
                    var r = rectIn(part[1], card);
                    verify(inside(r, card), name + ": " + part[0] + " " + describe(r) + " inside the card, " + card.height + " tall");
                }
                // At normal text the site group draws its button and the plain system none, as on main. At larger
                // text the group's button gives way where the title would fall under 96 px (issue #901 G4) and its
                // count leads the source line instead.
                var sites = findChild(card, "homeSiteChooserButton");
                var source = findChild(card, "savedSystemSource").text;
                if (data.scale === 1.0)
                    verify(sites.visible === (name === "Grouped system"), name + ": the site button at normal text");
                else if (name === "Grouped system")
                    verify(sites.visible || source.indexOf("1 site · ") === 0, name + ": neither the button nor the count: " + source);
                else
                    verify(!sites.visible && source.indexOf("site") < 0, name + ": a site count outside a group");
            }
        }

        // Issue #901 B1: a site group of @a count sites, one of which plays (the rest avoided) when @a playable.
        function addGroup(name, sid, count, playable) {
            var rows = [];
            for (var i = 0; i < count; ++i)
                rows.push({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5", rrSid: sid,
                    rrSiteId: sid * 1000 + i, siteName: "Site " + i});
            verify(savedSystems.addAll(rows, "") >= 0);
            wait(0);
            if (!playable)
                return;
            for (i = 0; i < savedSystems.count; ++i) {
                var row = savedSystems.get(i);
                if (row.rrSid === sid && row.rrSiteId !== sid * 1000)
                    verify(savedSystems.setAvoidSite(i, true));
            }
            wait(0);
        }
        function settleCards() {
            wait(0);
            verify(waitForPolish(root.Window.window));
            findChild(home, "listenSystemsSection").forceLayout();
            verify(waitForPolish(root.Window.window));
        }
        // Every part inside the card, the actions 48 px targets apart from each other and from the labels, and the
        // title at least 96 px wide (the site button's rule).
        function checkActions(card, label) {
            var labels = findChild(card, "savedSystemTitle").parent;
            var title = findChild(card, "savedSystemTitle");
            var labelsRect = rectIn(labels, card);
            verify(inside(labelsRect, card), label + ": labels " + describe(labelsRect) + " inside, " + card.height);
            var minimum = 96;
            verify(title.width >= minimum - 0.5, label + ": the title is " + title.width + " wide, under " + minimum);
            var actions = [];
            for (var name of ["homeSiteChooserButton", "savedSystemPlay", "savedSystemManageButton"]) {
                var control = findChild(card, name);
                if (control.visible && control.opacity > 0)
                    actions.push([name, rectIn(control, card)]);
            }
            for (var i = 0; i < actions.length; ++i) {
                var a = actions[i];
                verify(inside(a[1], card), label + ": " + a[0] + " " + describe(a[1]) + " inside, " + card.height);
                verify(a[1].w >= 47.5 && a[1].h >= 47.5, label + ": " + a[0] + " a 48 px target");
                verify(!overlaps(a[1], labelsRect), label + ": " + a[0] + " " + describe(a[1]) + " clear of the labels "
                    + describe(labelsRect));
                for (var j = i + 1; j < actions.length; ++j)
                    verify(!overlaps(a[1], actions[j][1]), label + ": " + a[0] + " " + describe(a[1]) + " and " + actions[j][0]
                        + " " + describe(actions[j][1]) + " apart");
            }
        }

        // B1 (issue #901): a site group with one site to play keeps the default card's 122 px at normal text and its
        // place for Play and More (More over Play at the right), at every width; its site button is drawn only while
        // the title keeps 96 px beside it, measured from natural widths, and otherwise the source
        // line leads with the count ("10 sites · ") and More offers Sites. The labels and the controls never meet, at
        // 1.0, 1.6 and 2.0.
        function test_site_button_gives_way_data() {
            var rows = [];
            for (var scale of [1.0, 1.6, 2.0]) {
                for (var w of [320, 360, 411]) {
                    for (var count of [1, 10, 100])
                        rows.push({tag: w + " at " + scale + ", " + count + (count === 1 ? " site" : " sites"), width: w,
                            scale: scale, count: count});
                }
            }
            return rows;
        }
        function test_site_button_gives_way(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            addGroup("Grouped county system", 21, data.count, true);
            settleCards();
            var card = cardNamed("Grouped county system");
            var sites = findChild(card, "homeSiteChooserButton");
            var play = findChild(card, "savedSystemPlay");
            var more = findChild(card, "savedSystemManageButton");
            var source = findChild(card, "savedSystemSource");
            verify(play.visible, "Play for the one playable site");
            checkActions(card, data.tag);
            // Play and More where they always were: More over Play at the right edge.
            var playRect = rectIn(play, card);
            var moreRect = rectIn(more, card);
            fuzzyCompare(playRect.x + playRect.w, card.width - 16, 0.5, "Play at the right edge");
            fuzzyCompare(moreRect.x + moreRect.w / 2, playRect.x + playRect.w / 2, 0.5, "More over Play");
            verify(moreRect.y + moreRect.h <= playRect.y + 0.5, "More above Play");
            // Drawn only where the title keeps its minimum beside the site button, Play and the gaps.
            var label = data.count === 1 ? "1 site" : data.count + " sites";
            var fits = card.width - 2 * 16 - (play.implicitWidth + 8 + sites.implicitWidth) - 12 >= 96;
            compare(sites.visible, fits, "the site button drawn only where it fits");
            // Pinned: one site's button at normal text on every phone width, as on main (a 110 px title at 320 dp);
            // left out for 10 sites at 320 dp and for one site at 2x text there.
            if (data.scale === 1.0 && data.count === 1)
                verify(sites.visible, "one site's button at " + data.width + " dp at normal text");
            if (data.width === 320 && (data.scale === 1.0 && data.count === 10 || data.scale === 2.0 && data.count === 1))
                verify(!sites.visible, "the button is drawn at " + data.tag);
            if (sites.visible) {
                compare(sites.text, label);
                verify(source.text.indexOf(label) < 0, "the count twice: " + source.text);
            } else {
                compare(source.text.indexOf(label + " · "), 0, "the count leads the source line: " + source.text);
            }
            if (data.scale === 1.0) {
                var labels = findChild(card, "savedSystemTitle").parent;
                fuzzyCompare(card.height, 122, 0.01, data.tag + ": the height at normal text");
                fuzzyCompare(labels.y, (122 - labels.height) / 2 - 10, 0.01, data.tag + ": the labels' place");
            }
        }

        // The full card's More offers Sites for a group whose site button is left out.
        function test_more_offers_sites_when_the_button_is_left_out() {
            root.width = 320;
            Ui.Theme.fontScale = 2.0;
            addGroup("Grouped county system", 23, 10, true);
            settleCards();
            var card = cardNamed("Grouped county system");
            verify(!findChild(card, "homeSiteChooserButton").visible);
            var more = findChild(card, "savedSystemManageButton");
            mouseClick(more, more.width / 2, more.height / 2);
            var menu = findChild(home, "savedSystemManageSheet");
            tryCompare(menu, "visible", true);
            verify(findChild(menu, "sitesSavedSystemButton") !== null, "More offers Sites");
            menu.visible = false;
        }

        // B4 (issue #901): a scan list's card is laid out as a saved system's: a bold title, its labels placed by the
        // same rule (10 px above the middle of the actions' box), at the same inset, and Play and More where a system
        // card has them, at every text size; as tall as what it holds, 122 px at normal text.
        function test_scan_list_card_matches_the_system_card_data() {
            return [{tag: "1.0", scale: 1.0}, {tag: "1.6", scale: 1.6}, {tag: "2.0", scale: 2.0}];
        }
        function textNamed(node, text) {
            for (var child of node.children) {
                if (child.text === text && child.font !== undefined)
                    return child;
                var found = textNamed(child, text);
                if (found)
                    return found;
            }
            return null;
        }
        function test_scan_list_card_matches_the_system_card(data) {
            root.width = 320;
            Ui.Theme.fontScale = data.scale;
            addSystem("Plain system");
            addList("Harbor loop");
            settleCards();
            var system = cardNamed("Plain system");
            var systemTitle = findChild(system, "savedSystemTitle");
            var systemPlay = rectIn(findChild(system, "savedSystemPlay"), system);
            var systemMore = rectIn(findChild(system, "savedSystemManageButton"), system);
            home.showSection("scanLists");
            settleCards();
            var list = findChild(findChild(home, "listenScanListsSection"), "scanListCard");
            var listTitle = textNamed(list, "Harbor loop");
            verify(listTitle !== null);
            compare(listTitle.font.weight, systemTitle.font.weight, "the title's weight");
            compare(listTitle.font.pixelSize, systemTitle.font.pixelSize, "the title's size");
            var listLabels = listTitle.parent;
            var listPlay = rectIn(findChild(list, "scanListPlay"), list);
            var listMore = rectIn(findChild(list, "scanListManageButton"), list);
            compare(describe(listPlay), describe(systemPlay), "Play where a system card has it");
            compare(describe(listMore), describe(systemMore), "More where a system card has it");
            var actionsBottom = Math.max(listPlay.y + listPlay.h, listMore.y + listMore.h);
            fuzzyCompare(rectIn(listLabels, list).x, rectIn(systemTitle.parent, system).x, 0.5, "the labels' inset");
            fuzzyCompare(listLabels.y, Math.max(16, (actionsBottom + 16 - listLabels.height) / 2 - 10), 0.5,
                "the labels 10 px above the middle of the actions' box");
            fuzzyCompare(list.height, Math.max(listLabels.y + listLabels.height, actionsBottom) + 16, 0.5, "as tall as it holds");
            if (data.scale === 1.0)
                fuzzyCompare(list.height, 122, 0.5, "122 px at normal text");
            verify(listLabels.x + listLabels.width <= listPlay.x - 11.5, "the labels clear of Play");
        }

        // B3: Tab down the body and round again: every control it stops on in the body is in full view once the
        // layout settles, the last one too, flush with the bottom although the content column starts below the
        // body's top (13 px under the Listen tabs, issue #646). On the Systems and on the Scan lists tab, each
        // with 12 cards, whose last Play ends the body's controls. In both tab placements: fixed at 411 x 700, and in
        // the body at 640 wide with larger text on a window just under twice the fixed region (the short-screen
        // rule), where the tabs scroll with the cards and are Tab stops of the body.
        function test_tab_stops_scroll_into_view_data() {
            var rows = [];
            for (var place of [{name: "", fixed: true, width: 411, height: 700, scale: 1.0},
                    {name: ", tabs in the body", fixed: false, width: 640, height: 0, scale: 1.6}]) {
                rows.push({tag: "desktop" + place.name, brokered: false, section: "systems", place: place},
                    {tag: "brokered header" + place.name, brokered: true, section: "systems", place: place},
                    {tag: "desktop scan lists" + place.name, brokered: false, section: "scanLists", place: place},
                    {tag: "brokered header scan lists" + place.name, brokered: true, section: "scanLists", place: place});
            }
            return rows;
        }
        function test_tab_stops_scroll_into_view(data) {
            if (data.brokered) {
                // The brokered capability is CONSTANT: set it before publishing the host (see tst_home_dongle_status).
                testContext.setDongleStatus(false, 0, "");
                testContext.useLifecycleHost(true);
            }
            root.width = data.place.width;
            root.height = data.place.height;
            Ui.Theme.fontScale = data.place.scale;
            for (var i = 0; i < 12; ++i) {
                if (data.section === "systems")
                    addSystem("System " + i);
                else
                    addList("List " + i);
            }
            home.showSection(data.section);
            verify(waitForPolish(home));
            if (!data.place.fixed) {
                root.height = Math.floor(2 * home.fixedExtent) - 2;
                verify(waitForPolish(home));
            }
            waitForRendering(home);
            compare(home.pinnedHeader, data.brokered);
            compare(home.tabsFixed, data.place.fixed, "the Listen tabs' placement");
            verify(body().contentItem.children[0].y > 0, "the content column starts below the body's top");
            findChild(home, data.section === "systems" ? "addSystemButton" : "addScanListButton").forceActiveFocus();
            var window = root.Window.window;
            var stops = 0;
            var scrolledDown = false;
            // The last card's Play, the lowest control of the section and so of the body.
            var plays = [];
            (function look(node) {
                for (var child of node.children) {
                    if (child.objectName === (data.section === "systems" ? "savedSystemPlay" : "scanListPlay"))
                        plays.push(child);
                    look(child);
                }
            })(findChild(home, data.section === "systems" ? "listenSystemsSection" : "listenScanListsSection"));
            compare(plays.length, 12);
            plays.sort(function (a, b) {
                return a.mapToItem(body().contentItem, 0, 0).y - b.mapToItem(body().contentItem, 0, 0).y;
            });
            var lastStop = plays[plays.length - 1];
            var reachedLast = false;
            var reachedTabs = false;
            for (var n = 0; n < 40; ++n) {
                keyClick(Qt.Key_Tab);
                var focus = window.activeFocusItem;
                if (!Ui.Navigation.contains(body().contentItem, focus))
                    continue;
                ++stops;
                var label = "Tab " + (n + 1) + " on " + (focus.objectName || focus.accessibleName || focus);
                tryVerify(function () {
                    var p = focus.mapToItem(body(), 0, 0);
                    return p.y >= -0.5 && p.y + focus.height <= body().height + 0.5;
                }, 2000, label + " is in full view");
                if (body().contentY > 0)
                    scrolledDown = true;
                if (focus.objectName.indexOf("listenTab_") === 0)
                    reachedTabs = true;
                if (focus === lastStop) {
                    reachedLast = true;
                    // The last card ends the column: its Play is revealed down to the body's bottom edge.
                    var bottom = focus.mapToItem(body(), 0, 0).y + focus.height;
                    fuzzyCompare(bottom, body().height, 0.5, label + ": the bottom control is shown to its end");
                }
            }
            // At least the action and each card's Play and More.
            verify(stops >= 25, "Tab stopped on the body's controls: " + stops);
            verify(scrolledDown, "the body scrolled down to follow the focus");
            verify(reachedLast, "Tab reached the last control");
            compare(reachedTabs, !data.place.fixed, "the Listen tabs are body stops only in the body");
        }

        // B3: Theme.revealFocus takes the place inside the Flickable's content, not inside the column it is handed,
        // so a column that starts below the top is scrolled far enough. Every caller's column starts at 0, where
        // the two are the same.
        function test_reveal_focus_counts_the_column_offset() {
            offsetScroll.contentY = 0;
            Ui.Theme.revealFocus(offsetScroll, offsetColumn, offsetRows.itemAt(4));
            fuzzyCompare(offsetScroll.contentY, 180, 0.5, "the last row ends at the bottom of the view");
            Ui.Theme.revealFocus(offsetScroll, offsetColumn, offsetRows.itemAt(0));
            fuzzyCompare(offsetScroll.contentY, 30, 0.5, "the first row starts at the top of the view");
            Ui.Theme.revealFocus(offsetScroll, offsetColumn, offsetRows.itemAt(1));
            fuzzyCompare(offsetScroll.contentY, 30, 0.5, "a row already in view does not move it");
        }
    }
}
