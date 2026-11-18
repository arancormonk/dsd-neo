// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import DsdNeo 1.0

// Issue #800: the map canvas (MapView), drawn by the software scene graph as every QML test is, over the production
// track model fed from a canned position stream and the production tile worker on a canned transport. What is checked
// is what reaches the screen: each state of a fix looks different on its own axis, the camera API moves the view as
// the screen's handlers and buttons expect, taps find the right track, and no tile request goes out before tiles are
// turned on. Places are made up.
Item {
    id: root

    width: 400
    height: 300

    Rectangle {
        id: backdrop

        anchors.fill: parent
        color: "#808080"
    }

    MapTileSource {
        id: tiles
    }

    MapView {
        id: map

        anchors.fill: parent
        model: positions
        tileSource: tiles
        cameraDurationMs: 0
        markerSize: 16
        trailWidth: 2
        markerColor: "#ffffff"
        haloColor: "#000000"
        alertColor: "#ff8000"
        selectedColor: "#00ffff"
        trailColor: "#ff00ff"
        accuracyColor: "#4000ff00"
        deviceColor: "#0000ff"
        labelColor: "#ffff00"
        gridColor: "#ff0000"
        tileInkColor: "#000000"
        tilePaperColor: "#204060"
        labelFont.pixelSize: 14
    }

    TestCase {
        id: tc

        name: "MapView"
        when: windowShown

        readonly property real cx: root.width / 2
        readonly property real cy: root.height / 2

        function init() {
            tiles.enabled = false
            map.active = true
            map.selectedTrackId = 0
            map.followTrackId = 0
            map.cameraDurationMs = 0
            map.trailColor = "#ff00ff"
            map.labelObstacles = []
            map.followDevice = false
            map.deviceValid = false
            map.resetCameraUsedDevicePosition()
            testContext.resetPositions()
            testContext.setPositionWallSeconds(testContext.positionWallStart())
            map.centerOn(10.0, 20.0, 12)
            waitForRendering(map)
        }

        function cleanupTestCase() {
            tiles.enabled = false
            testContext.resetPositions()
        }

        function push(fix) {
            testContext.pushPosition(fix)
            waitForRendering(map)
        }

        function rgb(image, x, y) {
            return { r: image.red(x, y), g: image.green(x, y), b: image.blue(x, y) }
        }

        function near(c, r, g, b, tol) {
            return Math.abs(c.r - r) <= tol && Math.abs(c.g - g) <= tol && Math.abs(c.b - b) <= tol
        }

        // How many pixels of a rectangle are within tol of a colour.
        function countNear(image, x0, y0, x1, y1, r, g, b, tol) {
            var n = 0
            for (var y = y0; y < y1; y++) {
                for (var x = x0; x < x1; x++) {
                    if (near(rgb(image, x, y), r, g, b, tol))
                        n++
                }
            }
            return n
        }

        function test_01_a_fresh_fix_is_a_solid_marker_where_it_was_heard() {
            push({ source: 1001, lat: 10.0, lon: 20.0 })
            compare(map.markerCount, 1)
            var id = map.trackAt(tc.cx, tc.cy)
            verify(id > 0, "a tap on the marker finds its track")
            compare(positions.trackFor(id).sourceId, 1001)
            compare(map.trackAt(10, 10), 0, "a tap on empty map finds nothing")
            var p = map.markerPoint(id)
            verify(Math.abs(p.x - tc.cx) < 0.5 && Math.abs(p.y - tc.cy) < 0.5, "drawn at its place: " + p)
            var image = grabImage(root)
            verify(near(rgb(image, tc.cx, tc.cy), 255, 255, 255, 12), "solid fill: " + JSON.stringify(rgb(image, tc.cx, tc.cy)))
        }

        function test_02_an_older_fix_is_faded_and_a_filtered_one_is_gone() {
            push({ source: 1001, lat: 10.0, lon: 20.0, ageSeconds: 1000 })
            var image = grabImage(root)
            var c = rgb(image, tc.cx, tc.cy)
            verify(c.r < 90 && c.r > 20, "faded fill over the halo colour, still visible: " + JSON.stringify(c))
            positions.ageFilterSeconds = 900
            waitForRendering(map)
            compare(map.markerCount, 0, "beyond the age filter: hidden")
            compare(map.trackAt(tc.cx, tc.cy), 0)
            positions.ageFilterSeconds = 0
            waitForRendering(map)
            compare(map.markerCount, 1)
        }

        function test_03_a_fix_that_may_be_invalid_is_an_open_ring() {
            push({ source: 1002, lat: 10.0, lon: 20.0, validityUnknown: true })
            compare(map.markerCount, 1)
            var image = grabImage(root)
            verify(near(rgb(image, tc.cx, tc.cy), 128, 128, 128, 6),
                   "no fill: the map shows through: " + JSON.stringify(rgb(image, tc.cx, tc.cy)))
            verify(countNear(image, tc.cx + 4, tc.cy - 1, tc.cx + 10, tc.cy + 1, 255, 255, 255, 60) > 0,
                   "the ring's outline is drawn")
        }

        function test_04_a_trail_joins_a_radio_s_fixes_and_a_provisional_one_is_dashed() {
            push({ source: 1003, lat: 10.0, lon: 19.99 })
            push({ source: 1003, lat: 10.0, lon: 20.01 })
            compare(map.markerCount, 1, "one track, one marker")
            var image = grabImage(root)
            verify(near(rgb(image, tc.cx, tc.cy), 255, 0, 255, 40), "the trail runs between the fixes: "
                   + JSON.stringify(rgb(image, tc.cx, tc.cy)))
            var solid = countNear(image, tc.cx - 12, tc.cy, tc.cx + 12, tc.cy + 1, 255, 0, 255, 60)
            verify(solid >= 22, "solid: every pixel along it (" + solid + ")")

            testContext.resetPositions()
            push({ source: 1004, lat: 10.0, lon: 19.99 })
            push({ source: 1004, lat: 10.0, lon: 20.01, provisional: true })
            image = grabImage(root)
            var on = countNear(image, tc.cx - 12, tc.cy, tc.cx + 12, tc.cy + 1, 255, 0, 255, 60)
            var off = countNear(image, tc.cx - 12, tc.cy, tc.cx + 12, tc.cy + 1, 128, 128, 128, 20)
            verify(on > 3 && off > 3, "dashed: drawn and gaps (" + on + " on, " + off + " off)")
        }

        function test_05_the_selected_track_takes_the_accent() {
            push({ source: 1005, lat: 10.0, lon: 19.99 })
            push({ source: 1005, lat: 10.0, lon: 20.01 })
            var id = map.trackAt(tc.cx + 29, tc.cy)
            verify(id > 0)
            map.selectedTrackId = id
            waitForRendering(map)
            var image = grabImage(root)
            verify(near(rgb(image, tc.cx, tc.cy), 0, 255, 255, 40), "selected trail: " + JSON.stringify(rgb(image, tc.cx, tc.cy)))
        }

        function test_06_an_emergency_is_the_alert_colour_and_says_so() {
            push({ source: 1006, lat: 10.0, lon: 20.0, emergency: true, label: "Unit 7" })
            var image = grabImage(root)
            verify(near(rgb(image, tc.cx, tc.cy), 255, 128, 0, 30), "alert fill: " + JSON.stringify(rgb(image, tc.cx, tc.cy)))
            verify(countNear(image, tc.cx + 12, tc.cy - 10, root.width, tc.cy + 10, 255, 128, 0, 50) > 20,
                   "an alert-coloured label beside it")
            // The word comes first: the name alone ("Unit 7") would end well before x = 280.
            verify(countNear(image, 280, tc.cy - 10, root.width, tc.cy + 10, 255, 128, 0, 50) > 10,
                   "the label says Emergency before the name")
        }

        // Trails overlap themselves at sharp turns, so they are drawn opaque whatever colour is bound.
        function test_04b_a_translucent_trail_colour_is_drawn_opaque() {
            map.trailColor = "#40ff00ff"
            push({ source: 1014, lat: 10.0, lon: 19.99 })
            push({ source: 1014, lat: 10.0, lon: 20.01 })
            var image = grabImage(root)
            verify(near(rgb(image, tc.cx, tc.cy), 255, 0, 255, 40), "opaque: " + JSON.stringify(rgb(image, tc.cx, tc.cy)))
        }

        // Two labels that want the same side: the emergency's label (its word) keeps it, and every other label,
        // the selected one included, yields and takes another side.
        function test_06b_a_label_that_cannot_have_its_side_takes_another() {
            push({ source: 1015, lat: 10.0, lon: 20.0, label: "Car 12 long name" })
            var selected = map.trackAt(tc.cx, tc.cy)
            map.selectedTrackId = selected
            push({ source: 1016, lat: 10.0001, lon: 20.0, emergency: true, label: "Unit 7" })
            waitForRendering(map)
            var image = grabImage(root)
            // From past the selection halo (the sprite is 28 px wide) to the right edge: the emergency's label.
            verify(countNear(image, tc.cx + 16, tc.cy - 12, root.width, tc.cy + 12, 255, 128, 0, 50) > 20,
                   "the emergency's label keeps the right")
            compare(countNear(image, tc.cx + 16, tc.cy - 12, root.width, tc.cy + 12, 0, 255, 255, 30), 0,
                    "the selected label is not over it")
            verify(countNear(image, 0, 0, tc.cx - 16, root.height, 0, 255, 255, 30)
                   + countNear(image, tc.cx - 60, 0, tc.cx + 60, tc.cy - 16, 0, 255, 255, 30)
                   + countNear(image, tc.cx - 60, tc.cy + 16, tc.cx + 60, root.height, 0, 255, 255, 30) > 20,
                   "the selected label is drawn on another side")
        }

        // A label never covers another marker, nor the screen's controls: it takes another side.
        function test_06c_labels_keep_clear_of_markers_and_obstacles() {
            push({ source: 1018, lat: 10.0, lon: 20.0, label: "Unit 9" })
            // A neighbour 40 px to the right, where the label would go (unnamed, so only its marker is in the way).
            push({ anonymous: true, lat: 10.0, lon: 20.0 + 40 * 360 / (256 * 4096) })
            waitForRendering(map)
            var image = grabImage(root)
            compare(countNear(image, tc.cx + 16, tc.cy - 12, tc.cx + 30, tc.cy + 12, 255, 255, 0, 50), 0,
                    "not toward the neighbour")
            verify(countNear(image, 0, tc.cy - 12, tc.cx - 12, tc.cy + 12, 255, 255, 0, 50) > 20, "to the left instead")
            // A keep-out box over the left side (the neighbour still holds the right): it goes above.
            map.labelObstacles = [Qt.rect(0, tc.cy - 30, tc.cx - 30, 60)]
            waitForRendering(map)
            image = grabImage(root)
            compare(countNear(image, 0, tc.cy - 30, tc.cx - 30, tc.cy + 30, 255, 255, 0, 50), 0, "never under the keep-out")
            verify(countNear(image, tc.cx - 40, tc.cy - 40, tc.cx + 40, tc.cy - 12, 255, 255, 0, 50) > 20, "above instead")
            map.labelObstacles = []
        }

        // An emergency hemmed in by markers on every side still says so: its label is never dropped.
        function test_06d_an_emergency_label_is_never_dropped() {
            var px = 360 / (256 * 4096) // degrees of longitude per pixel at zoom 12
            var py = px * Math.cos(10.0 * Math.PI / 180)
            push({ source: 1020, lat: 10.0, lon: 20.0, emergency: true })
            var around = [[30, 0], [-30, 0], [0, -24], [0, 24]]
            for (var i = 0; i < around.length; i++)
                push({ anonymous: true, lat: 10.0 - around[i][1] * py, lon: 20.0 + around[i][0] * px })
            compare(map.markerCount, 5)
            var image = grabImage(root)
            // The word, in the alert colour, somewhere past the marker itself.
            var outside = countNear(image, 0, 0, root.width, tc.cy - 10, 255, 128, 0, 50)
                + countNear(image, 0, tc.cy + 10, root.width, root.height, 255, 128, 0, 50)
                + countNear(image, 0, tc.cy - 10, tc.cx - 10, tc.cy + 10, 255, 128, 0, 50)
                + countNear(image, tc.cx + 10, tc.cy - 10, root.width, tc.cy + 10, 255, 128, 0, 50)
            verify(outside > 20, "the emergency's label is drawn: " + outside)
        }

        // A marker whose centre is just past the edge still shows, and keeps its label: an emergency there is not
        // shown by colour alone.
        function test_06e_a_marker_half_off_the_edge_keeps_its_label() {
            var px = 360 / (256 * 4096)
            push({ source: 1021, lat: 10.0, lon: 20.0 - (tc.cx + 3) * px, emergency: true })
            compare(map.markerCount, 1)
            var image = grabImage(root)
            verify(countNear(image, 8, tc.cy - 12, 160, tc.cy + 12, 255, 128, 0, 50) > 20, "the word beside it")
        }

        // The settings sheet checks a template and names the server the viewed area goes to.
        function test_14_the_tile_source_checks_a_template_for_the_settings_sheet() {
            verify(tiles.isValidTemplate("https://tiles.example.org/{z}/{x}/{y}.png"))
            compare(tiles.templateHost("https://tiles.example.org:8443/{z}/{x}/{y}.png"), "tiles.example.org:8443")
            verify(!tiles.isValidTemplate("http://tiles.example.org/{z}/{x}/{y}.png"), "https only")
            verify(!tiles.isValidTemplate("https://tiles.example.org/{z}/{x}.png"), "every placeholder")
            compare(tiles.templateHost("http://tiles.example.org/{z}/{x}/{y}.png"), "")
        }

        // A follow step that lands while a zoom button animates keeps the zoom it was going to.
        function test_15_a_follow_step_does_not_cut_a_zoom_short() {
            push({ source: 1017, lat: 10.0, lon: 20.0 })
            map.followTrackId = map.trackAt(tc.cx, tc.cy)
            map.cameraDurationMs = 250
            map.zoomIn()
            wait(60)
            push({ source: 1017, lat: 10.0, lon: 20.001 })
            tryCompare(map, "zoom", 13, 2000)
            var p = map.markerPoint(map.followTrackId)
            verify(Math.abs(p.x - tc.cx) < 0.5, "and is centred on the new fix: " + p.x)
        }

        function test_07_a_named_radio_is_labelled_and_an_anonymous_one_is_not() {
            push({ source: 1007, lat: 10.0, lon: 20.0, label: "Car 12" })
            var image = grabImage(root)
            verify(countNear(image, tc.cx + 12, tc.cy - 10, root.width, tc.cy + 10, 255, 255, 0, 50) > 20,
                   "the name beside the marker")
            var named = grabImage(root)
            testContext.resetPositions()
            push({ anonymous: true, lat: 10.0, lon: 20.0 })
            compare(map.markerCount, 1)
            image = grabImage(root)
            // Same place, same age: only the outline differs, dashed for the unconfirmed source.
            var differ = 0
            for (var y = tc.cy - 10; y <= tc.cy + 10; y++) {
                for (var x = tc.cx - 10; x <= tc.cx + 10; x++) {
                    if (!near(rgb(image, x, y), named.red(x, y), named.green(x, y), named.blue(x, y), 40))
                        differ++
                }
            }
            verify(differ > 6, "a dashed outline: " + differ + " pixels differ")
            verify(near(rgb(image, tc.cx, tc.cy), 255, 255, 255, 12), "the same solid fill")
            // Even when fresh, the outline's gaps read as gaps: along one edge of the diamond some pixels are the
            // halo's (the fill stands apart from the outline), where a solid outline is unbroken.
            var gaps = 0, dashes = 0, solidGaps = 0
            for (var t = 1; t < 8; t++) {
                var ex = Math.round(tc.cx + t), ey = Math.round(tc.cy - 8 + t)
                var c = rgb(image, ex, ey)
                if (c.r < 80)
                    gaps++
                else if (c.r > 180)
                    dashes++
                if (named.red(ex, ey) < 80)
                    solidGaps++
            }
            verify(gaps >= 2 && dashes >= 2, "dashes and gaps on the edge: " + dashes + " / " + gaps)
            compare(solidGaps, 0, "a confirmed source's outline is unbroken")
            compare(countNear(image, tc.cx + 12, tc.cy - 10, root.width, tc.cy + 10, 255, 255, 0, 50), 0,
                    "no name for an unconfirmed source")
        }

        function test_08_an_accuracy_circle_is_faint() {
            push({ source: 1008, lat: 10.0, lon: 20.0, errM: 500 })
            var image = grabImage(root)
            // 500 m at zoom 12 and 10 degrees north is about 13 px; 20 px out is plain map.
            var inside = rgb(image, tc.cx, tc.cy - 11)
            var outside = rgb(image, tc.cx, tc.cy - 20)
            verify(inside.g > outside.g + 10 && inside.r < 128, "a faint green tint inside: " + JSON.stringify(inside))
            verify(near(outside, 128, 128, 128, 6), "nothing outside: " + JSON.stringify(outside))
        }

        function test_09_pan_and_zoom_keep_the_map_under_the_fingers() {
            push({ source: 1009, lat: 10.0, lon: 20.0 })
            var id = map.trackAt(tc.cx, tc.cy)
            map.panBy(10, 5)
            var p = map.markerPoint(id)
            verify(Math.abs(p.x - (tc.cx + 10)) < 0.01 && Math.abs(p.y - (tc.cy + 5)) < 0.01, "content follows the finger: " + p)
            map.zoomAbout(map.zoom + 1.5, p.x, p.y)
            var q = map.markerPoint(id)
            verify(Math.abs(q.x - p.x) < 0.01 && Math.abs(q.y - p.y) < 0.01, "the anchor stays put: " + q)
            compare(map.zoom, 13.5)
            map.zoom = 40
            compare(map.zoom, map.maximumZoom)
            map.zoom = -3
            compare(map.zoom, map.minimumZoom, "the world always fills the width")
            verify(map.minimumZoom > 0.6 && map.minimumZoom < 0.7)
            map.centerOn(10.0, 20.0, 12)
            map.zoomIn()
            compare(map.zoom, 13)
            map.zoomOut()
            map.zoomOut()
            compare(map.zoom, 11)
            map.centerLatitude = -22.5
            map.centerLongitude = 45.0
            verify(Math.abs(map.centerLatitude + 22.5) < 1e-9 && Math.abs(map.centerLongitude - 45.0) < 1e-9)
        }

        function test_10_fit_shows_every_marker() {
            push({ source: 1010, lat: 10.0, lon: 20.0 })
            push({ source: 1011, lat: 10.5, lon: 21.0 })
            compare(map.markerCount, 2)
            map.fitPadding = 40
            verify(map.fitAll())
            var shown = 0
            for (var y = 0; y < root.height; y += 4) {
                for (var x = 0; x < root.width; x += 4) {
                    if (map.trackAt(x, y) > 0) {
                        var pt = map.markerPoint(map.trackAt(x, y))
                        verify(pt.x >= 39 && pt.x <= root.width - 39 && pt.y >= 39 && pt.y <= root.height - 39,
                               "inside the padding: " + pt)
                        shown++
                    }
                }
            }
            verify(shown > 0)
            testContext.resetPositions()
            waitForRendering(map)
            verify(!map.fitAll(), "nothing to fit")
        }

        function test_11_following_a_track_keeps_it_centred_until_a_pan() {
            push({ source: 1012, lat: 10.0, lon: 20.0 })
            var id = map.trackAt(tc.cx, tc.cy)
            map.followTrackId = id
            push({ source: 1012, lat: 10.0, lon: 20.05 })
            var p = map.markerPoint(id)
            verify(Math.abs(p.x - tc.cx) < 0.01 && Math.abs(p.y - tc.cy) < 0.01, "re-centred on the new fix: " + p)
            map.zoomAbout(map.zoom + 1, 0, 0)
            p = map.markerPoint(id)
            verify(Math.abs(p.x - tc.cx) < 0.01, "a zoom while following keeps the followed point centred")
            map.panBy(30, 0)
            compare(map.followTrackId, 0, "a pan ends the follow")
            testContext.resetPositions()
            map.followTrackId = id
            compare(map.followTrackId, 0, "a track the model no longer holds cannot be followed")
        }

        function test_12_the_device_position_marks_the_camera() {
            map.deviceLatitude = 10.2
            map.deviceLongitude = 20.3
            verify(!map.centerOnDevice(), "no position, nothing to centre on")
            map.deviceValid = true
            verify(!map.cameraUsedDevicePosition)
            verify(map.centerOnDevice())
            verify(map.cameraUsedDevicePosition, "the view now derives from the device")
            verify(Math.abs(map.centerLatitude - 10.2) < 1e-6 && Math.abs(map.centerLongitude - 20.3) < 1e-6)
            waitForRendering(map)
            var image = grabImage(root)
            verify(near(rgb(image, tc.cx, tc.cy), 0, 0, 255, 30), "the device's dot: " + JSON.stringify(rgb(image, tc.cx, tc.cy)))
            map.resetCameraUsedDevicePosition()
            verify(!map.cameraUsedDevicePosition)
            push({ source: 1013, lat: 10.0, lon: 20.0 })
            verify(map.fitAll())
            verify(map.cameraUsedDevicePosition, "a fit that included the dot counts too")
            map.resetCameraUsedDevicePosition()
            map.followDevice = true
            verify(map.cameraUsedDevicePosition)
            map.panBy(5, 5)
            verify(!map.followDevice, "a pan ends it")
        }

        function test_13_no_tile_request_before_tiles_are_on_and_tiles_draw_toned() {
            var before = testContext.mapTileRequests()
            map.panBy(40, 40)
            waitForRendering(map)
            compare(testContext.mapTileRequests(), before, "tiles off: nothing asked")
            var image = grabImage(root)
            verify(countNear(image, 0, 0, root.width, 2, 255, 0, 0, 60) > 0, "the plain grid instead")
            tiles.enabled = true
            tryVerify(function () { return testContext.mapTileRequests() > before }, 5000, "tiles on: the view is asked for")
            tryVerify(function () {
                var im = grabImage(root)
                return near(rgb(im, 5, 5), 0x20, 0x40, 0x60, 6) && near(rgb(im, root.width - 5, root.height - 5), 0x20, 0x40, 0x60, 6)
            }, 5000, "white tiles drawn in the paper colour")
            map.active = false
            waitForRendering(map)
            image = grabImage(root)
            verify(near(rgb(image, 5, 5), 128, 128, 128, 6), "inactive: tiles dropped")
            var asked = testContext.mapTileRequests()
            map.panBy(300, 0)
            wait(100)
            compare(testContext.mapTileRequests(), asked, "inactive: nothing asked")
            map.active = true
            tiles.enabled = false
            waitForRendering(map)
            image = grabImage(root)
            verify(near(rgb(image, 5, 5), 128, 128, 128, 6), "tiles off: none left on screen")
        }
    }
}
