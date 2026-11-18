// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #800 (spec 4.7): the map's own position. The dot is the saved
// one-shot fix (prefs, 24 h), which "Locate me" refreshes through the shared
// one-shot broker (locationRequests, requester "map"); "Follow my position"
// holds the live feed's "map" lease only while the map shows with follow on,
// and needs precise location. A live fix is never saved: it moves the dot and
// the camera, nothing else. A desktop has no location: `supported` is false
// and the screen hides the button.
//
// One button walks the three steps: Locate me, then (while the view is still
// on the dot it found) Follow my position, then Stop following.
Item {
    id: locator

    visible: false
    width: 0
    height: 0

    // Whether the map is on screen (the map, not the List): the live lease is
    // held only then.
    property bool shown: false
    // Whether the app is in the foreground: the lease is let go while it is
    // away (an Item stays visible then) and taken again on return while follow
    // is on. The feed itself only restarts leases still held, so the two never
    // both start the platform feed.
    property bool foreground: true
    property bool followMe: false
    // Set once a Locate me centred the view on the dot, until the view moves.
    property bool readyToFollow: false
    property double requestId: 0
    // A line for the screen's status column, and what its action opens
    // ("appSettings", "locationSettings" or "" for none).
    property string notice: ""
    property string noticeAction: ""

    // The broker's answer centred the view (the screen does it).
    signal located

    readonly property bool supported: typeof decoderHost !== "undefined" && !!decoderHost && decoderHost.locationSupported === true
    readonly property bool locating: requestId > 0
    readonly property bool busyElsewhere: locationRequests.busy && locationRequests.pendingRequester !== "map"
    readonly property var record: locationFeed.record || ({})
    // The feed's record is the device's live fix whoever's lease runs it; the
    // map reads it only while it holds its own (following on a shown map).
    readonly property bool live: leaseWanted && locationFeed.active && record.state === "ok" && record.hasFix === true
    readonly property bool saved: supported && prefs.lastFixAt > 0
    readonly property bool deviceValid: supported && (live || saved)
    readonly property double deviceLatitude: live ? record.lat : prefs.lastLat
    readonly property double deviceLongitude: live ? record.lon : prefs.lastLon
    readonly property double deviceAccuracyM: live ? (record.accuracyM || 0) : prefs.lastAccuracyM
    readonly property string actionName: followMe ? qsTr("Stop following my position") : readyToFollow ? qsTr("Follow my position") : qsTr("Locate me")

    function act() {
        if (followMe)
            stopFollow();
        else if (readyToFollow)
            startFollow();
        else
            locate();
    }
    function locate() {
        if (!supported || busyElsewhere)
            return;
        notice = qsTr("Finding your location…");
        noticeAction = "";
        if (requestId)
            locationRequests.cancel(requestId);
        requestId = locationRequests.requestSite("map");
    }
    function cancelLocate() {
        if (requestId)
            locationRequests.cancel(requestId);
        requestId = 0;
        if (notice === qsTr("Finding your location…"))
            notice = "";
    }
    function startFollow() {
        if (!supported)
            return;
        cancelLocate();
        readyToFollow = false;
        notice = "";
        noticeAction = "";
        followMe = true;
    }
    function stopFollow() {
        followMe = false;
        if (noticeAction.length === 0 && notice === qsTr("Finding your position…"))
            notice = "";
    }
    // The view moved by hand: the next press locates again.
    function cameraMoved() {
        readyToFollow = false;
    }
    function clearNotice() {
        notice = "";
        noticeAction = "";
    }
    function openNoticeAction() {
        if (noticeAction === "appSettings")
            decoderHost.openAppLocationSettings();
        else if (noticeAction === "locationSettings")
            decoderHost.openLocationSourceSettings();
    }

    // The live lease: held exactly while following on a shown map.
    readonly property bool leaseWanted: supported && followMe && shown && foreground
    onLeaseWantedChanged: syncLease()
    function syncLease() {
        if (leaseWanted) {
            locationFeed.acquire("map");
            if (!live)
                notice = qsTr("Finding your position…");
        } else {
            locationFeed.release("map");
        }
    }
    onShownChanged: {
        if (!shown)
            cancelLocate();
    }
    Component.onDestruction: {
        locationFeed.release("map");
        if (requestId)
            locationRequests.cancel(requestId);
    }

    // What the feed says while following.
    Connections {
        target: locationFeed
        function onRecordChanged() {
            locator.readFeed();
        }
        function onLeasesDropped() {
            locator.followMe = false;
        }
    }
    function readFeed() {
        if (!followMe || !locationFeed.holds("map"))
            return;
        var state = record.state;
        if (state === undefined)
            return;
        if (state === "ok") {
            if (record.hasFix === true && notice === qsTr("Finding your position…"))
                notice = "";
            return;
        }
        followMe = false;
        noticeAction = state === "noPermission" ? "appSettings" : state === "locationOff" ? "locationSettings" : "";
        notice = state === "approximateOnly" ? qsTr("Live position needs precise location.")
            : state === "noPermission" ? qsTr("Live position needs location permission.")
            : state === "locationOff" ? qsTr("Location is off on this device.")
            : qsTr("Live position is not available on this device.");
    }

    // Only the map's own request is ever answered here.
    Connections {
        target: locationRequests
        function onAnswered(requester, answeredId, result) {
            if (!locator.requestId || answeredId !== locator.requestId)
                return;
            locator.requestId = 0;
            if (result.fixOk) {
                prefs.setLocationFix(result.lat, result.lon, result.fixAtMs, result.accuracyM);
                locator.notice = "";
                locator.noticeAction = "";
                locator.readyToFollow = true;
                locator.located();
            } else {
                locator.notice = prefs.lastFixAt > 0 ? qsTr("Location unavailable. The dot shows your saved location.") : qsTr("Location unavailable. Check location access in your device settings.");
                locator.noticeAction = prefs.lastFixAt > 0 ? "" : "appSettings";
            }
        }
        function onEnded(requester, endedId) {
            if (locator.requestId && endedId === locator.requestId) {
                locator.requestId = 0;
                if (locator.notice === qsTr("Finding your location…"))
                    locator.notice = "";
            }
        }
    }
}
