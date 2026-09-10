// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import DsdNeo 1.0
import "Util.js" as Util

ModalSheet {
    id: sheet
    objectName: "siteChooserSheet"
    property string groupUid: ""
    property int sessionState: decoderHost.sessionState
    property var rows: []
    property int revision: 0
    property double locationId: 0
    SiteInteractionGuard {
        id: interactionGuard
        onInteraction: sheet.userAction()
    }
    property string notice: ""
    readonly property int groupRow: (revision, savedSystems.rowForUid(groupUid))
    readonly property int nearest: (revision, prefs.lastFixAt > 0 ? savedSystems.nearestRow(groupRow, prefs.lastLat, prefs.lastLon) : -1)
    signal startSite(int row)
    signal restartSite(string uid)
    signal editSite(int row)
    signal userAction
    function refresh() {
        revision++;
        rows = savedSystems.siblingRows(groupRow);
    }
    function openFor(row) {
        groupUid = savedSystems.get(row).uid || "";
        notice = "";
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
    function setAvoid(row, avoid) {
        userAction();
        savedSystems.setAvoidSite(row, avoid);
        refresh();
    }
    function useLocation() {
        userAction();
        if (locationId)
            decoderHost.cancelLocationRequest(locationId);
        locationId = radioReference.nextLocationRequestId();
        decoderHost.requestCurrentLocation(locationId);
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
            if (fixOk)
                prefs.setLocationFix(lat, lon, fixAtMs, accuracyM);
            sheet.notice = fixOk ? "" : qsTr("Location unavailable. Choose a site manually.");
        }
    }
    Text {
        text: qsTr("Choose a site")
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Use my location")
        visible: decoderHost.locationSupported
        enabled: !sheet.locationId
        onClicked: sheet.useLocation()
    }
    Text {
        width: parent.width
        text: sheet.notice
        visible: text.length > 0
        wrapMode: Text.Wrap
        color: Theme.textSecondary
    }
    GradientButton {
        objectName: "nearestSiteButton"
        width: parent.width
        text: qsTr("Nearest site")
        enabled: sheet.sessionState === 0 && sheet.nearest >= 0
        onClicked: sheet.choose(sheet.nearest)
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
            property var site: (sheet.revision, savedSystems.get(modelData))
            property real distance: (sheet.revision, prefs.lastFixAt > 0 ? savedSystems.distanceKm(modelData, prefs.lastLat, prefs.lastLon) : -1)
            readonly property string distanceText: Util.fmtDistanceKm(distance, prefs.metricUnits)
            OutlineButton {
                objectName: "siteChoice" + siteRow.modelData
                width: parent.width
                text: (siteRow.site.siteName || siteRow.site.name) + (siteRow.distanceText.length > 0 ? " · " + siteRow.distanceText : "")
                enabled: sheet.sessionState === 0 && !siteRow.site.avoidSite
                onClicked: sheet.choose(siteRow.modelData)
            }
            OutlineButton {
                width: parent.width
                text: qsTr("Edit site")
                enabled: sheet.sessionState === 0
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
