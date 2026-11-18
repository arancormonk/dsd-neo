// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #800 (spec 4.8): what the Map screen keeps between visits in this
// device's prefs, besides its tile source: the age filter (map/ageFilter) and
// the view (map/viewport). The view is saved only while it owes nothing to the
// device's own position. Once Locate me, follow or a fit has used the device
// dot (MapView.cameraUsedDevicePosition, reset at every open), nothing more is
// saved that visit, since a later pan or zoom still starts from there; leaving
// puts the view back where it was last saved, so the next visit starts from a
// view the device did not shape.
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
    Connections {
        target: keeper.screen
        function onVisibleChanged() {
            if (keeper.screen.visible) {
                // A first visit opens on the saved view, else the screen
                // places the camera (it fits the radios).
                if (!keeper.screen.cameraPlaced)
                    keeper.screen.cameraPlaced = keeper.restore();
            } else if (keeper.map.cameraUsedDevicePosition) {
                // Following the device ends first, whichever handler of this
                // close runs first: the dot moving back from a live fix to the
                // saved one must not re-centre the camera after the restore.
                keeper.map.followDevice = false;
                if (!keeper.restore()) {
                    // Nothing saved to go back to: the whole world, and the
                    // next visit places the camera again.
                    keeper.map.zoom = keeper.map.minimumZoom;
                    keeper.map.centerLatitude = 0;
                    keeper.map.centerLongitude = 0;
                    keeper.screen.cameraPlaced = false;
                }
            }
        }
    }
}
