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
            testContext.resetPositions();
            testContext.useAppPrefs(false);
            testContext.setLocationSupported(false);
            Ui.Theme.resetFontScale();
            Ui.Theme.resetReducedMotion();
        }
        function init() {
            testContext.setMapTilesFailing(false);
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
                described.push(row.Accessible.name + " | " + row.Accessible.description);
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
                                                  lat: 10.25, lon: 20.25, accuracyM: 8, fixAtMs: 1000});
            verify(locator.live);
            tryVerify(function () { return Math.abs(canvas().deviceLatitude - 10.25) < 1e-9; });
            compare(prefs.lastFixAt, savedAt, "a live fix is not saved");
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

        // Review P7 minor 4: a stopped replay's fix has no age; the line under an invalid newer fix still reads whole.
        function test_14c_a_fix_with_no_age_still_reads_whole() {
            sheet().pin = null;
            sheet().trackId = 0;
            sheet().take({trackId: 7, label: "Unit 41", validityUnknown: true, hasPosition: true, hasUnknownFix: true,
                          ageSeconds: -1, unknownAgeSeconds: -1});
            sheet().visible = true;
            compare(item("mapPositionLastValid").text, "The last valid position is still shown");
            sheet().visible = false;
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
    }
}
