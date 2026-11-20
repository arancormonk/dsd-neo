// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #800 (spec 4.8): what the Map screen keeps in the production preferences. The tile source, the age filter and
// the view come back on the next open; the view saved is never one the device's own position shaped: once Locate me,
// follow, a fit or the opening placement has used the device dot, nothing more is saved for the rest of the run.
// Leaving and coming back keeps the view as it was left, and with location allowed a new run opens on the user.
// Each "restart" is a new MapScreen over the same preferences, as the app builds one at its first map open.
// Places are made up.
Item {
    id: root

    width: 360
    height: 780

    Loader {
        id: loader
        anchors.fill: parent
        active: false
        // Built hidden and then shown, as Main's Loader is: the screen places its camera when it becomes visible.
        visible: false
        source: uiDir + "/MapScreen.qml"
    }

    TestCase {
        id: tc

        name: "MapPrefs"
        when: windowShown

        readonly property var screen: loader.item
        // Made-up places: a saved view, and where the device says it is.
        readonly property var savedPlace: ({latitude: 10.0, longitude: 20.0, zoom: 12})
        readonly property var devicePlace: ({latitude: 22.5, longitude: 45.0})

        function item(name) {
            var found = findChild(screen, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        function canvas() {
            return item("mapCanvas");
        }
        function near(a, b, tolerance) {
            return Math.abs(a - b) < (tolerance === undefined ? 1e-5 : tolerance);
        }
        function sameView(view, latitude, longitude, zoom) {
            return view !== undefined && view.zoom !== undefined && near(view.latitude, latitude) && near(view.longitude, longitude) && near(view.zoom, zoom, 1e-3);
        }
        function canvasView() {
            return {latitude: canvas().centerLatitude, longitude: canvas().centerLongitude, zoom: canvas().zoom};
        }
        // A new screen over the same preferences, shown.
        function restart() {
            loader.visible = false;
            loader.active = false;
            loader.active = true;
            tryVerify(function () { return loader.item !== null; });
            loader.visible = true;
            waitForRendering(screen);
        }
        function close() {
            loader.visible = false;
        }
        function reopen() {
            loader.visible = true;
            waitForRendering(screen);
        }
        // Locate me through the shared broker, answered with the device's place: the camera centres on the dot.
        function locate() {
            var locate = item("mapLocate");
            compare(locate.Accessible.name, "Locate me");
            locate.activate();
            var id = item("mapLocator").requestId;
            verify(id > 0);
            decoderHost.locationResult(id, true, devicePlace.latitude, devicePlace.longitude, 12, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return canvas().cameraUsedDevicePosition; }, 2000, "the camera used the device dot");
            verify(canvas().deviceValid && near(canvas().deviceLatitude, devicePlace.latitude), "the dot is the device's");
        }

        function resetPrefs() {
            prefs.mapViewport = {};
            prefs.mapTilesEnabled = false;
            prefs.mapTileUrl = "";
            prefs.mapTileAttribution = "";
            prefs.mapAgeFilter = 0;
            positions.ageFilterSeconds = 0;
        }

        // The map's own one-shot request in flight, and whether it is the opening one (no prompt, no notice).
        function pendingLocate() {
            return item("mapLocator").requestId;
        }
        function sameAsDevice(view) {
            return near(view.latitude, devicePlace.latitude, 0.5) && near(view.longitude, devicePlace.longitude, 1e-5);
        }

        function initTestCase() {
            testContext.setLocationSupported(true);
            testContext.useAppPrefs(true);
        }
        function cleanupTestCase() {
            loader.active = false;
            resetPrefs();
            testContext.lapseLocationFix();
            testContext.resetPositions();
            testContext.useAppPrefs(false);
            testContext.setLocationSupported(false);
            testContext.setLocationAccess("precise");
            Ui.Theme.resetReducedMotion();
        }
        function init() {
            loader.visible = false;
            loader.active = false;
            testContext.resetPositions();
            resetPrefs();
            // No saved device fix: a case that wants the dot asks Locate me for it.
            testContext.lapseLocationFix();
            verify(prefs.lastFixAt === 0, "no saved fix");
            testContext.setLocationAccess("precise");
            // The camera jumps (reduced motion), so a case reads where it lands at once.
            Ui.Theme.reducedMotion = true;
        }

        // The tile switch, its source and the age filter are kept, and a new screen starts from them.
        function test_01_tiles_and_age_filter_are_kept() {
            restart();
            compare(screen.tileUrl, screen.defaultTileUrl);
            compare(prefs.mapTileUrl, screen.defaultTileUrl, "the prefs and the screen agree on the default source");
            compare(prefs.mapTileAttribution, screen.defaultAttribution, "and on its attribution");
            verify(!screen.tilesEnabled);
            testContext.pushPosition({source: 1001, lat: 10.0, lon: 20.0, label: "Engine 7"});
            item("mapLoadTiles").activate();
            verify(prefs.mapTilesEnabled, "Load map tiles is kept");

            var settings = item("mapSettingsSheet");
            settings.openEditor();
            item("mapSettingsUrl").text = "https://tiles.example.org/{z}/{x}/{y}.png";
            item("mapSettingsAttribution").text = "Example tiles";
            item("mapSettingsSave").activate();
            compare(prefs.mapTileUrl, "https://tiles.example.org/{z}/{x}/{y}.png");
            compare(prefs.mapTileAttribution, "Example tiles");

            item("mapAge1").activate();
            compare(prefs.mapAgeFilter, 3600, "the age pill is kept");

            positions.ageFilterSeconds = 900;
            compare(prefs.mapAgeFilter, 900, "the List's filter is the same filter");
            positions.ageFilterSeconds = 0;
            prefs.mapAgeFilter = 3600;
            restart();
            verify(screen.tilesEnabled);
            compare(screen.tileUrl, "https://tiles.example.org/{z}/{x}/{y}.png");
            compare(screen.tileAttribution, "Example tiles");
            compare(positions.ageFilterSeconds, 3600, "a new screen applies the kept filter");
        }

        // A view the user set is kept as it moves, and a new screen opens on it instead of fitting the radios.
        function test_02_the_view_is_kept() {
            testContext.pushPosition({source: 1001, lat: -30.0, lon: -60.0, label: "Engine 7"});
            restart();
            verify(near(canvas().centerLatitude, -30.0, 1.0), "the first open fits the radios");
            canvas().centerOn(savedPlace.latitude, savedPlace.longitude, savedPlace.zoom);
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), JSON.stringify(prefs.mapViewport));
            canvas().panBy(40, -25);
            var moved = canvasView();
            verify(sameView(prefs.mapViewport, moved.latitude, moved.longitude, moved.zoom), "every move is kept");
            restart();
            verify(sameView(canvasView(), moved.latitude, moved.longitude, moved.zoom), "a new screen opens where it was left: " + JSON.stringify(canvasView()));
        }

        // Spec 4.8: follow on, follow off then zoom, follow off then fit, close, reopen: none writes a view the
        // device shaped, and leaving keeps the view as it was left (the user's report: it went back to an older view).
        function test_03_a_view_the_device_shaped_is_never_saved_but_is_kept() {
            testContext.setLocationAccess("none"); // the first open places by the saved view, as without location
            testContext.pushPosition({source: 1001, lat: -30.0, lon: -60.0, label: "Engine 7"});
            restart();
            compare(pendingLocate(), 0, "no location access: the opening asks nothing");
            canvas().centerOn(savedPlace.latitude, savedPlace.longitude, savedPlace.zoom);
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom));
            testContext.setLocationAccess("precise");

            locate();
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "Locate me is not saved");
            verify(sameAsDevice(canvasView()), "Locate me centres on the dot: " + JSON.stringify(canvasView()));
            verify(near(canvas().zoom, canvas().deviceZoom(), 1e-6) && canvas().zoom > savedPlace.zoom, "and zooms in to the streets around it: " + canvas().zoom);

            // Follow on.
            item("mapLocate").activate();
            verify(canvas().followDevice, "following the device");
            canvas().zoomAbout(14, 100, 100);
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "a zoom while following is not saved");

            // Follow off, then zoom.
            item("mapLocate").activate();
            verify(!canvas().followDevice, "follow is off");
            item("mapZoomIn").activate();
            canvas().panBy(30, 30);
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "a zoom or pan after following is not saved");

            // Follow off, then fit (the fit takes in the device dot as well as the radio).
            item("mapFit").activate();
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "a fit is not saved");
            canvas().panBy(-10, 5);
            var left = canvasView();

            // Close: nothing is written, and the view stays where it was left.
            close();
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "closing writes nothing");
            verify(sameView(canvasView(), left.latitude, left.longitude, left.zoom), "the view is kept: " + JSON.stringify(canvasView()));

            // The next visit opens on it, still owes it to the device, and saves nothing for the rest of the run.
            reopen();
            verify(sameView(canvasView(), left.latitude, left.longitude, left.zoom), "back where it was left: " + JSON.stringify(canvasView()));
            compare(pendingLocate(), 0, "a return asks for no fix");
            verify(canvas().cameraUsedDevicePosition);
            canvas().panBy(-20, 0);
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "a move later in the run is not saved");
        }

        // With location allowed the first open of a run centres on the user: on the saved dot at once, then on a fresh
        // fix asked for without a prompt. The radios are not fitted, and nothing of it is saved.
        function test_04_a_run_opens_on_the_user() {
            testContext.pushPosition({source: 1001, lat: -30.0, lon: -60.0, label: "Engine 7"});
            prefs.setLocationFix(devicePlace.latitude, devicePlace.longitude, savedSystems.realtimeNowMs(), 12);
            restart();
            tryVerify(function () { return screen.cameraPlaced; }, 2000);
            verify(sameAsDevice(canvasView()), "the saved dot, at once: " + JSON.stringify(canvasView()));
            verify(near(canvas().zoom, canvas().deviceZoom(), 1e-6) && canvas().zoom > 14, "at street level: " + canvas().zoom);
            var id = pendingLocate();
            verify(id > 0 && item("mapLocator").quiet, "a fresh fix is asked for");
            verify(testContext.hostCalls().some(function (c) { return c.name === "requestQuietLocation" && c.id === id; }), "never with a prompt: " + JSON.stringify(testContext.hostCalls()));
            compare(item("mapLocate").Accessible.name, "Locate me", "the button is free while it runs");
            verify(item("mapLocate").enabled);
            compare(item("mapLocator").notice, "", "the opening fix says nothing");
            decoderHost.locationResult(id, true, devicePlace.latitude + 0.01, devicePlace.longitude + 0.02, 8, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return near(canvas().centerLongitude, devicePlace.longitude + 0.02, 1e-5); }, 2000, "the fresh fix re-centres");
            compare(item("mapLocate").Accessible.name, "Locate me", "the first press still finds the user again");
            compare(Object.keys(prefs.mapViewport).length, 0, "nothing is saved: " + JSON.stringify(prefs.mapViewport));
            close();
            reopen();
            verify(near(canvas().centerLongitude, devicePlace.longitude + 0.02, 1e-5), "a return keeps it");
            compare(pendingLocate(), 0, "and asks for no fix");
        }

        // The opening fix places the camera at its own accuracy's zoom: a precise saved dot's street zoom does not stay
        // over an approximate fresh fix kilometres wide.
        function test_04a_the_opening_fix_sets_its_own_zoom() {
            prefs.setLocationFix(devicePlace.latitude, devicePlace.longitude, savedSystems.realtimeNowMs(), 5);
            restart();
            tryVerify(function () { return screen.cameraPlaced; }, 2000);
            verify(near(canvas().zoom, 15, 1e-6), "a precise saved dot: " + canvas().zoom);
            decoderHost.locationResult(pendingLocate(), true, devicePlace.latitude, devicePlace.longitude, 3000, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return canvas().zoom < 13; }, 2000, "the approximate fix's zoom: " + canvas().zoom);
            verify(near(canvas().zoom, canvas().deviceZoom(), 1e-6));
        }

        // Back while the camera still moves: the view kept is the one shown, and a return opens on it.
        function test_04e_back_mid_animation_keeps_the_shown_view() {
            Ui.Theme.reducedMotion = false;
            restart();
            item("mapLocate").activate();
            decoderHost.locationResult(pendingLocate(), true, devicePlace.latitude, devicePlace.longitude, 8, savedSystems.realtimeNowMs(), false, "", "", "");
            wait(60); // part way
            close();
            var shown = canvasView();
            verify(!sameAsDevice(shown) || !near(shown.zoom, 15, 1e-3), "still on its way when closed: " + JSON.stringify(shown));
            wait(600);
            verify(sameView(canvasView(), shown.latitude, shown.longitude, shown.zoom), "it stopped where it was: " + JSON.stringify(canvasView()));
            reopen();
            verify(sameView(canvasView(), shown.latitude, shown.longitude, shown.zoom), "and a return opens there: " + JSON.stringify(canvasView()));
        }

        // The opening fix leaves a camera the user moved alone, and fails without a word.
        function test_04b_the_opening_fix_keeps_a_moved_view_and_fails_quietly() {
            restart();
            var id = pendingLocate();
            verify(id > 0, "a fresh fix is asked for with no saved dot");
            item("mapZoomIn").activate();
            var moved = canvasView();
            decoderHost.locationResult(id, true, devicePlace.latitude, devicePlace.longitude, 8, savedSystems.realtimeNowMs(), false, "", "", "");
            wait(50);
            verify(sameView(canvasView(), moved.latitude, moved.longitude, moved.zoom), "the moved view stays: " + JSON.stringify(canvasView()));
            verify(prefs.lastFixAt > 0, "the dot is saved all the same");
            testContext.lapseLocationFix();
            restart();
            id = pendingLocate();
            decoderHost.locationResult(id, false, 0, 0, 0, 0, false, "", "", "Location request timed out; use Browse.");
            compare(item("mapLocator").notice, "", "no notice for the opening fix");
        }

        // Without location access the first open neither asks nor prompts: the saved view, else the radios.
        function test_04c_no_location_access_opens_as_before() {
            testContext.setLocationAccess("none");
            testContext.pushPosition({source: 1001, lat: -30.0, lon: -60.0, label: "Engine 7"});
            restart();
            compare(pendingLocate(), 0);
            verify(near(canvas().centerLatitude, -30.0, 1.0), "the radios: " + JSON.stringify(canvasView()));
        }

        // Approximate location: Locate me offers precise, and when the user keeps approximate the button stays Locate
        // me (every press finds the user again) and says how to get a closer fix.
        function test_04d_approximate_location_relocates_every_press() {
            testContext.setLocationAccess("approximate");
            restart();
            var opening = pendingLocate();
            verify(opening > 0, "approximate access still opens on the user");
            var button = item("mapLocate");
            button.activate();
            var id = pendingLocate();
            verify(id > 0 && id !== opening && !item("mapLocator").quiet, "Locate me replaces the opening fix");
            verify(testContext.hostCalls().some(function (c) { return c.name === "requestPreciseLocation" && c.id === id; }), "and offers precise location: " + JSON.stringify(testContext.hostCalls()));
            decoderHost.locationResult(id, true, devicePlace.latitude, devicePlace.longitude, 2000, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return sameAsDevice(canvasView()); }, 2000);
            compare(button.Accessible.name, "Locate me", "no Follow without precise location");
            verify(canvas().zoom > 10.9 && canvas().zoom < 15, "zoomed to the approximate circle: " + canvas().zoom);
            compare(item("mapLocator").notice, "Your location is approximate. Allow precise location for a closer fix.");
            compare(item("mapLocator").noticeAction, "appSettings");
            button.activate();
            verify(pendingLocate() > id, "the next press locates again");
        }

        // A saved dot places nothing without precise location: it may be a precise fix from before access was lowered
        // to approximate, and the tile server would see the streets around it. The approximate fix places the map.
        function test_04f_approximate_access_does_not_open_on_a_saved_dot() {
            testContext.setLocationAccess("approximate");
            prefs.setLocationFix(devicePlace.latitude, devicePlace.longitude, savedSystems.realtimeNowMs(), 5);
            prefs.mapViewport = {latitude: savedPlace.latitude, longitude: savedPlace.longitude, zoom: savedPlace.zoom};
            restart();
            tryVerify(function () { return screen.cameraPlaced; }, 2000);
            verify(sameView(canvasView(), savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "the saved view: " + JSON.stringify(canvasView()));
            var id = pendingLocate();
            verify(id > 0, "the approximate fix is asked for");
            decoderHost.locationResult(id, true, devicePlace.latitude + 0.01, devicePlace.longitude, 2000, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return near(canvas().centerLongitude, devicePlace.longitude, 1e-5); }, 2000, "it places the map");
            verify(canvas().zoom < 15, "at the approximate circle's zoom: " + canvas().zoom);
        }

        // The app leaving before the opening placement runs: no opening fix is asked for.
        function test_04g_no_opening_fix_in_the_background() {
            loader.visible = false;
            loader.active = false;
            loader.active = true;
            tryVerify(function () { return loader.item !== null; });
            loader.visible = true;
            screen.appForeground = false; // before the deferred placement runs
            wait(50);
            verify(screen.cameraPlaced);
            compare(pendingLocate(), 0, "nothing asked while away");
        }

        // The List interrupting a Locate me on its way: the next press locates again rather than following.
        function test_04h_the_list_disarms_follow() {
            Ui.Theme.reducedMotion = false;
            restart();
            item("mapLocate").activate();
            decoderHost.locationResult(pendingLocate(), true, devicePlace.latitude, devicePlace.longitude, 8, savedSystems.realtimeNowMs(), false, "", "", "");
            screen.mapShown = false;
            screen.mapShown = true;
            compare(item("mapLocate").Accessible.name, "Locate me");
        }

        // History's "Show on map" as the first visit starts from the saved zoom, as an open would.
        function test_04i_a_first_pin_keeps_the_saved_zoom() {
            testContext.setLocationAccess("none");
            prefs.mapViewport = {latitude: savedPlace.latitude, longitude: savedPlace.longitude, zoom: 18};
            loader.visible = false;
            loader.active = false;
            loader.active = true;
            tryVerify(function () { return loader.item !== null; });
            screen.showPin({uid: "row-2", label: "Engine 7", position: {latitude: -12.5, longitude: 33.0}});
            loader.visible = true;
            waitForRendering(screen);
            verify(near(canvas().zoom, 18, 1e-3), "the saved zoom: " + canvas().zoom);
            verify(near(canvas().centerLongitude, 33.0, 1e-5), "on the pin");
        }

        // Locate me keeps a closer zoom only when the user chose it: one the map set itself gives way to the fix's own.
        function test_04j_a_zoom_the_user_chose_is_kept() {
            prefs.setLocationFix(devicePlace.latitude, devicePlace.longitude, savedSystems.realtimeNowMs(), 5);
            restart();
            tryVerify(function () { return near(canvas().zoom, 15, 1e-6); }, 2000, "opened at street level");
            var button = item("mapLocate");
            button.activate();
            decoderHost.locationResult(pendingLocate(), true, devicePlace.latitude, devicePlace.longitude, 2000, savedSystems.realtimeNowMs(), false, "", "", "");
            tryVerify(function () { return canvas().zoom < 14; }, 2000, "the wide fix's zoom: " + canvas().zoom);
            item("mapZoomIn").activate();
            item("mapZoomIn").activate();
            var chosen = canvas().zoom;
            button.activate();
            decoderHost.locationResult(pendingLocate(), true, devicePlace.latitude, devicePlace.longitude, 2000, savedSystems.realtimeNowMs() + 1, false, "", "", "");
            tryVerify(function () { return sameAsDevice(canvasView()); }, 2000);
            verify(near(canvas().zoom, chosen, 1e-6), "the user's closer zoom stays: " + canvas().zoom + " vs " + chosen);
        }

        // An answer older than a fix another screen saved since leaves the newer one.
        function test_04k_an_older_answer_keeps_a_newer_saved_fix() {
            restart();
            var id = pendingLocate();
            var newer = savedSystems.realtimeNowMs();
            prefs.setLocationFix(10.0, 10.0, newer, 5);
            decoderHost.locationResult(id, true, devicePlace.latitude, devicePlace.longitude, 8, newer - 20000, false, "", "", "");
            verify(near(prefs.lastLat, 10.0) && prefs.lastFixAt === newer, "the newer fix stays");
        }

        // Close while following a live fix that is not the saved one: letting the lease go moves the dot back to the
        // saved fix, and that must not re-centre the camera after the close: the view stays where it was followed to.
        // With and without animation, on the host that runs the live feed.
        function closeWhileFollowingLive(reduced) {
            testContext.useLifecycleHost(true);
            testContext.setLocationSupported(true);
            testContext.useAppPrefs(true);
            Ui.Theme.reducedMotion = reduced;
            testContext.pushPosition({source: 1001, lat: -30.0, lon: -60.0, label: "Engine 7"});
            restart();
            canvas().centerOn(savedPlace.latitude, savedPlace.longitude, savedPlace.zoom);
            tryVerify(function () { return sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom); }, 2000);
            locate();
            item("mapLocate").activate();
            verify(canvas().followDevice, "following the device");
            testContext.emitLocationStreamUpdate({generation: 1, state: "ok", hasFix: true, provider: "gps", measurementNs: 2000,
                                                  lat: 25.0, lon: 47.0, accuracyM: 5, fixAtMs: locationFeed.nowMs()});
            tryVerify(function () { return item("mapLocator").live; }, 2000, "the live fix is in use");
            tryVerify(function () { return near(canvas().centerLatitude, 25.0, 0.5); }, 2000, "the camera follows it");
            wait(reduced ? 0 : 600); // the camera at rest
            var followed = canvasView();
            close();
            wait(reduced ? 50 : 600);
            verify(!canvas().followDevice, "leaving ends the follow");
            // Where it was followed to; the last follow step may still centre it below the chips as the screen hides.
            var kept = canvasView();
            verify(near(kept.latitude, followed.latitude, 0.01) && near(kept.longitude, followed.longitude, 1e-5) && near(kept.zoom, followed.zoom, 1e-3), "the view stays where it was followed to: " + JSON.stringify(kept) + " vs " + JSON.stringify(followed));
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "nothing written: " + JSON.stringify(prefs.mapViewport));
            reopen();
            verify(sameView(canvasView(), kept.latitude, kept.longitude, kept.zoom), "a return opens there: " + JSON.stringify(canvasView()));
            canvas().panBy(-5, 0);
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "and saves nothing: " + JSON.stringify(prefs.mapViewport));
            loader.visible = false;
            loader.active = false;
            testContext.useLifecycleHost(false);
            testContext.setLocationSupported(true);
            testContext.useAppPrefs(true);
        }
        function test_06_close_while_following_a_live_fix() {
            closeWhileFollowingLive(true);
        }
        function test_07_close_while_following_a_live_fix_animated() {
            closeWhileFollowingLive(false);
        }

        // A "Show on map" pin centres on a History row's fix, which is not the device's: the view is kept.
        function test_05_a_pin_view_is_kept() {
            restart();
            screen.showPin({uid: "row-1", label: "Engine 7", position: {latitude: -12.5, longitude: 33.0}});
            // The pin sits below the chips (MapView.topInset), so the view's centre is a little north of it.
            var shown = canvasView();
            verify(near(shown.latitude, -12.5, 0.1) && near(shown.longitude, 33.0, 1e-5), JSON.stringify(shown));
            verify(sameView(prefs.mapViewport, shown.latitude, shown.longitude, shown.zoom), JSON.stringify(prefs.mapViewport));
        }
    }
}
