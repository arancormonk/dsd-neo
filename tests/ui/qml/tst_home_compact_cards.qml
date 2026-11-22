// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #901 (D7, D8, D11): Listen's cards in Compact view. A saved system's card is one row: the bold title over one
// supporting line (the source line, led by the distance in Nearest order and by the site count while the site button
// is not drawn), with the site button, Play and More on the title's row. The site button is drawn only while the title
// keeps its minimum width beside it; More offers Sites for every site group. The receiver line and the refresh hint
// stay; the place and the "Heard" line are not drawn, and the title's description reads them. A scan list's card is
// its name over its count with Play and More on the same row. The cards are the production savedSystemCards over the
// real saved systems, and `prefs` is the real AppPrefs. Names, places and addresses are made up.
Item {
    id: root
    width: 411
    height: 800

    Ui.HomeScreen {
        id: home
        anchors.fill: parent
    }
    SignalSpy { id: played; target: home; signalName: "playSystem" }
    SignalSpy { id: sitesChosen; target: home; signalName: "chooseSites" }
    SignalSpy { id: listPlayed; target: home; signalName: "playScanList" }

    TestCase {
        name: "HomeCompactCards"
        when: windowShown

        function init() {
            testContext.useAppPrefs(true);
            testContext.setLocationSupported(false);
            testContext.resetSystemsSort();
            testContext.resetSavedSystemCards();
            prefs.setLocationFix(0, 0, 0, 0);
            prefs.metricUnits = false;
            prefs.compactView = true;
            emptyModels();
            home.visible = true;
            home.showSection("systems");
            home.resetScrollMemory();
            root.width = 411;
            root.height = 800;
            Ui.Theme.resetFontScale();
            played.clear();
            sitesChosen.clear();
            listPlayed.clear();
            settle();
        }
        function cleanup() {
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            root.forceActiveFocus();
            savedSystemCards.screenReaderActive = false;
            prefs.compactView = false;
            home.visible = true;
            home.showSection("systems");
            emptyModels();
            testContext.resetSavedSystemCards();
            testContext.resetSystemsSort();
            prefs.setLocationFix(0, 0, 0, 0);
            testContext.setLocationSupported(false);
            testContext.useAppPrefs(false);
            root.width = 411;
            root.height = 800;
            Ui.Theme.resetFontScale();
            settle();
        }
        function emptyModels() {
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
        }
        // The cards' coalesced rebuild, then layout, the columns inside the cards too.
        function settle() {
            wait(0);
            verify(waitForPolish(root.Window.window));
            findChild(home, "listenSystemsSection").forceLayout();
            findChild(home, "listenScanListsSection").forceLayout();
            verify(waitForPolish(root.Window.window));
        }
        function add(name, fields) {
            verify(savedSystems.add(Object.assign({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "851.5"}, fields || {})), "could not add " + name);
        }
        // Site @a n of a RadioReference system @a sid, at @a lat, @a lon (none when undefined).
        function site(name, sid, n, lat, lon, fields) {
            var row = {rrSid: sid, rrSiteId: sid * 10 + n, siteName: "Site " + n};
            if (lat !== undefined)
                Object.assign(row, {siteLat: lat, siteLon: lon, hasSitePos: true});
            add(name, Object.assign(row, fields || {}));
        }
        function rowOfSite(sid, n) {
            for (var i = 0; i < savedSystems.count; ++i) {
                if (savedSystems.get(i).rrSiteId === sid * 10 + n)
                    return i;
            }
            fail("no site " + sid + "/" + n);
        }
        function entries() {
            var out = [];
            for (var child of findChild(home, "listenSystemsSection").children) {
                if (child.objectName === "savedSystemEntry" && child.visible && !child.held)
                    out.push(child);
            }
            out.sort(function (a, b) {
                return a.y - b.y;
            });
            return out;
        }
        function entryTitled(title) {
            for (var entry of entries()) {
                if (entry.title === title)
                    return entry;
            }
            fail("no card " + title);
        }
        function cardOf(title) {
            return findChild(entryTitled(title), "savedSystemCard");
        }
        function part(card, name) {
            var found = findChild(card, name);
            verify(found !== null, name);
            return found;
        }
        function tap(control) {
            verify(waitForPolish(home));
            mouseClick(control, control.width / 2, control.height / 2);
        }
        function rectIn(item, ancestor) {
            var p = item.mapToItem(ancestor, 0, 0);
            return {x: p.x, y: p.y, w: item.width, h: item.height};
        }
        function overlaps(a, b) {
            return a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 && b.y < a.y + a.h - 0.5;
        }
        function inside(r, card) {
            return r.x >= -0.5 && r.y >= -0.5 && r.x + r.w <= card.width + 0.5 && r.y + r.h <= card.height + 0.5;
        }
        function describe(r) {
            return "[" + Math.round(r.x) + "," + Math.round(r.y) + " " + Math.round(r.w) + "x" + Math.round(r.h) + "]";
        }
        // The title's minimum beside the site button (issue #901 G4): 96 px.
        function minimumTitle(card) {
            return 96;
        }
        // What a card draws: every visible, opaque part inside it, no two of its actions meeting, each action at least
        // 48 px square, the actions on one row beside the labels, and the title at least its minimum width.
        function checkCompactCard(card, label) {
            var labels = part(card, "savedSystemTitle").parent;
            var title = part(card, "savedSystemTitle");
            var actions = [];
            for (var name of ["homeSiteChooserButton", "savedSystemPlay", "savedSystemManageButton"]) {
                var control = part(card, name);
                if (control.visible && control.opacity > 0)
                    actions.push([name, control, rectIn(control, card)]);
            }
            var labelsRect = rectIn(labels, card);
            verify(inside(labelsRect, card), label + ": labels " + describe(labelsRect) + " inside, " + card.height);
            verify(title.width >= minimumTitle(card) - 0.5, label + ": the title is " + title.width + " wide, under "
                + minimumTitle(card));
            for (var i = 0; i < actions.length; ++i) {
                var a = actions[i];
                verify(inside(a[2], card), label + ": " + a[0] + " " + describe(a[2]) + " inside, " + card.height);
                verify(a[2].w >= 48 - 0.5 && a[2].h >= 48 - 0.5, label + ": " + a[0] + " is a 48 px target " + describe(a[2]));
                verify(!overlaps(a[2], labelsRect), label + ": " + a[0] + " clear of the labels");
                // One row: centred on the title's row, as the first action is.
                fuzzyCompare(a[2].y + a[2].h / 2, actions[0][2].y + actions[0][2].h / 2, 0.5, label + ": " + a[0] + " on the row");
                for (var j = i + 1; j < actions.length; ++j)
                    verify(!overlaps(a[2], actions[j][2]), label + ": " + a[0] + " " + describe(a[2]) + " and "
                        + actions[j][0] + " " + describe(actions[j][2]) + " apart");
            }
            verify(actions.length > 0 && actions[actions.length - 1][0] === "savedSystemManageButton", label + ": More");
            var hint = part(card, "savedSystemRefreshHint");
            if (hint.visible) {
                var h = rectIn(hint, card);
                verify(inside(h, card), label + ": the hint inside");
                for (var b of actions)
                    verify(!overlaps(b[2], h), label + ": " + b[0] + " clear of the hint");
                verify(!overlaps(labelsRect, h), label + ": labels clear of the hint");
            }
            verify(!part(card, "savedSystemPlace").visible, label + ": no place line");
            verify(!part(card, "savedSystemMeta").visible, label + ": no Heard line");
        }

        // Compact cards at 320, 360 and 411 dp at text 1.0, 1.6 and 2.0: a plain card, a one-site group (site button and
        // Play), a group of two (its site button plays), every part inside, the 48 px targets apart and the title at its
        // minimum (96 px) at least. A one-site card's site button is drawn only where it leaves the title that minimum:
        // at 411 dp at every size, not at 320 or 360 dp; elsewhere the supporting line leads with "1 site". Play stays.
        function test_compact_cards_fit_data() {
            var rows = [];
            for (var w of [320, 360, 411]) {
                for (var s of [1.0, 1.6, 2.0])
                    rows.push({tag: w + " at " + s, width: w, scale: s});
            }
            return rows;
        }
        function test_compact_cards_fit(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            add("Plain Vale");
            site("Single Vale", 31, 1);
            site("Twin Vale", 32, 1);
            site("Twin Vale", 32, 2);
            settle();
            for (var title of ["Plain Vale", "Single Vale", "Twin Vale"])
                checkCompactCard(cardOf(title), data.tag + " " + title);
            var single = cardOf("Single Vale");
            var sites = part(single, "homeSiteChooserButton");
            verify(part(single, "savedSystemPlay").visible, "the one-site card keeps Play");
            // Drawn only where the title keeps 96 px beside the site button, Play, More and the gaps: at 411 dp at
            // every size; not at 360 dp at normal text (the title would get 94 px) or at 320 dp.
            var drawn = single.width - 32 - (sites.implicitWidth + 8 + part(single, "savedSystemPlay").implicitWidth + 8
                + part(single, "savedSystemManageButton").implicitWidth) - 12 >= 96;
            compare(sites.visible, drawn, "the site button drawn only where it fits");
            if (data.width === 411)
                verify(drawn, "one site's button at 411 dp, " + data.tag);
            if (data.width === 360 && data.scale === 1.0)
                verify(!drawn, "the button at 360 dp at normal text in compact");
            var source = part(single, "savedSystemSource").text;
            compare(source.indexOf("1 site · ") === 0, !drawn, "the count leads the line when the button is not drawn: " + source);
            if (data.scale === 1.0)
                fuzzyCompare(cardOf("Plain Vale").height, 68, 0.5, "a compact card at normal text");
        }

        // Compact one-site card at 320 and 360 dp: one row without its site button, and More → Sites opens the chooser
        // for it, as its long press does.
        function test_one_site_card_sites_from_more_data() {
            return [{tag: "320", width: 320}, {tag: "360", width: 360}];
        }
        function test_one_site_card_sites_from_more(data) {
            root.width = data.width;
            site("Single Vale", 31, 1);
            settle();
            var card = cardOf("Single Vale");
            verify(!part(card, "homeSiteChooserButton").visible);
            tap(part(card, "savedSystemManageButton"));
            var menu = findChild(home, "savedSystemManageSheet");
            tryCompare(menu, "visible", true);
            verify(waitForRendering(menu));
            var sites = findChild(menu, "sitesSavedSystemButton");
            verify(sites !== null, "More offers Sites");
            tap(sites);
            tryCompare(menu, "visible", false);
            compare(sitesChosen.count, 1);
            compare(sitesChosen.signalArguments[0][0], rowOfSite(31, 1));
        }

        // The full card's More offers Sites for a group of one site too (it offered it only for two or more), as compact
        // does where it leaves the site button out; a system outside a group has none.
        function test_default_one_site_card_offers_sites() {
            prefs.compactView = false;
            site("Single Vale", 31, 1);
            add("Plain Vale");
            settle();
            var menu = findChild(home, "savedSystemManageSheet");
            tap(part(cardOf("Single Vale"), "savedSystemManageButton"));
            tryCompare(menu, "visible", true);
            verify(waitForRendering(menu));
            var sites = findChild(menu, "sitesSavedSystemButton");
            verify(sites !== null, "More offers Sites for one site");
            tap(sites);
            tryCompare(menu, "visible", false);
            compare(sitesChosen.count, 1);
            compare(sitesChosen.signalArguments[0][0], rowOfSite(31, 1));
            tap(part(cardOf("Plain Vale"), "savedSystemManageButton"));
            tryCompare(menu, "visible", true);
            compare(findChild(menu, "sitesSavedSystemButton"), null, "no Sites outside a group");
            menu.visible = false;
        }

        // Play and the site button do what they say, through the controls themselves (411 dp, where both are drawn):
        // Play starts the one site, the site button opens the chooser.
        function test_play_and_chooser_through_their_controls() {
            site("Single Vale", 31, 1);
            settle();
            var card = cardOf("Single Vale");
            var sites = part(card, "homeSiteChooserButton");
            verify(sites.visible && sites.opacity === 1);
            tap(part(card, "savedSystemPlay"));
            compare(played.count, 1);
            compare(played.signalArguments[0][0], rowOfSite(31, 1));
            tap(sites);
            compare(sitesChosen.count, 1);
            compare(sitesChosen.signalArguments[0][0], rowOfSite(31, 1));
        }

        // A group of two whose first site is avoided: one site plays, which is not the card's lead, and Play starts that
        // one. Compact at 320 dp, as a one-site card: no site button, the count first, More → Sites.
        function test_one_playable_site_plays_its_own_uid() {
            root.width = 320;
            site("Pair Vale", 33, 1);
            site("Pair Vale", 33, 2);
            settle();
            verify(savedSystems.setAvoidSite(rowOfSite(33, 1), true));
            settle();
            var entry = entryTitled("Pair Vale");
            var card = cardOf("Pair Vale");
            verify(entry.playUid.length > 0 && entry.playUid !== entry.leadUid, "the playable site is not the lead");
            compare(entry.playUid, savedSystems.get(rowOfSite(33, 2)).uid);
            checkCompactCard(card, "one playable site");
            verify(!part(card, "homeSiteChooserButton").visible);
            compare(part(card, "savedSystemSource").text.indexOf("2 sites · "), 0);
            tap(part(card, "savedSystemPlay"));
            compare(played.count, 1);
            compare(played.signalArguments[0][0], rowOfSite(33, 2), "Play starts the site that plays");
            tap(part(card, "savedSystemManageButton"));
            var menu = findChild(home, "savedSystemManageSheet");
            tryCompare(menu, "visible", true);
            verify(findChild(menu, "sitesSavedSystemButton") !== null, "More offers Sites");
            menu.visible = false;
        }

        // Every site avoided: no Play; the site button (which fits beside More at 320 dp at normal text) is how it
        // plays, and the card still draws within itself.
        function test_all_sites_avoided() {
            root.width = 320;
            site("Hush Vale", 34, 1);
            site("Hush Vale", 34, 2);
            settle();
            verify(savedSystems.setAvoidSite(rowOfSite(34, 1), true));
            verify(savedSystems.setAvoidSite(rowOfSite(34, 2), true));
            settle();
            var card = cardOf("Hush Vale");
            verify(!part(card, "savedSystemPlay").visible, "nothing to play alone");
            verify(part(card, "homeSiteChooserButton").visible, "the site button fits beside More");
            checkCompactCard(card, "all avoided");
            Ui.Theme.fontScale = 2.0;
            settle();
            checkCompactCard(card, "all avoided at 2.0");
            verify(!part(card, "homeSiteChooserButton").visible);
            compare(part(card, "savedSystemSource").text.indexOf("2 sites · "), 0);
        }

        // One system on two receivers: compact keeps each card's receiver line, which wraps, and the card grows by it.
        // Two TCP cards of one title differ by their addresses on their faces.
        function test_receivers_tell_cards_apart_data() {
            return [{tag: "320 at 1.0 compact", scale: 1.0, compact: true}, {tag: "320 at 2.0 compact", scale: 2.0, compact: true},
                    {tag: "320 at 1.0 full", scale: 1.0, compact: false}, {tag: "320 at 2.0 full", scale: 2.0, compact: false}];
        }
        function test_receivers_tell_cards_apart(data) {
            prefs.compactView = data.compact;
            root.width = 320;
            Ui.Theme.fontScale = data.scale;
            site("Twin Mesa", 35, 1, undefined, undefined, {sourceType: "usb", freqMhz: "851.0125"});
            site("Twin Mesa", 35, 1, undefined, undefined, {host: "10.20.30.40", port: 61234, freqMhz: "852.0125"});
            // G1: endpoints and files that differ only past where the source line is cut at large text.
            add("Wire Mesa", {sourceType: "tcp", host: "10.0.0.1", port: 7355});
            add("Wire Mesa", {sourceType: "tcp", host: "10.0.0.2", port: 7355});
            add("Port Mesa", {sourceType: "udp", port: 7355});
            add("Port Mesa", {sourceType: "udp", port: 7356});
            add("Tape Mesa", {sourceType: "file", filePath: "/captures/north-county-dispatch-recording-01.wav"});
            add("Tape Mesa", {sourceType: "file", filePath: "/captures/north-county-dispatch-recording-02.wav"});
            add("Plain Mesa");
            settle();
            var plain = cardOf("Plain Mesa");
            for (var entry of entries()) {
                var card = findChild(entry, "savedSystemCard");
                if (data.compact)
                    checkCompactCard(card, entry.title + " " + entry.y);
                if (entry.title !== "Plain Mesa") {
                    var line = part(card, "savedSystemReceiver");
                    verify(line.visible && line.opacity === 1, entry.title + ": the receiver line stays");
                    verify(!line.truncated && line.contentWidth <= line.width + 0.5, entry.title + ": never cut");
                    // Compact grows by it; the full card holds it inside its 122 px at normal text, or grows.
                    if (data.compact)
                        verify(card.height >= plain.height + line.height - 0.5, entry.title + ": the card grows by it");
                    else
                        verify(line.mapToItem(card, 0, line.height).y <= card.height + 0.5, entry.title + ": the line inside the card");
                }
            }
            // What each card draws: its source line as cut to its width, and its receiver line, which wraps whole.
            var drawn = Qt.createQmlObject("import QtQuick; TextMetrics { elide: Text.ElideRight }", root);
            var faces = function (title) {
                return entries().filter(function (e) {
                    return e.title === title;
                }).map(function (e) {
                    var card = findChild(e, "savedSystemCard");
                    var receiver = part(card, "savedSystemReceiver");
                    var source = part(card, "savedSystemSource");
                    drawn.font = source.font;
                    drawn.text = source.text;
                    drawn.elideWidth = source.width;
                    return drawn.elidedText + "|" + (receiver.visible && receiver.opacity > 0 ? receiver.text : "");
                });
            };
            try {
                for (var title of ["Twin Mesa", "Wire Mesa", "Port Mesa", "Tape Mesa"]) {
                    var shown = faces(title);
                    compare(shown.length, 2);
                    verify(shown[0] !== shown[1], title + " cards differ on their faces: " + JSON.stringify(shown));
                }
                // The address is on the receiver line once, not again on the source line.
                for (var e of entries()) {
                    if (e.title !== "Wire Mesa")
                        continue;
                    var c = findChild(e, "savedSystemCard");
                    var receiverText = part(c, "savedSystemReceiver").text;
                    verify(receiverText.indexOf("TCP 10.0.0.") === 0, "the TCP card's receiver: " + receiverText);
                    verify(part(c, "savedSystemSource").text.indexOf("10.0.0.") < 0, "the address twice");
                }
            } finally {
                drawn.destroy();
            }
        }

        // Issue #901 r2 A: two cards of one title must look different, whatever single fact tells them apart. At
        // 320 dp and text 2.0 in Nearest order two systems of one name on one USB dongle at one place, on 151 and
        // 851 MHz, both drew "6.9 mi ·…" over "USB"; two replay files of one name in different folders both drew
        // "Auto" over "dispatch.wav". A card whose title another shown card shares names what sets it apart on its
        // receiver line, which wraps and is never cut: the receiver, endpoint or file (the shortest end of its path
        // that no other card's path shares), the frequency where theirs differ, and the decode mode where theirs
        // differ. Each pair below differs in one fact only; the two cards' faces as drawn (each line cut as it is
        // drawn, the receiver line whole) differ, in both views, in Order added and Nearest order.
        function sharedTitlePairs() {
            var at = {siteLat: 10.1, siteLon: 20.0, hasSitePos: true};
            return [
                {title: "Ridge Dispatch Freq", a: {sourceType: "usb", freqMhz: "151.0000"}, b: {sourceType: "usb", freqMhz: "851.0000"}},
                {title: "Ridge Dispatch Receiver", a: {sourceType: "usb"}, b: {sourceType: "airspy", airspy: {serial: "a1b2c3d4"}}},
                {title: "Ridge Dispatch Endpoint", a: {sourceType: "rtltcp", host: "10.0.0.1"}, b: {sourceType: "rtltcp", host: "10.0.0.2"}},
                {title: "Ridge Dispatch Audio", a: {sourceType: "tcp", host: "10.0.0.1", port: 7355}, b: {sourceType: "tcp", host: "10.0.0.2", port: 7355}},
                {title: "Ridge Dispatch Folder", a: {sourceType: "file", filePath: "/east/dispatch.wav"}, b: {sourceType: "file", filePath: "/west/dispatch.wav"}},
                {title: "Ridge Dispatch Mode", a: {sourceType: "usb", decodeFlag: "-fs"}, b: {sourceType: "usb", decodeFlag: "-fi"}}
            ].map(function (pair) {
                pair.a = Object.assign({}, at, pair.a);
                pair.b = Object.assign({}, at, pair.b);
                return pair;
            });
        }
        function test_shared_titles_look_different_data() {
            var rows = [];
            for (var sort of ["added", "nearest"]) {
                for (var compact of [true, false]) {
                    for (var scale of [1.0, 2.0])
                        rows.push({tag: sort + (compact ? " compact" : " full") + " at " + scale, sort: sort, compact: compact, scale: scale});
                }
            }
            return rows;
        }
        function test_shared_titles_look_different(data) {
            testContext.setLocationSupported(true);
            prefs.compactView = data.compact;
            root.width = 320;
            Ui.Theme.fontScale = data.scale;
            var pairs = sharedTitlePairs();
            for (var pair of pairs) {
                add(pair.title, pair.a);
                add(pair.title, pair.b);
            }
            add("Ridge Dispatch Alone", {sourceType: "usb"});
            prefs.systemsSort = data.sort;
            if (data.sort === "nearest")
                prefs.setLocationFix(10.0, 20.0, savedSystems.realtimeNowMs(), 25);
            settle();
            compare(savedSystemCards.effectiveSort, data.sort);
            // A line as drawn: cut at its width where it elides, whole where it wraps; nothing where it is not shown.
            var drawn = Qt.createQmlObject("import QtQuick; TextMetrics { elide: Text.ElideRight }", root);
            var face = function (entry) {
                var card = findChild(entry, "savedSystemCard");
                var lines = [];
                for (var name of ["savedSystemTitle", "savedSystemSource", "savedSystemPlace", "savedSystemMeta"]) {
                    var line = part(card, name);
                    if (!line.visible || line.opacity === 0 || line.height === 0) {
                        lines.push("");
                        continue;
                    }
                    drawn.font = line.font;
                    drawn.text = line.text;
                    drawn.elideWidth = line.width;
                    lines.push(drawn.elidedText);
                }
                var receiver = part(card, "savedSystemReceiver");
                lines.push(receiver.visible && receiver.opacity > 0 ? receiver.text : "");
                return lines.join(" | ");
            };
            try {
                for (var p of pairs) {
                    var shown = entries().filter(function (e) {
                        return e.title === p.title;
                    });
                    compare(shown.length, 2, p.title);
                    var faces = shown.map(face);
                    verify(faces[0] !== faces[1], p.title + " cards look the same: " + JSON.stringify(faces));
                    for (var e of shown) {
                        var receiver = part(findChild(e, "savedSystemCard"), "savedSystemReceiver");
                        verify(receiver.visible && receiver.opacity === 1 && !receiver.truncated, p.title + ": the receiver line, whole");
                        verify(receiver.contentWidth <= receiver.width + 0.5, p.title + ": the receiver line fits its width");
                    }
                }
                // A title no other card shares draws no receiver line.
                var alone = part(cardOf("Ridge Dispatch Alone"), "savedSystemReceiver");
                verify(!(alone.visible && alone.opacity > 0 && alone.height > 0), "a lone title draws a receiver line");
            } finally {
                drawn.destroy();
                prefs.systemsSort = "added";
            }
        }

        // Nearest: the supporting line leads with the distance when it is known; with no location fix, or no
        // coordinates for the system, it is the source line alone. With the site count on the line too (320 dp),
        // the order is distance, count, source.
        function test_nearest_line() {
            testContext.setLocationSupported(true);
            site("Near Vale", 36, 1, 10.1, 20.0);
            add("Far Plain");
            prefs.systemsSort = "nearest";
            settle();
            var near = function () {
                return part(cardOf("Near Vale"), "savedSystemSource").text;
            };
            verify(near().indexOf(" mi · ") < 0, "no fix, no distance: " + near());
            prefs.setLocationFix(10.0, 20.0, savedSystems.realtimeNowMs(), 25);
            settle();
            compare(savedSystemCards.effectiveSort, "nearest");
            compare(near().indexOf("6.9 mi · "), 0, "the distance first: " + near());
            var plain = part(cardOf("Far Plain"), "savedSystemSource").text;
            compare(plain.indexOf("851.5 MHz"), 0, "no coordinates, no distance: " + plain);
            // At 320 dp the one-site card's button is not drawn and its count joins the line: the distance still
            // leads (Nearest sorts by it), then the count, then the source.
            root.width = 320;
            settle();
            verify(!part(cardOf("Near Vale"), "homeSiteChooserButton").visible, "the site button is drawn at 320 dp");
            compare(near().indexOf("6.9 mi · 1 site · 851.5 MHz"), 0, "distance, then the count, then the source: " + near());
            prefs.systemsSort = "name";
            settle();
            verify(near().indexOf(" mi · ") < 0, "off Nearest, no distance");
            compare(near().indexOf("1 site · "), 0, "off Nearest, the count leads: " + near());
        }

        // The refresh hint stays, full width under the row, inside the card.
        function test_refresh_hint_data() {
            return [{tag: "320 at 1.0", width: 320, scale: 1.0}, {tag: "320 at 2.0", width: 320, scale: 2.0}];
        }
        function test_refresh_hint(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            var path = testContext.writeFixtureCsv("compact-provenance.csv", "Decimal,Mode,Alpha Tag\n123,A,Dispatch\n");
            var result = importedFiles.importGeneratedFile(path, "Compact provenance.csv", "group",
                {origin: "radioreference", rrSid: 12, rrSiteIds: "16863", rrKind: "group"});
            verify(result.ok);
            try {
                add("Hinted Vale", {trunking: true, groupCsvPath: result.path});
                settle();
                var card = cardOf("Hinted Vale");
                verify(card.refreshHint);
                var hint = part(card, "savedSystemRefreshHint");
                verify(hint.visible);
                compare(hint.height, hint.implicitHeight, "every line of the hint is shown");
                checkCompactCard(card, data.tag);
                fuzzyCompare(card.height, hint.y + hint.height + 12, 0.5, "12 px under the hint");
            } finally {
                importedFiles.remove(importedFiles.rowForPath(result.path));
            }
        }

        // D8: what compact does not draw is read on the title's description: the place and the "Heard" line's words,
        // from their own text. The full card reads them on their own lines, and its title has no description.
        function test_title_reads_what_is_not_drawn() {
            add("Placed Vale", {keyConfigured: true, place: {states: [{stid: 90, name: "Westmark"}], counties: [],
                candidateStates: [90]}});
            settle();
            var card = cardOf("Placed Vale");
            var title = part(card, "savedSystemTitle");
            var description = title.Accessible.description;
            verify(description.indexOf("Westmark") >= 0, "the place: " + description);
            verify(description.indexOf("Not yet listened") >= 0, "when last heard: " + description);
            compare(part(card, "savedSystemPlace").Accessible.name, "", "the place line is not read itself");
            prefs.compactView = false;
            settle();
            compare(title.Accessible.description, "", "the full card's title has no description");
            compare(part(card, "savedSystemPlace").Accessible.name, "Westmark");
            verify(part(card, "savedSystemMeta").Accessible.name.indexOf("Not yet listened") >= 0);
        }

        // With a screen reader the rows are positional: the lines compact does not draw are held (visible, transparent,
        // unnamed, no height), never hidden, and come back in place when the full view returns.
        function test_screen_reader_holds_the_undrawn_lines() {
            root.width = 320;
            add("Placed Vale", {place: {states: [{stid: 90, name: "Westmark"}], counties: [], candidateStates: [90]}});
            site("Single Vale", 31, 1);
            settle();
            savedSystemCards.screenReaderActive = true;
            settle();
            var placed = cardOf("Placed Vale");
            var single = cardOf("Single Vale");
            for (var pair of [[placed, "savedSystemPlace"], [placed, "savedSystemMeta"], [single, "homeSiteChooserButton"]]) {
                var held = part(pair[0], pair[1]);
                verify(held.visible, pair[1] + ": held, not hidden");
                compare(held.opacity, 0, pair[1] + ": transparent");
                compare(held.Accessible.name, "", pair[1] + ": unnamed");
                verify(held.Accessible.ignored, pair[1] + ": out of the tree");
                if (pair[1] !== "homeSiteChooserButton")
                    compare(held.parent.height, 0, pair[1] + ": no height");
            }
            verify(!part(single, "homeSiteChooserButton").enabled, "the held site button takes no press");
            fuzzyCompare(placed.height, 68, 0.5, "held lines take no room");
            prefs.compactView = false;
            settle();
            compare(part(placed, "savedSystemPlace").opacity, 1);
            compare(part(placed, "savedSystemPlace").Accessible.name, "Westmark");
            verify(part(single, "homeSiteChooserButton").enabled);
            fuzzyCompare(placed.height, 122, 0.5);
        }

        // A card is as tall with a screen reader as without, compact or full: the lines it holds take no room,
        // spacing included (a Column skips a slot of no height, its spacing with it), at any width and text size,
        // and the receiver line that stays is the same height either way.
        function test_screen_reader_keeps_the_card_height_data() {
            var rows = [];
            for (var compact of [true, false]) {
                for (var w of [320, 411]) {
                    for (var s of [1.0, 2.0])
                        rows.push({tag: (compact ? "compact " : "full ") + w + " at " + s, width: w, scale: s, compact: compact});
                }
            }
            return rows;
        }
        function test_screen_reader_keeps_the_card_height(data) {
            prefs.compactView = data.compact;
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            add("Plain Vale");
            add("Placed Vale", {keyConfigured: true, place: {states: [{stid: 90, name: "Westmark"}], counties: [],
                candidateStates: [90]}});
            site("Single Vale", 31, 1);
            site("Twin Mesa", 35, 1, undefined, undefined, {sourceType: "usb", freqMhz: "851.0125"});
            site("Twin Mesa", 35, 1, undefined, undefined, {host: "10.20.30.40", port: 61234, freqMhz: "852.0125"});
            settle();
            var heights = function () {
                var out = [];
                for (var entry of entries())
                    out.push(entry.title + " " + findChild(entry, "savedSystemCard").height);
                return out.sort();
            };
            var without = heights();
            compare(without.length, 5);
            savedSystemCards.screenReaderActive = true;
            settle();
            var withReader = heights();
            compare(withReader, without, "a screen reader changed the compact cards' heights");
            // The held slots are there, of no height, and what is drawn sits where it did.
            var plain = cardOf("Plain Vale");
            var held = data.compact ? [part(cardOf("Placed Vale"), "savedSystemPlace"), part(plain, "savedSystemMeta")]
                : [part(plain, "savedSystemPlace"), part(plain, "savedSystemReceiver")];
            for (var line of held) {
                verify(line.visible, line.objectName + " is held, not hidden");
                compare(line.parent.height, 0, line.objectName + " takes no room");
            }
        }

        // A scan list's compact card: its name over its count, Play and More on the name's row, inside the card at
        // every width and text size, 68 px at normal text. Play keeps its gating (a draft does not play) and the count
        // follows the list.
        function test_compact_scan_list_card_data() {
            return test_compact_cards_fit_data();
        }
        function test_compact_scan_list_card(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            verify(scanLists.add({name: "Harbor Loop", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]}));
            home.showSection("scanLists");
            settle();
            var card = findChild(findChild(home, "listenScanListsSection"), "scanListCard");
            verify(card !== null && card.visible);
            verify(card.compact);
            var name = part(card, "scanListTitle");
            var count = part(card, "scanListCount");
            compare(name.font.weight, Font.Bold);
            compare(count.text, "1 entry");
            verify(count.y > name.y, "the count under the name");
            var labels = rectIn(name.parent, card);
            var play = rectIn(part(card, "scanListPlay"), card);
            var more = rectIn(part(card, "scanListManageButton"), card);
            for (var r of [["labels", labels], ["Play", play], ["More", more]])
                verify(inside(r[1], card), data.tag + ": " + r[0] + " " + describe(r[1]) + " inside, " + card.height);
            verify(!overlaps(play, more) && !overlaps(labels, play) && !overlaps(labels, more), data.tag + ": apart");
            fuzzyCompare(play.y + play.h / 2, more.y + more.h / 2, 0.5, "one row");
            verify(name.width >= minimumTitle(card) - 0.5, "the name at its minimum at least");
            if (data.scale === 1.0)
                fuzzyCompare(card.height, 68, 0.5);
            tap(part(card, "scanListPlay"));
            compare(listPlayed.count, 1);
            verify(scanLists.update(0, {entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5",
                enabled: true}, {kind: "freq", name: "Repeater", protocol: "p25", freqMhz: "852.5", enabled: true}]}));
            tryCompare(count, "text", "2 entries");
        }

        // G6: a compact card's title and supporting line are one line each, cut with an ellipsis, so a long or
        // multi-line name (an imported one) and a long draft line never grow the card; the full words stay in what a
        // screen reader reads. Only the receiver line and the refresh hint may wrap.
        function test_compact_heads_stay_two_lines_data() {
            return [{tag: "320 at 1.0", scale: 1.0}, {tag: "320 at 2.0", scale: 2.0}];
        }
        function test_compact_heads_stay_two_lines(data) {
            root.width = 320;
            Ui.Theme.fontScale = data.scale;
            var longName = "Northern county public safety\ninteroperability network\nwith a very long imported name";
            add("Plain Vale");
            add(longName);
            verify(scanLists.add({name: "Loop", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]}));
            verify(scanLists.add({name: "Harbor and river and coastal\nwatch draft list", sourceType: "usb", isDraft: true}));
            settle();
            var plain = cardOf("Plain Vale");
            var long = null;
            for (var entry of entries()) {
                if (entry.title !== "Plain Vale")
                    long = findChild(entry, "savedSystemCard");
            }
            verify(long !== null);
            var title = part(long, "savedSystemTitle");
            compare(title.lineCount, 1, "the long name on one line");
            verify(title.truncated, "the long name is cut");
            fuzzyCompare(long.height, plain.height, 0.5, "a long name grows the card");
            for (var word of ["Northern", "interoperability", "imported name"])
                verify(title.Accessible.name.indexOf(word) >= 0, "the title says " + word + ": " + title.Accessible.name);
            compare(part(long, "savedSystemSource").lineCount, 1);
            if (data.scale === 1.0)
                fuzzyCompare(long.height, 68, 0.5);
            home.showSection("scanLists");
            settle();
            tryCompare(home, "restorePending", false);
            var cards = [];
            for (var child of findChild(home, "listenScanListsSection").children) {
                if (child.objectName === "scanListCard" && child.visible)
                    cards.push(child);
            }
            compare(cards.length, 2);
            var loop = cards[0].listName === "Loop" ? cards[0] : cards[1];
            var draft = loop === cards[0] ? cards[1] : cards[0];
            var count = part(draft, "scanListCount");
            compare(count.text, "Not ready · draft");
            compare(count.lineCount, 1, "the draft line on one line");
            compare(part(draft, "scanListTitle").lineCount, 1, "the draft's name on one line");
            fuzzyCompare(draft.height, loop.height, 0.5, "the draft line grows the card");
            verify(count.Accessible.name.indexOf("Not ready") >= 0 && count.Accessible.name.indexOf("draft") >= 0,
                   "the draft line is said whole: " + count.Accessible.name);
            verify(part(draft, "scanListTitle").Accessible.name.indexOf("draft list") >= 0, "the name is said whole");
        }

        // G2: with a screen reader on, a resize that draws or leaves out a card's site button (no rebuild, no tab or
        // sheet change) owes Android's copy of the tree one refresh, and it is given: exactly one per change, the
        // owed flag cleared.
        function test_site_button_change_refreshes_the_tree() {
            root.width = 320;
            site("Single Vale", 31, 1);
            add("Plain Vale");
            settle();
            savedSystemCards.screenReaderActive = true;
            settle();
            wait(50);
            var rebuilt = Qt.createQmlObject("import QtTest; SignalSpy { signalName: \"rebuilt\" }", root);
            rebuilt.target = savedSystemCards;
            var toggles = 0;
            var counted = null;
            var onToggle = function () {
                ++toggles;
            };
            var watch = function () {
                if (home.systemsTreeRefresh !== null && counted !== home.systemsTreeRefresh) {
                    counted = home.systemsTreeRefresh;
                    counted.visibleChanged.connect(onToggle);
                }
            };
            try {
                // The refresh object exists once a refresh has been given; make sure of one first.
                home.systemsTreeOwed = true;
                home.refreshSystemsTree();
                watch();
                verify(home.systemsTreeRefresh !== null);
                compare(home.systemsTreeOwed, false);
                var single = cardOf("Single Vale");
                verify(!part(single, "homeSiteChooserButton").opacity, "the button is held at 320 dp");
                for (var step of [{width: 411, drawn: true}, {width: 320, drawn: false}]) {
                    toggles = 0;
                    root.width = step.width;
                    settle();
                    wait(50);
                    compare(part(cardOf("Single Vale"), "homeSiteChooserButton").opacity === 1, step.drawn,
                            step.width + " dp: the site button");
                    compare(toggles, 1, step.width + " dp: tree refreshes");
                    compare(home.systemsTreeOwed, false, step.width + " dp: still owed");
                }
                compare(rebuilt.count, 0, "the cards were rebuilt");
            } finally {
                if (counted !== null)
                    counted.visibleChanged.disconnect(onToggle);
                rebuilt.destroy();
            }
        }

        // D11: a change of Compact view puts every Listen tab back at its top (Systems, Scan lists and Explore), the
        // one shown at once and the others when next shown, with Home shown or hidden.
        function test_density_change_resets_listen_places_data() {
            return [{tag: "shown", hidden: false}, {tag: "hidden", hidden: true}];
        }
        function test_density_change_resets_listen_places(data) {
            root.height = 700;
            for (var i = 0; i < 20; ++i)
                add("Vale " + String(i).padStart(2, "0"));
            for (i = 0; i < 12; ++i)
                verify(scanLists.add({name: "Loop " + i, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                    entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]}));
            settle();
            var body = findChild(home, "dongleScrollBody");
            var scrollTo = function (y) {
                body.contentY = Math.min(y, body.originY + body.contentHeight - body.height);
                verify(body.contentY > body.originY + 100, "scrolled down");
            };
            // Explore's place, left part way down; a short window at large text, so it scrolls.
            root.height = 360;
            Ui.Theme.fontScale = 2.0;
            home.showSection("explore");
            settle();
            tryCompare(home, "restorePending", false);
            var exploreRoom = body.contentHeight - body.height;
            verify(exploreRoom > 40, "Explore scrolls at this height: " + exploreRoom);
            body.contentY = body.originY + Math.round(exploreRoom / 2);
            var explorePlace = body.contentY;
            home.showSection("scanLists");
            root.height = 700;
            Ui.Theme.fontScale = 1.0;
            settle();
            tryCompare(home, "restorePending", false);
            scrollTo(600);
            home.showSection("systems");
            settle();
            tryCompare(home, "restorePending", false);
            scrollTo(900);
            if (data.hidden)
                home.visible = false;
            prefs.compactView = false;
            wait(0);
            settle();
            if (data.hidden) {
                home.visible = true;
                settle();
            }
            tryVerify(function () {
                return Math.abs(body.contentY - body.originY) < 0.5;
            }, 1000, "the Systems tab at its top: " + body.contentY);
            home.showSection("scanLists");
            tryCompare(home, "restorePending", false);
            settle();
            fuzzyCompare(body.contentY, body.originY, 0.5, "the Scan lists tab at its top");
            // Explore, left part way down before the change, opens at its top too.
            root.height = 360;
            Ui.Theme.fontScale = 2.0;
            home.showSection("explore");
            settle();
            tryCompare(home, "restorePending", false);
            settle();
            verify(explorePlace > body.originY + 20, "Explore was left part way down");
            fuzzyCompare(body.contentY, body.originY, 0.5, "Explore at its top");
        }

        // Issue #901 r2 E3: the reset is owed from the change, not from the timer that applies it. A tab chosen in the
        // same turn as the change saved the old tab's place back (Systems left at 900 reopened at 900): the tab
        // left while the reset is owed keeps no place, so every tab opens at its top.
        function test_tab_chosen_before_the_reset_keeps_no_old_place() {
            root.height = 700;
            for (var i = 0; i < 20; ++i)
                add("Vale " + String(i).padStart(2, "0"));
            settle();
            var body = findChild(home, "dongleScrollBody");
            body.contentY = Math.min(900, body.originY + body.contentHeight - body.height);
            verify(body.contentY > body.originY + 100, "Systems scrolled down");
            prefs.compactView = false;
            verify(home.listenResetPending, "the reset is owed at once");
            home.showSection("explore");
            settle();
            tryCompare(home, "restorePending", false);
            verify(!home.listenResetPending, "the reset is applied");
            fuzzyCompare(body.contentY, body.originY, 0.5, "Explore at its top");
            home.showSection("systems");
            settle();
            tryCompare(home, "restorePending", false);
            settle();
            fuzzyCompare(body.contentY, body.originY, 0.5, "Systems at its top, not back at its old place");
        }

        // D11 with Explore shown: a change of Compact view puts it at its top at once.
        function test_density_change_resets_explore_shown() {
            root.height = 360;
            Ui.Theme.fontScale = 2.0;
            home.showSection("explore");
            settle();
            tryCompare(home, "restorePending", false);
            var body = findChild(home, "dongleScrollBody");
            var room = body.contentHeight - body.height;
            verify(room > 40, "Explore scrolls at this height: " + room);
            body.contentY = body.originY + Math.round(room / 2);
            prefs.compactView = !prefs.compactView;
            tryVerify(function () {
                return Math.abs(body.contentY - body.originY) < 0.5;
            }, 1000, "Explore at its top: " + body.contentY);
        }

        // The card stride, top to top, at 411 x 914 and normal text, in both views (logged for the PR).
        function test_card_stride() {
            root.height = 914;
            for (var i = 0; i < 4; ++i)
                add("Vale " + i);
            var strides = {};
            for (var compact of [false, true]) {
                prefs.compactView = compact;
                settle();
                var shown = entries();
                var stride = shown[1].y - shown[0].y;
                strides[compact ? "compact" : "default"] = stride;
            }
            console.log("Listen card stride at 411x914 @1.0: default " + strides["default"] + " px, compact "
                + strides.compact + " px");
            fuzzyCompare(strides["default"], 122 + Ui.Theme.gap, 0.5);
            fuzzyCompare(strides.compact, 68 + Ui.Theme.gap, 0.5);
        }
    }
}
