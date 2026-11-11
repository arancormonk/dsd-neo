// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Listen's saved systems, above their cards: Search and Sort, then the lines
// a search or an order puts there (how many a search shows, Nearest's
// location, the systems an update could place, an update's progress and
// summary) and the card a search with no match shows. Shown whenever a
// system is saved. Every height is keyed on its own condition, never on
// `visible`, which also goes false while Main hides Home (issue #646 B1).
// HomeScreen owns the cards; this owns what the user asks of them.
Column {
    id: controls

    objectName: "savedSystemsControls"

    // The app's AppPrefs (the stored order, the location fix).
    property var prefsStore: null
    // Home shows Systems: what lands is read out, and a location request may run.
    property bool current: false
    // Re-derives the fix's age once a minute.
    property int heardTick: 0
    // The order choices' sheet, which Home hosts over the screen.
    property var sortSheet: null

    // Update from RadioReference needs the account: signed out, Main opens it.
    signal openRadioReferenceAccount
    // A change the user asked for moved the list: the search row comes into view.
    signal revealRequested
    // The last system went: the controls go, and @a hadFocus says whether the keyboard was on them.
    signal emptied(bool hadFocus)

    readonly property bool shown: savedSystemCards.totalCount >= 1
    // Where a keyboard left without a card lands, and the row that comes into view.
    readonly property Item searchRow: searchBar
    readonly property Item searchInput: systemsSearch.input

    visible: shown
    height: shown ? implicitHeight : 0
    spacing: Theme.gap

    // A line (and an optional second one) with its action at the end, a
    // text-style button; the action drops under the lines, aligned to their
    // start, only when both do not fit. The notice is about as tall as the
    // words it draws: the action's 48 dp target reaches past it by at most
    // half the column's gap, so two neighbours' targets never meet. Keyed on
    // `shown` and its own words, never on `visible`.
    component SystemsNotice: Item {
        id: notice

        property bool shown: true
        property alias text: noticeText.text
        property alias textObjectName: noticeText.objectName
        property alias detail: noticeDetail.text
        property alias actionText: noticeAction.text
        property alias actionObjectName: noticeAction.objectName
        property alias actionEnabled: noticeAction.enabled
        readonly property alias actionItem: noticeAction
        // @a hadFocus: the action had the keyboard when pressed.
        signal action(bool hadFocus)
        readonly property real inset: noticeAction.flatInset
        readonly property bool stacked: Math.max(noticeText.implicitWidth, notice.detail.length > 0 ? noticeDetail.implicitWidth : 0)
            + 8 + noticeAction.implicitWidth - inset > width
        // The action's drawn label, and how far its target reaches past it either way.
        readonly property real labelHeight: noticeAction.labelHeight
        readonly property real reach: (noticeAction.height - labelHeight) / 2
        visible: shown
        width: parent ? parent.width : 0
        height: !shown ? 0 : stacked ? noticeLines.height + 4 - reach + noticeAction.height - Theme.gap / 2
            : Math.max(noticeLines.height, noticeAction.height - Theme.gap)

        Column {
            id: noticeLines
            width: notice.stacked ? notice.width : notice.width - noticeAction.width - 8 + notice.inset
            y: notice.stacked ? 0 : (notice.height - height) / 2
            spacing: 2
            ReadableText {
                id: noticeText
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
            }
            ReadableText {
                id: noticeDetail
                width: parent.width
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
            }
        }
        OutlineButton {
            id: noticeAction
            flat: true
            x: notice.stacked ? -notice.inset : notice.width - width + notice.inset
            y: notice.stacked ? noticeLines.height + 4 - notice.reach : (notice.height - height) / 2
            width: Math.min(implicitWidth, notice.width + 2 * notice.inset)
            onClicked: notice.action(activeFocus)
        }
    }

    // The order shown: the stored one, except Nearest where this device takes
    // no location fix, which shows as Name (the cards fall back the same way)
    // and leaves the stored choice alone.
    readonly property bool locationSupported: !!decoderHost && decoderHost.locationSupported === true
    readonly property string shownSort: savedSystemCards.sortId === "nearest" && !locationSupported ? "name" : savedSystemCards.sortId
    function sortLabel(id) {
        return id === "name" ? qsTr("Name") : id === "place" ? qsTr("Place") : id === "nearest" ? qsTr("Nearest")
            : id === "recent" ? qsTr("Last listened") : qsTr("Order added");
    }
    function openSortChoices() {
        if (!sortSheet)
            return;
        var ids = ["name", "place"];
        if (locationSupported)
            ids.push("nearest");
        ids.push("recent", "added");
        sortSheet.open(qsTr("Sort systems"), ids.map(sortLabel), function (index) {
            controls.chooseSort(ids[index]);
        }, ids.indexOf(shownSort));
    }
    // The sheet hands the focus back to the pill; the list moves under it.
    function chooseSort(id) {
        if (prefsStore && prefsStore.systemsSort !== undefined && prefsStore.systemsSort !== id)
            prefsStore.systemsSort = id;
        revealRequested();
    }

    // Search: the field's text reaches the cards 250 ms after the last key
    // (as History's), and each change that lands is read out once, with
    // how many it shows, while Home shows Systems with nothing over it.
    // Leaving Systems lands what was typed without reading it (leave()).
    property bool announceOwed: false
    property string announcedQuery: ""
    // Navigation.announce()'s record of what was read out.
    property string announced: ""
    property int announcements: 0
    function commitSearch(announce) {
        searchDebounce.stop();
        if (savedSystemCards.query === systemsSearch.text)
            return;
        savedSystemCards.query = systemsSearch.text;
        announceOwed = announce;
        // After the cards' own rebuild, which the query change has queued.
        if (announce)
            Qt.callLater(controls.announceCount);
        revealRequested();
    }
    function flushSearch() {
        if (searchDebounce.running)
            commitSearch(false);
    }
    // From the field's ×, or Clear search; @a hadFocus: the control had the
    // keyboard, which then goes to the field (the control goes away).
    function clearSearch(hadFocus) {
        systemsSearch.text = "";
        commitSearch(true);
        if (hadFocus)
            systemsSearch.input.forceActiveFocus();
    }
    readonly property string countText: savedSystemCards.totalCount === 1 ? qsTr("%1 of 1 system").arg(savedSystemCards.shownCount)
        : qsTr("%1 of %2 systems").arg(savedSystemCards.shownCount).arg(savedSystemCards.totalCount)
    readonly property string noMatchText: qsTr("No saved systems match “%1”").arg(savedSystemCards.query)
    function announceCount() {
        if (!announceOwed)
            return;
        announceOwed = false;
        if (!current || !shown || !Navigation.allows(controls))
            return;
        var query = savedSystemCards.query;
        var text = query.length > 0 && savedSystemCards.shownCount === 0 ? noMatchText : countText;
        if (query === announcedQuery && text === announced)
            return;
        if (Navigation.announce(controls, null, text))
            announcedQuery = query;
    }
    Timer {
        id: searchDebounce
        interval: 250
        onTriggered: controls.commitSearch(true)
    }
    // With the last system gone the search is dropped.
    Connections {
        target: savedSystemCards
        function onTotalCountChanged() {
            if (savedSystemCards.totalCount > 0)
                return;
            var hadFocus = Navigation.contains(controls, controls.Window.window ? controls.Window.window.activeFocusItem : null);
            searchDebounce.stop();
            systemsSearch.text = "";
            savedSystemCards.query = "";
            controls.emptied(hadFocus);
        }
    }
    // Leaving Systems (another tab, or Home hidden): what was typed lands
    // unread, a location request is given up and an update's summary goes.
    function leave() {
        flushSearch();
        cancelNearestLocation();
        locationNotice = "";
        rrSummaryShown = false;
    }

    // Nearest: the location fix is asked for only from its own button, under
    // the site chooser's contract: one request id (radioReference's), a newer
    // request anywhere supersedes it, leaving Systems cancels it. The cards
    // are told the next location change is the user's own, and told again
    // when the request ends without one.
    property double locationId: 0
    property string locationNotice: ""
    readonly property bool hasFix: !!prefsStore && prefsStore.lastFixAt > 0
    function requestNearestLocation() {
        locationNotice = "";
        var previous = locationId;
        locationId = 0;
        if (previous)
            decoderHost.cancelLocationRequest(previous);
        locationId = radioReference.nextLocationRequestId();
        savedSystemCards.markLocationRequested();
        decoderHost.requestSiteLocation(locationId);
    }
    function cancelNearestLocation() {
        if (!locationId)
            return;
        decoderHost.cancelLocationRequest(locationId);
        locationId = 0;
        savedSystemCards.clearLocationRequest();
    }
    Component.onDestruction: cancelNearestLocation()
    Connections {
        target: radioReference
        ignoreUnknownSignals: true
        function onLocationRequestAllocated(requestId) {
            if (controls.locationId && controls.locationId !== requestId) {
                decoderHost.cancelLocationRequest(controls.locationId);
                controls.locationId = 0;
                savedSystemCards.clearLocationRequest();
            }
        }
    }
    Connections {
        target: decoderHost
        ignoreUnknownSignals: true
        function onLocationResult(requestId, fixOk, lat, lon, accuracyM, fixAtMs, geocodeOk, postal, country, error) {
            if (!controls.locationId || requestId !== controls.locationId)
                return;
            controls.locationId = 0;
            var store = controls.prefsStore;
            if (fixOk && store && typeof store.setLocationFix === "function")
                store.setLocationFix(lat, lon, fixAtMs, accuracyM);
            // Spent by the change the fix made; a fix that changed nothing, or none, leaves no mark behind.
            savedSystemCards.clearLocationRequest();
            var placed = fixOk && controls.hasFix;
            controls.locationNotice = placed ? "" : controls.hasFix ? qsTr("Location unavailable. Distances still use your saved location. Try again.") : qsTr("Location unavailable. Check location access in your device settings, then try again.");
            if (placed)
                controls.revealRequested();
        }
        // A permission prompt Android closed unanswered: nothing failed.
        function onLocationCancelled(requestId) {
            if (controls.locationId && requestId === controls.locationId) {
                controls.locationId = 0;
                savedSystemCards.clearLocationRequest();
            }
        }
    }
    // How old the fix is, as the location line says it.
    function fixAgeText(fixAtMs, nowMs) {
        var minutes = Math.max(0, Math.floor((nowMs - fixAtMs) / 60000));
        if (minutes < 1)
            return qsTr("a moment ago");
        if (minutes < 60)
            return qsTr("%1 min ago").arg(minutes);
        return qsTr("%1 h ago").arg(Math.floor(minutes / 60));
    }
    readonly property string locationText: locationId ? qsTr("Finding your location…") : locationNotice.length > 0 ? locationNotice
        : hasFix ? qsTr("Using your location from %1").arg((heardTick, fixAgeText(prefsStore.lastFixAt, savedSystems.realtimeNowMs())))
        : qsTr("Nearest needs your location")

    // Update from RadioReference (a card's menu and the place notice): signed
    // out, the account screen opens instead. Its progress and, once done, its
    // summary show here until Systems is left. The keyboard follows the
    // control that replaces the one pressed: Update → Cancel → Update (or the
    // search field).
    property bool rrSummaryShown: false
    function updateFromRadioReference(uids) {
        if (!rrPlaces.updateFromRadioReference(uids)) {
            openRadioReferenceAccount();
            return;
        }
        rrSummaryShown = true;
    }
    function updateAllFromRadioReference() {
        var uids = [];
        for (var row = 0; row < savedSystems.count; ++row)
            uids.push(savedSystems.get(row).uid);
        updateFromRadioReference(uids);
    }
    function announceRrSummary() {
        if (rrSummaryShown && current && rrPlaces.summaryText.length > 0)
            Navigation.announce(controls, rrSummaryLine, rrPlaces.summaryText);
    }
    function moveFocusAfterUpdate(fromUpdate, fromCancel) {
        if (fromUpdate && rrPlaces.updating)
            rrCancel.forceActiveFocus();
        else if (fromUpdate || fromCancel)
            (placeNotice.shown ? placeNotice.actionItem : systemsSearch.input).forceActiveFocus();
    }
    Connections {
        target: rrPlaces
        ignoreUnknownSignals: true
        function onUpdatingChanged() {
            // Read now, while the pressed control still holds the keyboard; moved once the lines have followed.
            Qt.callLater(controls.moveFocusAfterUpdate, placeNotice.actionItem.activeFocus, rrCancel.activeFocus);
        }
        function onUpdateFinished() {
            Qt.callLater(controls.announceRrSummary);
        }
    }

    // Search and Sort: one row while the pill takes at most 40% of it and the
    // field keeps 180 px; otherwise the field, then the pill under it,
    // left-aligned. Tab order: the field, its ×, the pill.
    Item {
        id: searchBar

        objectName: "systemsControls"
        readonly property bool stacked: sortPill.implicitWidth > 0.4 * width || width - sortPill.implicitWidth - Theme.gap < 180
        width: parent.width
        height: stacked ? systemsSearch.height + 8 + sortPill.height : Math.max(systemsSearch.height, sortPill.height)

        PlexTextField {
            id: systemsSearch

            objectName: "systemsSearch"
            width: searchBar.stacked ? parent.width : parent.width - sortPill.width - Theme.gap
            height: implicitHeight
            label: qsTr("Search saved systems")
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            // Room for the × over the end of the line.
            input.anchors.rightMargin: systemsSearchClear.visible ? systemsSearchClear.width + 4 : 14
            // Changed from outside (a reset, the last system removed): the field follows.
            Binding {
                target: systemsSearch
                property: "text"
                value: savedSystemCards.query
            }
            onTextChanged: {
                if (text !== savedSystemCards.query)
                    searchDebounce.restart();
                else
                    searchDebounce.stop();
            }
            Component.onCompleted: input.Accessible.searchEdit = true
        }
        IconButton {
            id: systemsSearchClear

            objectName: "systemsSearchClear"
            icon: "close"
            accessibleName: qsTr("Clear search text")
            visible: systemsSearch.text.length > 0
            x: systemsSearch.x + systemsSearch.width - width - 2
            y: systemsSearch.y + systemsSearch.labelHeight + (systemsSearch.height - systemsSearch.labelHeight - systemsSearch.helperHeight - height) / 2
            onClicked: controls.clearSearch(activeFocus)
        }
        FilterPill {
            id: sortPill

            objectName: "systemsSortPill"
            x: searchBar.stacked ? 0 : parent.width - width
            y: searchBar.stacked ? systemsSearch.height + 8 : (parent.height - height) / 2
            width: Math.min(implicitWidth, parent.width)
            wrapLabel: true
            text: qsTr("Sort: %1").arg(controls.sortLabel(controls.shownSort))
            accessibleName: qsTr("Sort systems, %1").arg(controls.sortLabel(controls.shownSort))
            onClicked: controls.openSortChoices()
        }
    }

    // While a search is applied: how many cards it shows, and the way back to
    // all of them. With none, the card below says so.
    SystemsNotice {
        objectName: "systemsSearchCount"
        textObjectName: "systemsSearchCountText"
        actionObjectName: "systemsClearSearch"
        shown: savedSystemCards.query.length > 0 && savedSystemCards.shownCount > 0
        text: controls.countText
        actionText: qsTr("Clear search")
        onAction: function (hadFocus) {
            controls.clearSearch(hadFocus);
        }
    }

    // Nearest: where the distances are measured from, and the one way to ask
    // for (or refresh) a fix. Never asked for on its own. Without a fix the
    // cards are in name order, and the line says so throughout.
    SystemsNotice {
        objectName: "systemsLocation"
        textObjectName: "systemsLocationText"
        actionObjectName: "systemsLocationButton"
        shown: controls.shownSort === "nearest"
        text: controls.locationText
        detail: !controls.hasFix ? qsTr("Showing names until a location arrives") : ""
        actionText: controls.hasFix ? qsTr("Update location") : qsTr("Use my location")
        actionEnabled: !controls.locationId
        onAction: controls.requestNearestLocation()
    }

    // Place and Nearest order cards by place: the systems an update could
    // place, and that update.
    SystemsNotice {
        id: placeNotice

        objectName: "systemsPlaceNotice"
        textObjectName: "systemsPlaceNoticeText"
        actionObjectName: "systemsPlaceUpdate"
        shown: (controls.shownSort === "place" || controls.shownSort === "nearest") && rrPlaces.unplacedText.length > 0
            && !rrPlaces.updating
        text: rrPlaces.unplacedText
        actionText: qsTr("Update from RadioReference")
        onAction: controls.updateAllFromRadioReference()
    }

    // An Update from RadioReference started here: how far it is, with Cancel,
    // then what it did.
    Item {
        id: rrStatus

        objectName: "systemsRrStatus"
        readonly property bool shown: rrPlaces.updating || (controls.rrSummaryShown && rrPlaces.summaryText.length > 0)
        readonly property real inset: rrCancel.flatInset
        readonly property bool stacked: rrProgress.implicitWidth + 8 + rrCancel.implicitWidth - inset > width
        visible: shown
        width: parent.width
        readonly property real reach: (rrCancel.height - rrCancel.labelHeight) / 2
        height: !shown ? 0 : !rrPlaces.updating ? rrSummaryLine.height
            : stacked ? rrProgress.height + 4 - reach + rrCancel.height - Theme.gap / 2
            : Math.max(rrProgress.height, rrCancel.height - Theme.gap)

        // Moves with every system done, so it is paced for a screen reader.
        LiveText {
            id: rrProgress
            objectName: "systemsRrProgress"
            visible: rrPlaces.updating
            width: rrStatus.stacked ? parent.width : parent.width - rrCancel.width - 8 + rrStatus.inset
            y: rrStatus.stacked ? 0 : (parent.height - height) / 2
            text: rrPlaces.progressText
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
        }
        OutlineButton {
            id: rrCancel
            objectName: "systemsRrCancel"
            flat: true
            visible: rrPlaces.updating
            x: rrStatus.stacked ? -rrStatus.inset : parent.width - width + rrStatus.inset
            y: rrStatus.stacked ? rrProgress.height + 4 - rrStatus.reach : (parent.height - height) / 2
            width: Math.min(implicitWidth, parent.width + 2 * rrStatus.inset)
            text: qsTr("Cancel")
            onClicked: rrPlaces.cancelUpdate()
        }
        ReadableText {
            id: rrSummaryLine
            objectName: "systemsRrSummary"
            visible: !rrPlaces.updating
            width: parent.width
            text: rrPlaces.summaryText
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
        }
    }

    // A search that matches nothing says so, and offers the way back.
    UiPanel {
        objectName: "systemsNoMatch"
        readonly property bool shown: savedSystemCards.query.length > 0 && savedSystemCards.shownCount === 0
        visible: shown
        width: parent.width
        height: shown ? noMatchBody.implicitHeight + 2 * Theme.cardPadding : 0
        Column {
            id: noMatchBody
            x: Theme.cardPadding
            y: Theme.cardPadding
            width: parent.width - 2 * Theme.cardPadding
            spacing: 10
            ReadableText {
                objectName: "systemsNoMatchText"
                width: parent.width
                text: controls.noMatchText
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
            }
            OutlineButton {
                objectName: "systemsNoMatchClear"
                width: Math.min(implicitWidth, parent.width)
                text: qsTr("Clear search")
                onClicked: controls.clearSearch(activeFocus)
            }
        }
    }
}
