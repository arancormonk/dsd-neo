// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #800 (spec 4.8): what the Map screen keeps in the production preferences. The tile source, the age filter and
// the view come back on the next open; the view is never one the device's own position shaped: once Locate me, follow
// or a fit has used the device dot, nothing more is saved that visit, and leaving puts the view back where it was
// saved. Each "restart" is a new MapScreen over the same preferences, as the app builds one at its first map open.
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

        // Spec 4.8: follow on, follow off then zoom, follow off then fit, close, restart: none writes a view the
        // device shaped, and every later visit starts from the last view it did not.
        function test_03_a_view_the_device_shaped_is_never_kept() {
            testContext.pushPosition({source: 1001, lat: -30.0, lon: -60.0, label: "Engine 7"});
            restart();
            canvas().centerOn(savedPlace.latitude, savedPlace.longitude, savedPlace.zoom);
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom));

            locate();
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "Locate me is not kept");

            // Follow on.
            item("mapLocate").activate();
            verify(canvas().followDevice, "following the device");
            canvas().zoomAbout(14, 100, 100);
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "a zoom while following is not kept");

            // Follow off, then zoom.
            item("mapLocate").activate();
            verify(!canvas().followDevice, "follow is off");
            item("mapZoomIn").activate();
            canvas().panBy(30, 30);
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "a zoom or pan after following is not kept");

            // Follow off, then fit (the fit takes in the device dot as well as the radio).
            item("mapFit").activate();
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "a fit is not kept");

            // Close: nothing is written, and the view goes back to the saved one.
            close();
            verify(sameView(prefs.mapViewport, savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "closing writes nothing");
            verify(sameView(canvasView(), savedPlace.latitude, savedPlace.longitude, savedPlace.zoom), "the view is back on the saved one: " + JSON.stringify(canvasView()));

            // The next visit starts there, owes nothing to the device, and is kept again.
            reopen();
            verify(!canvas().cameraUsedDevicePosition);
            canvas().panBy(-20, 0);
            var moved = canvasView();
            verify(near(moved.latitude, savedPlace.latitude, 0.01) && !near(moved.latitude, devicePlace.latitude, 1.0), JSON.stringify(moved));
            verify(sameView(prefs.mapViewport, moved.latitude, moved.longitude, moved.zoom), "a move from the saved view is kept");

            // Restart: a new screen opens on that view, never on the device's.
            restart();
            verify(sameView(canvasView(), moved.latitude, moved.longitude, moved.zoom), JSON.stringify(canvasView()));
            verify(!canvas().cameraUsedDevicePosition);
        }

        // With no view saved, a first open whose fit takes in the device dot keeps nothing; closing leaves the whole
        // world, and the next visit places the camera again.
        function test_04_nothing_saved_and_the_first_fit_used_the_device() {
            restart();
            locate();
            close();
            testContext.pushPosition({source: 1001, lat: -30.0, lon: -60.0, label: "Engine 7"});
            restart();
            verify(canvas().cameraUsedDevicePosition, "the first fit took in the device dot");
            canvas().panBy(50, 0);
            item("mapZoomIn").activate();
            compare(Object.keys(prefs.mapViewport).length, 0, "nothing is kept: " + JSON.stringify(prefs.mapViewport));
            close();
            compare(Object.keys(prefs.mapViewport).length, 0, "closing writes nothing");
            verify(near(canvas().zoom, canvas().minimumZoom, 1e-6) && near(canvas().centerLatitude, 0) && near(canvas().centerLongitude, 0), "the whole world: " + JSON.stringify(canvasView()));
            verify(!screen.cameraPlaced, "the next visit places the camera again");
            reopen();
            tryVerify(function () { return screen.cameraPlaced; }, 2000);
            verify(canvas().cameraUsedDevicePosition, "it fits the radio and the dot again");
            compare(Object.keys(prefs.mapViewport).length, 0, "and keeps nothing");
        }

        // A "Show on map" pin centres on a History row's fix, which is not the device's: the view is kept.
        function test_05_a_pin_view_is_kept() {
            restart();
            screen.showPin({uid: "row-1", label: "Engine 7", position: {latitude: -12.5, longitude: 33.0}});
            verify(sameView(prefs.mapViewport, -12.5, 33.0, canvas().zoom), JSON.stringify(prefs.mapViewport));
        }
    }
}
