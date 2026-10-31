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
        locationUpdated = false;
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
        savedSystems.setAvoidSite(row, avoid);
        refresh();
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
        text: sheet.nearest >= 0 ? qsTr("Nearest site: %1 · %2").arg(sheet.nearestSite.siteName || sheet.nearestSite.name).arg(Util.fmtDistanceKm(sheet.nearestDistance, prefs.metricUnits).replace(" ", "\u00a0")) : qsTr("No site can be ranked. Check the sites’ coordinates and Avoid settings below.")
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
    Repeater {
        model: sheet.rows
        Column {
            id: siteRow
            required property var modelData
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
                text: qsTr("No coordinates. Re-import this site from RadioReference to check for updated location data.")
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
            PlexToggle {
                objectName: "siteAvoid" + siteRow.modelData
                text: qsTr("Avoid site")
                checked: !!siteRow.site.avoidSite
                onToggled: sheet.setAvoid(siteRow.modelData, checked)
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
