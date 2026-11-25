// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #800 (spec 4.6): the Map screen over the production track model (fed from the canned position stream) and the
// production tile worker on the canned transport. What is checked is what a listener meets: no tile request before
// tiles are turned on, every state of a fix said in words (sheet and List alike), every map action a button, the List
// as the canvas's complete equivalent, the attribution readable above the detail sheet, a History row's pin, the
// age filter, tile trouble as a quiet chip, reduced motion, Locate me through the shared broker, and no horizontal
// clipping at a large text size. Places and IDs are made up.
Item {
    id: root

    width: 360
    height: 780

    Loader {
        id: loader
        anchors.fill: parent
        active: false
        source: uiDir + "/MapScreen.qml"
    }

    TestCase {
        id: tc

        name: "MapScreen"
        when: windowShown

        readonly property var screen: loader.item

        function item(name) {
            var found = findChild(screen, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        function canvas() {
            return item("mapCanvas");
        }
        function push(fix) {
            testContext.pushPosition(fix);
            waitForRendering(screen);
        }
        // The track id of the newest track named @p label (ids are never reused, so search upward from the start).
        function trackNamed(label) {
            for (var id = 1; id < 20000; id++) {
                var t = positions.trackFor(id);
                if (t.trackId !== undefined && (t.label === label || (label === "" && t.anonymous)))
                    return id;
            }
            return 0;
        }
        function tapMarker(id) {
            var p = canvas().markerPoint(id);
            verify(p !== undefined, "the marker is drawn");
            mouseClick(canvas(), p.x, p.y);
        }
        function sheet() {
            return item("mapPositionSheet");
        }
        // Every visible text a sheet shows, joined, for "is this said in words".
        function sheetWords() {
            var words = [];
            var walk = function (node) {
                if (!node || !node.visible)
                    return;
                if (node.text !== undefined && typeof node.text === "string" && node.text.length > 0)
                    words.push(node.text);
                for (var i = 0; i < node.children.length; i++)
                    walk(node.children[i]);
                if (node.contentItem)
                    walk(node.contentItem);
            };
            walk(sheet());
            return words.join("\n");
        }

        function initTestCase() {
            // Locate me needs a host with location, and the saved fix lives in the production preferences.
            testContext.setLocationSupported(true);
            testContext.useAppPrefs(true);
            loader.active = true;
            tryVerify(function () { return loader.item !== null; });
        }
        function cleanupTestCase() {
            loader.active = false;
            testContext.setMapTilesFailing(false);
            testContext.setMapTilesNotPng(false);
            testContext.resetPositions();
            testContext.useAppPrefs(false);
            testContext.setLocationSupported(false);
            testContext.resetLocalDay();
            Ui.Theme.resetFontScale();
            Ui.Theme.resetReducedMotion();
        }
        // Issue #902: the day and zone a case moved end with it, before the
        // next case's _data() rows are built.
        function cleanup() {
            testContext.resetLocalDay();
        }
        function init() {
            Ui.Theme.resetFontScale();
            root.width = 360;
            root.height = 780;
            testContext.setMapTilesFailing(false);
            testContext.setMapTilesNotPng(false);
            testContext.resetPositions();
            testContext.setPositionWallSeconds(testContext.positionWallStart());
            screen.tilesEnabled = false;
            screen.tileUrl = screen.defaultTileUrl;
            screen.tileAttribution = screen.defaultAttribution;
            screen.tilesPromptDismissed = false;
            screen.mapShown = true;
            screen.clearPin();
            canvas().followTrackId = 0;
            canvas().selectedTrackId = 0;
            // The camera jumps (reduced motion), so a case reads where it lands at once.
            Ui.Theme.reducedMotion = true;
            canvas().centerOn(10.0, 20.0, 12);
            for (var name of ["mapPositionSheet", "mapSettingsSheet", "mapOverflow", "mapClearConfirm"])
                item(name).visible = false;
            waitForRendering(screen);
        }

        // Spec 4.4: nothing at all goes to a tile server until tiles are turned on from the map, and the prompt names
        // the server the viewed area is sent to.
        function test_01_no_tile_request_before_tiles_are_turned_on() {
            var before = testContext.mapTileRequests();
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7"});
            canvas().zoomIn();
            wait(200);
            compare(testContext.mapTileRequests(), before, "tiles off: nothing asked");
            var prompt = item("mapTilesPrompt");
            verify(prompt.visible);
            var area = item("mapArea");
            verify(area.y >= prompt.y + prompt.height, "the prompt sits above the canvas, over no radio");
            verify(item("mapTilesPromptHost").text.indexOf("tile.openstreetmap.org") >= 0, item("mapTilesPromptHost").text);
            item("mapLoadTiles").activate();
            verify(screen.tilesEnabled);
            tryVerify(function () { return testContext.mapTileRequests() > before; }, 5000, "tiles on: the view is asked for");
            verify(!prompt.visible, "the prompt goes once tiles are on");
        }

        // Nothing heard: why, which reports show, and how to load tiles. A filter that hides everything says the rows
        // are hidden, not missing, and Show all brings them back.
        function test_02_empty_states() {
            var title = item("mapEmptyTitle");
            verify(item("mapEmptyState").visible);
            compare(title.text, "No positions yet");
            verify(item("mapEmptyDetail").text.indexOf("LRRP") >= 0);
            verify(item("mapTilesPrompt").visible && !item("mapEmptyLoadTiles").visible,
                   "tiles off: offered once, by the prompt above the canvas");
            push({source: 1001, lat: 10.0, lon: 20.0, ageSeconds: 2000});
            verify(!item("mapEmptyState").visible);
            item("mapAge0").activate();
            compare(positions.ageFilterSeconds, 900);
            verify(item("mapEmptyState").visible);
            compare(title.text, "No positions in the last 15 minutes");
            compare(item("mapEmptyDetail").text, "1 radio was heard earlier than that.");
            item("mapShowAll").activate();
            compare(positions.ageFilterSeconds, 0);
            verify(!item("mapEmptyState").visible);
        }

        // Each pill is a checkable node, and only the chosen one is checked.
        function test_03_age_filter_pills() {
            var seconds = [900, 3600, 0];
            for (var i = 0; i < 3; ++i) {
                item("mapAge" + i).activate();
                compare(positions.ageFilterSeconds, seconds[i]);
                for (var j = 0; j < 3; ++j)
                    compare(item("mapAge" + j).Accessible.checked, i === j, "pill " + j);
            }
        }

        // Spec 4.6: tile trouble is a quiet chip on the map, never a modal.
        function test_04_tile_trouble_is_a_chip_not_a_modal() {
            push({source: 1001, lat: 10.0, lon: 20.0});
            testContext.setMapTilesFailing(true);
            screen.tilesEnabled = true;
            var chip = item("mapTilesState");
            tryVerify(function () { return chip.visible; }, 5000);
            compare(chip.text, "Map tiles unavailable — retrying");
            compare(Ui.Navigation.modals.length, 0, "no sheet or dialog");
            screen.tileUrl = "http://tiles.example.invalid/{z}/{x}/{y}.png";
            tryCompare(chip, "text", "The tile address is not valid. Check Map settings.");
            // A server that answers with something other than PNG is named as such, not "retrying".
            testContext.setMapTilesFailing(false);
            testContext.setMapTilesNotPng(true);
            screen.tileUrl = "https://jpeg.example.invalid/{z}/{x}/{y}.jpg";
            tryCompare(chip, "text", "The tile server sent no PNG tiles the map can use. Check Map settings.", 5000);
            verify(chip.visible);
        }

        // A marker's sheet says every state in words: freshness (its age), a source not confirmed, a fix that may be
        // invalid, a format no published specification confirms, an emergency.
        function test_05_the_sheet_says_every_state_in_words() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", ageSeconds: 90, emergency: true, provisional: true,
                  speedDkmh: 420, headingDdeg: 900, altitudeDm: 1000, errM: 25, targetId: 100, group: true, fixAgeSeconds: 95});
            var id = trackNamed("Engine 7");
            tapMarker(id);
            verify(sheet().visible, "a tap on the marker opens its sheet");
            compare(canvas().selectedTrackId, id, "and selects it");
            var words = sheetWords();
            for (var w of ["Engine 7", "Emergency", "Format not confirmed by a published specification", "Received 1 min ago",
                           "Measured 1 min ago", "Radio ID 1001", "Talkgroup 100", "Report type LRRP", "Heading 90° (east)"])
                verify(words.indexOf(w) >= 0, "the sheet says \"" + w + "\":\n" + words);
            sheet().visible = false;
            push({anonymous: true, lat: 10.001, lon: 20.001});
            tapMarker(trackNamed(""));
            words = sheetWords();
            verify(words.indexOf("Radio not confirmed") >= 0, words);
            verify(words.indexOf("Unconfirmed radio") >= 0, words);
            sheet().visible = false;
            push({source: 1003, lat: 10.002, lon: 19.998, label: "Unit 41", validityUnknown: true});
            tapMarker(trackNamed("Unit 41"));
            verify(sheetWords().indexOf("Fix may be old or invalid") >= 0, sheetWords());
        }

        // Follow from the sheet keeps the radio centred and says so on the map, with Stop.
        function test_06_follow_from_the_sheet() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7"});
            var id = trackNamed("Engine 7");
            screen.openTrack(id, false);
            item("mapPositionFollow").activate();
            verify(!sheet().visible);
            compare(canvas().followTrackId, id);
            var chip = item("mapFollowing");
            verify(chip.visible);
            compare(chip.text, "Following Engine 7");
            screen.openTrack(id, false);
            compare(item("mapPositionFollow").text, "Stop following");
            sheet().visible = false;
            item("mapFollowingAction").activate();
            compare(canvas().followTrackId, 0);
            verify(!chip.visible);
        }

        // The List is the canvas in words: the same rows the filter shows, each a node read whole with its states,
        // the same sheet with Follow, and Show on map back on the canvas. Hidden, it holds no rows.
        function test_07_the_list_is_the_canvas_in_words() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", provisional: true});
            push({source: 1002, lat: 10.01, lon: 20.01, label: "Medic 3", validityUnknown: true});
            push({anonymous: true, lat: 10.02, lon: 20.02});
            var list = item("mapList");
            compare(list.model, null, "hidden behind the map, the List holds no rows");
            screen.mapShown = false;
            verify(!canvas().visible);
            compare(list.model, positionView);
            compare(list.count, positionView.count);
            tryVerify(function () { return list.itemAtIndex(2) !== null; });
            var described = [];
            for (var i = 0; i < list.count; i++) {
                var row = list.itemAtIndex(i);
                described.push(row.node.Accessible.name + " | " + row.node.Accessible.description);
            }
            var all = described.join("\n");
            for (var w of ["Engine 7", "Format not confirmed by a published specification", "Medic 3", "Fix may be old or invalid",
                           "Unconfirmed radio", "Radio not confirmed"])
                verify(all.indexOf(w) >= 0, "the List says \"" + w + "\":\n" + all);
            var id = trackNamed("Medic 3");
            list.itemAtIndex(positionView.rowForTrack(id)).activate();
            verify(sheet().visible);
            verify(item("mapPositionShowOnMap").visible, "from the List the sheet offers the map");
            item("mapPositionShowOnMap").activate();
            verify(screen.mapShown);
            compare(canvas().selectedTrackId, id);
            tryCompare(list, "model", null);
        }

        // Every map action has a button; the canvas itself is out of the accessibility tree.
        function test_08_every_action_has_a_button() {
            push({source: 1001, lat: 10.0, lon: 20.0});
            verify(canvas().Accessible.ignored);
            var names = {mapZoomIn: "Zoom in", mapZoomOut: "Zoom out", mapFit: "Show every radio", mapLocate: "Locate me",
                         mapBack: "Back", mapMore: "Map options"};
            for (var key in names) {
                var b = item(key);
                verify(b.visible, key);
                compare(b.Accessible.role, Accessible.Button, key);
                compare(b.Accessible.name, names[key], key);
            }
            var zoom = canvas().zoom;
            item("mapZoomIn").activate();
            tryVerify(function () { return canvas().zoom > zoom + 0.9; });
            item("mapZoomOut").activate();
            tryVerify(function () { return Math.abs(canvas().zoom - zoom) < 0.01; });
            canvas().centerOn(0, 0, 3);
            item("mapFit").activate();
            tryVerify(function () { return Math.abs(canvas().centerLatitude - 10.0) < 0.01; }, 2000, "Fit shows the radio");
        }

        // Spec 4.4: the attribution stays readable above the detail sheet, whose panel stops short of it, and links
        // the default server's copyright page.
        function test_09_attribution_stays_above_the_detail_sheet() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", speedDkmh: 100, headingDdeg: 100, altitudeDm: 10,
                  errM: 5, targetId: 100, group: true, fixAgeSeconds: 2, freqHz: 451000000});
            var chip = item("mapAttribution");
            compare(chip.Accessible.name, "© OpenStreetMap contributors");
            compare(chip.Accessible.role, Accessible.Link);
            root.height = 420;
            screen.openTrack(trackNamed("Engine 7"), false);
            waitForRendering(screen);
            verify(chip.visible);
            verify(chip.z > sheet().z, "drawn over the sheet's dim");
            var panel = findChild(sheet(), "modalSheetScroll").parent;
            var panelBottom = panel.mapToItem(screen, 0, panel.height).y;
            var chipTop = chip.mapToItem(screen, 0, chip.height - chip.chipHeight).y;
            verify(panelBottom <= chipTop, "the panel stops short of the attribution: " + panelBottom + " > " + chipTop);
            root.height = 780;
            sheet().visible = false;
            screen.tileUrl = "https://tiles.example.org/{z}/{x}/{y}.png";
            compare(chip.Accessible.role, Accessible.StaticText, "another server's attribution is not linked");
        }

        // Spec 4.5: a History row's fix drops one pin, and its sheet says what the row kept and what it did not.
        function test_10_a_history_row_drops_one_pin() {
            screen.showPin({uid: 41, name: "CALL 041", when: 1700000000, tg: 100, src: 2001, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, errorM: 30, errorLowerBound: false, validityUnknown: true,
                                       provisional: false, weakCheck: false, anonymous: false, emergency: false, kindText: "LIP"}});
            var pin = item("mapPin");
            waitForRendering(screen);
            wait(20);
            verify(pin.visible);
            verify(!item("mapEmptyState").visible, "a pin is something to show");
            var c = canvas();
            var below = item("mapStatus").y + item("mapStatus").height;
            verify(Math.abs(pin.tipX - c.width / 2) < 1 && Math.abs(pin.tipY - (below + c.height) / 2) < 1,
                   "centred on the row's fix in the part below the chips: " + pin.tipY + " vs " + (below + c.height) / 2);
            mouseClick(c, pin.tipX, pin.tipY - 12);
            verify(sheet().visible && sheet().pin !== null);
            var words = sheetWords();
            for (var w of ["CALL 041", "Fix may be old or invalid", "Report type LIP", "History does not keep the radio's trail"])
                verify(words.indexOf(w) >= 0, "the pin's sheet says \"" + w + "\":\n" + words);
            verify(!item("mapPositionFollow").visible, "a pin has nothing to follow");
            sheet().visible = false;
            // Show every radio shows the pin too, however far the tracks are.
            push({source: 1001, lat: 10.5, lon: 20.5, label: "Engine 7"});
            canvas().centerOn(10.5, 20.5, 14);
            item("mapFit").activate();
            waitForRendering(screen);
            var marker = c.markerPoint(trackNamed("Engine 7"));
            // Re-review minor 1: the chips at the top (the pin's among them) cover that band, so a fit aims below it.
            var status = item("mapStatus");
            var band = status.y + status.height;
            verify(item("mapPinChip").visible && band > 40, "the pin's chip covers the top: " + band);
            compare(c.topInset, band);
            for (var p of [Qt.point(pin.tipX, pin.tipY), marker])
                verify(p.x >= 0 && p.x <= c.width && p.y >= band && p.y <= c.height, "in view below the chips after a fit: " + p + ", band " + band);
            screen.clearPin();
            verify(!pin.visible);
        }

        // Spec 4.6, review P7 #4: the drawn pin is not a node, so it has a chip with a button on the map and a row
        // at the top of the List, each opening its sheet.
        function test_10b_the_pin_is_reachable_without_seeing_it() {
            screen.showPin({uid: 42, name: "CALL 042", when: 1700000000, tg: 0, src: 0, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, validityUnknown: false, provisional: true,
                                       weakCheck: false, anonymous: false, emergency: false, kindText: "LRRP"}});
            var chip = item("mapPinChip");
            verify(chip.visible);
            verify(chip.text.indexOf("CALL 042") >= 0, chip.text);
            item("mapPinChipAction").activate();
            verify(sheet().visible && sheet().pin !== null, "the chip's Details opens the pin's sheet");
            sheet().visible = false;
            screen.mapShown = false;
            verify(!item("mapEmptyState").visible);
            var row = item("mapPinRow");
            verify(row.visible);
            compare(row.Accessible.role, Accessible.Button);
            compare(row.Accessible.name, "Saved position from History: CALL 042");
            var day = Qt.formatDateTime(new Date(1700000000 * 1000), "yyyy-MM-dd");
            verify(row.Accessible.description.indexOf("received " + day) === 0, "a History row's day is said: " + row.Accessible.description);
            verify(row.Accessible.description.indexOf("Format not confirmed by a published specification") >= 0, row.Accessible.description);
            row.activate();
            verify(sheet().visible && sheet().pin !== null, "the List row opens the pin's sheet");
            sheet().visible = false;
            screen.clearPin();
            verify(!row.visible && !item("mapPinChip").visible);
        }

        // Map settings: https only with {z}{x}{y}, the host named, Save applies, an edit asks before it goes.
        function test_11_map_settings() {
            var settings = item("mapSettingsSheet");
            settings.openEditor();
            var url = item("mapSettingsUrl");
            url.text = "http://tiles.example.org/{z}/{x}/{y}.png";
            verify(!item("mapSettingsSave").inputValid, "http is refused");
            verify(url.error.length > 0);
            verify(url.hint.indexOf("PNG") >= 0, url.hint);
            url.text = "https://tiles.example.org/{z}/{y}/{x}/{z}.png";
            verify(!item("mapSettingsSave").inputValid, "a placeholder twice is refused, as the error says");
            url.text = "https://tiles.example.org/{z}/{x}/{y}.png";
            verify(item("mapSettingsSave").inputValid);
            verify(item("mapSettingsTiles").subtitle.indexOf("tiles.example.org") >= 0, item("mapSettingsTiles").subtitle);
            item("mapSettingsTiles").activate();
            item("mapSettingsSave").activate();
            verify(!settings.visible);
            compare(screen.tileUrl, "https://tiles.example.org/{z}/{x}/{y}.png");
            verify(screen.tilesEnabled);
            settings.openEditor();
            item("mapSettingsAttribution").text = "Example tiles";
            settings.requestDismiss();
            verify(settings.visible, "an edit is not dropped without asking");
            var discard = findChild(settings, "discardDialogDiscard");
            discard.activate();
            verify(!settings.visible);
            compare(screen.tileAttribution, screen.defaultAttribution);
        }

        // Review P7 #2: set aside, the tiles prompt stays away for the app's run (closing and opening the map does not
        // bring it back), and the empty state offers tiles instead.
        function test_11b_a_dismissed_tiles_prompt_stays_dismissed() {
            var prompt = item("mapTilesPrompt");
            verify(prompt.visible);
            verify(!item("mapEmptyLoadTiles").visible, "the prompt above already offers tiles");
            item("mapTilesNotNow").activate();
            verify(!prompt.visible);
            compare(item("mapArea").y, item("mapAgeFilter").mapToItem(screen, 0, item("mapAgeFilter").height).y + 10,
                    "the canvas takes the room back");
            verify(item("mapEmptyLoadTiles").visible, "the empty state offers tiles once the prompt is set aside");
            screen.visible = false;
            screen.visible = true;
            verify(!prompt.visible, "not asked again on the next visit");
            screen.tilesPromptDismissed = false;
        }

        // Clear asks first, then empties the map (History keeps its rows).
        function test_12_clear_asks_first() {
            push({source: 1001, lat: 10.0, lon: 20.0});
            item("mapMore").activate();
            var menu = item("mapOverflow");
            verify(menu.visible);
            findChild(menu, "mapMenuClear").activate();
            var confirm = item("mapClearConfirm");
            verify(confirm.visible);
            compare(positions.count, 1);
            confirm.confirm();
            compare(positions.count, 0);
        }

        // Reduced motion: the camera jumps; otherwise it glides for 250 ms.
        function test_13_reduced_motion_jumps_the_camera() {
            compare(canvas().cameraDurationMs, 0);
            Ui.Theme.reducedMotion = false;
            compare(canvas().cameraDurationMs, 250);
            Ui.Theme.reducedMotion = true;
            compare(canvas().cameraDurationMs, 0);
        }

        // Spec 4.7: Locate me asks the shared broker as "map", saves the one-shot fix and centres on it; it waits
        // while another screen's request runs, and takes no answer meant for another.
        function test_14_locate_me_through_the_broker() {
            var locate = item("mapLocate");
            verify(locate.visible);
            var other = locationRequests.requestSite("siteChooser");
            verify(other > 0);
            verify(!locate.enabled, "another screen is finding the location");
            locationRequests.cancel(other);
            verify(locate.enabled);
            locate.activate();
            compare(locationRequests.pendingRequester, "map");
            var id = item("mapLocator").requestId;
            verify(id > 0);
            decoderHost.locationResult(id + 1000, true, 1.0, 2.0, 5, savedSystems.realtimeNowMs(), false, "", "", "");
            compare(locationRequests.pendingRequester, "map", "an answer for another id changes nothing");
            decoderHost.locationResult(id, true, 10.5, 20.5, 12, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return !locationRequests.busy; });
            verify(canvas().deviceValid);
            tryVerify(function () { return Math.abs(canvas().centerLatitude - 10.5) < 0.001; }, 2000, "centred on the fix");
            compare(locate.Accessible.name, "Follow my position", "the next press follows");
            canvas().panBy(30, 0);
            item("mapZoomIn").activate();
            compare(locate.Accessible.name, "Locate me", "a moved view locates again");
        }

        // Review P7 #3: Follow my position holds the "map" lease only while the map shows in the foreground; the app
        // leaving lets it go and its return takes it again while follow is on; the List lets it go; a coarse-only
        // permission ends follow with words. The feed itself starts the platform feed once per take.
        function test_14b_follow_holds_the_lease_only_in_the_foreground() {
            testContext.setLifecycleLocationSupported(true);
            testContext.useLifecycleHost(true);
            verify(item("mapLocate").visible);
            var locator = item("mapLocator");
            locator.readyToFollow = true;
            compare(item("mapLocate").Accessible.name, "Follow my position");
            item("mapLocate").activate();
            verify(locator.followMe);
            verify(locationFeed.holds("map"), "following on a shown map holds the lease");
            var starts = testContext.roamingHostCalls().streamStarts;
            compare(starts, 1);
            // A live fix moves the dot; it is never saved.
            var savedAt = prefs.lastFixAt;
            testContext.emitLocationStreamUpdate({generation: 1, state: "ok", hasFix: true, provider: "gps", measurementNs: 1,
                                                  lat: 10.25, lon: 20.25, accuracyM: 8, fixAtMs: locationFeed.nowMs()});
            verify(locator.live);
            tryVerify(function () { return Math.abs(canvas().deviceLatitude - 10.25) < 1e-9; });
            compare(prefs.lastFixAt, savedAt, "a live fix is not saved");
            // Final review M5: the sheet measures from the live position then, and says so.
            push({source: 1001, lat: 10.3, lon: 20.3, label: "Engine 7"});
            screen.openTrack(trackNamed("Engine 7"), false);
            var words = sheetWords();
            verify(/ from you\b/.test(words) && words.indexOf("from your saved location") < 0, words);
            sheet().visible = false;
            testContext.emitApplicationState(Qt.ApplicationHidden);
            verify(!locationFeed.holds("map"), "the app away: let go");
            compare(testContext.roamingHostCalls().streamActive, false);
            testContext.emitApplicationState(Qt.ApplicationActive);
            verify(locationFeed.holds("map"), "back with follow on: taken again");
            compare(testContext.roamingHostCalls().streamStarts, starts + 1, "one start for the return, not two");
            screen.mapShown = false;
            verify(!locationFeed.holds("map"), "the List lets it go");
            screen.mapShown = true;
            verify(locationFeed.holds("map"));
            testContext.emitLocationStreamUpdate({generation: 1, state: "approximateOnly", hasFix: false});
            verify(!locator.followMe);
            verify(!locationFeed.holds("map"));
            compare(item("mapLocationNotice").text, "Live position needs precise location.");
            locator.clearNotice();
            testContext.useLifecycleHost(false);
            testContext.setLifecycleLocationSupported(false);
            testContext.setLocationSupported(true);
            testContext.useAppPrefs(true);
        }

        // Review P7 minor 4: a stopped replay's fix has no age; its received line still reads whole (the time alone).
        function test_14c_a_fix_with_no_age_still_reads_whole() {
            sheet().pin = null;
            sheet().trackId = 0;
            sheet().take({trackId: 7, label: "Unit 41", hasPosition: true, hasUnknownFix: false, ageSeconds: -1,
                          currentFix: {latitude: 10.0, longitude: 20.0, timeText: "12:04:33", newest: true}});
            sheet().visible = true;
            compare(item("mapShownFixReceived").text, "Received at 12:04:33");
            sheet().visible = false;
        }

        // Final review M1: the sheet and the List describe the fix the map draws. A weak-check, unconfirmed-format fix
        // is the filled marker (its double outline); a newer error-7 report is the open ring. The sheet says the
        // marker's states with its own readings beside "Position", and the ring's report on its own lines; the List
        // row says the marker's states and that a newer report may be invalid.
        function test_14d_the_sheet_and_list_describe_the_shown_fix() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", weakCheck: true, provisional: true, speedDkmh: 420,
                  ageSeconds: 30});
            push({source: 1001, lat: 10.01, lon: 20.01, label: "Engine 7", validityUnknown: true, weakCheck: true, altitudeDm: 1230});
            var id = trackNamed("Engine 7");
            screen.openTrack(id, false);
            var states = [0, 1, 2].map(function (i) { return item("mapShownFixState" + i); }).filter(function (t) {
                return t.visible;
            }).map(function (t) { return t.text; });
            compare(JSON.stringify(states), JSON.stringify(["Format not confirmed by a published specification", "Weak error check"]),
                    "the marker's own states");
            var shown = [];
            for (var i = 0; i < 9; ++i) {
                var line = item("mapShownFixLine" + i);
                if (line.visible)
                    shown.push(line.text);
            }
            verify(shown.indexOf("Position 10.00000° N, 20.00000° E") >= 0, JSON.stringify(shown));
            verify(shown.some(function (t) { return t.indexOf("Speed") === 0; }), "the marker's speed: " + JSON.stringify(shown));
            verify(!shown.some(function (t) { return t.indexOf("Altitude") === 0; }), "not the newer report's altitude");
            verify(item("mapRingFix").visible);
            compare(item("mapRingFixHeading").text, "Newer report");
            compare(item("mapRingFixState0").text, "Fix may be old or invalid");
            var ringLines = [];
            for (i = 0; i < 9; ++i) {
                if (item("mapRingFixLine" + i).visible)
                    ringLines.push(item("mapRingFixLine" + i).text);
            }
            verify(ringLines.indexOf("Position 10.01000° N, 20.01000° E") >= 0, JSON.stringify(ringLines));
            verify(ringLines.some(function (t) { return t.indexOf("Altitude") === 0; }), JSON.stringify(ringLines));
            sheet().visible = false;

            screen.mapShown = false;
            var list = item("mapList");
            tryVerify(function () { return list.itemAtIndex(0) !== null; });
            var row = list.itemAtIndex(positionView.rowForTrack(id));
            var said = row.node.Accessible.description;
            verify(said.indexOf("Weak error check") >= 0 && said.indexOf("Format not confirmed by a published specification") >= 0, said);
            verify(said.indexOf("Newer report may be old or invalid, weak error check") >= 0, "F2 review M2: the ring's own weak check: " + said);
            screen.mapShown = true;
        }

        // F2 review: the words follow the marks the map draws under the active filter. "Last 15 min" hides Engine 7's
        // 40-minute-old weak fix but shows its 5-minute-old error-7 ring: the List row says the ring's age and states
        // (no weak check, no "Newer report"), and the sheet's one block is the ring, under "Position".
        function test_14f_a_filtered_out_fix_is_not_described() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", weakCheck: true, speedDkmh: 420, ageSeconds: 2400});
            push({source: 1001, lat: 10.01, lon: 20.01, label: "Engine 7", validityUnknown: true, ageSeconds: 300});
            positions.ageFilterSeconds = 900;
            var id = trackNamed("Engine 7");
            compare(positions.trackFor(id).currentShown, false);
            compare(positions.trackFor(id).unknownShown, true);
            screen.mapShown = false;
            var list = item("mapList");
            tryVerify(function () { return list.itemAtIndex(positionView.rowForTrack(id)) !== null; });
            var row = list.itemAtIndex(positionView.rowForTrack(id));
            compare(row.ageShown, "5 min", "the drawn ring's age");
            var said = row.node.Accessible.description;
            verify(said.indexOf("Fix may be old or invalid") >= 0, said);
            verify(said.indexOf("Weak error check") < 0 && said.indexOf("Newer report") < 0, said);
            screen.mapShown = true;
            screen.openTrack(id, false);
            compare(item("mapShownFixHeading").text, "Position");
            compare(item("mapShownFixState0").text, "Fix may be old or invalid");
            verify(!item("mapRingFix").visible, "no second block for a ring that is the only mark drawn");
            var lines = [];
            for (var i = 0; i < 9; ++i) {
                if (item("mapShownFixLine" + i).visible)
                    lines.push(item("mapShownFixLine" + i).text);
            }
            verify(lines.indexOf("Position 10.01000° N, 20.01000° E") >= 0, JSON.stringify(lines));
            verify(!lines.some(function (t) { return t.indexOf("Speed") === 0; }), "not the hidden fix's speed");
            sheet().visible = false;
            positions.ageFilterSeconds = 0;
        }

        // Final review M2: after a loss (a gap row), a filter that hides every radio still shows the filtered-empty
        // state with Show all, on the map and in the List (under the List's gap row).
        function test_14e_a_gap_row_does_not_hide_the_filtered_empty_state() {
            push({source: 1001, lat: 10.0, lon: 20.0, ageSeconds: 2000});
            testContext.startNewPositionStream();
            compare(positionView.rowForTrack(0) >= 0, true, "the gap row is there");
            positions.ageFilterSeconds = 900;
            compare(positionView.trackCount, 0);
            verify(item("mapEmptyState").visible, "the map says the filter hides every radio");
            compare(item("mapEmptyTitle").text, "No positions in the last 15 minutes");
            screen.mapShown = false;
            var list = item("mapList");
            tryVerify(function () { return list.itemAtIndex(0) !== null && list.itemAtIndex(0).gap; });
            verify(item("mapPositionList").visible && item("mapEmptyState").visible);
            var show = item("mapShowAll");
            verify(show.visible);
            // F2 review M1: the gap row is read and tabbed before the empty state below it (item order).
            var area = item("mapArea");
            var kids = area.children;
            var at = function (it) { for (var k = 0; k < kids.length; ++k) { if (kids[k] === it) return k; } return -1; };
            verify(at(item("mapPositionList")) < at(item("mapEmptyScroll")), "the List comes before the empty state");
            verify(show.mapToItem(screen, 0, 0).y >= list.mapToItem(screen, 0, list.itemAtIndex(0).height).y - 1,
                   "Show all sits below the gap row");
            show.activate();
            compare(positions.ageFilterSeconds, 0);
            screen.mapShown = true;
        }

        // A large text size: nothing in the top bar, the filter or the controls runs off the screen's sides.
        function test_15_large_text_clips_nothing() {
            push({source: 1001, lat: 10.0, lon: 20.0});
            for (var scale of [1.0, 1.3, 1.6]) {
                Ui.Theme.fontScale = scale;
                for (var width of [360, 320]) {
                    root.width = width;
                    waitForRendering(screen);
                    for (var name of ["mapBack", "mapTitle", "mapViewChoice", "mapMore", "mapAge0", "mapAge1", "mapAge2",
                                      "mapZoomIn", "mapFit", "mapLocate", "mapAttribution"]) {
                        var it = item(name);
                        var left = it.mapToItem(screen, 0, 0).x;
                        var right = it.mapToItem(screen, it.width, 0).x;
                        verify(left >= -0.5 && right <= screen.width + 0.5,
                               name + " runs off the side at scale " + scale + ", width " + width + ": " + left + ".." + right);
                    }
                    verify(!item("mapAttributionText").truncated, "the attribution is owed whole, wrapped not cut");
                    if (width === 360 && scale <= 1.3)
                        compare(canvas().fitPadding, Math.round(72 * Ui.Theme.fontSize(12) / 12),
                                "on a phone a label's room is kept whole (re-review minor 4)");
                    var chrome = item("mapAgeFilter");
                    verify(chrome.mapToItem(screen, 0, chrome.height).y < screen.height / 2, "the map keeps most of the screen");
                }
            }
            root.width = 360;
            Ui.Theme.resetFontScale();
        }

        // Labels keep clear of what the screen lays over the canvas: the control column, the chips at the top and
        // the attribution are the canvas's label keep-outs, where they are drawn.
        function test_16_labels_keep_clear_of_the_controls_chips_and_attribution() {
            var c = canvas();
            var ctl = item("mapControls");
            var keepOut = c.labelObstacles;
            compare(keepOut.length, 3);
            var at = c.mapFromItem(ctl, 0, 0);
            compare(keepOut[0].x, at.x);
            compare(keepOut[0].y, at.y);
            compare(keepOut[0].width, ctl.width);
            compare(keepOut[0].height, ctl.height);
            var attribution = item("mapAttribution");
            var a = c.mapFromItem(attribution, 0, 0);
            compare(keepOut[2].x, a.x);
            compare(keepOut[2].y, a.y);
            compare(keepOut[2].width, attribution.width);
            // A chip shown at the top joins them.
            screen.tilesEnabled = true;
            testContext.setMapTilesFailing(true);
            var chip = item("mapTilesState");
            tryVerify(function () { return chip.visible; }, 5000);
            var status = item("mapStatus");
            tryVerify(function () { return c.labelObstacles[1].height > 0; });
            compare(c.labelObstacles[1].y, c.mapFromItem(status, 0, 0).y);
            compare(c.labelObstacles[1].height, status.height);
        }

        // Issue #902 B9: in List view the attribution chip hides; its name, role and description all go with it.
        function test_hidden_attribution_describes_nothing() {
            screen.mapShown = true;
            screen.tilesEnabled = true;
            var attribution = item("mapAttribution");
            tryVerify(function () { return attribution.visible; });
            screen.mapShown = false;
            try {
                verify(!attribution.visible, "List view hides it");
                compare(attribution.Accessible.name, "", "nameless");
                compare(attribution.Accessible.description, "", "and describes nothing");
                compare(attribution.Accessible.role, Accessible.Pane, "a plain pane");
            } finally {
                screen.mapShown = true;
            }
        }

        // ---- PR2 review round 1 ----

        // A modal over the map owns navigation: a drag or the wheel on the sheet's backdrop moves nothing under it.
        function test_17a_gestures_stand_down_under_a_sheet() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7"});
            var c = canvas();
            c.centerOn(10.0, 20.0, 12);
            screen.openTrack(trackNamed("Engine 7"), false);
            verify(sheet().visible);
            var lon = c.centerLongitude, zoom = c.zoom;
            mouseDrag(c, 60, 120, 140, 0);
            mouseWheel(c, 120, 140, 0, 480);
            waitForRendering(screen);
            verify(Math.abs(c.centerLongitude - lon) < 1e-9, "a drag on the backdrop moved the map: " + lon + " -> " + c.centerLongitude);
            verify(Math.abs(c.zoom - zoom) < 1e-9, "the wheel on the backdrop zoomed the map: " + zoom + " -> " + c.zoom);
            sheet().visible = false;
            mouseDrag(c, 60, 300, 140, 0);
            verify(Math.abs(c.centerLongitude - lon) > 1e-6, "uncovered, a drag pans");
        }

        // Roaming already holds the feed and it stands at "approximate only": following joins it and says at once
        // that precise location is needed, rather than "Finding your position…" for ever (the host sends no repeat).
        function test_17b_follow_reads_the_standing_record() {
            testContext.setLifecycleLocationSupported(true);
            testContext.useLifecycleHost(true);
            var locator = item("mapLocator");
            verify(locationFeed.acquire("roaming"));
            testContext.emitLocationStreamUpdate({generation: 1, state: "approximateOnly", hasFix: false});
            compare(locationFeed.record.state, "approximateOnly");
            locator.readyToFollow = true;
            item("mapLocate").activate();
            tryVerify(function () { return !locator.followMe; }, 1000, "follow ends on the standing answer");
            verify(!locationFeed.holds("map"));
            compare(item("mapLocationNotice").text, "Live position needs precise location.");
            locationFeed.release("roaming");
            locator.clearNotice();
            testContext.useLifecycleHost(false);
            testContext.setLifecycleLocationSupported(false);
            testContext.setLocationSupported(true);
            testContext.useAppPrefs(true);
        }

        // A live fix that no new record replaces goes stale: the dot stays on it, but it is the last known
        // position, not "you", and following says it is finding the position again. One already old when it comes
        // is never live.
        function test_17c_an_old_device_fix_is_not_live() {
            testContext.setLifecycleLocationSupported(true);
            testContext.useLifecycleHost(true);
            var locator = item("mapLocator");
            locator.maxFixAgeMs = 400;
            locator.readyToFollow = true;
            item("mapLocate").activate();
            verify(locator.followMe);
            testContext.emitLocationStreamUpdate({generation: 1, state: "ok", hasFix: true, provider: "gps", measurementNs: 1,
                                                  lat: 10.25, lon: 20.25, accuracyM: 8, fixAtMs: locationFeed.nowMs()});
            verify(locator.live);
            compare(item("mapLocationNotice").text, "");
            push({source: 1001, lat: 10.3, lon: 20.3, label: "Engine 7"});
            screen.openTrack(trackNamed("Engine 7"), false);
            verify(/ from you\b/.test(sheetWords()), sheetWords());
            tryVerify(function () { return !locator.live; }, 3000, "the fix ages out with no new record");
            verify(locator.lastKnown);
            verify(Math.abs(canvas().deviceLatitude - 10.25) < 1e-9, "the dot stays on the last fix");
            var words = sheetWords();
            verify(words.indexOf("from your last known position") >= 0 && !/ from you\b/.test(words), words);
            compare(item("mapLocationNotice").text, "Finding your position…");
            sheet().visible = false;
            // Already old when it comes.
            testContext.emitLocationStreamUpdate({generation: 1, state: "ok", hasFix: true, provider: "gps", measurementNs: 2,
                                                  lat: 10.26, lon: 20.26, accuracyM: 8, fixAtMs: locationFeed.nowMs() - 120000});
            verify(!locator.live, "a fix two minutes old on arrival is not live");
            // A fresh one is live again, and the notice goes.
            testContext.emitLocationStreamUpdate({generation: 1, state: "ok", hasFix: true, provider: "gps", measurementNs: 3,
                                                  lat: 10.27, lon: 20.27, accuracyM: 8, fixAtMs: locationFeed.nowMs()});
            verify(locator.live);
            compare(item("mapLocationNotice").text, "");
            locator.stopFollow();
            locator.maxFixAgeMs = 60000;
            locator.clearNotice();
            testContext.useLifecycleHost(false);
            testContext.setLifecycleLocationSupported(false);
            testContext.setLocationSupported(true);
            testContext.useAppPrefs(true);
        }

        // Showing another radio, a History pin or a located dot is a one-shot move: it ends a follow that would
        // otherwise take the camera back on its next fix (even a fix from a third radio).
        function test_17d_showing_another_position_ends_the_follow() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7"});
            push({source: 1002, lat: 11.0, lon: 21.0, label: "Medic 3"});
            var a = trackNamed("Engine 7"), b = trackNamed("Medic 3");
            var c = canvas();
            screen.follow(a);
            compare(c.followTrackId, a);
            screen.mapShown = false;
            screen.showOnMap(b);
            compare(c.followTrackId, 0, "Show on map for another radio ends the follow");
            push({source: 1003, lat: 12.0, lon: 22.0, label: "Unit 9"});
            push({source: 1001, lat: 10.01, lon: 20.01, label: "Engine 7"});
            verify(Math.abs(c.centerLatitude - 11.0) < 0.01, "the camera stays on the radio shown: " + c.centerLatitude);
            // Showing the radio followed keeps following it.
            screen.follow(a);
            screen.showOnMap(a);
            compare(c.followTrackId, a);
            // A History pin.
            screen.showPin({uid: 41, name: "CALL 041", when: 1700000000, tg: 100, src: 2001, sourceName: "",
                            position: {latitude: 13.0, longitude: 23.0, kindText: "LIP"}});
            compare(c.followTrackId, 0, "a pin ends the follow");
            screen.clearPin();
            // Locate me.
            screen.follow(a);
            item("mapLocate").activate();
            var id = item("mapLocator").requestId;
            verify(id > 0);
            decoderHost.locationResult(id, true, 14.0, 24.0, 12, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return !locationRequests.busy; });
            compare(c.followTrackId, 0, "a located dot ends the radio's follow");
            verify(Math.abs(c.centerLatitude - 14.0) < 0.01, "on the dot: " + c.centerLatitude);
            // Showing a radio then takes the view off the dot: the next press locates again.
            compare(item("mapLocate").Accessible.name, "Follow my position");
            screen.showOnMap(b);
            compare(item("mapLocate").Accessible.name, "Locate me", "the view left the dot");
        }

        // A Locate me arms Follow only while the view is on the dot it found: leaving the map puts the saved view
        // back, so the next visit's press locates again.
        function test_17e_leaving_resets_locate() {
            item("mapLocate").activate();
            var id = item("mapLocator").requestId;
            decoderHost.locationResult(id, true, 10.5, 20.5, 12, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return !locationRequests.busy; });
            compare(item("mapLocate").Accessible.name, "Follow my position");
            screen.visible = false;
            screen.visible = true;
            compare(item("mapLocate").Accessible.name, "Locate me", "a restored view locates again");
        }

        // Each report names who it was sent to: a valid fix to TG 1, then a newer validity-unknown one to TG 2. The
        // List row and the sheet's identity say TG 1 (the marker's); the ring's block says its own, TG 2.
        function test_17f_the_shown_report_names_its_recipient() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", targetId: 1, group: true, ageSeconds: 30});
            push({source: 1001, lat: 10.01, lon: 20.01, label: "Engine 7", targetId: 2, group: true, validityUnknown: true});
            var id = trackNamed("Engine 7");
            screen.openTrack(id, false);
            var identity = [0, 1, 2, 3].map(function (i) { return item("mapIdentityLine" + i).text; });
            verify(identity.indexOf("Talkgroup 1") >= 0 && identity.indexOf("Talkgroup 2") < 0, JSON.stringify(identity));
            var ring = [];
            for (var i = 0; i < 10; ++i) {
                if (item("mapRingFixLine" + i).visible)
                    ring.push(item("mapRingFixLine" + i).text);
            }
            verify(ring.indexOf("Talkgroup 2") >= 0, "the ring's own recipient: " + JSON.stringify(ring));
            sheet().visible = false;
            screen.mapShown = false;
            var list = item("mapList");
            tryVerify(function () { return list.itemAtIndex(positionView.rowForTrack(id)) !== null; });
            var said = list.itemAtIndex(positionView.rowForTrack(id)).node.Accessible.description;
            verify(said.indexOf("TG 1") >= 0 && said.indexOf("TG 2") < 0, said);
        }

        // With nothing heard, Locate me centres the dot where the "No positions yet" card would sit: the card moves
        // clear of it.
        function test_17g_the_empty_card_keeps_off_the_dot() {
            verify(screen.emptyLog);
            item("mapLocate").activate();
            var id = item("mapLocator").requestId;
            decoderHost.locationResult(id, true, 10.5, 20.5, 12, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return !locationRequests.busy; });
            waitForRendering(screen);
            var c = canvas();
            var dot = c.pointFor(10.5, 20.5);
            var card = item("mapEmptyState");
            verify(card.visible);
            var at = c.mapFromItem(card, 0, 0);
            var inside = dot.x >= at.x - 8 && dot.x <= at.x + card.width + 8 && dot.y >= at.y - 8 && dot.y <= at.y + card.height + 8;
            verify(!inside, "the dot " + dot + " is under the card at " + at + " " + card.width + "x" + card.height);
            item("mapLocator").cameraMoved();
        }

        // The device's dot is in the List in words, with no radio heard: where it is and how sure.
        function test_17h_the_list_has_the_device_dot() {
            prefs.setLocationFix(10.5, 20.5, savedSystems.realtimeNowMs(), 30);
            verify(screen.emptyLog);
            screen.mapShown = false;
            var listItem = item("mapPositionList");
            verify(listItem.visible, "the List shows for the device's dot alone");
            var row = item("mapDeviceRow");
            verify(row.visible);
            compare(row.Accessible.name, "Your saved location");
            var said = row.Accessible.description;
            verify(said.indexOf("Position 10.50000° N, 20.50000° E") >= 0 && said.indexOf("Accurate to") >= 0, said);
            verify(!locationFeed.holds("map"), "the List takes no live lease");
            row.activate();
            verify(screen.mapShown, "the row shows the dot on the map");
            // A radio followed: the row's one-shot move ends that follow, as Show on map's does.
            push({source: 1001, lat: 11.0, lon: 21.0, label: "Engine 7"});
            screen.follow(trackNamed("Engine 7"));
            screen.mapShown = false;
            row.activate();
            compare(canvas().followTrackId, 0, "showing the dot ends the radio's follow");
            item("mapLocator").cameraMoved();
        }

        // LIP's direction of travel 15 is 337.5° or not known: the sheet says both, and no opposite bearing.
        function test_17i_lip_heading_15_may_be_unknown() {
            sheet().pin = null;
            sheet().trackId = 0;
            sheet().take({trackId: 7, label: "Unit 41", hasPosition: true, hasUnknownFix: false, ageSeconds: 5,
                          currentFix: {latitude: 10.0, longitude: 20.0, timeText: "12:04:33", newest: true, headingDdeg: 3375,
                                       headingAmbiguous: true}});
            sheet().visible = true;
            var words = sheetWords();
            verify(words.indexOf("Heading 338° (north), or the direction is not known") >= 0, words);
            verify(words.indexOf("opposite") < 0, words);
            sheet().visible = false;
        }

        // The pin is drawn over a radio's marker at the same place: a tap on the pin opens the pin's sheet.
        function test_17j_the_pin_is_tapped_first() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7"});
            screen.showPin({uid: 41, name: "CALL 041", when: 1700000000, tg: 100, src: 2001, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, kindText: "LIP"}});
            waitForRendering(screen);
            var pin = item("mapPin");
            verify(canvas().trackAt(pin.tipX, pin.tipY - 20) > 0, "the marker is under the pin's head");
            mouseClick(canvas(), pin.tipX, pin.tipY - 20);
            verify(sheet().visible);
            verify(sheet().pin !== null, "the pin's sheet, not the radio's");
            sheet().visible = false;
        }

        // OpenStreetMap's own tiles need its credit: the form does not save their address without it, and a credit
        // saved without it is shown with it. Another server takes any credit.
        function test_17k_the_osm_credit_stays() {
            var settings = item("mapSettingsSheet");
            settings.openEditor();
            var attribution = item("mapSettingsAttribution");
            attribution.text = "My own credit";
            verify(!item("mapSettingsSave").inputValid, "the default tiles without their credit do not save");
            verify(attribution.error.indexOf("© OpenStreetMap contributors") >= 0, attribution.error);
            attribution.text = "© OpenStreetMap contributors, styled locally";
            verify(item("mapSettingsSave").inputValid);
            item("mapSettingsUrl").text = "https://tiles.example/{z}/{x}/{y}.png";
            attribution.text = "My own credit";
            verify(item("mapSettingsSave").inputValid, "another server takes any credit");
            settings.closeEditor();
            screen.tileAttribution = "My own credit";
            verify(item("mapAttributionText").text.indexOf("© OpenStreetMap contributors") >= 0, item("mapAttributionText").text);
            screen.tileUrl = "https://tiles.example/{z}/{x}/{y}.png";
            compare(item("mapAttributionText").text, "My own credit");
        }

        // A LIP report with no time of its own says how old the position was when sent, at most: the sheet in its
        // Measured line, the List from 5 minutes up (its row age is only when the report came).
        function test_17m_lip_says_how_old_the_position_was_when_sent() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", kind: 5, ageClass: 2, ageSeconds: 20});
            push({source: 1002, lat: 10.1, lon: 20.1, label: "Medic 3", kind: 12, ageClass: 0, ageSeconds: 20});
            var e7 = trackNamed("Engine 7"), m3 = trackNamed("Medic 3");
            screen.openTrack(e7, false);
            compare(item("mapShownFixMeasured").text, "Measured less than 30 minutes before it was sent");
            sheet().visible = false;
            screen.openTrack(m3, false);
            compare(item("mapShownFixMeasured").text, "Measured less than 5 seconds before it was sent");
            sheet().visible = false;
            screen.mapShown = false;
            var list = item("mapList");
            tryVerify(function () { return list.itemAtIndex(positionView.rowForTrack(e7)) !== null; });
            var said = list.itemAtIndex(positionView.rowForTrack(e7)).node.Accessible.description;
            verify(said.indexOf("Measured less than 30 minutes before it was sent") >= 0, said);
            tryVerify(function () { return list.itemAtIndex(positionView.rowForTrack(m3)) !== null; });
            said = list.itemAtIndex(positionView.rowForTrack(m3)).node.Accessible.description;
            verify(said.indexOf("Measured") < 0, "under 5 s is not worth a List line: " + said);
        }

        // The pin leaves the map as the marks do: its layer is clipped to the canvas.
        function test_17l_the_pin_is_clipped_to_the_canvas() {
            screen.showPin({uid: 41, name: "CALL 041", when: 1700000000, tg: 100, src: 2001, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, kindText: "LIP"}});
            var pin = item("mapPin");
            var layer = pin.parent;
            var c = canvas();
            verify(layer.clip, "the pin's layer clips");
            var at = c.mapFromItem(layer, 0, 0);
            verify(at.x === 0 && at.y === 0 && layer.width === c.width && layer.height === c.height, "to the canvas");
        }

        // ---- PR2 review round 2 ----

        function resize(w, h) {
            root.width = w;
            root.height = h;
            waitForRendering(screen);
        }
        function overlaps(a, b) {
            return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
        }

        // A short landscape canvas under the tiles prompt: the controls lie in a row (then a flat pair) inside the
        // canvas, never over the prompt, at every text size; a tap on "Not now" is "Not now".
        function test_18a_controls_stay_in_the_canvas() {
            for (var scale of [1.0, 1.3, 1.6]) {
                Ui.Theme.fontScale = scale;
                resize(640, 320);
                verify(item("mapTilesPrompt").visible);
                var controls = item("mapControls");
                var c = canvas();
                var at = c.mapFromItem(controls, 0, 0);
                verify(at.y >= 0, "the controls reach above the canvas at " + scale + "x: " + at.y);
                var prompt = item("mapTilesPrompt");
                var p = screen.mapFromItem(prompt, 0, 0), q = screen.mapFromItem(controls, 0, 0);
                verify(!overlaps(Qt.rect(p.x, p.y, prompt.width, prompt.height), Qt.rect(q.x, q.y, controls.width, controls.height)),
                       "the controls overlap the prompt at " + scale + "x");
                var notNow = item("mapTilesNotNow");
                var n = screen.mapFromItem(notNow, notNow.width / 2, notNow.height / 2);
                var zoom = c.zoom;
                mouseClick(screen, n.x, n.y);
                verify(screen.tilesPromptDismissed, "Not now at " + scale + "x");
                verify(Math.abs(c.zoom - zoom) < 1e-9, "no zoom from the prompt's tap");
                screen.tilesPromptDismissed = false;
            }
            Ui.Theme.resetFontScale();
            resize(360, 780);
            compare(item("mapControls").arrangement, 0, "a column on a phone in portrait");
        }

        // Review r2 I2: one rule keeps the chips at the top and the controls at the bottom apart at every size: with
        // three chips (the pin, a follow, a location notice) the chip band stays on the canvas, never under a control,
        // and never squeezed into a tall narrow column; the pin's Details button scrolls into the band and is never
        // under a control. Phone and split-screen sizes at 1.0 and 1.6, landscape, a tablet.
        function test_18a2_chips_and_controls_never_collide_data() {
            return [{tag: "360x420 1.0", w: 360, h: 420, s: 1.0}, {tag: "360x420 1.6", w: 360, h: 420, s: 1.6},
                    {tag: "500x440 1.0", w: 500, h: 440, s: 1.0}, {tag: "500x440 1.6", w: 500, h: 440, s: 1.6},
                    {tag: "640x320 1.6", w: 640, h: 320, s: 1.6}, {tag: "360x780 1.6", w: 360, h: 780, s: 1.6},
                    {tag: "1280x720 1.0", w: 1280, h: 720, s: 1.0}].concat(narrow());
        }
        // Re-review R1: 320 px wide (a 360 dp phone at a larger display size, or split screen).
        function narrow() {
            var rows = [];
            for (var h of [420, 480, 520])
                for (var sc of [1.0, 1.3, 1.6])
                    rows.push({tag: "320x" + h + " " + sc, w: 320, h: h, s: sc});
            return rows;
        }
        function test_18a2_chips_and_controls_never_collide(data) {
            push({source: 1001, lat: 11.0, lon: 21.0, label: "Engine 7"});
            screen.tilesPromptDismissed = true;
            item("mapLocator").notice = "Location unavailable. The dot shows your saved location.";
            screen.showPin({uid: 41, name: "CALL 041", when: 1700000000, tg: 100, src: 2001, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, kindText: "LIP"}});
            screen.follow(trackNamed("Engine 7"));
            Ui.Theme.fontScale = data.s;
            resize(data.w, data.h);
            verify(item("mapLocationNotice").visible && item("mapPinChip").visible && item("mapFollowing").visible);
            waitForRendering(screen);
            var status = item("mapStatus"), controls = item("mapControls"), c = canvas();
            var a = c.mapFromItem(status, 0, 0), b = c.mapFromItem(controls, 0, 0);
            var band = Qt.rect(a.x, a.y, status.width, status.height), ctl = Qt.rect(b.x, b.y, controls.width, controls.height);
            verify(!overlaps(band, ctl), "chips " + band + " over controls " + ctl);
            verify(a.y >= 0 && a.y + status.height <= c.height && b.y >= 0 && b.y + controls.height <= c.height + 0.5,
                   "both on the canvas: chips " + band + ", controls " + ctl + ", canvas " + c.height);
            verify(status.width >= Math.min(220, c.width - 24), "not a tall narrow column: " + status.width);
            // A floor: the band shows at least one chip (40 px). On the tightest canvas (170 px: a 420 px window at 1.6x
            // text, the credit stacked over flat controls, every touch target kept at 48 px) it is a scrolling strip
            // of at least a line of chip text.
            var floor = c.height >= 180 ? 40 : 28;
            verify(status.height >= Math.min(floor, status.contentHeight), "the chips have a band: " + status.height + " of " + status.contentHeight + " (canvas " + c.height + ")");
            verify(c.topInset <= c.height, "the centring aims inside the canvas: " + c.topInset + " of " + c.height);
            verify(!item("mapAttributionText").truncated, "the credit is never cut: " + item("mapAttribution").width);
            // The pin's Details, scrolled into the band if it has to be, is never under a control.
            var pinChip = item("mapPinChip");
            var details = item("mapPinChipAction");
            verify(details !== null && details.visible);
            var inContent = status.contentItem.mapFromItem(details, 0, 0);
            status.contentY = Math.max(0, Math.min(status.contentHeight - status.height, inContent.y + details.height - status.height));
            var d = c.mapFromItem(details, 0, 0);
            // Wholly when the band is that tall; on the tightest canvas (a split-screen phone at 1.6x) the band is
            // a little shorter than the button and the button fills it.
            var top = Math.max(d.y, a.y), bottom = Math.min(d.y + details.height, a.y + status.height);
            verify(bottom - top >= Math.min(details.height, status.height) - 0.5, "Details scrolls into the band: " + d + " in " + band);
            verify(!overlaps(Qt.rect(d.x, d.y, details.width, details.height), ctl), "Details " + d + " under a control " + ctl);
            status.contentY = 0;
            item("mapLocator").clearNotice();
            Ui.Theme.resetFontScale();
            resize(360, 780);
        }

        // Review r2 M3: on the shortest screens at a large text size the tiles prompt scrolls in its band, so the
        // canvas keeps room for the controls lying flat and none reaches over the prompt.
        function test_18a3_the_prompt_leaves_room_for_the_controls_data() {
            return [{tag: "568x320", w: 568, h: 320, s: 1.6}, {tag: "600x280", w: 600, h: 280, s: 1.6}].concat(narrow());
        }
        function test_18a3_the_prompt_leaves_room_for_the_controls(data) {
            Ui.Theme.fontScale = data.s;
            resize(data.w, data.h);
            verify(item("mapTilesPrompt").visible);
            var c = canvas(), controls = item("mapControls");
            var b = c.mapFromItem(controls, 0, 0);
            verify(b.y >= 0, "the controls reach above the canvas: " + b.y);
            var band = item("mapTilesPromptScroll");
            var p = screen.mapFromItem(band, 0, 0), q = screen.mapFromItem(controls, 0, 0);
            verify(!overlaps(Qt.rect(p.x, p.y, band.width, band.height), Qt.rect(q.x, q.y, controls.width, controls.height)));
            verify(b.y + controls.height <= c.height + 0.5, "the controls stay on the canvas");
            verify(!item("mapAttributionText").truncated, "the credit is never cut");
            var credit = item("mapAttribution"), r = screen.mapFromItem(credit, 0, 0);
            verify(!overlaps(Qt.rect(p.x, p.y, band.width, band.height), Qt.rect(r.x, r.y, credit.width, credit.height)),
                   "the credit stays off the prompt: " + r + " vs band bottom " + (p.y + band.height));
            if (data.h <= 320)
                verify(band.contentHeight > band.height, "the prompt scrolls here: " + band.contentHeight + " in " + band.height);
            Ui.Theme.resetFontScale();
            resize(360, 780);
        }

        // Following a radio takes the view off the found dot: the next press locates again.
        function test_18b_follow_resets_locate() {
            push({source: 1001, lat: 11.0, lon: 21.0, label: "Engine 7"});
            item("mapLocate").activate();
            var id = item("mapLocator").requestId;
            decoderHost.locationResult(id, true, 10.5, 20.5, 12, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return !locationRequests.busy; });
            compare(item("mapLocate").Accessible.name, "Follow my position");
            screen.follow(trackNamed("Engine 7"));
            compare(item("mapLocate").Accessible.name, "Locate me");
        }

        // Review r2 I3: a Locate me still running does not undo a later explicit camera choice: its answer saves the
        // dot, but the follow (or Show on map, or a pin) stays and the button still locates.
        function test_18k_a_late_locate_answer_keeps_the_choice() {
            push({source: 1001, lat: 11.0, lon: 21.0, label: "Engine 7"});
            push({source: 1002, lat: 12.0, lon: 22.0, label: "Medic 3"});
            var c = canvas();
            var answer = function (lat) {
                var id = item("mapLocator").requestId;
                verify(id > 0, "a request in flight");
                decoderHost.locationResult(id, true, lat, 20.5, 12, savedSystems.realtimeNowMs(), false, "", "", "");
                tryVerify(function () { return !locationRequests.busy; });
            };
            item("mapLocate").activate();
            screen.follow(trackNamed("Engine 7"));
            answer(10.5);
            compare(c.followTrackId, trackNamed("Engine 7"), "the follow chosen meanwhile stays");
            compare(item("mapLocate").Accessible.name, "Locate me");
            verify(Math.abs(prefs.lastLat - 10.5) < 1e-6, "the answer still saves the dot");
            c.followTrackId = 0;
            item("mapLocate").activate();
            screen.showOnMap(trackNamed("Medic 3"));
            answer(10.6);
            verify(Math.abs(c.centerLatitude - 12.0) < 0.01, "Show on map's view stays: " + c.centerLatitude);
            compare(item("mapLocate").Accessible.name, "Locate me");
            // Re-review r3: the zoom buttons and the wheel are explicit camera actions too.
            for (var act of ["zoomIn", "wheel"]) {
                c.centerOn(12.0, 22.0, 12);
                item("mapLocate").activate();
                if (act === "zoomIn")
                    item("mapZoomIn").activate();
                else
                    mouseWheel(c, c.width / 2, c.height / 2, 0, 120);
                answer(10.65);
                verify(Math.abs(c.centerLatitude - 10.65) > 0.5, act + " keeps the view: " + c.centerLatitude);
                compare(item("mapLocate").Accessible.name, "Locate me", act);
            }
            // Uninterrupted, it still centres and arms Follow.
            item("mapLocate").activate();
            answer(10.7);
            verify(Math.abs(c.centerLatitude - 10.7) < 0.01, "a plain Locate centres: " + c.centerLatitude);
            compare(item("mapLocate").Accessible.name, "Follow my position");
            item("mapLocator").cameraMoved();
        }

        // The pin's and the device's rows scroll with the radios: a short landscape List at a large text size still
        // reaches every radio.
        function test_18c_the_leading_rows_scroll_with_the_list() {
            prefs.setLocationFix(10.5, 20.5, savedSystems.realtimeNowMs(), 30);
            push({source: 1001, lat: 11.0, lon: 21.0, label: "Engine 7"});
            screen.showPin({uid: 41, name: "CALL 041", when: 1700000000, tg: 100, src: 2001, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, kindText: "LIP", validityUnknown: true}});
            Ui.Theme.fontScale = 1.6;
            resize(640, 320);
            screen.mapShown = false;
            var list = item("mapList");
            verify(list.height > 0, "the List has room: " + list.height);
            var row = positionView.rowForTrack(trackNamed("Engine 7"));
            list.positionViewAtIndex(row, ListView.Contain);
            var radio = list.itemAtIndex(row);
            verify(radio !== null);
            var at = list.mapFromItem(radio, 0, 0);
            verify(at.y >= 0 && at.y + radio.height <= list.height + 1, "the radio scrolls into view: " + at.y + " in " + list.height);
            verify(item("mapPinRow").visible && item("mapDeviceRow").visible);
            Ui.Theme.resetFontScale();
            resize(360, 780);
        }

        // OpenStreetMap's server is known by its host name however the address writes it: a port, a lettered name,
        // upper case. Its credit is owed and links to its copyright page.
        function test_18d_osm_by_host_name() {
            var settings = item("mapSettingsSheet");
            settings.openEditor();
            item("mapSettingsUrl").text = "https://tile.openstreetmap.org:443/{z}/{x}/{y}.png";
            item("mapSettingsAttribution").text = "My own credit";
            verify(!item("mapSettingsSave").inputValid, "an explicit :443 is the same server");
            settings.closeEditor();
            for (var url of ["https://tile.openstreetmap.org:443/{z}/{x}/{y}.png", "https://a.tile.openstreetmap.org/{z}/{x}/{y}.png",
                             "https://TILE.OpenStreetMap.org/{z}/{x}/{y}.png"]) {
                screen.tileUrl = url;
                screen.tileAttribution = "My own credit";
                verify(item("mapAttributionText").text.indexOf("© OpenStreetMap contributors") >= 0, url + ": " + item("mapAttributionText").text);
                verify(screen.attributionLinked, url + " links to the copyright page");
            }
            screen.tileUrl = "https://tiles.example/{z}/{x}/{y}.png";
            verify(!screen.attributionLinked);
        }

        // An anonymous report is a single point: Show on map, never Follow.
        function test_18e_an_anonymous_point_is_not_followed() {
            push({anonymous: true, lat: 10.0, lon: 20.0});
            var id = trackNamed("");
            verify(id > 0);
            screen.openTrack(id, true);
            verify(!item("mapPositionFollow").visible, "no Follow for a single point");
            verify(item("mapPositionShowOnMap").visible);
            sheet().visible = false;
            screen.follow(id);
            compare(canvas().followTrackId, 0, "nothing follows a single point");
        }

        // An anonymous report keeps the recipient its checked provenance named.
        function test_18f_an_anonymous_report_keeps_its_recipient() {
            push({anonymous: true, lat: 10.0, lon: 20.0, targetId: 9, group: true});
            screen.openTrack(trackNamed(""), false);
            var identity = [0, 1, 2, 3].map(function (i) { return item("mapIdentityLine" + i).text; });
            verify(identity.indexOf("Talkgroup 9") >= 0, JSON.stringify(identity));
            verify(identity[0].indexOf("could not be confirmed") >= 0, "still not confirmed");
        }

        // Review r2 I1: the List opens at its start, the pin's and the device's rows in view, on the first switch to
        // it (a freshly built screen) and on every later one, with more radios than fit.
        function test_18c2_the_list_opens_at_its_start() {
            loader.active = false;
            loader.active = true;
            tryVerify(function () { return loader.item !== null; });
            prefs.setLocationFix(10.5, 20.5, savedSystems.realtimeNowMs(), 30);
            for (var i = 0; i < 15; ++i)
                push({source: 2001 + i, lat: 10.0 + i * 0.01, lon: 20.0, label: "Unit " + (i + 1)});
            screen.showPin({uid: 41, name: "CALL 041", when: 1700000000, tg: 100, src: 2001, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, kindText: "LIP"}});
            var check = function (when) {
                var list = item("mapList");
                tryVerify(function () { return list.headerItem !== null && list.headerItem.height > 0; }, 1000, when);
                waitForRendering(screen);
                verify(Math.abs(list.contentY - list.originY) < 0.5, when + ": at the start, contentY " + list.contentY + " origin " + list.originY);
                for (var name of ["mapPinRow", "mapDeviceRow"]) {
                    var row = item(name);
                    var at = list.mapFromItem(row, 0, 0);
                    verify(row.visible && at.y >= -0.5 && at.y + row.height <= list.height + 0.5, when + ": " + name + " in view at " + at.y);
                }
            };
            screen.mapShown = false;
            check("first switch");
            screen.mapShown = true;
            push({source: 3001, lat: 10.2, lon: 20.2, label: "Unit 99"});
            screen.mapShown = false;
            check("Map then List again, a radio heard meanwhile");
            // A reader who scrolled further back keeps their place (issue #782's rule), as every list does.
            var list = item("mapList");
            list.positionViewAtEnd();
            var y = list.contentY;
            screen.mapShown = true;
            screen.mapShown = false;
            waitForRendering(screen);
            verify(Math.abs(list.contentY - y) < Ui.Theme.rowHeight, "a scrolled reader keeps their place: " + list.contentY + " vs " + y);
        }

        // A History row's target is its call's: a talkgroup only on the saved report's word.
        function test_18g_a_pin_says_talkgroup_only_when_it_is_one() {
            screen.showPin({uid: 41, name: "CALL 041", when: 1700000000, tg: 42, src: 2001, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, kindText: "LRRP", targetGroup: false}});
            screen.openPinDetails();
            var identity = [0, 1, 2, 3].map(function (i) { return item("mapIdentityLine" + i).text; });
            verify(identity.indexOf("Sent to 42") >= 0 && identity.indexOf("Talkgroup 42") < 0, JSON.stringify(identity));
            sheet().visible = false;
            screen.showPin({uid: 42, name: "CALL 042", when: 1700000000, tg: 42, src: 2001, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, kindText: "LRRP", targetGroup: true}});
            screen.openPinDetails();
            identity = [0, 1, 2, 3].map(function (i) { return item("mapIdentityLine" + i).text; });
            verify(identity.indexOf("Talkgroup 42") >= 0, JSON.stringify(identity));
            sheet().visible = false;
            // The row's own kind (gi) comes first: a private call is a radio whatever the position's flag, a group
            // call a talkgroup even where the position's flag was cleared (an unverified report).
            for (var c of [[1, true, "Sent to radio 42"], [0, false, "Talkgroup 42"]]) {
                screen.showPin({uid: 43, name: "CALL 043", when: 1700000000, tg: 42, gi: c[0], src: 2001, sourceName: "",
                                position: {latitude: 10.0, longitude: 20.0, kindText: "LRRP", targetGroup: c[1]}});
                screen.openPinDetails();
                identity = [0, 1, 2, 3].map(function (i) { return item("mapIdentityLine" + i).text; });
                verify(identity.indexOf(c[2]) >= 0, c[2] + ": " + JSON.stringify(identity));
                sheet().visible = false;
            }
        }

        // Zero satellites is a reported count, said as such; none reported says nothing.
        function test_18h_zero_satellites_is_a_count() {
            sheet().pin = null;
            sheet().trackId = 0;
            for (var c of [[0, "0 satellites"], [1, "1 satellite"], [7, "7 satellites"]]) {
                sheet().take({trackId: 7, label: "Unit 41", hasPosition: true, hasUnknownFix: false, ageSeconds: 5,
                              currentFix: {latitude: 10.0, longitude: 20.0, timeText: "12:04:33", newest: true, sats: c[0]}});
                sheet().visible = true;
                verify(sheetWords().indexOf(c[1]) >= 0, c[1] + ":\n" + sheetWords());
                sheet().visible = false;
            }
            sheet().take({trackId: 7, label: "Unit 41", hasPosition: true, hasUnknownFix: false, ageSeconds: 5,
                          currentFix: {latitude: 10.0, longitude: 20.0, timeText: "12:04:33", newest: true}});
            sheet().visible = true;
            verify(sheetWords().indexOf("satellite") < 0, sheetWords());
            sheet().visible = false;
        }

        // A sheet left open while the age filter hides its radio: Show on map waits (the camera would find nothing)
        // and the sheet says why.
        function test_18i_show_on_map_waits_for_a_mark() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", ageSeconds: 600});
            positions.ageFilterSeconds = 900;
            screen.mapShown = false;
            screen.openTrack(trackNamed("Engine 7"), true);
            verify(item("mapPositionShowOnMap").enabled);
            verify(!item("mapPositionFiltered").visible);
            testContext.setPositionWallSeconds(testContext.positionWallStart() + 400);
            testContext.refreshPositionAges();
            tryVerify(function () { return !item("mapPositionShowOnMap").enabled; }, 3000, "no mark to show");
            verify(item("mapPositionFiltered").visible);
            verify(!item("mapPositionFollow").enabled, "nothing drawn to start following");
            sheet().visible = false;
            positions.ageFilterSeconds = 0;
        }

        // A report's own time with no age to say it by reads as a time, with its date when it is not today.
        function test_18l_measured_at_has_its_date_when_not_today() {
            sheet().pin = null;
            sheet().trackId = 0;
            // One fixed sample for the local day and the times below (issue
            // #902), so a run across midnight reads them on one day.
            var now = Math.floor(new Date(2025, 4, 20, 9, 30, 0).getTime() / 1000);
            testContext.setLocalNowMs(now * 1000);
            var take = function (fixTime) {
                sheet().take({trackId: 7, label: "Unit 41", hasPosition: true, hasUnknownFix: false, ageSeconds: -1,
                              currentFix: {latitude: 10.0, longitude: 20.0, timeText: "12:04:33", newest: true, fixTime: fixTime}});
                sheet().visible = true;
                return item("mapShownFixMeasured").text;
            };
            var old = now - 3 * 86400;
            compare(take(old), "Measured at " + Qt.formatDateTime(new Date(old * 1000), "yyyy-MM-dd hh:mm:ss"));
            sheet().visible = false;
            var d = new Date(now * 1000);
            var noon = Math.floor(new Date(d.getFullYear(), d.getMonth(), d.getDate(), 12, 0, 0).getTime() / 1000);
            compare(take(noon), "Measured at 12:00:00");
            sheet().visible = false;
            // Re-review r2: "Received at" by the same rule.
            var received = function (rx) {
                sheet().take({trackId: 7, label: "Unit 41", hasPosition: true, hasUnknownFix: false, ageSeconds: -1,
                              currentFix: {latitude: 10.0, longitude: 20.0, timeText: "12:04:33", newest: true, receivedTime: rx}});
                sheet().visible = true;
                return item("mapShownFixReceived").text;
            };
            compare(received(old), "Received at " + Qt.formatDateTime(new Date(old * 1000), "yyyy-MM-dd hh:mm:ss"));
            sheet().visible = false;
            compare(received(noon), "Received at 12:00:00");
            sheet().visible = false;
        }

        // Re-review 2 s1: Tab into a scrolling band (the chips', the tiles prompt's) brings the focused action into
        // its view, as the other scrolling screens do, where the band clips it.
        function test_18m_tab_reveals_a_clipped_action_data() {
            return [{tag: "320x420 1.6", w: 320, h: 420, s: 1.6}, {tag: "320x480 1.3", w: 320, h: 480, s: 1.3}];
        }
        function test_18m_tab_reveals_a_clipped_action(data) {
            push({source: 1001, lat: 11.0, lon: 21.0, label: "Engine 7"});
            screen.tilesPromptDismissed = true;
            screen.showPin({uid: 41, name: "CALL 041", when: 1700000000, tg: 100, src: 2001, sourceName: "",
                            position: {latitude: 10.0, longitude: 20.0, kindText: "LIP"}});
            screen.follow(trackNamed("Engine 7"));
            item("mapLocator").notice = "Location unavailable. The dot shows your saved location.";
            Ui.Theme.fontScale = data.s;
            resize(data.w, data.h);
            var status = item("mapStatus");
            verify(status.contentHeight > status.height, "the chips band clips here");
            // Wholly in the band, or filling it where the band is shorter than the action (the tightest canvas).
            var inBand = function (band, it) {
                var at = band.mapFromItem(it, 0, 0);
                var shown = Math.min(at.y + it.height, band.height) - Math.max(at.y, 0);
                return shown >= Math.min(it.height, band.height) - 0.5;
            };
            var last = item("mapLocationNoticeAction");
            verify(!inBand(status, last), "the last action starts clipped");
            item("mapPinChipAction").forceActiveFocus();
            for (var i = 0; i < 6 && !last.activeFocus; ++i)
                keyClick(Qt.Key_Tab);
            verify(last.activeFocus, "Tab reaches the notice's action");
            tryVerify(function () { return inBand(status, last); }, 1000, "the focused action scrolls into the band");
            item("mapLocator").clearNotice();
            // The tiles prompt's band.
            screen.clearPin();
            canvas().followTrackId = 0;
            screen.tilesPromptDismissed = false;
            var prompt = item("mapTilesPromptScroll");
            var load = item("mapLoadTiles");
            verify(prompt.contentHeight > prompt.height, "the prompt clips here");
            verify(!inBand(prompt, load), "Load map tiles starts clipped");
            item("mapTilesNotNow").forceActiveFocus();
            for (i = 0; i < 4 && !load.activeFocus; ++i)
                keyClick(Qt.Key_Tab, Qt.ShiftModifier);
            if (!load.activeFocus) {
                item("mapTilesNotNow").forceActiveFocus();
                for (i = 0; i < 4 && !load.activeFocus; ++i)
                    keyClick(Qt.Key_Tab);
            }
            verify(load.activeFocus, "Tab reaches Load map tiles");
            tryVerify(function () { return inBand(prompt, load); }, 1000, "the focused button scrolls into the prompt's band");
            Ui.Theme.resetFontScale();
            resize(360, 780);
        }

        // Re-review 2 s2: a List row's time, and the gap row's, carry the date when not today.
        function test_18n_list_times_have_their_date() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", ageSeconds: 30});
            testContext.startNewPositionStream();
            screen.mapShown = false;
            var list = item("mapList");
            // The local day pinned to one sample (issue #902), so a run across midnight compares on one day.
            var nowMs = savedSystems.realtimeNowMs();
            testContext.setLocalNowMs(nowMs);
            var expected = function (seconds) {
                var at = new Date(seconds * 1000), now = new Date(nowMs);
                var same = at.getFullYear() === now.getFullYear() && at.getMonth() === now.getMonth() && at.getDate() === now.getDate();
                return Qt.formatDateTime(at, same ? "hh:mm:ss" : "yyyy-MM-dd hh:mm:ss");
            };
            var id = trackNamed("Engine 7");
            tryVerify(function () { return list.itemAtIndex(positionView.rowForTrack(id)) !== null; });
            var row = list.itemAtIndex(positionView.rowForTrack(id));
            compare(row.clock, expected(positions.trackFor(id).currentFix.receivedTime));
            var gapRow = list.itemAtIndex(positionView.rowForTrack(0));
            verify(gapRow !== null && gapRow.gap);
            var gapTime = positions.trackFor(0).receivedTime;
            if (gapTime > 0)
                compare(gapRow.gapTitle, "Positions may be missing from " + expected(gapTime));
        }

        // Issue #902 B1: the sheet's Measured and Received times, a List row's
        // and the gap row's follow the local day on the same items: a time of
        // today takes its date at midnight. Literal local times in a zone west
        // of UTC.
        function test_18o_times_follow_the_local_day() {
            if (!testContext.localTimeZoneIs("America/Los_Angeles"))
                skip("this run is not in America/Los_Angeles (CTest starts it with TZ set; Windows reads no IANA name in TZ)");
            testContext.setLocalNowMs(Date.UTC(2025, 5, 15, 3, 0, 0));
            sheet().pin = null;
            sheet().trackId = 0;
            var noon = Date.UTC(2025, 5, 14, 19, 0, 0) / 1000;
            sheet().take({trackId: 7, label: "Unit 41", hasPosition: true, hasUnknownFix: false, ageSeconds: -1,
                          currentFix: {latitude: 10.0, longitude: 20.0, timeText: "12:04:33", newest: true, fixTime: noon, receivedTime: noon}});
            sheet().visible = true;
            var measured = item("mapShownFixMeasured");
            var received = item("mapShownFixReceived");
            compare(measured.text, "Measured at 12:00:00");
            compare(received.text, "Received at 12:00:00");
            testContext.setLocalNowMs(Date.UTC(2025, 5, 15, 7, 0, 5));
            compare(measured.text, "Measured at 2025-06-14 12:00:00", "midnight dates the measured time");
            compare(received.text, "Received at 2025-06-14 12:00:00", "and the received time");
            sheet().visible = false;

            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", ageSeconds: 30});
            testContext.startNewPositionStream();
            screen.mapShown = false;
            var list = item("mapList");
            var id = trackNamed("Engine 7");
            tryVerify(function () { return list.itemAtIndex(positionView.rowForTrack(id)) !== null; });
            var row = list.itemAtIndex(positionView.rowForTrack(id));
            var gapRow = list.itemAtIndex(positionView.rowForTrack(0));
            var rx = positions.trackFor(id).currentFix.receivedTime;
            var gapTime = positions.trackFor(0).receivedTime;
            verify(rx > 0 && gapRow !== null && gapRow.gap && gapTime > 0, "a row and a gap with receive times");
            testContext.setLocalNowMs(rx * 1000);
            compare(row.clock, Qt.formatDateTime(new Date(rx * 1000), "hh:mm:ss"), "today's row says its clock time");
            testContext.setLocalNowMs(rx * 1000 + 2 * 86400000);
            compare(list.itemAtIndex(positionView.rowForTrack(id)), row, "the same row");
            compare(row.clock, Qt.formatDateTime(new Date(rx * 1000), "yyyy-MM-dd hh:mm:ss"), "two days on, the row's time has its date");
            testContext.setLocalNowMs(gapTime * 1000);
            compare(gapRow.gapTitle, "Positions may be missing from " + Qt.formatDateTime(new Date(gapTime * 1000), "hh:mm:ss"));
            testContext.setLocalNowMs(gapTime * 1000 + 2 * 86400000);
            compare(gapRow.gapTitle, "Positions may be missing from " + Qt.formatDateTime(new Date(gapTime * 1000), "yyyy-MM-dd hh:mm:ss"), "and the gap's");
        }

        // A sheet whose radio is evicted keeps its last details, its times said as times, not frozen ages.
        function test_18j_a_gone_track_says_times() {
            push({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7", ageSeconds: 5});
            screen.openTrack(trackNamed("Engine 7"), false);
            var received = item("mapShownFixReceived");
            verify(received.text.indexOf("Received 5 s ago") === 0, received.text);
            positions.clear();
            tryVerify(function () { return sheet().trackGone; }, 2000);
            tryVerify(function () { return received.text.indexOf("Received at ") === 0; }, 2000, received.text);
            sheet().visible = false;
        }
    }
}
