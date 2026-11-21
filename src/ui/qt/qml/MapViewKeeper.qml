// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #800 (spec 4.8): what the Map screen keeps between visits in this
// device's prefs, besides its tile source: the age filter (map/ageFilter) and
// the view (map/viewport). The view is saved only while it owes nothing to the
// device's own position. Once Locate me, follow, a fit or the opening placement
// has used the device dot (MapView.cameraUsedDevicePosition), nothing more is
// saved for the rest of the app's run, since every later pan or zoom still
// starts from there. Leaving and coming back keeps the view as it was left (the
// screen lives for the run); the saved view is only where a later run opens
// when location is not allowed (MapScreen.placeCamera()).
Item {
    id: keeper

    visible: false

    // The MapScreen (its visible and cameraPlaced) and its MapView.
    property Item screen: null
    property var map: null

    property bool restoring: false

    function save() {
        if (screen.visible && screen.cameraPlaced && !restoring && !map.cameraUsedDevicePosition)
            prefs.mapViewport = {latitude: map.centerLatitude, longitude: map.centerLongitude, zoom: map.zoom};
    }
    // The saved view, at once; false when none is saved.
    function restore() {
        var view = prefs.mapViewport;
        if (!view || view.zoom === undefined)
            return false;
        restoring = true;
        map.zoom = view.zoom;
        map.centerLatitude = view.latitude;
        map.centerLongitude = view.longitude;
        restoring = false;
        return true;
    }

    Component.onCompleted: positions.ageFilterSeconds = prefs.mapAgeFilter || 0
    Connections {
        target: positions
        function onAgeFilterSecondsChanged() {
            prefs.mapAgeFilter = positions.ageFilterSeconds;
        }
    }
    Connections {
        target: keeper.map
        function onViewportChanged() {
            keeper.save();
        }
    }
}
