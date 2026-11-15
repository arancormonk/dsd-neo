// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import DsdNeo 1.0
import "Util.js" as Util

ModalSheet {
    id: sheet
    objectName: "siteChooserSheet"
    accessibleName: qsTr("Choose a site")
    property string groupUid: ""
    property string currentSiteUid: ""
    property int sessionState: decoderHost.sessionState
    readonly property var rows: (revision, orderedRows())
    property int revision: 0
    property double locationId: 0
    property bool locationUpdated: false
    SiteInteractionGuard {
        id: interactionGuard
        onInteraction: sheet.userAction()
    }
    property string notice: ""
    // Issue #801 B7: the site whose avoid change could not be saved, and the
    // change asked for, so its row says so. Kept here, not on the row: the
    // rows are rebuilt on every change.
    property string avoidFailedUid: ""
    property bool avoidFailedWanted: false
    readonly property int groupRow: (revision, savedSystems.rowForUid(groupUid))
    readonly property int nearest: (revision, prefs.lastFixAt > 0 ? savedSystems.nearestRow(groupRow, prefs.lastLat, prefs.lastLon) : -1)
    readonly property var nearestSite: (revision, savedSystems.get(nearest))
    readonly property real nearestDistance: (revision, nearest >= 0 ? savedSystems.distanceKm(nearest, prefs.lastLat, prefs.lastLon) : -1)
    readonly property bool listeningToNearest: sessionState === 2 && !!nearestSite.uid && nearestSite.uid === currentSiteUid
    readonly property string accuracyText: {
        var meters = prefs.lastAccuracyM;
        if (prefs.lastFixAt <= 0 || !isFinite(meters) || meters <= 0)
            return "";
        if (prefs.metricUnits && meters < 1000)
            return qsTr("Location accuracy: about %1\u00a0m").arg(Math.ceil(meters));
        if (!prefs.metricUnits && meters < 1609.344)
            return qsTr("Location accuracy: about %1\u00a0ft").arg(Math.ceil(meters / 0.3048));
        return qsTr("Location accuracy: about %1").arg(Util.fmtDistanceKm(meters / 1000, prefs.metricUnits).replace(" ", "\u00a0"));
    }
    readonly property bool uncertainNearest: {
        if (nearest < 0 || prefs.lastAccuracyM <= 0)
            return false;
        for (var row of rows) {
            if (row === nearest || savedSystems.get(row).avoidSite)
                continue;
            var distance = savedSystems.distanceKm(row, prefs.lastLat, prefs.lastLon);
            if (distance >= 0 && distance - nearestDistance <= 2 * prefs.lastAccuracyM / 1000)
                return true;
        }
        return false;
    }
    signal startSite(int row)
    signal restartSite(string uid)
    signal editSite(int row)
    signal userAction
    // Update from RadioReference needs the account: signed out, the sheet
    // closes and Main opens it.
    signal openRadioReferenceAccount
    // A site with no coordinates: Update from RadioReference reads the sites
    // again (re-importing one used to add a second copy of it).
    readonly property bool missingPositions: (revision, rows.some(function (row) {
        return !savedSystems.get(row).hasSitePos;
    }))
    // An update this sheet started, whose summary it then shows.
    property bool rrUpdateStarted: false
    // Where the keyboard is in this sheet, kept as it moves: a site's row (by
    // uid, and which of its controls) or another control. The rows are
    // rebuilt whenever a saved site changes (an update started here or
    // anywhere else saves them), and Update disables while an update runs;
    // either leaves the keyboard on no control here, and it goes back: to
    // the same control of that site's row, else that row's first; with the
    // site gone, or the control no longer offered, the first site's, else
    // Update.
    property var keyboardPlace: null
    function orderedRows() {
        var siblings = savedSystems.siblingRows(groupRow);
        if (prefs.lastFixAt <= 0)
            return siblings;
        var lat = prefs.lastLat;
        var lon = prefs.lastLon;
        return siblings.map(function (row) {
            return {row: row, avoid: !!savedSystems.get(row).avoidSite, distance: savedSystems.distanceKm(row, lat, lon)};
        }).sort(function (a, b) {
            if (a.avoid !== b.avoid)
                return a.avoid ? 1 : -1;
            if ((a.distance < 0) !== (b.distance < 0))
                return a.distance < 0 ? 1 : -1;
            return a.distance - b.distance || a.row - b.row;
        }).map(function (entry) { return entry.row; });
    }
    function refresh() {
        revision++;
    }
    function openFor(row) {
        groupUid = savedSystems.get(row).uid || "";
        notice = "";
        avoidFailedUid = "";
        locationUpdated = false;
        rrUpdateStarted = false;
        keyboardPlace = null;
        refresh();
        visible = rows.length > 0;
    }
    function choose(row) {
        userAction();
        var site = savedSystems.get(row);
        if (sessionState !== 0 || !site.uid || site.avoidSite || rows.indexOf(row) < 0)
            return;
        visible = false;
        startSite(row);
    }
    function stopAndSwitch(row) {
        userAction();
        var site = savedSystems.get(row);
        if (sessionState !== 2 || !site.uid || site.avoidSite || rows.indexOf(row) < 0)
            return;
        visible = false;
        restartSite(site.uid);
    }
    function chooseNearest() {
        if (locationId || nearest < 0 || listeningToNearest)
            return;
        if (sessionState === 2)
            stopAndSwitch(nearest);
        else
            choose(nearest);
    }
    function setAvoid(row, avoid) {
        userAction();
        var saved = savedSystems.setAvoidSite(row, avoid);
        avoidFailedUid = saved ? "" : savedSystems.get(row).uid || "";
        avoidFailedWanted = avoid;
        // Rebuilt either way, so the toggle shows what is saved.
        refresh();
    }
    // Issue #801: does what the button read (@a offeredPinned: Unpin). pin()'s
    // refusal needs no line of its own: the sheet is modal, so nothing fills
    // the row behind it, and a full row's Pin is disabled with the reason.
    function togglePin(uid, offeredPinned) {
        userAction();
        if (offeredPinned)
            listenPins.unpin("site", uid);
        else
            listenPins.pin("site", uid);
    }
    function updateFromRadioReference() {
        userAction();
        // One update at a time: Update is disabled while one runs.
        if (rrPlaces.updating)
            return;
        var uids = rows.map(function (row) {
            return savedSystems.get(row).uid;
        });
        if (!rrPlaces.updateFromRadioReference(uids)) {
            visible = false;
            openRadioReferenceAccount();
            return;
        }
        rrUpdateStarted = true;
    }
    function usable(item) {
        return !!item && item !== sheet && Navigation.contains(sheet, item) && Navigation.presented(item)
            && item.enabled && item.activeFocusOnTab;
    }
    function siteRowOf(item) {
        for (var at = item; at && at !== sheet; at = at.parent) {
            if (at.parent && at.parent.isSiteRow === true)
                return {row: at.parent, control: at};
        }
        return null;
    }
    // The keyboard's place here, or null when no control here has it.
    function keyboardMark() {
        var window = sheet.Window.window;
        var focused = window ? window.activeFocusItem : null;
        if (!focused || focused === sheet || !Navigation.contains(sheet, focused))
            return null;
        var inRow = siteRowOf(focused);
        if (!inRow)
            return {item: focused};
        for (var i = 0; i < inRow.row.children.length; ++i) {
            if (inRow.row.children[i] === inRow.control)
                return {uid: inRow.row.siteUid, control: i};
        }
        return {uid: inRow.row.siteUid, control: -1};
    }
    function keyboardMoved() {
        if (!visible || !isTopModal)
            return;
        var mark = keyboardMark();
        if (mark)
            keyboardPlace = mark;
        else if (keyboardPlace)
            Qt.callLater(restoreKeyboard);
    }
    function firstUsable(row) {
        for (var i = 0; row && i < row.children.length; ++i) {
            if (usable(row.children[i]))
                return row.children[i];
        }
        return null;
    }
    function siteRowFor(uid) {
        for (var i = 0; i < siteRows.count; ++i) {
            var row = siteRows.itemAt(i);
            if (row && row.siteUid === uid)
                return row;
        }
        return null;
    }
    function keyboardTarget(place) {
        var row = place.uid !== undefined ? siteRowFor(place.uid) : null;
        if (row) {
            var same = place.control >= 0 && place.control < row.children.length ? row.children[place.control] : null;
            return usable(same) ? same : firstUsable(row);
        }
        if (place.uid === undefined && usable(place.item))
            return place.item;
        return firstUsable(siteRows.count > 0 ? siteRows.itemAt(0) : null) || (usable(rrUpdate) ? rrUpdate : null);
    }
    // Run after the change that moved the keyboard has settled (a rebuild
    // makes the new rows first): only while it is still on no control here.
    function restoreKeyboard() {
        var place = keyboardPlace;
        if (!place || !visible || !isTopModal || keyboardMark())
            return;
        var target = keyboardTarget(place);
        if (target)
            target.forceActiveFocus();
    }
    Connections {
        target: sheet.Window.window
        function onActiveFocusItemChanged() {
            sheet.keyboardMoved();
        }
    }
    function useLocation() {
        userAction();
        notice = "";
        locationUpdated = false;
        if (locationId)
            decoderHost.cancelLocationRequest(locationId);
        locationId = radioReference.nextLocationRequestId();
        decoderHost.requestSiteLocation(locationId);
    }
    onVisibleChanged: {
        if (!visible)
            keyboardPlace = null;
        if (!visible && locationId) {
            decoderHost.cancelLocationRequest(locationId);
            locationId = 0;
        }
    }
    Component.onDestruction: {
        if (locationId)
            decoderHost.cancelLocationRequest(locationId);
    }
    Connections {
        target: radioReference
        function onLocationRequestAllocated(requestId) {
            if (sheet.locationId && sheet.locationId !== requestId) {
                decoderHost.cancelLocationRequest(sheet.locationId);
                sheet.locationId = 0;
            }
        }
    }
    onDismissed: userAction()
    Connections {
        target: savedSystems
        function onSitesChanged() {
            sheet.refresh();
        }
    }
    // What an update changed (names, positions) shows at once.
    Connections {
        target: rrPlaces
        ignoreUnknownSignals: true
        function onUpdateFinished() {
            sheet.refresh();
        }
    }
    Connections {
        target: decoderHost
        ignoreUnknownSignals: true
        function onLocationResult(requestId, fixOk, lat, lon, accuracyM, fixAtMs, geocodeOk, postal, country, error) {
            if (!sheet.visible || requestId !== sheet.locationId || !sheet.locationId)
                return;
            sheet.locationId = 0;
            if (fixOk) {
                prefs.setLocationFix(lat, lon, fixAtMs, accuracyM);
                sheet.locationUpdated = prefs.lastFixAt > 0;
            }
            sheet.notice = sheet.locationUpdated ? "" : (prefs.lastFixAt > 0 ? qsTr("Location unavailable. Distances still use your saved location. Try again or choose a site manually.") : qsTr("Location unavailable. Check location access in your device settings, then try again or choose a site manually."));
        }
        // A permission prompt Android closed unanswered (#782): nothing failed, so the sheet just returns to idle.
        function onLocationCancelled(requestId) {
            if (requestId === sheet.locationId && sheet.locationId)
                sheet.locationId = 0;
        }
    }
    Text {
        text: qsTr("Choose a site")
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    OutlineButton {
        objectName: "siteLocationButton"
        width: parent.width
        text: sheet.locationId ? qsTr("Finding your location…") : qsTr("Use my location")
        visible: decoderHost.locationSupported
        enabled: !sheet.locationId
        onClicked: sheet.useLocation()
    }
    ReadableText {
        objectName: "siteLocationNotice"
        width: parent.width
        text: sheet.locationId ? qsTr("Finding the nearest saved site in this system…") : sheet.notice || (prefs.lastFixAt > 0 ? (sheet.locationUpdated ? qsTr("Sites ordered by distance from your location.") : qsTr("Using your saved location. Refresh if you have moved.")) : qsTr("Use your location to find the nearest saved site in this system."))
        visible: text.length > 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(14)
        color: Theme.textSecondary
    }
    ReadableText {
        objectName: "nearestSiteSummary"
        width: parent.width
        visible: prefs.lastFixAt > 0 && !sheet.locationId
        text: sheet.nearest >= 0 ? Util.fill(qsTr("Nearest site: %1 · %2"), [sheet.nearestSite.siteName || sheet.nearestSite.name, Util.fmtDistanceKm(sheet.nearestDistance, prefs.metricUnits).replace(" ", "\u00a0")]) : qsTr("No site can be ranked. Check the sites’ coordinates and Avoid settings below.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(16)
        font.weight: Font.DemiBold
        color: Theme.textPrimary
    }
    ReadableText {
        objectName: "siteLocationAccuracy"
        width: parent.width
        visible: text.length > 0 && !sheet.locationId
        text: sheet.accuracyText + (sheet.uncertainNearest ? qsTr(". Nearby sites may change order within this accuracy. For a better fix, enable precise location in your device settings.") : "")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(14)
        color: Theme.textSecondary
    }
    GradientButton {
        objectName: "nearestSiteButton"
        width: parent.width
        text: sheet.listeningToNearest ? qsTr("Listening to nearest site") : sheet.sessionState === 2 ? qsTr("Stop and switch to nearest") : qsTr("Listen to nearest site")
        enabled: (sheet.sessionState === 0 || sheet.sessionState === 2) && sheet.nearest >= 0 && !sheet.locationId && !sheet.listeningToNearest
        onClicked: sheet.chooseNearest()
    }
    OutlineButton {
        objectName: "siteRecoverButton"
        width: parent.width
        text: qsTr("Dismiss failure and choose a site")
        visible: sheet.sessionState === 4
        onClicked: {
            sheet.userAction();
            decoderHost.stop();
        }
    }
    OutlineButton {
        id: rrUpdate
        objectName: "siteUpdateFromRr"
        width: parent.width
        visible: sheet.missingPositions && radioReference.available
        enabled: !rrPlaces.updating
        text: qsTr("Update from RadioReference")
        onClicked: sheet.updateFromRadioReference()
    }
    // Moves with every system done, so it is paced for a screen reader.
    LiveText {
        objectName: "siteUpdateProgress"
        width: parent.width
        visible: sheet.missingPositions && rrPlaces.updating
        text: rrPlaces.progressText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(14)
        color: Theme.textSecondary
    }
    ReadableText {
        objectName: "siteUpdateSummary"
        width: parent.width
        visible: sheet.rrUpdateStarted && !rrPlaces.updating && text.length > 0
        text: rrPlaces.summaryText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(14)
        color: Theme.textSecondary
    }
    Repeater {
        id: siteRows
        model: sheet.rows
        Column {
            id: siteRow
            required property var modelData
            readonly property bool isSiteRow: true
            readonly property string siteUid: site.uid || ""
            width: parent.width
            spacing: 6
            property var site: (sheet.revision, savedSystems.get(modelData))
            property real distance: (sheet.revision, prefs.lastFixAt > 0 ? savedSystems.distanceKm(modelData, prefs.lastLat, prefs.lastLon) : -1)
            readonly property string distanceText: Util.fmtDistanceKm(distance, prefs.metricUnits)
            readonly property string title: (site.siteName || site.name) + (distanceText.length > 0 ? " · " + distanceText : "")
            ReadableText {
                objectName: "siteTitle" + siteRow.modelData
                width: parent.width
                visible: sheet.sessionState !== 0
                text: siteRow.title
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(16)
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }
            OutlineButton {
                objectName: "siteChoice" + siteRow.modelData
                width: parent.width
                visible: sheet.sessionState === 0
                text: siteRow.title
                enabled: sheet.sessionState === 0 && !siteRow.site.avoidSite
                onClicked: sheet.choose(siteRow.modelData)
            }
            ReadableText {
                objectName: "sitePositionNotice" + siteRow.modelData
                width: parent.width
                visible: !siteRow.site.hasSitePos
                text: radioReference.available ? qsTr("No coordinates. Use Update from RadioReference to check for new location data.")
                    : qsTr("No coordinates.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
                color: Theme.textSecondary
            }
            OutlineButton {
                width: parent.width
                text: qsTr("Edit site")
                visible: sheet.sessionState === 0
                onClicked: {
                    sheet.userAction();
                    sheet.visible = false;
                    sheet.editSite(siteRow.modelData);
                }
            }
            // Issue #801 D4: one site pinned to Listen on its own. It re-reads
            // the pins, so it flips in place; while Pinned is full it is
            // disabled and the line under it says why, here in the chooser.
            OutlineButton {
                id: sitePin
                objectName: "sitePin" + siteRow.modelData
                readonly property var action: (listenPins.revision, listenPins.action("site", siteRow.site.uid || ""))
                width: parent.width
                text: action.text
                accessibleName: action.accessibleName
                enabled: action.enabled
                onClicked: sheet.togglePin(siteRow.site.uid || "", action.pinned)
            }
            ReadableText {
                objectName: "sitePinReason" + siteRow.modelData
                width: parent.width
                visible: text.length > 0
                text: sitePin.action.description
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
                color: Theme.textSecondary
            }
            PlexToggle {
                objectName: "siteAvoid" + siteRow.modelData
                text: qsTr("Avoid site")
                checked: !!siteRow.site.avoidSite
                onToggled: sheet.setAvoid(siteRow.modelData, checked)
            }
            ReadableText {
                objectName: "siteAvoidError" + siteRow.modelData
                width: parent.width
                visible: sheet.avoidFailedUid.length > 0 && sheet.avoidFailedUid === siteRow.site.uid
                text: sheet.avoidFailedWanted ? qsTr("Could not avoid this site. Try again.") : qsTr("Could not stop avoiding this site. Try again.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
                color: Theme.alert
            }
            OutlineButton {
                width: parent.width
                text: qsTr("Stop and switch")
                visible: sheet.sessionState === 2
                enabled: !siteRow.site.avoidSite
                onClicked: sheet.stopAndSwitch(siteRow.modelData)
            }
        }
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Close")
        onClicked: {
            sheet.userAction();
            sheet.visible = false;
        }
    }
}
