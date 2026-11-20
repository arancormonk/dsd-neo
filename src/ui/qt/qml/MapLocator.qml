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
// on the dot it found, and with precise location) Follow my position, then
// Stop following. Locate me offers precise location when only approximate is
// allowed; the map's opening fix never prompts (locateQuietly()), says
// nothing, and leaves the button on Locate me.
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
    // Whether the answer to the Locate me in flight may still move the view:
    // an explicit camera choice made meanwhile (a follow, Show on map, a fit,
    // a drag) keeps it (review r2). The answer still saves the dot.
    property bool moveOnAnswer: false
    // The request in flight is the opening one: it shows no notice.
    property bool quiet: false
    property double requestId: 0
    // A line for the screen's status column, and what its action opens
    // ("appSettings", "locationSettings" or "" for none).
    property string notice: ""
    property string noticeAction: ""

    // The broker's answer centres the view (the screen does it). canFollow:
    // an explicit Locate me with precise location allowed, so Follow is the
    // next step; opening: the map's opening fix, placed at the fix's own zoom.
    signal located(bool canFollow, bool opening)

    readonly property bool supported: typeof decoderHost !== "undefined" && !!decoderHost && decoderHost.locationSupported === true
    // A Locate me in flight (the opening fix does not hold the button).
    readonly property bool locating: requestId > 0 && !quiet
    readonly property bool busyElsewhere: locationRequests.busy && locationRequests.pendingRequester !== "map"
    readonly property var record: locationFeed.record || ({})
    // The feed's record is the device's live fix whoever's lease runs it; the
    // map reads it only while it holds its own (following on a shown map).
    readonly property bool streamFix: leaseWanted && locationFeed.active && record.state === "ok" && record.hasFix === true
    // A fix stops being live once it is older than roaming's limit (60 s):
    // the platform keeps handing back its last measurement, however old, and
    // sends nothing new while it has nothing new (review r1). Aged on the
    // clock fixAtMs is on (locationFeed.nowMs()), read once a second while
    // the stream runs and at every record.
    property int maxFixAgeMs: 60000
    property double clockMs: 0
    readonly property double fixAgeMs: streamFix && record.fixAtMs !== undefined ? clockMs - record.fixAtMs : -1
    readonly property bool live: streamFix && fixAgeMs >= 0 && fixAgeMs <= maxFixAgeMs
    // The stream's last fix, gone stale: the dot stays on it (it is the
    // newest position known), but it is no longer called "you".
    readonly property bool lastKnown: streamFix && !live
    readonly property bool saved: supported && prefs.lastFixAt > 0
    readonly property bool deviceValid: supported && (streamFix || saved)
    readonly property double deviceLatitude: streamFix ? record.lat : prefs.lastLat
    readonly property double deviceLongitude: streamFix ? record.lon : prefs.lastLon
    readonly property double deviceAccuracyM: streamFix ? (record.accuracyM || 0) : prefs.lastAccuracyM
    function tick() {
        clockMs = locationFeed.nowMs();
    }
    Timer {
        interval: 1000
        repeat: true
        running: locator.streamFix
        onTriggered: locator.tick()
    }
    // A live fix going stale says so while following; a fresh one clears it.
    onLiveChanged: {
        if (!followMe)
            return;
        if (streamFix && !live && notice.length === 0)
            notice = qsTr("Finding your position…");
        else if (live && notice === qsTr("Finding your position…"))
            notice = "";
    }
    readonly property string actionName: followMe ? qsTr("Stop following my position") : readyToFollow ? qsTr("Follow my position") : qsTr("Locate me")

    function act() {
        if (followMe)
            stopFollow();
        else if (readyToFollow)
            startFollow();
        else
            locate();
    }
    // What the platform allows now: "none", "approximate" or "precise".
    function access() {
        return supported && typeof decoderHost.locationAccess === "function" ? decoderHost.locationAccess() : "none";
    }
    function allowed() {
        return access() !== "none";
    }
    function locate() {
        if (!supported || busyElsewhere)
            return;
        notice = qsTr("Finding your location…");
        noticeAction = "";
        if (requestId)
            locationRequests.cancel(requestId);
        moveOnAnswer = true;
        quiet = false;
        requestId = locationRequests.requestPrecise("map");
    }
    // The map's opening fix: only with location already allowed (no prompt),
    // never over another screen's request, and silent whatever it answers.
    function locateQuietly() {
        if (!supported || !shown || !foreground || busyElsewhere || locating || !allowed())
            return;
        moveOnAnswer = true;
        quiet = true;
        requestId = locationRequests.requestQuiet("map");
    }
    function cancelLocate() {
        if (requestId)
            locationRequests.cancel(requestId);
        requestId = 0;
        quiet = false;
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
    // The view was placed on purpose while a Locate me runs: its answer
    // saves the dot and leaves the view (and the Follow step) alone.
    function keepView() {
        moveOnAnswer = false;
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
            tick();
            if (!live)
                notice = qsTr("Finding your position…");
            // Joining a feed another lease (roaming) already runs: its record
            // stands, and nothing new comes while it stays the same (the host
            // sends only changes), so it is read now, a coarse-only answer
            // included (review r1). Just after this change, not inside it: an
            // answer that ends follow changes leaseWanted again.
            Qt.callLater(locator.readFeed);
        } else {
            locationFeed.release("map");
        }
    }
    onShownChanged: {
        if (!shown)
            cancelLocate();
    }
    // The opening fix is for a map on screen: the app going away drops it.
    onForegroundChanged: {
        if (!foreground && quiet)
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
            locator.tick();
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
            if (live && notice === qsTr("Finding your position…"))
                notice = "";
            return;
        }
        followMe = false;
        noticeAction = state === "noPermission" || state === "approximateOnly" ? "appSettings" : state === "locationOff" ? "locationSettings" : "";
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
            var quiet = locator.quiet;
            locator.requestId = 0;
            locator.quiet = false;
            if (result.fixOk) {
                // A cached answer can be older than a fix another screen saved
                // since: the newer one stays, and the dot shows it.
                var saved = !(prefs.lastFixAt > result.fixAtMs);
                if (saved)
                    prefs.setLocationFix(result.lat, result.lon, result.fixAtMs, result.accuracyM);
                var precise = locator.access() === "precise";
                if (!quiet) {
                    // An approximate fix after Locate me offered precise:
                    // say how to get a closer one.
                    locator.notice = precise ? "" : qsTr("Your location is approximate. Allow precise location for a closer fix.");
                    locator.noticeAction = precise ? "" : "appSettings";
                }
                // A newer fix kept over this answer places the camera only with
                // precise access: under approximate access it may be a precise
                // fix from before access was lowered.
                if (locator.moveOnAnswer && (saved || precise))
                    locator.located(precise && !quiet, quiet);
                locator.moveOnAnswer = false;
            } else if (!quiet) {
                // Where to fix it: no access, or approximate access with no
                // provider it may use, is the app's settings; location
                // switched off is the device's. (The two errors are
                // LocationSupport.kt's own fixed texts.)
                var error = String(result.error || "");
                var off = error.indexOf("Location is off") === 0;
                var noProvider = error.indexOf("Location provider unavailable") === 0;
                var access = locator.access();
                locator.noticeAction = access === "none" || (noProvider && access === "approximate") ? "appSettings" : off ? "locationSettings" : "";
                locator.notice = off && access !== "none" ? qsTr("Location is off on this device.")
                    : noProvider && access === "approximate" ? qsTr("Location unavailable with approximate location. Allow precise location to use GPS.")
                    : prefs.lastFixAt > 0 ? qsTr("Location unavailable. The dot shows your saved location.")
                    : qsTr("Location unavailable. Check location access in your device settings.");
            }
        }
        function onEnded(requester, endedId) {
            if (locator.requestId && endedId === locator.requestId) {
                locator.requestId = 0;
                locator.quiet = false;
                if (locator.notice === qsTr("Finding your location…"))
                    locator.notice = "";
            }
        }
    }
}
