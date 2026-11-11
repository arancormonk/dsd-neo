// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Listen → Systems: search and sort the saved systems. The cards are the production savedSystemCards over the real
// saved systems, and `prefs` is the real AppPrefs (the stored sort, the location fix), with the decoderHost map's
// location support and requests; `rrPlaces` is the recorded fake.
Item {
    id: root
    width: 411
    height: 800

    Ui.HomeScreen {
        id: home
        anchors.fill: parent
    }
    Component {
        id: secondHome
        Ui.HomeScreen {
            width: 411
            height: 800
        }
    }
    SignalSpy { id: played; target: home; signalName: "playSystem" }
    SignalSpy { id: accountOpened; target: home; signalName: "openRadioReferenceAccount" }

    TestCase {
        name: "HomeSystemsSearchSort"
        when: windowShown

        function init() {
            testContext.useAppPrefs(true);
            testContext.setLocationSupported(false);
            testContext.resetSystemsSort();
            testContext.resetSavedSystemCards();
            testContext.resetRrPlaces();
            testContext.resetHostPlayback();
            testContext.resetPins();
            prefs.setLocationFix(0, 0, 0, 0);
            prefs.metricUnits = false;
            emptySystems();
            home.visible = true;
            home.showSection("systems");
            home.resetScrollMemory();
            home.announcements = 0;
            home.announced = "";
            home.announcedQuery = "";
            root.width = 411;
            Ui.Theme.resetFontScale();
            played.clear();
            accountOpened.clear();
            settle();
        }
        function cleanup() {
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            root.forceActiveFocus();
            home.cancelNearestLocation();
            home.visible = true;
            home.showSection("systems");
            emptySystems();
            testContext.resetSavedSystemCards();
            testContext.resetSystemsSort();
            testContext.resetRrPlaces();
            testContext.resetPins();
            prefs.setLocationFix(0, 0, 0, 0);
            testContext.setLocationSupported(false);
            testContext.useAppPrefs(false);
            root.width = 411;
            Ui.Theme.resetFontScale();
            settle();
        }

        function emptySystems() {
            while (savedSystems.count)
                savedSystems.remove(0);
        }
        // The cards' coalesced rebuild, then layout.
        function settle() {
            wait(0);
            item("listenSystemsSection").forceLayout();
            verify(waitForPolish(home));
        }
        function item(name, under) {
            var found = findChild(under || home, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function add(name, fields) {
            verify(savedSystems.add(Object.assign({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "851.5"}, fields || {})), "could not add " + name);
        }
        function iowa(county, ctid) {
            return {states: [{stid: 19, name: "Iowa"}], counties: [{ctid: ctid, stid: 19, name: county}],
                candidateStates: [19]};
        }
        // A RadioReference site of its own system, at @a lat, @a lon (none when undefined).
        function site(name, sid, lat, lon, fields) {
            var row = {rrSid: sid, rrSiteId: sid * 10, siteName: "Site " + sid};
            if (lat !== undefined)
                Object.assign(row, {siteLat: lat, siteLon: lon, hasSitePos: true});
            add(name, Object.assign(row, fields || {}));
        }
        // Each shown card's entry, in screen order.
        function entries() {
            var out = [];
            for (var child of item("listenSystemsSection").children) {
                if (child.objectName === "savedSystemEntry" && child.visible)
                    out.push(child);
            }
            out.sort(function (a, b) {
                return a.y - b.y;
            });
            return out;
        }
        function titles() {
            return entries().map(function (entry) {
                return entry.title;
            });
        }
        function entryTitled(title) {
            for (var entry of entries()) {
                if (entry.title === title)
                    return entry;
            }
            fail("no card " + title);
        }
        function sectionsShown() {
            var out = [];
            for (var entry of entries()) {
                var label = findChild(entry, "savedSystemSection");
                if (label.visible)
                    out.push(label.text + ":" + entry.title);
            }
            return out;
        }
        function field() {
            return item("systemsSearch");
        }
        // Types @a text into the search field and waits for it to land (250 ms debounce).
        function search(text) {
            field().text = text;
            tryCompare(savedSystemCards, "query", text, 2000);
            settle();
        }
        function tap(control) {
            verify(waitForPolish(home));
            for (var at = control; at; at = at.parent)
                verify(waitForPolish(at));
            mouseClick(control, control.width / 2, control.height / 2);
        }
        // Chooses @a label in the Sort sheet, as a finger does.
        function chooseSort(label) {
            tap(item("systemsSortPill"));
            var sheet = item("systemsSortSheet");
            tryCompare(sheet, "visible", true);
            verify(waitForRendering(sheet));
            var choice = null;
            for (var i = 0; i < sheet.choices.length; ++i) {
                if (sheet.choices[i] === label)
                    choice = item("systemsSortChoice" + i, sheet);
            }
            verify(choice !== null, label + " is offered");
            tap(choice);
            tryCompare(sheet, "visible", false);
            settle();
        }
        function hostCallsNamed(name) {
            return testContext.hostCalls().filter(function (call) {
                return call.name === name;
            });
        }
        function addThree() {
            add("Johnson County", {freqMhz: "851.0125", decodeFlag: "-ft"});
            add("Linn Fire", {freqMhz: "460.000", decodeFlag: "-fs"});
            add("Story Sheriff", {freqMhz: "155.775"});
            settle();
        }

        // Every term must match: a name word, a frequency, a protocol label.
        function test_search_filters_and_terms() {
            addThree();
            compare(titles(), ["Johnson County", "Linn Fire", "Story Sheriff"]);
            verify(!item("systemsSearchCount").visible, "no count line without a search");
            compare(field().input.Accessible.name, "Search saved systems");
            verify(field().input.Accessible.searchEdit, "read as a search field");
            search("county 851");
            compare(titles(), ["Johnson County"]);
            verify(item("systemsSearchCount").visible);
            compare(item("systemsSearchCountText").text, "1 of 3 systems");
            search("851");
            compare(titles(), ["Johnson County"], "a bare number finds a frequency");
            search("460000 kHz");
            compare(titles(), ["Linn Fire"]);
            search("dmr");
            compare(titles(), ["Linn Fire"], "the protocol label");
            search("sheriff 851");
            compare(titles(), [], "both terms must match");
        }

        // A search that lands is read out once, with its count; one landing on another tab, under a sheet or with
        // Home hidden is not, and switching back reads nothing either.
        function test_search_announced_once() {
            addThree();
            search("fire");
            compare(home.announcements, 1);
            compare(home.announced, "1 of 3 systems");
            wait(300);
            compare(home.announcements, 1, "read once");

            field().text = "story";
            home.showSection("scanLists");
            compare(savedSystemCards.query, "story", "a tab switch lands what was typed");
            wait(300);
            home.showSection("systems");
            wait(300);
            compare(home.announcements, 1, "not read on another tab, nor on coming back");

            field().text = "linn";
            item("systemsSortSheet").open("Sort systems", ["Name"], null);
            tryCompare(savedSystemCards, "query", "linn", 2000);
            wait(50);
            compare(home.announcements, 1, "not read under a sheet");
            item("systemsSortSheet").visible = false;

            field().text = "johnson";
            home.visible = false;
            compare(savedSystemCards.query, "johnson", "hiding lands it");
            wait(300);
            home.visible = true;
            compare(home.announcements, 1, "not read while hidden");
        }

        // No match: a card says so and offers Clear search, which brings every card back and says how many.
        function test_no_match_and_clear() {
            addThree();
            search("zzz");
            compare(titles(), []);
            var card = item("systemsNoMatch");
            verify(card.visible);
            compare(item("systemsNoMatchText").text, "No saved systems match “zzz”");
            verify(!item("systemsSearchCount").visible, "no 0 of 3 line beside the card");
            compare(home.announced, "No saved systems match “zzz”");
            tap(item("systemsNoMatchClear"));
            compare(field().text, "");
            compare(savedSystemCards.query, "");
            settle();
            verify(!card.visible);
            compare(titles().length, 3);
            compare(home.announced, "3 of 3 systems");
            // The field's own ×.
            search("linn");
            var clear = item("systemsSearchClear");
            verify(clear.visible);
            compare(clear.accessibleName, "Clear search");
            tap(clear);
            compare(savedSystemCards.query, "");
            verify(!clear.visible);
        }

        // Each order, chosen in the sheet, stored on the device and shown on the pill.
        function test_each_order() {
            add("Charlie", {place: iowa("Linn", 2), lastHeard: 100});
            add("alpha", {place: iowa("Johnson", 1)});
            add("Bravo", {lastHeard: 300, place: {states: [{stid: 19, name: "Iowa"}, {stid: 17, name: "Illinois"}],
                counties: [], candidateStates: [19, 17]}});
            add("Delta");
            settle();
            var pill = item("systemsSortPill");
            compare(pill.text, "Sort: Order added");
            compare(pill.accessibleName, "Sort systems, Order added");
            compare(titles(), ["Charlie", "alpha", "Bravo", "Delta"]);

            chooseSort("Name");
            compare(prefs.systemsSort, "name");
            compare(pill.text, "Sort: Name");
            compare(titles(), ["alpha", "Bravo", "Charlie", "Delta"]);
            compare(sectionsShown(), [], "no section labels in name order");

            chooseSort("Last listened");
            compare(prefs.systemsSort, "recent");
            compare(titles(), ["Bravo", "Charlie", "alpha", "Delta"]);

            chooseSort("Place");
            compare(prefs.systemsSort, "place");
            compare(titles(), ["alpha", "Charlie", "Bravo", "Delta"]);
            compare(sectionsShown(), ["Iowa:alpha", "Several states:Bravo", "No place:Delta"]);
            compare(findChild(entryTitled("alpha"), "savedSystemPlace").text, "Johnson, Iowa");

            chooseSort("Order added");
            compare(prefs.systemsSort, "added");
            compare(titles(), ["Charlie", "alpha", "Bravo", "Delta"]);
        }

        // The stored order survives a new Home (and the app's next start: it is the real AppPrefs).
        function test_order_persists() {
            add("Bravo");
            add("Alpha");
            settle();
            chooseSort("Name");
            var other = createTemporaryObject(secondHome, root);
            verify(other !== null);
            compare(findChild(other, "systemsSortPill").text, "Sort: Name");
            testContext.resetSystemsSort();
            compare(findChild(other, "systemsSortPill").text, "Sort: Order added");
        }

        // Where the device takes no location fix, Nearest is not offered, and a stored Nearest shows (and sorts) as
        // Name while the stored choice stays.
        function test_desktop_has_no_nearest() {
            add("Bravo");
            add("Alpha");
            settle();
            tap(item("systemsSortPill"));
            var sheet = item("systemsSortSheet");
            tryCompare(sheet, "visible", true);
            compare(sheet.choices, ["Name", "Place", "Last listened", "Order added"]);
            verify(item("systemsSortChoice3", sheet).checked, "the order in force is checked");
            verify(!item("systemsSortChoice0", sheet).checked);
            sheet.visible = false;
            prefs.systemsSort = "nearest";
            settle();
            compare(item("systemsSortPill").text, "Sort: Name");
            compare(titles(), ["Alpha", "Bravo"]);
            verify(!item("systemsLocation").visible);
            compare(prefs.systemsSort, "nearest", "the stored choice stays");
        }

        // Nearest without a fix lists names and says so; Use my location asks once, and the fix orders the cards by
        // distance with each one's distance shown.
        function test_nearest_asks_for_location() {
            testContext.setLocationSupported(true);
            site("Far", 1, 43.6, -93.6);
            site("Near", 2, 41.7, -93.6);
            site("Unplaced", 3);
            settle();
            chooseSort("Nearest");
            compare(prefs.systemsSort, "nearest");
            compare(item("systemsSortPill").text, "Sort: Nearest");
            var line = item("systemsLocation");
            verify(line.visible);
            compare(item("systemsLocationText").text, "Nearest needs your location");
            compare(line.detail, "Showing names until a location arrives");
            compare(titles(), ["Far", "Near", "Unplaced"]);
            compare(hostCallsNamed("requestSiteLocation").length, 0, "never asked for on its own");

            var button = item("systemsLocationButton");
            compare(button.text, "Use my location");
            tap(button);
            var requests = hostCallsNamed("requestSiteLocation");
            compare(requests.length, 1);
            var id = requests[0].id;
            compare(home.locationId, id);
            verify(!button.enabled, "one request at a time");
            decoderHost.locationResult(id, true, 41.6, -93.6, 25, savedSystems.realtimeNowMs(), false, "", "", "");
            settle();
            compare(home.locationId, 0);
            verify(prefs.lastFixAt > 0);
            compare(titles(), ["Near", "Far", "Unplaced"]);
            compare(sectionsShown(), ["No coordinates:Unplaced"]);
            compare(findChild(entryTitled("Near"), "savedSystemPlace").text, "6.9 mi");
            compare(item("systemsLocationText").text, "Using your location from a moment ago");
            compare(button.text, "Update location");
            verify(button.enabled);
        }

        // Update location asks again; a failure says so and keeps the last fix; a newer request elsewhere and Home
        // hiding each give the request up; a late answer to a retired request changes nothing.
        function test_update_location() {
            testContext.setLocationSupported(true);
            site("Far", 1, 43.6, -93.6);
            site("Near", 2, 41.7, -93.6);
            settle();
            prefs.systemsSort = "nearest";
            prefs.setLocationFix(43.6, -93.6, savedSystems.realtimeNowMs(), 25);
            settle();
            compare(titles(), ["Far", "Near"]);
            var button = item("systemsLocationButton");
            compare(button.text, "Update location");
            tap(button);
            var first = home.locationId;
            verify(first > 0);
            decoderHost.locationResult(first, false, 0, 0, 0, 0, false, "", "", "denied");
            compare(home.locationId, 0);
            compare(item("systemsLocationText").text, "Location unavailable. Distances still use your saved location. Try again.");
            compare(titles(), ["Far", "Near"]);

            tap(button);
            var second = home.locationId;
            verify(second > first);
            radioReference.nextLocationRequestId(); // the site chooser asks
            compare(home.locationId, 0, "superseded");
            compare(hostCallsNamed("cancelLocationRequest").pop().id, second);
            decoderHost.locationResult(second, true, 41.6, -93.6, 25, savedSystems.realtimeNowMs(), false, "", "", "");
            settle();
            compare(titles(), ["Far", "Near"], "a retired request's answer is not taken");

            tap(button);
            var third = home.locationId;
            home.visible = false;
            compare(home.locationId, 0, "hiding gives it up");
            compare(hostCallsNamed("cancelLocationRequest").pop().id, third);
            home.visible = true;

            tap(button);
            decoderHost.locationResult(home.locationId, true, 41.6, -93.6, 25, savedSystems.realtimeNowMs(), false, "", "", "");
            settle();
            compare(titles(), ["Near", "Far"]);
        }

        // The place notice under Place and Nearest, its Update, the progress and the summary; signed out, the account
        // screen opens instead. The card menu offers the update for a RadioReference card only.
        function test_place_notice_and_update() {
            site("County", 1);
            add("Manual");
            settle();
            testContext.setRrPlacesUnplaced(1);
            verify(!item("systemsPlaceNotice").visible, "not in Order added");
            chooseSort("Place");
            var notice = item("systemsPlaceNotice");
            verify(notice.visible);
            compare(item("systemsPlaceNoticeText").text, "1 saved system has no place yet");
            testContext.setRrPlacesUnplaced(12);
            compare(item("systemsPlaceNoticeText").text, "12 saved systems have no place yet");

            testContext.setRrPlacesSignedIn(false);
            tap(item("systemsPlaceUpdate"));
            compare(accountOpened.count, 1, "signed out: the account screen");
            testContext.setRrPlacesSignedIn(true);

            tap(item("systemsPlaceUpdate"));
            compare(testContext.rrPlacesUpdateUids().length, 2, "every saved system");
            verify(!notice.visible, "the notice gives way to the progress");
            verify(item("systemsRrStatus").visible);
            compare(item("systemsRrProgress").text, "Updating 1 of 2");
            tap(item("systemsRrCancel"));
            compare(testContext.rrPlacesCancels(), 1);

            tap(item("systemsPlaceUpdate"));
            testContext.finishRrPlacesUpdate(2, 1);
            var summary = item("systemsRrSummary");
            verify(summary.visible);
            compare(summary.text, "Updated 2 systems. 1 could not be reached.");
            tryCompare(home, "announced", "Updated 2 systems. 1 could not be reached.");
            home.showSection("explore");
            home.showSection("systems");
            verify(!item("systemsRrStatus").visible, "the summary goes once Systems is left");

            // The card menu.
            var menu = item("savedSystemManageSheet");
            home.manageCard(entryTitled("County").leadUid, "County", entryTitled("County").rrLinked);
            tryCompare(menu, "visible", true);
            var update = item("updateSavedSystemButton", menu);
            compare(update.text, "Update from RadioReference");
            tap(update);
            tryCompare(menu, "visible", false);
            compare(testContext.rrPlacesUpdateUids(), [savedSystems.get(0).uid]);
            testContext.finishRrPlacesUpdate(1, 0);
            home.manageCard(entryTitled("Manual").leadUid, "Manual", entryTitled("Manual").rrLinked);
            tryCompare(menu, "visible", true);
            compare(findChild(menu, "updateSavedSystemButton"), null, "not for a system RadioReference knows nothing of");
            menu.visible = false;
        }

        // Without a screen reader a sort moves the cards' delegates: the same object stands for a card after it.
        function test_delegate_survives_sort() {
            add("Charlie");
            add("Alpha");
            add("Bravo");
            settle();
            var charlie = entryTitled("Charlie");
            chooseSort("Name");
            compare(titles(), ["Alpha", "Bravo", "Charlie"]);
            verify(entryTitled("Charlie") === charlie, "the card kept its delegate");
            search("charlie");
            verify(entryTitled("Charlie") === charlie, "and through a search");
        }

        // With a screen reader the sort is a reset (Android hears of a move only through a hide and show), and the
        // keyboard stays on the pill.
        function test_screen_reader_sort_resets_focus_on_pill() {
            add("Charlie");
            add("Alpha");
            settle();
            savedSystemCards.screenReaderActive = true;
            var charlie = entryTitled("Charlie");
            var pill = item("systemsSortPill");
            pill.forceActiveFocus();
            keyClick(Qt.Key_Space);
            var sheet = item("systemsSortSheet");
            tryCompare(sheet, "visible", true);
            tap(item("systemsSortChoice0", sheet)); // Name
            tryCompare(sheet, "visible", false);
            settle();
            compare(titles(), ["Alpha", "Charlie"]);
            verify(entryTitled("Charlie") !== charlie, "rebuilt, not moved");
            tryCompare(pill, "activeFocus", true);
            savedSystemCards.screenReaderActive = false;
        }

        // A card filtered out with the keyboard on it hands the keyboard to the search field.
        function test_focus_falls_back_to_search() {
            addThree();
            var play = findChild(entryTitled("Story Sheriff"), "savedSystemPlay");
            play.forceActiveFocus();
            verify(play.activeFocus);
            savedSystemCards.query = "linn";
            settle();
            tryVerify(function () {
                return field().input.activeFocus;
            }, 1000, "the search field has the keyboard");
        }

        // Removing the last saved system while searching: the keyboard lands on Add a system, the controls go, and no
        // 0 of 0 line stays.
        function test_last_system_removed_focus() {
            add("Only");
            add("Other");
            settle();
            search("only");
            compare(titles(), ["Only"]);
            savedSystems.remove(savedSystems.rowForUid(savedSystems.get(1).uid));
            settle();
            var more = findChild(entryTitled("Only"), "savedSystemManageButton");
            more.forceActiveFocus();
            keyClick(Qt.Key_Space);
            var menu = item("savedSystemManageSheet");
            tryCompare(menu, "visible", true);
            tap(item("removeSavedSystemButton", menu));
            var confirm = item("removeSystemConfirm");
            tryCompare(confirm, "visible", true);
            tap(item("confirmActionButton", confirm));
            tryCompare(savedSystems, "count", 0);
            settle();
            tryVerify(function () {
                return item("addSystemButton").activeFocus;
            }, 1000, "Add a system has the keyboard");
            verify(!item("systemsControls").visible);
            verify(!item("systemsSearchCount").visible);
            verify(!item("systemsNoMatch").visible);
            compare(savedSystemCards.query, "");
            compare(field().text, "");
        }

        // The field and the pill share a row while the pill takes at most 40% of it and the field keeps 180 px;
        // otherwise the pill goes under the field, left-aligned.
        function test_controls_stack_data() {
            return [{tag: "1000 x 1.0 beside Recent activity", width: 1000, scale: 1.0, sort: "added", stacked: false},
                {tag: "411 x 1.0, Name", width: 411, scale: 1.0, sort: "name", stacked: false},
                {tag: "411 x 1.0, Order added", width: 411, scale: 1.0, sort: "added", stacked: true},
                {tag: "320 x 1.0", width: 320, scale: 1.0, sort: "name", stacked: true},
                {tag: "320 x 1.6", width: 320, scale: 1.6, sort: "name", stacked: true},
                {tag: "320 x 2.0", width: 320, scale: 2.0, sort: "added", stacked: true},
                {tag: "1000 x 2.0", width: 1000, scale: 2.0, sort: "added", stacked: false}];
        }
        function test_controls_stack(data) {
            add("Alpha");
            prefs.systemsSort = data.sort;
            settle();
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            settle();
            var controls = item("systemsControls");
            var pill = item("systemsSortPill");
            var input = field();
            compare(controls.stacked, data.stacked);
            if (data.stacked) {
                compare(pill.x, 0);
                verify(pill.y >= input.y + input.height, "the pill under the field");
                compare(input.width, controls.width);
            } else {
                verify(pill.implicitWidth <= 0.4 * controls.width);
                verify(input.width >= 180);
                verify(input.x + input.width <= pill.x, "side by side");
            }
            verify(pill.x + pill.width <= controls.width + 0.5, "the pill inside the column");
            verify(pill.height >= 48 && input.height >= 48, "touch size");
            verify(controls.height >= pill.y + pill.height - 0.5, "the row holds the pill");
        }

        // Tab order: the field, its ×, the pill, then the cards.
        function test_tab_order() {
            add("Alpha");
            settle();
            search("alpha");
            var input = field().input;
            input.forceActiveFocus();
            keyClick(Qt.Key_Tab);
            verify(item("systemsSearchClear").activeFocus, "the ×");
            keyClick(Qt.Key_Tab);
            verify(item("systemsSortPill").activeFocus, "the pill");
            keyClick(Qt.Key_Tab);
            verify(item("systemsClearSearch").activeFocus, "the count line's Clear search");
            keyClick(Qt.Key_Tab);
            var card = entryTitled("Alpha");
            verify(findChild(card, "savedSystemManageButton").activeFocus || findChild(card, "savedSystemPlay").activeFocus,
                "then the card");
        }

        // A card's title is the group's, the name its Listen pin shows.
        function test_pin_label_is_card_title() {
            add("County — North", {rrSid: 7, rrSiteId: 70, siteName: "North"});
            add("County — South", {rrSid: 7, rrSiteId: 71, siteName: "South"});
            settle();
            compare(titles(), ["County"]);
            compare(listenPins.pin("system", savedSystems.get(0).uid), "");
            compare(listenPins.entries[0].label, entryTitled("County").title);
            compare(findChild(entryTitled("County"), "savedSystemTitle").text, "County");
        }
    }
}
