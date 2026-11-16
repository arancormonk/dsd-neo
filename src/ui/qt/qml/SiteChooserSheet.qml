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
    // The fix every distance here is measured from: GPS roaming's own while
    // it supplies one (#786, `live`), else the one last saved; null with
    // neither.
    readonly property var activeFix: {
        var roaming = siteRoaming.fix;
        if (roaming && roaming.lat !== undefined)
            return {lat: roaming.lat, lon: roaming.lon, accuracyM: roaming.accuracyM, live: true};
        if (prefs.lastFixAt > 0)
            return {lat: prefs.lastLat, lon: prefs.lastLon, accuracyM: prefs.lastAccuracyM, live: false};
        return null;
    }
    readonly property bool liveFix: activeFix !== null && activeFix.live
    // The group's sites in row order, each {row, uid, avoid, distance} with
    // its distance from the fix measured once here (-1 without a fix or a
    // position); every binding below reads it from here.
    readonly property var measured: (revision, measure())
    // The order the sites are shown in, by uid: nearest first while a fix
    // is there (open sites first, measured before the rest). While GPS
    // roaming supplies fixes (#786) it stays as it was when the sheet opened
    // or Use my location answered, and only each row's distance moves: the
    // rows are made once per uid, so a fix rebuilds none of them and the
    // keyboard and a screen reader stay on the control they are on. A site
    // added or gone takes the new order.
    property var order: []
    readonly property var liveOrder: ranked(measured).map(function (entry) { return entry.uid; })
    onLiveOrderChanged: {
        if (!liveFix || !sameSites(order, liveOrder))
            takeOrder();
    }
    // The sites in the order shown, each with its distance now.
    readonly property var rowModel: {
        var byUid = {};
        for (var entry of measured)
            byUid[entry.uid] = entry;
        return order.filter(function (uid) { return byUid[uid] !== undefined; }).map(function (uid) { return byUid[uid]; });
    }
    readonly property var rows: rowModel.map(function (entry) { return entry.row; })
    property int revision: 0
    property double locationId: 0
    property bool locationUpdated: false
    SiteInteractionGuard {
        id: interactionGuard
        onInteraction: sheet.userAction()
    }
    property string notice: ""
    // Issue #801 B7: the site whose avoid change could not be saved, and the
    // change asked for, so its row says so. Kept here, not on the row: a
    // change can rebuild the rows.
    property string avoidFailedUid: ""
    property bool avoidFailedWanted: false
    readonly property int groupRow: (revision, savedSystems.rowForUid(groupUid))
    // The nearest open, measured site, whatever the order shown.
    readonly property var nearestEntry: {
        var best = null;
        for (var entry of measured) {
            if (!entry.avoid && entry.distance >= 0 && (best === null || closer(entry, best) < 0))
                best = entry;
        }
        return best;
    }
    readonly property int nearest: nearestEntry ? nearestEntry.row : -1
    readonly property var nearestSite: (revision, savedSystems.get(nearest))
    readonly property real nearestDistance: nearestEntry ? nearestEntry.distance : -1
    readonly property bool listeningToNearest: sessionState === 2 && !!nearestSite.uid && nearestSite.uid === currentSiteUid
    readonly property string accuracyText: {
        var meters = activeFix ? activeFix.accuracyM : 0;
        if (!activeFix || !isFinite(meters) || meters <= 0)
            return "";
        if (prefs.metricUnits && meters < 1000)
            return qsTr("Location accuracy: about %1\u00a0m").arg(Math.ceil(meters));
        if (!prefs.metricUnits && meters < 1609.344)
            return qsTr("Location accuracy: about %1\u00a0ft").arg(Math.ceil(meters / 0.3048));
        return qsTr("Location accuracy: about %1").arg(Util.fmtDistanceKm(meters / 1000, prefs.metricUnits).replace(" ", "\u00a0"));
    }
    readonly property bool uncertainNearest: {
        if (nearest < 0 || !activeFix || !(activeFix.accuracyM > 0))
            return false;
        for (var entry of measured) {
            if (entry.row === nearest || entry.avoid)
                continue;
            if (entry.distance >= 0 && entry.distance - nearestDistance <= 2 * activeFix.accuracyM / 1000)
                return true;
        }
        return false;
    }
    // #786 GPS roaming, as this sheet shows it. Roaming follows one session
    // (siteRoaming says which); this card is that session's system when the
    // site it runs is one of these.
    readonly property int roamableCount: (revision, rows.filter(function (row) { return savedSystems.roamable(row); }).length)
    readonly property bool roamingHere: siteRoaming.currentUid.length > 0 && order.indexOf(siteRoaming.currentUid) >= 0
    readonly property int roamingState: siteRoaming.state
    // Roaming judges the session (neither Off nor Idle): it follows a site,
    // checks one, waits, switches, holds or is paused.
    readonly property bool roamingJudges: roamingState !== SiteRoaming.Off && roamingState !== SiteRoaming.Idle
    // A state change is read out once (the distances moving are not).
    readonly property string roamingStep: [roamingState, siteRoaming.reason, siteRoaming.candidateUid, siteRoaming.destinationUid].join("|")
    onRoamingStepChanged: Qt.callLater(announceRoaming)
    // Once the bindings have settled, and only while the line shows here.
    function announceRoaming() {
        if (visible && isTopModal && roamingStateLine.text.length > 0 && Navigation.presented(roamingStateLine))
            Navigation.announce(roamingSpeech, roamingStateLine, roamingStateLine.text);
    }
    QtObject {
        id: roamingSpeech
        objectName: "siteRoamingSpeech"
        property string announced: ""
        property int announcements: 0
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
    function measure() {
        var fix = activeFix;
        return savedSystems.siblingRows(groupRow).map(function (row) {
            var site = savedSystems.get(row);
            return {row: row, uid: site.uid || "", avoid: !!site.avoidSite, distance: fix ? savedSystems.distanceKm(row, fix.lat, fix.lon) : -1};
        });
    }
    // Open sites first, measured ones before the rest, then the nearer; ties
    // keep row order.
    function closer(a, b) {
        if (a.avoid !== b.avoid)
            return a.avoid ? 1 : -1;
        if ((a.distance < 0) !== (b.distance < 0))
            return a.distance < 0 ? 1 : -1;
        return a.distance - b.distance || a.row - b.row;
    }
    function ranked(entries) {
        return activeFix ? entries.slice().sort(closer) : entries;
    }
    function sameSites(a, b) {
        return a.length === b.length && a.every(function (uid) { return b.indexOf(uid) >= 0; });
    }
    // Shows the order as it is now; the rows are remade only when it moved.
    function takeOrder() {
        var next = liveOrder;
        if (next.join("\n") !== order.join("\n"))
            order = next;
    }
    function distanceOf(uid) {
        for (var entry of rowModel) {
            if (entry.uid === uid)
                return entry.distance;
        }
        return -1;
    }
    function setRoaming(on) {
        userAction();
        siteRoaming.setEnabledByUser(on);
    }
    function roamingAction() {
        userAction();
        siteRoaming.performAction();
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
        takeOrder();
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
            // Asked for: the rows take the order of now, also while GPS roaming holds it.
            sheet.takeOrder();
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
        text: sheet.locationId ? qsTr("Finding the nearest saved site in this system…") : sheet.notice || (sheet.liveFix ? qsTr("Distances follow your location while GPS roaming runs.") : prefs.lastFixAt > 0 ? (sheet.locationUpdated ? qsTr("Sites ordered by distance from your location.") : qsTr("Using your saved location. Refresh if you have moved.")) : qsTr("Use your location to find the nearest saved site in this system."))
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
        visible: sheet.activeFix !== null && !sheet.locationId
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
    // #786: GPS roaming, the same switch as Settings' (one for the app). Off
    // it says what it would do; on, what it is doing for the session it
    // follows. Hidden where the platform has no location feed.
    Column {
        id: roamingBlock
        objectName: "siteRoamingBlock"
        width: parent.width
        spacing: 6
        // Set apart from the site rows below, whose names are as bold.
        bottomPadding: 12
        visible: siteRoaming.available
        PlexToggle {
            id: roamingToggle
            objectName: "siteRoamingToggle"
            width: parent.width
            text: qsTr("GPS roaming")
            checked: siteRoaming.enabled
            // Always switchable off; on only where this system has two sites
            // roaming can use.
            enabled: siteRoaming.enabled || sheet.roamableCount >= 2
            onToggled: {
                sheet.setRoaming(checked);
                checked = Qt.binding(function () { return siteRoaming.enabled; });
            }
        }
        ReadableText {
            objectName: "siteRoamingDisclosure"
            width: parent.width
            text: qsTr("Switch to the nearest saved site of the system you are listening to as you move. Uses precise location while DSD-neo is on screen.")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            color: Theme.textSecondary
        }
        ReadableText {
            objectName: "siteRoamingUnavailable"
            width: parent.width
            visible: !roamingToggle.enabled
            text: qsTr("Needs two or more sites in this system that GPS roaming can use: trunking on, a receiver it can tune, and no extra options.")
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            color: Theme.textSecondary
        }
        // The session roaming follows, here: the site it runs and how far it
        // is (the distance moves with each fix, so a screen reader hears the
        // site alone). For another system, or nothing roaming can follow,
        // one line says so instead, and the state line below is left out.
        ReadableText {
            id: roamingContextLine
            objectName: "siteRoamingContext"
            readonly property string distanceText: Util.fmtDistanceKm(siteRoaming.currentDistanceKm, prefs.metricUnits)
            readonly property string spoken: {
                if (!siteRoaming.enabled)
                    return "";
                if (sheet.roamingJudges && sheet.roamingHere)
                    return siteRoaming.currentName.length > 0 ? qsTr("Listening to %1").arg(siteRoaming.currentName) : "";
                if (sheet.roamingJudges && siteRoaming.systemName.length > 0)
                    return qsTr("GPS roaming follows %1, the system you are listening to").arg(siteRoaming.systemName);
                return siteRoaming.state === SiteRoaming.Idle ? siteRoaming.statusText : "";
            }
            width: parent.width
            // Its own paragraph, under what the switch does.
            topPadding: 6
            visible: text.length > 0
            text: sheet.roamingJudges && sheet.roamingHere && spoken.length > 0 && distanceText.length > 0
                ? Util.fill(qsTr("%1 · %2"), [spoken, distanceText.replace(" ", "\u00a0")]) : spoken
            Accessible.name: navigationAllowed && visible ? spoken : ""
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            color: Theme.textSecondary
        }
        ReadableText {
            id: roamingStateLine
            objectName: "siteRoamingState"
            readonly property bool problem: siteRoaming.action === SiteRoaming.OpenAppSettings || siteRoaming.action === SiteRoaming.TurnOnLocation || siteRoaming.reason === SiteRoaming.SwitchFailed
            width: parent.width
            visible: text.length > 0
            text: siteRoaming.enabled && sheet.roamingJudges && sheet.roamingHere ? siteRoaming.statusText : ""
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            color: problem ? Theme.alert : Theme.textPrimary
        }
        OutlineButton {
            objectName: "siteRoamingAction"
            width: parent.width
            visible: roamingStateLine.visible && text.length > 0
            text: siteRoaming.action === SiteRoaming.ResumeRoaming ? qsTr("Resume roaming")
                : siteRoaming.action === SiteRoaming.OpenAppSettings ? qsTr("Open app settings")
                : siteRoaming.action === SiteRoaming.TurnOnLocation ? qsTr("Turn on location") : ""
            onClicked: sheet.roamingAction()
        }
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
        // One row per site uid, in the order shown: a fix that only moves
        // the distances leaves every row as it is (#786).
        model: sheet.order
        Column {
            id: siteRow
            required property var modelData
            readonly property bool isSiteRow: true
            readonly property string siteUid: modelData
            readonly property int row: (sheet.revision, savedSystems.rowForUid(modelData))
            width: parent.width
            spacing: 6
            property var site: (sheet.revision, savedSystems.getByUid(modelData))
            readonly property real distance: sheet.distanceOf(modelData)
            readonly property string distanceText: Util.fmtDistanceKm(distance, prefs.metricUnits)
            readonly property string name: site.siteName || site.name || ""
            readonly property string title: name + (distanceText.length > 0 ? " · " + distanceText : "")
            // While GPS roaming moves the distances, a screen reader hears the
            // site alone: a focused row is not read out again on every fix.
            readonly property string spokenTitle: sheet.liveFix ? name : title
            ReadableText {
                objectName: "siteTitle" + siteRow.row
                width: parent.width
                visible: sheet.sessionState !== 0
                text: siteRow.title
                Accessible.name: navigationAllowed && visible ? siteRow.spokenTitle : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(16)
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }
            OutlineButton {
                objectName: "siteChoice" + siteRow.row
                width: parent.width
                visible: sheet.sessionState === 0
                text: siteRow.title
                accessibleName: siteRow.spokenTitle
                enabled: sheet.sessionState === 0 && !siteRow.site.avoidSite
                onClicked: sheet.choose(siteRow.row)
            }
            ReadableText {
                objectName: "sitePositionNotice" + siteRow.row
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
            // #786 (§2.1): a site with options of its own is never a roaming
            // destination; while roaming is on, its row says so.
            ReadableText {
                objectName: "siteRoamingExtraArgs" + siteRow.row
                width: parent.width
                visible: siteRoaming.available && siteRoaming.enabled && String(siteRow.site.extraArgs || "").trim().length > 0
                text: qsTr("Not used by GPS roaming: this site has extra options")
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
                    sheet.editSite(siteRow.row);
                }
            }
            // Issue #801 D4: one site pinned to Listen on its own. It re-reads
            // the pins, so it flips in place; while Pinned is full it is
            // disabled and the line under it says why, here in the chooser.
            OutlineButton {
                id: sitePin
                objectName: "sitePin" + siteRow.row
                readonly property var action: (listenPins.revision, listenPins.action("site", siteRow.site.uid || ""))
                width: parent.width
                text: action.text
                accessibleName: action.accessibleName
                enabled: action.enabled
                onClicked: sheet.togglePin(siteRow.site.uid || "", action.pinned)
            }
            ReadableText {
                objectName: "sitePinReason" + siteRow.row
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
                objectName: "siteAvoid" + siteRow.row
                text: qsTr("Avoid site")
                checked: !!siteRow.site.avoidSite
                // Shows what is saved, also when the save failed.
                onToggled: {
                    sheet.setAvoid(siteRow.row, checked);
                    checked = Qt.binding(function () { return !!siteRow.site.avoidSite; });
                }
            }
            ReadableText {
                objectName: "siteAvoidError" + siteRow.row
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
                onClicked: sheet.stopAndSwitch(siteRow.row)
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
