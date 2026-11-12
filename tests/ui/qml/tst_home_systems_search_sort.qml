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
    // Opens the Sort sheet as a card leaves (once, after Home has noted it), for the focus hand-on case.
    Connections {
        id: sheetOnRemoval
        property var sheet: null
        enabled: false
        function onItemRemoved() {
            enabled = false;
            sheet.open("Sort systems", ["Name"], null);
        }
    }
    // Return and Enter a field let through to the screen around it (a11y2 E).
    property int enterPassedOn: 0
    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
            ++enterPassedOn;
    }
    Keys.onReleased: function (event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
            ++enterPassedOn;
    }
    SignalSpy { id: played; target: home; signalName: "playSystem" }
    SignalSpy { id: accountOpened; target: home; signalName: "openRadioReferenceAccount" }
    SignalSpy { id: sitesChosen; target: home; signalName: "chooseSites" }

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
            controls().announcements = 0;
            controls().announced = "";
            controls().announcedQuery = "";
            root.width = 411;
            Ui.Theme.resetFontScale();
            played.clear();
            accountOpened.clear();
            sitesChosen.clear();
            settle();
        }
        function cleanup() {
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            root.forceActiveFocus();
            controls().cancelNearestLocation();
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
        // Each shown card's entry, in screen order (a row held past a shorter list with a screen reader is none).
        function entries() {
            var out = [];
            for (var child of item("listenSystemsSection").children) {
                if (child.objectName === "savedSystemEntry" && child.visible && !child.held)
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
        function controls() {
            return item("savedSystemsControls");
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
            compare(controls().announcements, 1);
            compare(controls().announced, "1 of 3 systems");
            wait(300);
            compare(controls().announcements, 1, "read once");

            field().text = "story";
            home.showSection("scanLists");
            compare(savedSystemCards.query, "story", "a tab switch lands what was typed");
            wait(300);
            home.showSection("systems");
            wait(300);
            compare(controls().announcements, 1, "not read on another tab, nor on coming back");

            field().text = "linn";
            item("systemsSortSheet").open("Sort systems", ["Name"], null);
            tryCompare(savedSystemCards, "query", "linn", 2000);
            wait(50);
            compare(controls().announcements, 1, "not read under a sheet");
            item("systemsSortSheet").visible = false;

            field().text = "johnson";
            home.visible = false;
            compare(savedSystemCards.query, "johnson", "hiding lands it");
            wait(300);
            home.visible = true;
            compare(controls().announcements, 1, "not read while hidden");
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
            compare(controls().announced, "No saved systems match “zzz”");
            tap(item("systemsNoMatchClear"));
            compare(field().text, "");
            compare(savedSystemCards.query, "");
            settle();
            verify(!card.visible);
            compare(titles().length, 3);
            compare(controls().announced, "3 of 3 systems");
            // The field's own ×.
            search("linn");
            var clear = item("systemsSearchClear");
            verify(clear.visible);
            compare(clear.accessibleName, "Clear search text");
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

        // The order is the device's AppPrefs value, not Home's own: a second Home reads the stored choice, and a change
        // to the stored value reaches it (UI_QT_PERSISTENCE covers the value across an AppPrefs reload).
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
            compare(item("systemsLocationText").text, "Finding your location…");
            compare(line.detail, "Showing names until a location arrives", "still names while it is found");
            decoderHost.locationResult(controls().locationId, false, 0, 0, 0, 0, false, "", "", "denied");
            compare(item("systemsLocationText").text, "Location unavailable. Check location access in your device settings, then try again.");
            compare(line.detail, "Showing names until a location arrives", "and after it failed");
            tap(button);
            var requests = hostCallsNamed("requestSiteLocation");
            compare(requests.length, 2);
            var id = requests[1].id;
            compare(controls().locationId, id);
            verify(!button.enabled, "one request at a time");
            decoderHost.locationResult(id, true, 41.6, -93.6, 25, savedSystems.realtimeNowMs(), false, "", "", "");
            settle();
            compare(controls().locationId, 0);
            verify(prefs.lastFixAt > 0);
            compare(titles(), ["Near", "Far", "Unplaced"]);
            compare(sectionsShown(), ["No coordinates:Unplaced"]);
            compare(findChild(entryTitled("Near"), "savedSystemPlace").text, "6.9 mi");
            compare(item("systemsLocationText").text, "Using your location from a moment ago");
            compare(line.detail, "", "names no longer");
            compare(button.text, "Update location");
            verify(button.enabled);
        }

        // A fix that lapses while the phone sleeps is dropped at the next read, with no expiry timer. When the cards'
        // rebuild is that read, the line follows them to names; when the minute tick is, it never shows the age of
        // a fix that is gone.
        function test_lapsed_fix_leaves_the_location_line() {
            testContext.setLocationSupported(true);
            site("Far", 1, 43.6, -93.6);
            site("Near", 2, 41.7, -93.6);
            prefs.systemsSort = "nearest";
            prefs.setLocationFix(41.6, -93.6, savedSystems.realtimeNowMs(), 25);
            settle();
            var line = item("systemsLocation");
            compare(titles(), ["Near", "Far"]);
            compare(item("systemsLocationText").text, "Using your location from a moment ago");

            testContext.lapseLocationFix();
            search("a");
            compare(titles(), ["Far", "Near"], "the cards fall back to names");
            compare(item("systemsLocationText").text, "Nearest needs your location");
            compare(line.detail, "Showing names until a location arrives");
            compare(item("systemsLocationButton").text, "Use my location");

            search("");
            prefs.setLocationFix(41.6, -93.6, savedSystems.realtimeNowMs(), 25);
            settle();
            compare(titles(), ["Near", "Far"]);
            compare(item("systemsLocationText").text, "Using your location from a moment ago");
            testContext.lapseLocationFix();
            home.heardTick++;
            compare(item("systemsLocationText").text, "Nearest needs your location", "no age for a fix that is gone");
            tryCompare(savedSystemCards, "effectiveSort", "name", 2000, "the queued change reaches the cards");
            settle();
            compare(item("systemsLocationText").text, "Nearest needs your location");
            compare(line.detail, "Showing names until a location arrives");
            compare(titles(), ["Far", "Near"]);
        }

        // Issue #646 B1: hidden, the location line keeps its height (its second line counts by its words, not by
        // whether it is drawn), so the body's range does not move.
        function test_location_line_height_held_while_hidden() {
            testContext.setLocationSupported(true);
            site("Far", 1, 43.6, -93.6);
            prefs.systemsSort = "nearest";
            settle();
            var line = item("systemsLocation");
            var height = line.height;
            verify(height > 0);
            home.visible = false;
            settle();
            compare(line.height, height, "a hidden line keeps its height");
            home.visible = true;
            settle();
            compare(line.height, height);
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
            var first = controls().locationId;
            verify(first > 0);
            decoderHost.locationResult(first, false, 0, 0, 0, 0, false, "", "", "denied");
            compare(controls().locationId, 0);
            compare(item("systemsLocationText").text, "Location unavailable. Distances still use your saved location. Try again.");
            compare(titles(), ["Far", "Near"]);

            tap(button);
            var second = controls().locationId;
            verify(second > first);
            radioReference.nextLocationRequestId(); // the site chooser asks
            compare(controls().locationId, 0, "superseded");
            compare(hostCallsNamed("cancelLocationRequest").pop().id, second);
            decoderHost.locationResult(second, true, 41.6, -93.6, 25, savedSystems.realtimeNowMs(), false, "", "", "");
            settle();
            compare(titles(), ["Far", "Near"], "a retired request's answer is not taken");

            tap(button);
            var third = controls().locationId;
            home.visible = false;
            compare(controls().locationId, 0, "hiding gives it up");
            compare(hostCallsNamed("cancelLocationRequest").pop().id, third);
            home.visible = true;

            tap(button);
            decoderHost.locationResult(controls().locationId, true, 41.6, -93.6, 25, savedSystems.realtimeNowMs(), false, "", "", "");
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

            // R23: the systems the notice counts, not every saved one.
            testContext.setRrPlacesUnplacedUids([savedSystems.get(0).uid]);
            tap(item("systemsPlaceUpdate"));
            compare(testContext.rrPlacesUpdateUids(), [savedSystems.get(0).uid], "only the systems it counts");
            verify(!notice.visible, "the notice gives way to the progress");
            verify(item("systemsRrStatus").visible);
            compare(item("systemsRrProgress").text, "Updating 1 of 1");
            tap(item("systemsRrCancel"));
            compare(testContext.rrPlacesCancels(), 1);

            tap(item("systemsPlaceUpdate"));
            testContext.finishRrPlacesUpdate(2, 1);
            var summary = item("systemsRrSummary");
            verify(summary.visible);
            compare(summary.text, "Updated 2 systems. 1 could not be reached.");
            tryCompare(controls(), "announced", "Updated 2 systems. 1 could not be reached.");
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

        // One update at a time. While one runs, a card menu's Update from RadioReference is disabled and says why
        // (the progress line under Systems says how far), the place notice gives way to that progress, and a call
        // from either asks the service nothing: no second card is taken as updated, and nobody is sent to the account
        // screen. When the update ends, the open menu's Update enables in place and runs.
        function test_update_refused_while_updating() {
            site("County", 1);
            site("Other", 2);
            settle();
            var county = entryTitled("County");
            var other = entryTitled("Other");
            var menu = item("savedSystemManageSheet");
            home.manageCard(county.leadUid, "County", county.rrLinked);
            tryCompare(menu, "visible", true);
            var update = item("updateSavedSystemButton", menu);
            verify(update.enabled);
            compare(update.accessibleName, "Update from RadioReference");
            tap(update);
            tryCompare(menu, "visible", false);
            verify(rrPlaces.updating);
            compare(testContext.rrPlacesUpdateCalls(), 1);

            home.manageCard(other.leadUid, "Other", other.rrLinked);
            tryCompare(menu, "visible", true);
            update = item("updateSavedSystemButton", menu);
            verify(!update.enabled, "disabled while an update runs");
            compare(update.accessibleName, "Update from RadioReference. Available when the running update ends");
            var reason = item("actionMenuDescription" + menu.actions.findIndex(function (action) {
                return action.key === "rrUpdate";
            }), menu);
            verify(reason.visible);
            compare(reason.text, "Available when the running update ends");
            tap(update);
            verify(menu.visible, "a disabled row does nothing");
            compare(testContext.rrPlacesUpdateCalls(), 1, "the service is not asked");
            compare(testContext.rrPlacesUpdateUids(), [savedSystems.get(0).uid], "still County's update");

            // Called anyway (the notice's path and the menu's): refused here, with no account screen.
            testContext.setRrPlacesUnplaced(1);
            testContext.setRrPlacesUnplacedUids([savedSystems.get(1).uid]);
            prefs.systemsSort = "place";
            settle();
            verify(!item("systemsPlaceNotice").visible, "the notice gives way to the progress");
            verify(item("systemsRrProgress").visible);
            controls().updateUnplacedFromRadioReference();
            controls().updateFromRadioReference([savedSystems.get(1).uid]);
            compare(testContext.rrPlacesUpdateCalls(), 1, "neither asks the service");
            compare(accountOpened.count, 0, "nor opens the account screen");

            // The update ends with the menu open: its Update enables in place, and runs.
            testContext.finishRrPlacesUpdate(1, 0);
            verify(update.enabled, "enabled in place");
            compare(update.accessibleName, "Update from RadioReference");
            verify(!reason.visible);
            tap(update);
            tryCompare(menu, "visible", false);
            compare(testContext.rrPlacesUpdateUids(), [savedSystems.get(1).uid], "Other's update");
            testContext.finishRrPlacesUpdate(1, 0);
            verify(item("systemsPlaceNotice").visible, "the notice is back");
            tap(item("systemsPlaceUpdate"));
            compare(testContext.rrPlacesUpdateCalls(), 3);
        }

        // Without a screen reader a sort moves the cards' delegates: the same object stands for a card after it.
        function test_delegate_survives_sort() {
            add("Charlie");
            add("Alpha");
            add("Bravo");
            settle();
            var charlie = entryTitled("Charlie");
            var play = findChild(charlie, "savedSystemPlay");
            play.forceActiveFocus();
            prefs.systemsSort = "name";
            settle();
            compare(titles(), ["Alpha", "Bravo", "Charlie"]);
            verify(entryTitled("Charlie") === charlie, "the card kept its delegate");
            verify(play.activeFocus, "and the keyboard on it");
            search("charlie");
            verify(entryTitled("Charlie") === charlie, "and through a search");
        }

        // With a screen reader the rows are positional (R28): the sort renames each row for the card now at its
        // place, and the keyboard stays on the pill.
        function test_screen_reader_sort_renames_rows_focus_on_pill() {
            add("Charlie");
            add("Alpha");
            settle();
            savedSystemCards.screenReaderActive = true;
            var first = entryTitled("Charlie");
            var second = entryTitled("Alpha");
            var pill = item("systemsSortPill");
            pill.forceActiveFocus();
            keyClick(Qt.Key_Space);
            var sheet = item("systemsSortSheet");
            tryCompare(sheet, "visible", true);
            tap(item("systemsSortChoice0", sheet)); // Name
            tryCompare(sheet, "visible", false);
            settle();
            compare(titles(), ["Alpha", "Charlie"]);
            verify(entryTitled("Alpha") === first && entryTitled("Charlie") === second, "renamed in place, not moved");
            compare(findChild(first, "savedSystemManageButton").Accessible.name, "More options for Alpha");
            tryCompare(pill, "activeFocus", true);
            savedSystemCards.screenReaderActive = false;
        }

        // The parts of a card a screen reader reaches, each of which a held row takes out of the tree and unnames.
        readonly property var cardParts: ["savedSystemSection", "savedSystemTitle", "savedSystemSource", "savedSystemPlace",
            "savedSystemMeta", "savedSystemRefreshHint", "homeSiteChooserButton", "savedSystemPlay",
            "savedSystemManageButton"]
        function checkHeld(entry, label) {
            verify(entry.held, label + ": held");
            compare(entry.opacity, 0, label + ": transparent");
            verify(!entry.enabled, label + ": takes no press or keyboard");
            for (var name of cardParts) {
                var part = findChild(entry, name);
                verify(part !== null, label + ": " + name);
                verify(part.Accessible.ignored, label + ": " + name + " out of the tree");
                compare(part.Accessible.name, "", label + ": " + name + " unnamed");
            }
        }
        function checkShown(entry, title, label) {
            verify(!entry.held, label + ": shown");
            compare(entry.opacity, 1, label + ": drawn");
            compare(entry.title, title, label + ": its card");
            compare(findChild(entry, "savedSystemTitle").Accessible.name, title, label + ": the title read");
            compare(findChild(entry, "savedSystemManageButton").Accessible.name, "More options for " + title,
                label + ": its actions named for it");
        }
        function pad(n) {
            return String(n).padStart(3, "0");
        }

        // R28: with a screen reader on, 300 cards keep their 300 delegates (none made or destroyed) through a sort, a
        // search that narrows them to 5 and back, and a Nearest reorder that a location the user asked for brings. A
        // row past the 5 is held: transparent, out of the tree and unnamed, below the section's end; a shown row reads
        // the card now at its place. The keyboard stays on the pill, the field, and the card row it was on. Timings
        // (settled model and layout, desktop) are printed.
        function test_screen_reader_300_cards_keep_their_rows() {
            testContext.setLocationSupported(true);
            var rows = [];
            for (var i = 0; i < 300; ++i) {
                // Name order is the reverse of the order added, and so is the distance from 44.0 against 41.0.
                rows.push({name: (i % 60 === 0 ? "Harbor " : "Bench ") + pad(299 - i), sourceType: "rtltcp", host: "127.0.0.1",
                    port: 1234, freqMhz: "851.5", rrSid: 1000 + i, rrSiteId: 1, siteName: "S", siteLat: 41 + i / 100,
                    siteLon: -93.6, hasSitePos: true});
            }
            verify(savedSystems.addAll(rows, "") >= 0);
            settle();
            savedSystemCards.screenReaderActive = true;
            var repeater = item("savedSystemCardsRepeater");
            compare(repeater.count, 300);
            var made = 0;
            var gone = 0;
            var onMade = function () {
                ++made;
            };
            var onGone = function () {
                ++gone;
            };
            repeater.itemAdded.connect(onMade);
            repeater.itemRemoved.connect(onGone);
            var delegates = [];
            for (i = 0; i < 300; ++i)
                delegates.push(repeater.itemAt(i));
            var sameDelegates = function (label) {
                compare(made, 0, label + ": no delegate made");
                compare(gone, 0, label + ": no delegate destroyed");
                compare(repeater.count, 300, label + ": 300 rows");
                for (var r = 0; r < 300; ++r) {
                    if (repeater.itemAt(r) !== delegates[r])
                        fail(label + ": row " + r + " has another delegate");
                }
            };
            try {
                // A sort through the pill and its sheet, from the keyboard.
                compare(titles()[0], "Harbor 299");
                var pill = item("systemsSortPill");
                pill.forceActiveFocus();
                keyClick(Qt.Key_Space);
                var sheet = item("systemsSortSheet");
                tryCompare(sheet, "visible", true);
                var started = Date.now();
                tap(item("systemsSortChoice0", sheet)); // Name
                tryCompare(sheet, "visible", false);
                settle();
                var sortMs = Date.now() - started;
                sameDelegates("sort");
                checkShown(repeater.itemAt(0), "Bench 000", "sort row 0");
                checkShown(repeater.itemAt(299), "Harbor 299", "sort row 299");
                tryCompare(pill, "activeFocus", true);

                // The same sort and back, on the model alone.
                started = Date.now();
                prefs.systemsSort = "added";
                settle();
                var resortMs = Date.now() - started;
                prefs.systemsSort = "name";
                settle();
                sameDelegates("resort");

                // A search typed into the field: 300 → 5, and back.
                var input = field().input;
                input.forceActiveFocus();
                started = Date.now();
                savedSystemCards.query = "harbor";
                settle();
                var searchMs = Date.now() - started;
                compare(savedSystemCards.shownCount, 5);
                compare(titles(), ["Harbor 059", "Harbor 119", "Harbor 179", "Harbor 239", "Harbor 299"]);
                sameDelegates("search");
                for (var row of [5, 6, 150, 299])
                    checkHeld(repeater.itemAt(row), "search row " + row);
                for (row = 5; row < 300; ++row) {
                    if (!repeater.itemAt(row).held || repeater.itemAt(row).opacity !== 0)
                        fail("search: row " + row + " is not held");
                }
                checkShown(repeater.itemAt(4), "Harbor 299", "search row 4");
                var last = repeater.itemAt(4);
                var section = item("listenSystemsSection");
                compare(section.height, last.y + last.height, "the section ends at the last card shown");
                verify(input.activeFocus, "the field keeps the keyboard");
                field().text = "";
                tryCompare(savedSystemCards, "query", "", 2000);
                settle();
                compare(savedSystemCards.shownCount, 300);
                sameDelegates("search cleared");
                checkShown(repeater.itemAt(299), "Harbor 299", "cleared row 299");
                verify(input.activeFocus, "the field keeps the keyboard");
                compare(section.height, section.implicitHeight, "every row shown again");

                // Nearest: the location the user asks for reorders the cards.
                prefs.setLocationFix(41.0, -93.6, savedSystems.realtimeNowMs() - 60000, 25);
                prefs.systemsSort = "nearest";
                settle();
                compare(titles()[0], "Harbor 299", "nearest 41.0: the first added is nearest");
                var manage = findChild(repeater.itemAt(0), "savedSystemManageButton");
                manage.forceActiveFocus();
                tap(item("systemsLocationButton"));
                manage.forceActiveFocus();
                started = Date.now();
                decoderHost.locationResult(controls().locationId, true, 44.0, -93.6, 25, savedSystems.realtimeNowMs(), false,
                    "", "", "");
                settle();
                var nearestMs = Date.now() - started;
                compare(titles()[0], "Bench 000", "nearest 44.0: the last added is nearest");
                sameDelegates("nearest");
                verify(manage.activeFocus, "the keyboard stays on the row");
                compare(manage.Accessible.name, "More options for Bench 000", "which reads the card now at its place");
                console.log("timing with a screen reader, 300 cards: sort through the sheet " + sortMs
                    + " ms, sort on the model " + resortMs + " ms, search 300 to 5 " + searchMs + " ms, Nearest reorder "
                    + nearestMs + " ms");
            } finally {
                repeater.itemAdded.disconnect(onMade);
                repeater.itemRemoved.disconnect(onGone);
            }
        }

        // R28: a card filtered out from under the keyboard with a screen reader on is held, not removed: the keyboard
        // goes to the search field, as for a card that leaves.
        function test_screen_reader_held_card_hands_the_keyboard_on() {
            addThree();
            savedSystemCards.screenReaderActive = true;
            var repeater = item("savedSystemCardsRepeater");
            var play = findChild(repeater.itemAt(2), "savedSystemPlay");
            play.forceActiveFocus();
            verify(play.activeFocus);
            savedSystemCards.query = "linn";
            settle();
            compare(repeater.count, 3, "the rows stay");
            checkHeld(repeater.itemAt(2), "row 2");
            tryVerify(function () {
                return field().input.activeFocus;
            }, 1000, "the search field has the keyboard");
            savedSystemCards.query = "";
            settle();
            compare(titles(), ["Johnson County", "Linn Fire", "Story Sheriff"]);
            verify(repeater.itemAt(2).enabled, "taken back, it takes the keyboard again");
            savedSystemCards.screenReaderActive = false;
            settle();
            compare(repeater.count, 3);
        }

        // r6 2: a part with the keyboard that the row's next card lacks (Play, the site button) is held, and so
        // disabled, with a screen reader on; without one the card keeps its row and the part is hidden as the card
        // loses it. Either way the keyboard goes to that row's More options, which every card has, never nowhere.
        // A line's action over the cards that goes (the place notice's, once nothing is left unplaced; the no-match
        // card's Clear search, once a system that matches is added) hands it to the search field.
        function test_focused_part_the_card_lacks_hands_on_data() {
            return [{tag: "Play, screen reader", reader: true, part: "savedSystemPlay", row: 0, title: "Alliant Energy"},
                {tag: "site button, screen reader", reader: true, part: "homeSiteChooserButton", row: 1, title: "Zulu"},
                {tag: "Play, no screen reader", reader: false, part: "savedSystemPlay", row: 1, title: "Alliant Energy",
                    avoided: true},
                {tag: "place notice, screen reader", reader: true, part: "systemsPlaceUpdate"},
                {tag: "place notice, no screen reader", reader: false, part: "systemsPlaceUpdate"},
                {tag: "no match, screen reader", reader: true, part: "systemsNoMatchClear"}];
        }
        function test_focused_part_the_card_lacks_hands_on(data) {
            add("Zulu");
            for (var i = 0; i < 2; ++i)
                site("Alliant Energy", 7, undefined, undefined, {rrSiteId: 70 + i, siteName: "Site " + i});
            prefs.systemsSort = data.part === "systemsPlaceUpdate" ? "place" : "added";
            // One of Alliant's two sites avoided: the other is its Play.
            var avoidedRow = -1;
            for (i = 0; i < savedSystems.count; ++i) {
                if (savedSystems.get(i).rrSiteId === 70)
                    avoidedRow = i;
            }
            if (data.avoided)
                verify(savedSystems.setAvoidSite(avoidedRow, true));
            if (data.part === "systemsPlaceUpdate")
                testContext.setRrPlacesUnplaced(1);
            if (data.part === "systemsNoMatchClear")
                search("yankee");
            settle();
            savedSystemCards.screenReaderActive = data.reader;
            settle();
            var repeater = item("savedSystemCardsRepeater");
            var part = data.row === undefined ? item(data.part) : findChild(repeater.itemAt(data.row), data.part);
            verify(part.visible && part.enabled, "the part is shown");
            part.forceActiveFocus();
            verify(part.activeFocus);
            if (data.avoided) {
                // The card keeps its row and loses Play: both its sites can play again.
                verify(savedSystems.setAvoidSite(avoidedRow, false));
            } else if (data.part === "systemsNoMatchClear") {
                add("Yankee");
            } else if (data.part === "systemsPlaceUpdate") {
                testContext.setRrPlacesUnplaced(0);
            } else {
                prefs.systemsSort = "name";
            }
            settle();
            if (data.row === undefined) {
                verify(!part.enabled && part.visible === data.reader, "the control went: held, or hidden without a screen reader");
                tryVerify(function () {
                    return field().input.activeFocus;
                }, 1000, "the search field has the keyboard, not " + home.Window.window.activeFocusItem);
            } else {
                var manage = findChild(repeater.itemAt(data.row), "savedSystemManageButton");
                compare(repeater.itemAt(data.row).title, data.title, "the row's card");
                verify(data.reader ? part.visible && part.opacity === 0 && !part.enabled : !part.visible, "the part went");
                tryVerify(function () {
                    return manage.activeFocus;
                }, 1000, "the row's More options has the keyboard, not " + home.Window.window.activeFocusItem);
                if (data.reader)
                    compare(manage.Accessible.name, "More options for " + data.title);
            }
            savedSystemCards.screenReaderActive = false;
        }

        // r6 1: a part held or shown in place, with the list's order and rows as they were, has Home refresh Android's
        // copy of the tree once (savedSystemsTreeRefresh, counted by tst_a11y_scan_churn.qml's m4 case with events):
        // here the location line's second line, which goes once a fix arrives while the one card keeps its row.
        function refreshToggles(change) {
            var node = home.systemsTreeRefresh;
            var before = node ? node.visible : undefined;
            change();
            settle();
            wait(20);
            node = home.systemsTreeRefresh;
            if (!node)
                return 0;
            return before === undefined || before !== node.visible ? 1 : 0;
        }
        function test_line_part_held_in_place_refreshes_the_tree() {
            testContext.setLocationSupported(true);
            site("Near", 2, 41.7, -93.6, {place: iowa("Story", 85)});
            prefs.systemsSort = "nearest";
            settle();
            savedSystemCards.screenReaderActive = true;
            settle();
            wait(20);
            var line = item("systemsLocation");
            compare(line.detail, "Showing names until a location arrives");
            var entry = entries()[0];
            compare(refreshToggles(function () {
                prefs.setLocationFix(41.6, -93.6, savedSystems.realtimeNowMs(), 25);
            }), 1, "the fix holds the second line: one refresh");
            compare(line.detail, "");
            compare(entries()[0], entry, "the card keeps its row");
            compare(findChild(entry, "savedSystemPlace").text, "6.9 mi · Story, Iowa");
            // The second line back while the line shows, so the next case finds the line laid out as it left it.
            prefs.setLocationFix(0, 0, 0, 0);
            savedSystemCards.screenReaderActive = false;
            settle();
            wait(20);
            compare(line.detail, "Showing names until a location arrives");
        }

        // a11y2 A: with a screen reader the rows are positional, and a part one card has and the row's next card
        // lacks (the section heading, the place and receiver lines, the refresh hint, the site button, Play) is held,
        // never hidden: still visible (a show or hide made Android's bridge fetch the scroll body, with every card
        // under it, again), but transparent, out of the tree and unnamed, with no height in its column, no Tab stop
        // and no press. A tap where it would be reaches nothing. Without a screen reader it is hidden, as before.
        function test_screen_reader_holds_the_parts_a_card_lacks_data() {
            return [{tag: "screen reader", reader: true}, {tag: "no screen reader", reader: false}];
        }
        function test_screen_reader_holds_the_parts_a_card_lacks(data) {
            add("Story Fire", {freqMhz: "154.4", place: iowa("Story", 85)});
            add("Story Sheriff", {freqMhz: "155.775", place: iowa("Story", 85)});
            for (var i = 0; i < 2; ++i)
                site("Alliant Energy", 7, undefined, undefined, {rrSiteId: 70 + i, siteName: "Site " + i});
            settle();
            savedSystemCards.screenReaderActive = data.reader;
            prefs.systemsSort = "place";
            settle();
            compare(titles(), ["Story Fire", "Story Sheriff", "Alliant Energy"]);
            var story = entryTitled("Story Sheriff");
            var alliant = entryTitled("Alliant Energy");
            var lacking = [[story, "homeSiteChooserButton"], [story, "savedSystemReceiver"],
                [story, "savedSystemRefreshHint"], [alliant, "savedSystemPlay"], [alliant, "savedSystemPlace"],
                [alliant, "savedSystemReceiver"], [alliant, "savedSystemRefreshHint"]];
            for (var pair of lacking) {
                var part = findChild(pair[0], pair[1]);
                var label = pair[0].title + " " + pair[1];
                verify(part !== null, label);
                if (!data.reader) {
                    verify(!part.visible, label + ": hidden");
                    continue;
                }
                verify(part.visible, label + ": held, not hidden");
                compare(part.opacity, 0, label + ": transparent");
                verify(part.Accessible.ignored, label + ": out of the tree");
                compare(part.Accessible.name, "", label + ": unnamed");
                if (pair[1] === "homeSiteChooserButton" || pair[1] === "savedSystemPlay")
                    verify(!part.enabled && !part.activeFocusOnTab, label + ": no press or Tab stop");
            }
            // r6 1: every part a card can lack is one of its lackableParts, whose `shown` set Home refreshes the tree
            // for: each part held here, and each part with a `shown` of its own.
            for (var entry of entries()) {
                var listed = entry.lackableParts;
                for (var listedPart of listed)
                    verify(typeof listedPart.shown === "boolean", entry.title + ": " + listedPart.objectName + " has a shown");
                var walk = function (node) {
                    for (var child of node.children) {
                        var isPart = typeof child.shown === "boolean" || (data.reader && child.visible && child.opacity === 0);
                        if (isPart && child.objectName.length > 0)
                            verify(listed.indexOf(child) >= 0, entry.title + ": " + child.objectName + " is a lackable part");
                        walk(child);
                    }
                };
                walk(entry);
            }
            var heading = findChild(story, "savedSystemSection");
            verify(data.reader ? heading.visible && heading.opacity === 0 && heading.Accessible.ignored
                && heading.Accessible.name === "" : !heading.visible, "the second card in its section has no heading");
            compare(findChild(story, "savedSystemCard").y, 0, "and no room is left for one");
            if (data.reader) {
                compare(heading.parent.height, 0, "the held heading takes no height");
                compare(findChild(alliant, "savedSystemPlace").parent.height, 0, "nor does a missing place line");
            }
            // What a card shows reads and draws as before.
            var storyPlay = findChild(story, "savedSystemPlay");
            verify(storyPlay.visible && storyPlay.opacity === 1 && !storyPlay.Accessible.ignored && storyPlay.enabled);
            compare(findChild(story, "savedSystemPlace").Accessible.name, "Story, Iowa");
            compare(findChild(entryTitled("Story Fire"), "savedSystemSection").Accessible.name, "Iowa");
            compare(findChild(alliant, "savedSystemSection").Accessible.name, "No place");
            // A tap where Alliant's Play would be plays nothing (its site button moves over there); one where Story's
            // site button would be opens no site chooser.
            tap(findChild(alliant, "savedSystemPlay"));
            compare(played.count, 0, "nothing played");
            sitesChosen.clear();
            if (data.reader)
                tap(findChild(story, "homeSiteChooserButton"));
            compare(sitesChosen.count, 0, "no site chooser");
            // The lines and the × over the cards follow the same rule: with no search, the × is held over the field,
            // and a tap on it reaches the field; the count line is held with no height.
            var clear = item("systemsSearchClear");
            var count = item("systemsSearchCount");
            if (data.reader) {
                verify(clear.visible && clear.opacity === 0 && clear.Accessible.ignored && clear.Accessible.name === ""
                    && !clear.enabled && !clear.activeFocusOnTab, "the × is held");
                verify(count.visible && count.opacity === 0 && count.height === 0 && !count.enabled, "the count line is held");
                tap(clear);
                verify(field().input.activeFocus, "a tap on the held × reaches the field");
            } else {
                verify(!clear.visible && !count.visible, "hidden without a screen reader");
            }
            savedSystemCards.screenReaderActive = false;
        }

        // R29: Nearest puts the distance before the place, so a long place elides and the distance stays.
        function test_nearest_distance_before_place() {
            testContext.setLocationSupported(true);
            site("Near", 2, 41.7, -93.6, {place: iowa("Johnson", 1)});
            settle();
            prefs.systemsSort = "nearest";
            prefs.setLocationFix(41.6, -93.6, savedSystems.realtimeNowMs(), 25);
            settle();
            compare(findChild(entryTitled("Near"), "savedSystemPlace").text, "6.9 mi · Johnson, Iowa");
            prefs.systemsSort = "name";
            settle();
            compare(findChild(entryTitled("Near"), "savedSystemPlace").text, "Johnson, Iowa", "no distance off Nearest");
        }

        // R29: one system saved on two receivers is two cards of one title; each card then names its receiver, on a
        // line of its own under the source line (a11y2 D: in the source line it was cut short at phone width and gone
        // at 1.3x text). The line wraps, so at 320 px and 1.3x the whole receiver is drawn and read. A title no other
        // shown card has has no receiver line, and its source line is unchanged.
        function test_shared_title_names_the_receiver() {
            site("Metro", 1, undefined, undefined, {sourceType: "usb", freqMhz: "851.0125", decodeFlag: "-fp"});
            site("Metro", 1, undefined, undefined, {host: "192.168.100.250", port: 61234, freqMhz: "852.0125",
                decodeFlag: "-fp"});
            add("Story Sheriff", {freqMhz: "155.775"});
            settle();
            compare(titles(), ["Metro", "Metro", "Story Sheriff"]);
            var receiver = function (entry) {
                var line = findChild(entry, "savedSystemReceiver");
                verify(line !== null, entry.title + " has a receiver line");
                return line;
            };
            var source = function (entry) {
                return findChild(entry, "savedSystemSource").text;
            };
            var cards = entries();
            compare(receiver(cards[0]).text, "USB");
            compare(receiver(cards[1]).text, "RTL-TCP 192.168.100.250:61234");
            compare(source(cards[0]).indexOf("USB"), -1, "the source line leaves the receiver to its own line");
            compare(source(cards[1]).indexOf("RTL-TCP"), -1, "which says RTL-TCP itself");
            verify(!receiver(cards[2]).visible, "a unique title has no receiver line");
            verify(source(cards[2]).indexOf("155.775 MHz · RTL-TCP · ") === 0 && source(cards[2]).indexOf("127.0.0.1") < 0,
                "a unique title is unchanged: " + source(cards[2]));

            root.width = 320;
            Ui.Theme.fontScale = 1.3;
            settle();
            var line = receiver(entries()[1]);
            verify(line.visible && line.opacity === 1, "shown");
            compare(line.Accessible.name, "RTL-TCP 192.168.100.250:61234", "read whole");
            verify(!line.truncated, "never cut");
            verify(line.width <= findChild(entries()[1], "savedSystemSource").width + 0.5, "within the labels' column");
            verify(line.contentWidth <= line.width + 0.5, "wrapped within its width: " + line.contentWidth + " > " + line.width);
            verify(line.parent.height >= line.implicitHeight - 0.5, "its row is as tall as its lines");

            search("852.0125");
            compare(titles(), ["Metro"]);
            verify(!receiver(entries()[0]).visible, "alone in the list, it names no receiver");
            compare(source(entries()[0]).indexOf("192.168.100.250"), -1);
        }

        // R29: keypad Enter (and Return) in the search field hides the keyboard and leaves the field with the focus.
        function test_enter_keeps_the_search_field() {
            addThree();
            var input = field().input;
            for (var key of [Qt.Key_Enter, Qt.Key_Return]) {
                input.forceActiveFocus();
                keyClick(Qt.Key_L);
                keyClick(key);
                verify(input.activeFocus, "the field keeps the focus after " + key);
                verify(!item("systemsSearchClear").activeFocus, "not the ×");
                field().text = "";
            }
        }

        // a11y2 E: a hardware keyboard's Enter (Return, or the keypad's Enter), pressed and released, stays in the
        // search field: neither the press nor the release reaches anything around it (on Android the keyboard focus
        // then moved on to the × and the next letter was lost), the field keeps the keyboard and typing goes on.
        function test_hardware_enter_stays_in_the_search_field() {
            addThree();
            var input = field().input;
            input.forceActiveFocus();
            keyClick(Qt.Key_L);
            for (var key of [Qt.Key_Return, Qt.Key_Enter]) {
                root.enterPassedOn = 0;
                keyPress(key);
                keyRelease(key);
                compare(root.enterPassedOn, 0, "Enter " + key + " stays in the field");
                verify(input.activeFocus, "the field keeps the keyboard after " + key);
                keyClick(Qt.Key_I);
            }
            compare(field().text, "lii", "every letter lands in the field");
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

        // A card that leaves with the keyboard on it while a sheet opens over Home: the sheet hands the keyboard back as
        // it closes (the card is gone), and then it goes to the search field.
        function test_focus_fallback_waits_for_a_sheet() {
            addThree();
            var play = findChild(entryTitled("Story Sheriff"), "savedSystemPlay");
            play.forceActiveFocus();
            var sheet = item("systemsSortSheet");
            // The sheet opens as the card leaves, before the keyboard is handed on.
            sheetOnRemoval.sheet = sheet;
            sheetOnRemoval.target = item("savedSystemCardsRepeater");
            sheetOnRemoval.enabled = true;
            savedSystemCards.query = "linn";
            settle();
            wait(50); // the hand-on's own turn passes under the sheet
            verify(!sheetOnRemoval.enabled, "it opened");
            verify(sheet.visible);
            verify(home.systemsFocusOwed, "the hand-on waits for the sheet");
            verify(!field().input.activeFocus, "nothing moves under the sheet");
            sheet.visible = false;
            tryVerify(function () {
                return field().input.activeFocus;
            }, 1000, "the search field has the keyboard once the sheet closed");
        }

        // One saved system: its count reads in the singular.
        function test_count_singular() {
            add("Alpha");
            add("Bravo");
            settle();
            search("alpha");
            compare(item("systemsSearchCountText").text, "1 of 2 systems");
            savedSystems.remove(1);
            settle();
            compare(item("systemsSearchCountText").text, "1 of 1 system");
        }

        // Update from RadioReference from the keyboard: the keyboard follows the control that replaces the one pressed,
        // Update → Cancel → Update; an update that ends on its own hands Cancel's keyboard to Update too, or to the
        // search field once nothing is left to place.
        function test_update_keeps_the_keyboard() {
            site("County", 1);
            settle();
            testContext.setRrPlacesUnplaced(1);
            prefs.systemsSort = "place";
            settle();
            var update = item("systemsPlaceUpdate");
            update.forceActiveFocus();
            keyClick(Qt.Key_Space);
            verify(rrPlaces.updating);
            var cancel = item("systemsRrCancel");
            tryVerify(function () {
                return cancel.activeFocus;
            }, 1000, "Cancel has the keyboard");
            keyClick(Qt.Key_Space);
            compare(testContext.rrPlacesCancels(), 1);
            tryVerify(function () {
                return update.activeFocus && update.visible;
            }, 1000, "Update has it again");

            keyClick(Qt.Key_Space);
            tryVerify(function () {
                return cancel.activeFocus;
            }, 1000);
            testContext.setRrPlacesUnplaced(0);
            testContext.finishRrPlacesUpdate(1, 0);
            tryVerify(function () {
                return field().input.activeFocus;
            }, 1000, "nothing left to place: the search field");
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

        // The notices' actions are text buttons at the end of their line (under it when both do not fit), each a 48 dp
        // target that meets neither the other notice's nor the first card.
        function test_notice_actions_clear_neighbours_data() {
            return [{tag: "360 x 1.0", width: 360, scale: 1.0}, {tag: "360 x 2.0", width: 360, scale: 2.0},
                {tag: "1000 x 1.0", width: 1000, scale: 1.0}];
        }
        function test_notice_actions_clear_neighbours(data) {
            testContext.setLocationSupported(true);
            site("County", 1, 41.7, -93.6);
            testContext.setRrPlacesUnplaced(2);
            prefs.systemsSort = "nearest";
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            settle();
            var section = item("listenSystemsSection");
            var rect = function (control) {
                var p = control.mapToItem(section, 0, 0);
                return {x: p.x, y: p.y, w: control.width, h: control.height};
            };
            var meet = function (a, b) {
                return a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 && b.y < a.y + a.h - 0.5;
            };
            var location = rect(item("systemsLocationButton"));
            var update = rect(item("systemsPlaceUpdate"));
            var card = rect(item("savedSystemCard", entryTitled("County")));
            var row = rect(item("systemsSortPill"));
            for (var target of [location, update])
                verify(target.h >= 48 && target.w >= 48, "a 48 dp target");
            verify(!meet(location, update), "the two actions apart");
            verify(!meet(update, card), "Update clear of the first card");
            verify(!meet(location, row), "Use my location clear of Sort");
            verify(item("systemsLocationButton").flat && item("systemsPlaceUpdate").flat, "text-style actions");
        }

        // The field's × while typing: a tap clears the text and leaves the keyboard in the field, so typing goes on;
        // a word still composing is finished first, and cleared with the rest. From the keyboard (Tab to the ×, then
        // Enter) the × goes away and the field has the keyboard. A tap while the field is not in use leaves the
        // keyboard where it was.
        function test_clear_keeps_typing() {
            addThree();
            var input = field().input;
            input.forceActiveFocus();
            keyClick(Qt.Key_L);
            keyClick(Qt.Key_I);
            tryCompare(savedSystemCards, "query", "li", 2000);
            settle();
            var clear = item("systemsSearchClear");
            verify(clear.visible);
            tap(clear);
            compare(field().text, "");
            compare(savedSystemCards.query, "");
            verify(input.activeFocus, "the field keeps the keyboard after a tap on its ×");
            keyClick(Qt.Key_S);
            compare(field().text, "s", "typing goes on in the field");
            tryCompare(savedSystemCards, "query", "s", 2000);
            settle();

            // A word still composing when the × is tapped.
            verify(testContext.composeText("to"), "the field holds a composing word");
            verify(Ui.Navigation.composing);
            tap(clear);
            compare(field().text, "");
            compare(input.preeditText, "", "the word was finished and cleared");
            verify(!Ui.Navigation.composing);
            verify(input.activeFocus, "and the field kept the keyboard");
            compare(savedSystemCards.query, "");

            search("linn");
            input.forceActiveFocus();
            keyClick(Qt.Key_Tab);
            verify(clear.activeFocus, "Tab reaches the ×");
            keyClick(Qt.Key_Return);
            compare(savedSystemCards.query, "");
            verify(!clear.visible);
            verify(input.activeFocus, "the field has the keyboard after the × goes");

            search("story");
            root.forceActiveFocus();
            tap(clear);
            compare(savedSystemCards.query, "");
            verify(!input.activeFocus, "a tap with the field not in use does not open the keyboard");
        }

        // Tab order: the field, its ×, the pill, then the cards.
        // a11y2 C: with a screen reader the rows are positional, so after a reorder a row shows a card whose parts
        // differ from its last one's (a site button, Play). Tab from the Sort pill then visits every control each card
        // shows, in screen order, once, and nothing a card lacks: as without a screen reader.
        function tabStops(fromItem) {
            var stops = [];
            fromItem.forceActiveFocus();
            var window = home.Window.window;
            for (var i = 0; i < 40; ++i) {
                keyClick(Qt.Key_Tab);
                var at = window.activeFocusItem;
                var entry = at;
                while (entry && entry.objectName !== "savedSystemEntry")
                    entry = entry.parent;
                if (!entry) {
                    if (stops.length > 0)
                        break;
                    continue;
                }
                stops.push(entry.title + ":" + at.objectName);
            }
            return stops;
        }
        function expectedStops() {
            var stops = [];
            for (var entry of entries()) {
                if (entry.siteCount > 0)
                    stops.push(entry.title + ":homeSiteChooserButton");
                if (entry.playUid.length > 0)
                    stops.push(entry.title + ":savedSystemPlay");
                stops.push(entry.title + ":savedSystemManageButton");
            }
            return stops;
        }
        function test_tab_after_a_positional_reorder_data() {
            return [{tag: "screen reader", reader: true}, {tag: "no screen reader", reader: false}];
        }
        function test_tab_after_a_positional_reorder(data) {
            testContext.setLocationSupported(true);
            // Near the first fix: groups of several sites (no Play) first; near the second, single sites first.
            for (var i = 0; i < 4; ++i)
                site("Alliant Energy", 7, 41.0 + i * 0.001, -93.6);
            site("Hill Union EMS", 8, 44.0, -93.6);
            site("Falls Harbor", 9, 43.9, -93.6);
            add("Story Sheriff", {freqMhz: "155.775"});
            site("Ridge Lake", 10, 41.1, -93.6);
            site("Ridge Lake", 10, 41.101, -93.6);
            settle();
            savedSystemCards.screenReaderActive = data.reader;
            prefs.setLocationFix(41.0, -93.6, savedSystems.realtimeNowMs() - 60000, 25);
            prefs.systemsSort = "nearest";
            settle();
            compare(titles(), ["Alliant Energy", "Ridge Lake", "Falls Harbor", "Hill Union EMS", "Story Sheriff"]);
            var pill = item("systemsSortPill");
            compare(tabStops(pill), expectedStops(), "before the reorder");
            var firstRow = item("savedSystemCardsRepeater").itemAt(0);
            compare(firstRow.playUid, "", "the first row's card has several sites to play");
            savedSystemCards.markLocationRequested();
            prefs.setLocationFix(44.0, -93.6, savedSystems.realtimeNowMs(), 25);
            settle();
            compare(titles(), ["Hill Union EMS", "Falls Harbor", "Ridge Lake", "Alliant Energy", "Story Sheriff"]);
            if (data.reader)
                verify(item("savedSystemCardsRepeater").itemAt(0) === firstRow && firstRow.playUid.length > 0,
                    "the first row now shows a card with Play");
            compare(tabStops(pill), expectedStops(), "after the reorder");
            // Backtab from the last card's More options walks the same stops back.
            var stops = expectedStops();
            var window = home.Window.window;
            var last = findChild(entries()[entries().length - 1], "savedSystemManageButton");
            last.forceActiveFocus();
            var back = [];
            for (i = 0; i < stops.length - 1; ++i) {
                keyClick(Qt.Key_Backtab);
                var at = window.activeFocusItem;
                var entry = at;
                while (entry && entry.objectName !== "savedSystemEntry")
                    entry = entry.parent;
                back.push((entry ? entry.title : "?") + ":" + at.objectName);
            }
            compare(back, stops.slice(0, stops.length - 1).reverse(), "Backtab");
            savedSystemCards.screenReaderActive = false;
        }

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
