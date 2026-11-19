// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window
import DsdNeo 1.0
import "MapText.js" as MapText

// Issue #800 (spec 4.6): where the radios that sent their position are,
// glanceable while audio plays. A quiet canvas (greyscale tiles, or a plain
// grid before tiles are on) so the radios carry the colour; the marks are
// MapView's, one visual variable per state. Over it: an age filter under the
// top bar, a status column at the top (tile trouble, what is followed, what
// location says), the controls at the bottom right (zoom pair, fit, locate),
// and the tile attribution at the bottom left, which stays readable above the
// detail sheet. The List is the canvas's complete equivalent: every mark, its
// states in words, the same sheet and Follow. Every map action has a button;
// the canvas itself is not in the accessibility tree.
//
// The tile settings, the age filter and the view are kept in this device's
// prefs (map/*, never in a backup); follow is never saved.
Item {
    id: screen

    signal closed

    readonly property string defaultTileUrl: "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
    readonly property string defaultAttribution: "© OpenStreetMap contributors"
    readonly property string copyrightUrl: "https://www.openstreetmap.org/copyright"

    // Spec 4.8: map/tilesEnabled, map/tileUrl, map/tileAttribution. What is
    // set here is written back; what the prefs already say is not.
    property bool tilesEnabled: prefs.mapTilesEnabled === true
    property string tileUrl: prefs.mapTileUrl || defaultTileUrl
    property string tileAttribution: prefs.mapTileAttribution || defaultAttribution
    onTilesEnabledChanged: if (tilesEnabled !== (prefs.mapTilesEnabled === true)) prefs.mapTilesEnabled = tilesEnabled
    onTileUrlChanged: if (tileUrl !== (prefs.mapTileUrl || defaultTileUrl)) prefs.mapTileUrl = tileUrl
    onTileAttributionChanged: if (tileAttribution !== (prefs.mapTileAttribution || defaultAttribution)) prefs.mapTileAttribution = tileAttribution

    // Which view shows: the map (true) or the List.
    property bool mapShown: true
    // A History row's saved fix dropped as one pin ("Show on map"), or null.
    property var pin: null
    // The tiles prompt set aside: for the rest of the app's run (the screen
    // is kept between visits). A listener who keeps tiles off on purpose is
    // not asked again on every open.
    property bool tilesPromptDismissed: false
    // Whether the camera was set up once; the view keeps its place after.
    property bool cameraPlaced: false
    // Whether the app is in the foreground. On Android an Item stays visible
    // while the app is hidden, so the live position and the age clock follow
    // the application's state, not only `visible`. Inactive (a dialog on top)
    // still counts as foreground, as the location feed reads it.
    property bool appForeground: Qt.application.state !== Qt.ApplicationHidden && Qt.application.state !== Qt.ApplicationSuspended
    Connections {
        target: Qt.application
        function onStateChanged(state) {
            screen.appForeground = state !== Qt.ApplicationHidden && state !== Qt.ApplicationSuspended;
        }
    }
    // Moves on every track change, for bindings that look a track up by id.
    property int tracksRevision: 0
    Connections {
        target: positions
        function onTracksChanged() {
            screen.tracksRevision++;
        }
    }

    // The age filter (map/ageFilter): 0 all, else seconds.
    readonly property var ageChoices: [
        {text: qsTr("Last 15 min"), name: qsTr("Last 15 minutes"), seconds: 900},
        {text: qsTr("Last hour"), name: qsTr("Last hour"), seconds: 3600},
        {text: qsTr("All"), name: qsTr("All positions"), seconds: 0}
    ]

    readonly property bool emptyLog: positions.count === 0 && !pin
    // Tracks only (final review M2): a gap row alone is no radio shown.
    readonly property bool emptyFilter: !emptyLog && positionView.trackCount === 0 && !pin
    readonly property bool tilesAvailable: tiles.available
    readonly property string tilesHost: tiles.templateHost(tileUrl)
    // OpenStreetMap's own server, however its address is written (a port, a
    // lettered name, upper case): its credit links to its copyright page.
    readonly property bool attributionLinked: MapText.isOsmTileHost(tilesHost)
    // The credit shown: OpenStreetMap's own tiles always with theirs, even
    // under a saved attribution that leaves it out (review: the default
    // source's credit is owed, whatever the field says).
    readonly property string shownAttribution: MapText.shownCredit(tilesHost, tileAttribution, defaultAttribution)

    Keys.onEscapePressed: Navigation.back(screen.Window.window)
    Keys.onBackPressed: Navigation.back(screen.Window.window)

    onVisibleChanged: {
        if (visible) {
            map.resetCameraUsedDevicePosition();
            Qt.callLater(screen.placeCamera);
            // With a screen reader on, the focus goes to Back, a node: TalkBack
            // follows only a focus change on one (issue #800, P9), so it no
            // longer stays a moment on the covered control that opened the
            // map. Without one, the screen takes it, as every pushed screen's.
            Qt.callLater(function () {
                if (!screen.visible || !Navigation.allows(screen))
                    return;
                if (typeof screenReader !== "undefined" && screenReader !== null && screenReader.active)
                    back.forceActiveFocus();
                else
                    screen.forceActiveFocus();
            });
        } else {
            positionSheet.visible = false;
            locator.stopFollow();
            // Leaving puts back a view the device did not shape (the keeper),
            // so the next press locates again rather than following.
            locator.cameraMoved();
        }
    }

    // The first open shows every radio (or the saved dot); later opens keep
    // the view where it was left.
    function placeCamera() {
        if (cameraPlaced || !visible)
            return;
        cameraPlaced = true;
        if (pin)
            return;
        if (!map.fitAll() && locator.deviceValid)
            map.centerOnDevice();
    }

    // A History row's fix ("Show on map"): its pin, centred, on the map.
    function showPin(record) {
        if (!record || !record.position || record.position.latitude === undefined)
            return;
        pin = record;
        mapShown = true;
        cameraPlaced = true;
        map.selectedTrackId = 0;
        // A one-shot move: a follow left on would take the camera back on its
        // next fix.
        endFollows();
        // The pin's chip laid out first, so the pin lands below it.
        status.forceLayout();
        map.centerOn(record.position.latitude, record.position.longitude, Math.max(map.zoom, 14));
    }
    function clearPin() {
        pin = null;
    }
    // The pin's sheet: from a tap on it, its chip on the map, or its List row.
    function openPinDetails() {
        if (!pin)
            return;
        selectTrack(0);
        positionSheet.openPin(pin);
    }

    function selectTrack(id) {
        map.selectedTrackId = id;
    }
    function openTrack(id, fromList) {
        selectTrack(id);
        positionSheet.fromList = fromList;
        positionSheet.open(id);
    }
    function follow(id) {
        // An anonymous report is a single point that nothing moves: shown,
        // never followed (review r2).
        if (positions.trackFor(id).anonymous === true)
            return;
        locator.stopFollow();
        // The camera goes to the radio, off the found dot: the next press
        // locates again (review r2).
        locator.cameraMoved();
        mapShown = true;
        map.selectedTrackId = id;
        map.followTrackId = id;
    }
    // Every follow ends: the radio's and the device's. Before an explicit
    // one-shot camera move, which a follow would otherwise undo on its next
    // fix (review r1). The view leaves the found dot too, so the next press
    // locates again rather than following.
    function endFollows() {
        map.followTrackId = 0;
        locator.stopFollow();
        locator.cameraMoved();
    }
    function showOnMap(id) {
        // Showing the radio already followed keeps following it.
        if (map.followTrackId !== id)
            endFollows();
        mapShown = true;
        map.selectedTrackId = id;
        map.centerOnTrack(id);
    }
    // A tap on the canvas: the pin first (it is drawn over the marks), then a
    // marker opens its sheet; empty map clears the selection.
    function tapAt(x, y) {
        if (pin && pinMark.visible && Math.abs(x - pinMark.tipX) <= map.hitRadius && y <= pinMark.tipY + 6 && y >= pinMark.tipY - pinMark.height) {
            openPinDetails();
            return;
        }
        var id = map.trackAt(x, y);
        if (id > 0) {
            openTrack(id, false);
            return;
        }
        selectTrack(0);
    }
    function userMovedCamera() {
        locator.cameraMoved();
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // ---- Top bar and the age filter ----
    // One row when everything fits (a tablet, landscape). Otherwise the
    // filter takes a second row, and where even the List/Map choice does not
    // fit beside the title (a narrow phone at a large text size) it leads
    // that row. Nothing is clipped at any text size: the filter pills wrap.
    TextMetrics {
        id: segmentMeasure
        font.family: Theme.sans
        font.pixelSize: Math.round(Theme.fontSize(13))
        font.weight: Font.DemiBold
        text: qsTr("List").length > qsTr("Map").length ? qsTr("List") : qsTr("Map")
    }
    Item {
        id: chrome

        x: Theme.screenPadding
        y: 10
        width: Math.max(0, screen.width - 2 * Theme.screenPadding)
        height: filterStrip.y + filterStrip.height

        readonly property real rowHeight: Math.max(48, segment.height)
        readonly property real gap: 12
        readonly property real titleEnd: back.width + 12 + title.implicitWidth
        readonly property real segmentWidth: 2 * (Math.ceil(segmentMeasure.advanceWidth) + 28) + 6
        readonly property real pillsWidth: pill0.implicitWidth + pill1.implicitWidth + pill2.implicitWidth + 2 * filterStrip.spacing
        readonly property real trailing: segmentWidth + 8 + more.width
        readonly property bool oneRow: titleEnd + gap + pillsWidth + gap + trailing <= width
        readonly property bool segmentInBar: titleEnd + gap + trailing <= width
        readonly property real secondRowY: rowHeight + 8
        readonly property bool pillsBesideSegment: !segmentInBar && segmentWidth + gap + pillsWidth <= width

        IconButton {
            id: back
            objectName: "mapBack"
            icon: "back"
            y: (chrome.rowHeight - height) / 2
            onClicked: screen.closed()
        }
        ReadableText {
            id: title
            objectName: "mapTitle"
            heading: true
            x: back.width + 12
            y: (chrome.rowHeight - height) / 2
            text: qsTr("Map")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(22)
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
        }
        SegmentedControl {
            id: segment
            objectName: "mapViewChoice"
            accessibleName: qsTr("Show as")
            width: chrome.segmentInBar ? chrome.segmentWidth : Math.min(chrome.segmentWidth, chrome.width)
            height: Math.max(48, labelHeight + 22)
            x: chrome.segmentInBar ? chrome.width - more.width - 8 - width : 0
            y: chrome.segmentInBar ? (chrome.rowHeight - height) / 2 : chrome.secondRowY
            model: [qsTr("List"), qsTr("Map")]
            currentIndex: screen.mapShown ? 1 : 0
            onSelected: function (index) {
                screen.mapShown = index === 1;
            }
        }
        IconButton {
            id: more
            objectName: "mapMore"
            icon: "more"
            accessibleName: qsTr("Map options")
            x: chrome.width - width
            y: (chrome.rowHeight - height) / 2
            onClicked: overflow.open()
        }

        // The age filter: one of three, as History's pills, each a radio
        // button that says whether it is the checked one.
        Flow {
            id: filterStrip
            objectName: "mapAgeFilter"
            spacing: 10
            x: chrome.oneRow ? chrome.titleEnd + chrome.gap : chrome.pillsBesideSegment ? chrome.segmentWidth + chrome.gap : 0
            y: chrome.oneRow ? (chrome.rowHeight - pill0.height) / 2 : chrome.segmentInBar || chrome.pillsBesideSegment ? chrome.secondRowY : segment.y + segment.height + 8
            width: chrome.oneRow ? chrome.pillsWidth + 1 : chrome.width - x

            FilterPill {
                id: pill0
                objectName: "mapAge0"
                readonly property var choice: screen.ageChoices[0]
                wrapLabel: true
                width: Math.min(implicitWidth, filterStrip.width)
                caret: false
                text: choice.text
                accessibleName: choice.name
                active: positions.ageFilterSeconds === choice.seconds
                Accessible.role: Accessible.RadioButton
                Accessible.checkable: true
                Accessible.checked: active
                Accessible.selected: false
                onClicked: positions.ageFilterSeconds = choice.seconds
            }
            FilterPill {
                id: pill1
                objectName: "mapAge1"
                readonly property var choice: screen.ageChoices[1]
                wrapLabel: true
                width: Math.min(implicitWidth, filterStrip.width)
                caret: false
                text: choice.text
                accessibleName: choice.name
                active: positions.ageFilterSeconds === choice.seconds
                Accessible.role: Accessible.RadioButton
                Accessible.checkable: true
                Accessible.checked: active
                Accessible.selected: false
                onClicked: positions.ageFilterSeconds = choice.seconds
            }
            FilterPill {
                id: pill2
                objectName: "mapAge2"
                readonly property var choice: screen.ageChoices[2]
                wrapLabel: true
                width: Math.min(implicitWidth, filterStrip.width)
                caret: false
                text: choice.text
                accessibleName: choice.name
                active: positions.ageFilterSeconds === choice.seconds
                Accessible.role: Accessible.RadioButton
                Accessible.checkable: true
                Accessible.checked: active
                Accessible.selected: false
                onClicked: positions.ageFilterSeconds = choice.seconds
            }
        }
    }

    // ---- Tiles off: the prompt, between the bar and the map ----
    // Laid out above the canvas rather than over it, so it never covers a
    // radio and a fit ("Show every radio") has the whole canvas to show them
    // in. Set aside, it stays away for the app's run; the empty state and
    // Map settings still offer tiles.
    MapTilesPrompt {
        id: tilesPrompt
        objectName: "mapTilesPrompt"
        x: Theme.screenPadding
        y: chrome.y + chrome.height + 10
        width: Math.max(0, screen.width - 2 * Theme.screenPadding)
        visible: screen.mapShown && !screen.tilesEnabled && screen.tilesAvailable && !screen.tilesPromptDismissed
        host: screen.tilesHost
        onLoad: screen.tilesEnabled = true
        onDismiss: screen.tilesPromptDismissed = true
    }

    // ---- The canvas and the List ----
    Item {
        id: area
        objectName: "mapArea"

        anchors.top: tilesPrompt.visible ? tilesPrompt.bottom : chrome.bottom
        anchors.topMargin: 10
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        // The view while the canvas is the List's: its own edge.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Theme.panelBorder
            z: 2
        }

        Rectangle {
            anchors.fill: parent
            visible: screen.mapShown
            color: Theme.mapPaper
        }

        MapTileSource {
            id: tiles
            enabled: screen.tilesEnabled
            urlTemplate: screen.tileUrl
        }

        MapView {
            id: map
            objectName: "mapCanvas"

            anchors.fill: parent
            visible: screen.mapShown
            active: screen.mapShown && screen.visible
            model: positions
            tileSource: tiles
            // The List is the canvas's words; a reader never lands here.
            Accessible.ignored: true

            markerColor: Theme.textPrimary
            haloColor: Theme.bg
            alertColor: Theme.alert
            selectedColor: Theme.cyan
            trailColor: Theme.textSecondary
            accuracyColor: Qt.alpha(Theme.textSecondary, Theme.dark ? 0.16 : 0.14)
            deviceColor: Theme.mapDevice
            labelColor: Theme.textPrimary
            gridColor: Theme.mapGrid
            tileInkColor: Theme.mapTileInk
            tilePaperColor: Theme.mapPaper
            labelFont.family: Theme.sans
            labelFont.pixelSize: Math.round(Theme.fontSize(12))
            labelFont.weight: Font.DemiBold
            emergencyText: qsTr("Emergency")
            hitRadius: Theme.minimumTouchSize / 2
            // Room for a label beside a marker at the edge (more at a larger
            // text size), less on a short landscape canvas (a quarter of its
            // height at most), where it would cost the fit most of its zoom.
            fitPadding: Math.min(Math.round(72 * Theme.fontSize(12) / 12), height / 4)
            // The chips at the top (the pin, what is followed, tile trouble,
            // location notices) cover the canvas there: fits and centring aim
            // below them.
            topInset: status.visible && status.height > 0 ? status.y + status.height : 0
            // What a label keeps clear of: the control column, the chips at
            // the top and the attribution (in the canvas's coordinates; the
            // attribution sits beside the area, not in it). A label with no
            // clear side is left out; the List has every name.
            labelObstacles: [
                controls.visible ? Qt.rect(controls.x, controls.y, controls.width, controls.height) : Qt.rect(0, 0, 0, 0),
                status.visible ? Qt.rect(status.x, status.y, status.width, status.height) : Qt.rect(0, 0, 0, 0),
                attribution.visible ? Qt.rect(attribution.x - area.x, attribution.y - area.y, attribution.width, attribution.height) : Qt.rect(0, 0, 0, 0)
            ]
            cameraDurationMs: Theme.reducedMotion ? 0 : 250

            deviceValid: locator.deviceValid
            deviceLatitude: locator.deviceLatitude
            deviceLongitude: locator.deviceLongitude
            deviceAccuracyM: locator.deviceAccuracyM

            onFollowChanged: {
                if (locator.followMe && !followDevice)
                    locator.stopFollow();
            }

            // Drags, pinches and the wheel are not taps (GatedTapHandler):
            // like Spectrum's, they stand down while a sheet covers the map,
            // or a drag on its backdrop would move the view under it.
            DragHandler {
                id: drag
                enabled: Navigation.allows(map)
                target: null
                maximumPointCount: 1
                onActiveChanged: {
                    if (active)
                        screen.userMovedCamera();
                }
                onTranslationChanged: function (delta) {
                    map.panBy(delta.x, delta.y);
                }
            }
            PinchHandler {
                id: pinch
                enabled: Navigation.allows(map)
                target: null
                property real startZoom: 0
                onActiveChanged: {
                    if (active) {
                        startZoom = map.zoom;
                        screen.userMovedCamera();
                    }
                }
                onActiveScaleChanged: {
                    if (active && activeScale > 0)
                        map.zoomAbout(startZoom + Math.log2(activeScale), centroid.position.x, centroid.position.y);
                }
            }
            WheelHandler {
                id: wheel
                enabled: Navigation.allows(map)
                target: null
                onWheel: function (event) {
                    screen.userMovedCamera();
                    map.zoomAbout(map.zoom + event.angleDelta.y / 240, wheel.point.position.x, wheel.point.position.y);
                }
            }
            GatedTapHandler {
                onAllowedTap: function (eventPoint) {
                    screen.tapAt(eventPoint.position.x, eventPoint.position.y);
                }
            }
        }

        // The pin a History row drops: a drawn map pin, a shape no live mark
        // uses, its tip on the row's saved fix. Clipped to the canvas, as the
        // marks are: a pin panned off the map leaves it, never painting over
        // the chips' row or the top bar.
        Item {
            id: pinLayer
            anchors.fill: map
            visible: screen.mapShown
            clip: true

            Item {
                id: pinMark
                objectName: "mapPin"
                readonly property var at: {
                    var lat = map.centerLatitude, lon = map.centerLongitude, z = map.zoom, w = map.width, h = map.height;
                    return screen.pin ? map.pointFor(screen.pin.position.latitude, screen.pin.position.longitude) : undefined;
                }
                readonly property real tipX: at ? at.x : 0
                readonly property real tipY: at ? at.y : 0
                visible: screen.mapShown && at !== undefined
                x: tipX - width / 2
                y: tipY - height
                width: 26
                height: 34
                Accessible.ignored: true

                Canvas {
                    id: pinCanvas
                    anchors.fill: parent
                    onPaint: {
                        var ctx = getContext("2d");
                        ctx.reset();
                        var w = width, h = height, c = w / 2, r = 10, cy = r + 2;
                        ctx.beginPath();
                        ctx.moveTo(c, h - 1);
                        ctx.bezierCurveTo(c - 4, h - 10, c - r - 1, cy + 6, c - r, cy);
                        ctx.arc(c, cy, r, Math.PI, 0, false);
                        ctx.bezierCurveTo(c + r + 1, cy + 6, c + 4, h - 10, c, h - 1);
                        ctx.closePath();
                        ctx.lineWidth = 2.5;
                        ctx.strokeStyle = String(Theme.bg);
                        ctx.stroke();
                        ctx.fillStyle = String(Theme.textPrimary);
                        ctx.fill();
                        ctx.beginPath();
                        ctx.arc(c, cy, 3.5, 0, 2 * Math.PI);
                        ctx.fillStyle = String(Theme.bg);
                        ctx.fill();
                    }
                    Connections {
                        target: Theme
                        function onDarkChanged() {
                            pinCanvas.requestPaint();
                        }
                    }
                }
            }
        }

        // ---- Status: tile trouble, what is followed, what location says ----
        Column {
            id: status
            objectName: "mapStatus"
            visible: screen.mapShown
            anchors.top: parent.top
            anchors.topMargin: 12
            // Centred over the canvas; on a short one, where the controls lie
            // in a row along the bottom, the chips keep to the room left of
            // them, so a stack of chips never runs into a control (review r2).
            readonly property real room: controls.arrangement === 0 ? parent.width - 2 * 12 : parent.width - controls.width - 3 * 12
            x: controls.arrangement === 0 ? (parent.width - width) / 2 : 12
            width: Math.max(0, Math.min(parent.width >= 640 ? 600 : 420, room))
            spacing: 8

            MapChip {
                objectName: "mapTilesState"
                visible: screen.tilesEnabled && (tiles.state === MapTileSource.Retrying || tiles.state === MapTileSource.Unavailable || tiles.state === MapTileSource.InvalidSource || tiles.state === MapTileSource.NoPngTiles)
                text: tiles.state === MapTileSource.Retrying ? qsTr("Map tiles unavailable — retrying")
                    : tiles.state === MapTileSource.Unavailable ? qsTr("Online map tiles are not available in this build")
                    : tiles.state === MapTileSource.NoPngTiles ? qsTr("The tile server sent no PNG tiles the map can use. Check Map settings.")
                    : qsTr("The tile address is not valid. Check Map settings.")
            }
            // The pin's words and its button: the drawn pin is not a node,
            // so this is how a screen reader or keyboard reaches its sheet.
            MapChip {
                objectName: "mapPinChip"
                visible: screen.pin !== null
                text: screen.pin ? qsTr("Saved position from History: %1").arg(screen.pin.name || screen.pin.sourceName || qsTr("unnamed")) : ""
                actionText: qsTr("Details")
                actionName: qsTr("Details of the saved position")
                onAction: screen.openPinDetails()
            }
            MapChip {
                objectName: "mapFollowing"
                visible: map.followTrackId > 0 || locator.followMe
                text: locator.followMe ? qsTr("Following your position") : qsTr("Following %1").arg(MapText.trackName(screen.tracksRevision >= 0 ? positions.trackFor(map.followTrackId) : null))
                actionText: qsTr("Stop")
                actionName: qsTr("Stop following")
                onAction: {
                    if (locator.followMe)
                        locator.stopFollow();
                    map.followTrackId = 0;
                    map.followDevice = false;
                }
            }
            MapChip {
                objectName: "mapLocationNotice"
                visible: locator.notice.length > 0
                text: locator.notice
                actionText: locator.noticeAction.length > 0 ? qsTr("Settings") : qsTr("OK")
                actionName: locator.noticeAction.length > 0 ? qsTr("Open location settings") : qsTr("Dismiss")
                onAction: {
                    locator.openNoticeAction();
                    locator.clearNotice();
                }
            }
        }

        // ---- Controls ----
        // A column at the bottom right while the canvas holds it; on a short
        // canvas (landscape, the tiles prompt above, a large text size) a row,
        // and on a shorter one the zoom pair lies flat too, so no control ever
        // reaches above the canvas over the prompt or the chips (review r2).
        Grid {
            id: controls
            objectName: "mapControls"
            visible: screen.mapShown
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            rowSpacing: 10
            columnSpacing: 10
            verticalItemAlignment: Grid.AlignBottom
            // 0: a column; 1: a row; 2: a row with the zoom pair flat.
            readonly property int arrangement: {
                var t = Theme.minimumTouchSize, room = parent.height - 2 * anchors.margins;
                var column = 2 * t + 1 + 10 + t + (locator.supported ? 10 + t : 0);
                return column <= room ? 0 : 2 * t + 1 <= room ? 1 : 2;
            }
            columns: arrangement === 0 ? 1 : 3

            Rectangle {
                id: zoomPair
                readonly property bool flat: controls.arrangement === 2
                width: flat ? 2 * Theme.minimumTouchSize + 1 : Theme.minimumTouchSize
                height: flat ? Theme.minimumTouchSize : 2 * Theme.minimumTouchSize + 1
                radius: Theme.radiusButton
                color: Theme.mapOverlay
                border.width: 1
                border.color: Theme.controlBorder

                MapControlButton {
                    objectName: "mapZoomIn"
                    framed: false
                    shape: "plus"
                    accessibleName: qsTr("Zoom in")
                    enabled: map.zoom < map.maximumZoom - 0.01
                    onClicked: {
                        screen.userMovedCamera();
                        map.zoomIn();
                    }
                }
                Rectangle {
                    x: zoomPair.flat ? Theme.minimumTouchSize : 8
                    y: zoomPair.flat ? 8 : Theme.minimumTouchSize
                    width: zoomPair.flat ? 1 : parent.width - 16
                    height: zoomPair.flat ? parent.height - 16 : 1
                    color: Theme.controlBorder
                }
                MapControlButton {
                    objectName: "mapZoomOut"
                    x: zoomPair.flat ? Theme.minimumTouchSize + 1 : 0
                    y: zoomPair.flat ? 0 : Theme.minimumTouchSize + 1
                    framed: false
                    shape: "minus"
                    accessibleName: qsTr("Zoom out")
                    enabled: map.zoom > map.minimumZoom + 0.01
                    onClicked: {
                        screen.userMovedCamera();
                        map.zoomOut();
                    }
                }
            }
            MapControlButton {
                objectName: "mapFit"
                shape: "fit"
                accessibleName: qsTr("Show every radio")
                enabled: map.markerCount > 0 || locator.deviceValid || screen.pin !== null
                onClicked: {
                    screen.userMovedCamera();
                    if (screen.pin)
                        map.fitAllWith(screen.pin.position.latitude, screen.pin.position.longitude);
                    else
                        map.fitAll();
                }
            }
            MapControlButton {
                objectName: "mapLocate"
                visible: locator.supported
                shape: locator.followMe ? "follow" : "locate"
                checkable: true
                checked: locator.followMe
                accessibleName: locator.actionName
                accessibleDescription: locator.busyElsewhere ? qsTr("Another screen is finding your location") : ""
                enabled: !locator.busyElsewhere && !locator.locating
                onClicked: locator.act()
            }
        }

        MapList {
            id: positionList
            objectName: "mapPositionList"
            anchors.fill: parent
            // Shown when filtered empty too: its gap row still says where
            // positions may be missing, above the empty state's Show all. The
            // device's own dot is a row of its own (review r1: the canvas's
            // mark and its accuracy, in words), so a located device shows the
            // List even before any radio is heard. The List holds no live
            // lease (follow runs only on a shown map): it is the saved fix.
            visible: !screen.mapShown && (!screen.emptyLog || locator.deviceValid)
            selectedTrackId: map.selectedTrackId
            followTrackId: map.followTrackId
            deviceKnown: locator.deviceValid
            deviceLive: locator.live
            deviceLatitude: locator.deviceLatitude
            deviceLongitude: locator.deviceLongitude
            deviceAccuracyM: locator.deviceAccuracyM
            deviceSavedAtMs: prefs.lastFixAt
            pin: screen.pin
            onActivated: function (trackId) {
                screen.openTrack(trackId, true);
            }
            onPinActivated: screen.openPinDetails()
            onDeviceActivated: {
                screen.endFollows();
                screen.mapShown = true;
                screen.userMovedCamera();
                map.centerOnDevice();
            }
        }

        // ---- Empty ----
        // Declared after the List: Tab and a screen reader follow item order,
        // so the List's gap row above it is read first (final review F2-M1).
        // Beside the controls, never over them: centred while it fits, and
        // scrolling from the top when a short landscape screen or a large
        // text size makes it taller than the room.
        PlexFlickable {
            id: emptyScroll
            objectName: "mapEmptyScroll"
            accessibleName: qsTr("Map explanation")
            visible: screen.emptyLog || screen.emptyFilter
            // Over the List, under its gap row when it has one (the List
            // keeps that row; this offers Show all below it).
            z: 1
            anchors.top: parent.top
            anchors.topMargin: !screen.mapShown ? (positionList.visible ? positionList.usedHeight + 4 : 0)
                : status.visible && status.height > 0 ? status.height + 12 : 0
            anchors.left: parent.left
            anchors.right: screen.mapShown ? controls.left : parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: screen.mapShown ? attribution.height : 0
            contentWidth: width
            contentHeight: Math.max(height, emptyState.y + emptyState.height + 12)
            clip: true
            interactive: contentHeight > height

            // The device's dot on the map, in this scroll's coordinates, while
            // it is drawn: the card keeps clear of it (review r1), since a
            // Locate me with no radio heard centres the dot right where a
            // centred card would hide it.
            readonly property var dot: {
                var lat = map.centerLatitude, lon = map.centerLongitude, z = map.zoom, w = map.width, h = map.height, top = emptyScroll.y;
                if (!screen.mapShown || !locator.deviceValid)
                    return undefined;
                var p = map.pointFor(locator.deviceLatitude, locator.deviceLongitude);
                return p ? Qt.point(p.x - emptyScroll.x, p.y - emptyScroll.y) : undefined;
            }
            // Centred while that clears the dot (with room for its accuracy
            // ring's edge and a finger), else in the larger room above or
            // below it, else below it, scrolling.
            function cardY(cardHeight) {
                var margin = 12, gap = 28;
                var centred = Math.max(margin, (height - cardHeight) / 2);
                var p = dot;
                if (!p || p.x < emptyState.x - gap || p.x > emptyState.x + emptyState.width + gap
                        || p.y < centred - gap || p.y > centred + cardHeight + gap)
                    return centred;
                var above = p.y - gap - margin;
                var below = height - margin - (p.y + gap);
                if (cardHeight <= below && (below >= above || cardHeight > above))
                    return p.y + gap + (below - cardHeight) / 2;
                if (cardHeight <= above)
                    return margin + (above - cardHeight) / 2;
                return Math.max(margin, p.y + gap);
            }
            boundsBehavior: Flickable.StopAtBounds

            MapEmptyState {
                id: emptyState
                objectName: "mapEmptyState"
                x: (emptyScroll.width - width) / 2
                y: emptyScroll.cardY(height)
                width: Math.max(0, Math.min(400, emptyScroll.width - 2 * 12))
                filtered: screen.emptyFilter
                hiddenCount: positions.count
                filterName: positions.ageFilterSeconds === 900 ? qsTr("the last 15 minutes") : qsTr("the last hour")
                // Offered here only once the prompt above is set aside.
                showTilesPrompt: screen.mapShown && !screen.tilesEnabled && screen.tilesAvailable && screen.tilesPromptDismissed
                tilesHost: screen.tilesHost
                onShowAll: positions.ageFilterSeconds = 0
                onLoadTiles: screen.tilesEnabled = true
            }
        }
    }

    // The tile attribution, kept readable above the detail sheet (the sheet's
    // panel stops short of it), and a link to the copyright page for the
    // default server.
    Item {
        id: attribution
        objectName: "mapAttribution"
        visible: screen.mapShown
        z: positionSheet.visible ? 300 : 3
        anchors.left: area.left
        anchors.bottom: area.bottom
        anchors.leftMargin: 8
        anchors.bottomMargin: 4
        width: Math.min(chip.implicitWidth, area.width - controls.width - 3 * 12)
        height: Math.max(Theme.minimumTouchSize, chip.height + 8)
        // What the detail sheet leaves clear: the chip as drawn, not its touch area.
        readonly property real chipHeight: chip.height + chip.anchors.bottomMargin + anchors.bottomMargin

        Accessible.role: screen.attributionLinked ? Accessible.Link : Accessible.StaticText
        Accessible.name: navigationAllowed && visible ? screen.shownAttribution : ""
        Accessible.description: screen.attributionLinked && navigationAllowed ? qsTr("Opens the map data copyright page") : ""
        readonly property bool navigationAllowed: Navigation.allows(attribution)
        Accessible.focusable: navigationAllowed && visible
        Accessible.ignored: !navigationAllowed || !visible
        Accessible.onPressAction: activate()
        activeFocusOnTab: (navigationAllowed && visible && screen.attributionLinked) || activeFocus
        function activate() {
            if (screen.attributionLinked && Navigation.allows(attribution))
                Qt.openUrlExternally(screen.copyrightUrl);
        }
        Keys.onPressed: function (event) {
            if (Navigation.activationPress(event, Window.activeFocusItem))
                activate();
        }
        Keys.onReleased: function (event) {
            Navigation.activationRelease(event);
        }
        FocusFrame {}

        Rectangle {
            id: chip
            implicitWidth: attributionText.implicitWidth + 16
            width: parent.width
            height: attributionText.height + 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 4
            radius: 6
            color: Theme.mapOverlay
            border.width: 1
            border.color: Theme.panelBorder

            // Wraps rather than cuts: the credit is owed whole, at any text size.
            Text {
                id: attributionText
                objectName: "mapAttributionText"
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - 16)
                text: screen.shownAttribution
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(11)
                font.underline: screen.attributionLinked
                color: Theme.textSecondary
            }
        }
        GatedTapHandler {
            onAllowedTap: attribution.activate()
        }
    }

    MapLocator {
        id: locator
        objectName: "mapLocator"
        shown: screen.visible && screen.mapShown
        foreground: screen.appForeground
        onLocated: {
            screen.userMovedCamera();
            // A radio followed would take the camera back from the dot.
            map.followTrackId = 0;
            locator.readyToFollow = true;
            map.centerOnDevice();
        }
        onFollowMeChanged: map.followDevice = followMe
    }

    // Ages move once a second while the screen shows, on the canvas and in
    // the List alike (the filter goes stale otherwise).
    Binding {
        target: positions
        property: "ageing"
        value: screen.visible && screen.appForeground
    }

    // Spec 4.8: the age filter and the view are kept between visits.
    MapViewKeeper {
        screen: screen
        map: map
    }

    MapPositionSheet {
        id: positionSheet
        anchors.fill: parent
        following: (trackId > 0 && map.followTrackId === trackId)
        bottomReserve: screen.mapShown ? attribution.chipHeight + 8 : 0
        deviceKnown: locator.deviceValid
        deviceLive: locator.live
        deviceLastKnown: locator.lastKnown
        deviceLatitude: locator.deviceLatitude
        deviceLongitude: locator.deviceLongitude
        onFollowRequested: function (trackId) {
            screen.follow(trackId);
        }
        onStopFollowRequested: map.followTrackId = 0
        onShowOnMapRequested: function (trackId) {
            screen.showOnMap(trackId);
        }
    }

    ActionMenu {
        id: overflow
        objectName: "mapOverflow"
        anchors.fill: parent
        title: qsTr("Map options")
        actions: [
            {text: qsTr("Map settings"), key: "settings", objectName: "mapMenuSettings"},
            {text: qsTr("Clear the map"), key: "clear", objectName: "mapMenuClear", destructive: true, enabled: function () {
                return positions.count > 0 || screen.pin !== null;
            }}
        ]
        onTriggered: function (index) {
            if (actions[index].key === "settings")
                settingsSheet.openEditor();
            else
                confirmClear.open();
        }
    }

    MapSettingsSheet {
        id: settingsSheet
        anchors.fill: parent
        tileSource: tiles
        savedEnabled: screen.tilesEnabled
        savedUrl: screen.tileUrl
        savedAttribution: screen.tileAttribution
        defaultUrl: screen.defaultTileUrl
        defaultAttribution: screen.defaultAttribution
        onSaved: function (enabled, url, attributionText) {
            screen.tileUrl = url;
            screen.tileAttribution = attributionText;
            screen.tilesEnabled = enabled;
        }
    }

    ConfirmDialog {
        id: confirmClear
        objectName: "mapClearConfirm"
        anchors.fill: parent
        title: qsTr("Clear the map?")
        message: qsTr("Every radio's position and trail is removed from the map. History keeps its rows, and new positions show as they're heard.")
        confirmText: qsTr("Clear the map")
        destructive: true
        onConfirmed: {
            map.followTrackId = 0;
            map.selectedTrackId = 0;
            screen.pin = null;
            positions.clear();
        }
    }

    // A quiet line over the map's top: a state, and at most one action.
    component MapChip: Rectangle {
        id: chipItem

        property string text: ""
        property string actionText: ""
        property string actionName: actionText
        signal action

        anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
        width: Math.min(parent ? parent.width : 0, chipText.implicitWidth + 28 + (chipAction.visible ? chipAction.implicitWidth + 4 : 0))
        height: Math.max(40, chipText.height + 16)
        radius: Theme.radiusChip
        color: Theme.mapOverlay
        border.width: 1
        border.color: Theme.panelBorder

        ReadableText {
            id: chipText
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, parent.width - 28 - (chipAction.visible ? chipAction.width + 4 : 0))
            text: chipItem.text
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.textPrimary
        }
        OutlineButton {
            id: chipAction
            objectName: chipItem.objectName + "Action"
            visible: chipItem.actionText.length > 0
            flat: true
            anchors.right: parent.right
            anchors.rightMargin: 2
            anchors.verticalCenter: parent.verticalCenter
            text: chipItem.actionText
            accessibleName: chipItem.actionName
            onClicked: chipItem.action()
        }
    }

    // Tiles are off: what loading them sends, and where. One line of text
    // with its button beside it where there is room, else the button under
    // it; a close mark sets it aside for the rest of the app's run.
    component MapTilesPrompt: UiPanel {
        id: prompt

        property string host: ""
        signal load
        signal dismiss

        readonly property bool wide: width >= 520
        readonly property real closeRoom: closePrompt.width + 2
        height: (wide ? Math.max(promptText.height, loadTiles.height) : promptText.height + 6 + loadTiles.height) + 2 * 8

        ReadableText {
            id: promptText
            objectName: "mapTilesPromptHost"
            x: 14
            y: prompt.wide ? (prompt.height - height) / 2 : 8 + Math.max(0, (closePrompt.height - height) / 2)
            width: prompt.width - 14 - prompt.closeRoom - (prompt.wide ? loadTiles.width + 10 : 0)
            text: prompt.host.length > 0 ? qsTr("Map tiles are off. Loading them sends the area you view to %1.").arg(prompt.host) : qsTr("Map tiles are off. Loading them sends the area you view to the tile server.")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.textPrimary
        }
        OutlineButton {
            id: loadTiles
            objectName: "mapLoadTiles"
            x: prompt.wide ? prompt.width - prompt.closeRoom - width : 14 - flatInset
            y: prompt.wide ? (prompt.height - height) / 2 : promptText.y + promptText.height + 6
            width: Math.min(implicitWidth, prompt.width - 28)
            flat: !prompt.wide
            text: qsTr("Load map tiles")
            onClicked: prompt.load()
        }
        IconButton {
            id: closePrompt
            objectName: "mapTilesNotNow"
            icon: "close"
            accessibleName: qsTr("Not now")
            x: prompt.width - width
            y: prompt.wide ? (prompt.height - height) / 2 : 0
            onClicked: prompt.dismiss()
        }
    }

    // Nothing to show: say why, which reports show, and, while tiles are off,
    // how to load them.
    component MapEmptyState: UiPanel {
        id: empty

        property bool filtered: false
        property int hiddenCount: 0
        property string filterName: ""
        property bool showTilesPrompt: false
        property string tilesHost: ""
        signal showAll
        signal loadTiles

        height: emptyColumn.height + 2 * Theme.cardPadding

        Column {
            id: emptyColumn
            x: Theme.cardPadding
            y: Theme.cardPadding
            width: parent.width - 2 * Theme.cardPadding
            spacing: 8

            ReadableText {
                objectName: "mapEmptyTitle"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: empty.filtered ? qsTr("No positions in %1").arg(empty.filterName) : qsTr("No positions yet")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(16)
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }
            ReadableText {
                objectName: "mapEmptyDetail"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: empty.filtered ? (empty.hiddenCount === 1 ? qsTr("1 radio was heard earlier than that.") : qsTr("%1 radios were heard earlier than that.").arg(empty.hiddenCount))
                    : qsTr("A radio shows here when it sends its position over the air and the report passes its error check: LRRP, LIP, NMEA, D-STAR DPRS, M17 and the other GPS reports DSD-neo decodes.")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSecondary
            }
            OutlineButton {
                objectName: "mapShowAll"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: empty.filtered
                width: Math.min(parent.width, Math.max(implicitWidth, 160))
                text: qsTr("Show all")
                onClicked: empty.showAll()
            }
            Item {
                width: 1
                height: 4
                visible: empty.showTilesPrompt
            }
            ReadableText {
                objectName: "mapEmptyTilesHost"
                width: parent.width
                visible: empty.showTilesPrompt
                horizontalAlignment: Text.AlignHCenter
                text: empty.tilesHost.length > 0 ? qsTr("Map tiles are off. Loading them sends the area you view to %1.").arg(empty.tilesHost) : qsTr("Map tiles are off. Loading them sends the area you view to the tile server.")
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSubdued
            }
            OutlineButton {
                objectName: "mapEmptyLoadTiles"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: empty.showTilesPrompt
                width: Math.min(parent.width, Math.max(implicitWidth, 160))
                text: qsTr("Load map tiles")
                onClicked: empty.loadTiles()
            }
        }
    }
}
