// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Home (Listen): saved systems, scan lists and Explore, one group at a time under
// the Listen tabs, with consistent listen and manage actions.
Item {
    id: screen

    readonly property bool supportingPane: width >= 900
    property var failure: ({})
    // Whether the failed or pending start used a brokered receiver (RTL-SDR or
    // Airspy); only then does its details panel belong next to the failure.
    property bool localDeviceRelevant: true
    // The receiver a start is waiting on. Main binds it; the host's record names
    // it only once the request lands.
    property string requestedSource: decoderHost && decoderHost.localDeviceRequestedSource ? decoderHost.localDeviceRequestedSource : ""
    property string completionMessage: ""
    signal retryFailure
    signal editFailure
    signal dismissFailure
    signal failureDetails
    signal replayAgain
    signal addScanList
    signal playScanList(int row)
    // Issue #697: start the list at @a row with the lists @a companionUids
    // name, in that order, as one session.
    signal playScanLists(int row, var companionUids)
    signal editScanList(int row)
    signal addSystem
    signal importSystem
    signal chooseSites(int row)
    property int siteRevision: 0
    Connections {
        target: savedSystems
        function onSitesChanged() {
            screen.siteRevision++;
        }
    }
    // Every library change, a file replaced in place included: a user's own
    // file over a RadioReference one no longer asks for a refresh (#832).
    Connections {
        target: importedFiles
        function onEntriesChanged() {
            screen.siteRevision++;
        }
    }
    function needsSiteRefresh(row) {
        var sys = savedSystems.get(row);
        if (!sys.trunking || (sys.rrSid > 0 && sys.rrSiteId > 0))
            return false;
        for (var path of [sys.chanCsvPath, sys.groupCsvPath]) {
            var libraryRow = importedFiles.rowForPath(path || "");
            if (libraryRow >= 0 && importedFiles.get(libraryRow).origin === "radioreference")
                return true;
        }
        return false;
    }
    signal playSystem(int row)
    signal editSystem(int row)
    // Tap starts exploring with what was used last; long-press changes it. Same
    // split as a system card, where the face plays and the press manages.
    signal explore
    signal signalSearch
    signal exploreSetup
    // Issue #801: a pinned shortcut was tapped; Main resolves @a kind and
    // @a uid at that moment (Main.startPin()).
    signal startPin(string kind, string uid)
    // The receiver a tap on the Explore card starts on. Main binds it to the
    // remembered radio resolved against what is plugged in, so the card states
    // what the tap will do.
    property string exploreSource: prefs.exploreSourceType

    // Re-derives "Heard n minutes ago" once a minute so rows do not go stale.
    property int heardTick: 0

    Timer {
        interval: 60000
        running: screen.visible
        repeat: true
        onTriggered: screen.heardTick++
    }

    // Keep auto-start blocked throughout menu-to-confirmation transitions.
    readonly property bool managementSheetOpen: manageMenu.visible || scanMenu.visible || addSystemMenu.visible
        || removeSystem.visible || systemRemovalError.visible || removeScanList.visible || scanListRemovalError.visible
        || scanTogether.visible || pinnedSheet.visible || exploreMenu.visible || searchMenu.visible

    // #647 WP6: where the host brokers USB access the title and the receiver
    // pill are pinned above the scrolling body, so the receiver's state and its
    // Connect or Retry never scroll away. The desktop opens its receiver itself
    // and has no pill; its title is fixed above the body only while the Pinned
    // strip or the Listen tabs are (fixedTitle).
    readonly property bool pinnedHeader: !!decoderHost && decoderHost.localDeviceBrokered === true
    // Issue #646: how tall the fixed region is with the Listen tabs in it,
    // down to the tabs' hairline, where the body starts: the title row (and
    // the receiver pill as it wraps), the Pinned strip as held, the gaps and
    // the tabs. It never reads where the tabs are, any `visible` or which tab
    // is chosen, so where they go cannot feed back into it.
    readonly property real fixedExtent: listenHeader.y + listenHeader.naturalHeight
        + (pinnedStrip.shown ? pinnedStrip.pinGap + pinnedStrip.height : 0) + pinnedStrip.pinGap + listenTabs.implicitHeight
    // The tabs stay fixed while that leaves the body at least half of Home
    // (a tie is fixed); on a shorter screen they are the first thing in the
    // body and scroll with it. With no height yet, the last answer stands
    // (fixed to begin with). Only placeTabs() sets it, from the inputs'
    // change signals: as a binding (even a readonly copy of a plain
    // property) it is first evaluated inside fixedTitle's own evaluation
    // while Home is built, which Qt reports as a binding loop.
    property bool tabsFixed: true
    function placeTabs() {
        if (height > 0)
            tabsFixed = fixedExtent <= height / 2;
    }
    onHeightChanged: placeTabs()
    onFixedExtentChanged: placeTabs()
    // Issue #801: the title is fixed above the body wherever the header is
    // pinned, and on the desktop too while the Pinned strip or the Listen tabs
    // are fixed, so the order is title, strip, tabs, body on both hosts. The
    // desktop's header then holds the title alone; with neither, the title
    // leads the body. Keyed on the strip's stored pins, not on `visible`
    // (below).
    readonly property bool fixedTitle: pinnedHeader || pinnedStrip.shown || tabsFixed
    readonly property bool dongleReady: pinnedHeader && deviceStatus.state === "ready"
    readonly property bool showDeviceDetails: pinnedHeader && localDeviceRelevant && !dongleReady && !(failure.message || "").length
    // The content column, which the pinned header lines up with.
    readonly property real columnWidth: supportingPane ? width - 280 - 3 * Theme.screenPadding : Math.min(Theme.formWidth, width - 2 * Theme.screenPadding)
    readonly property real columnX: supportingPane ? Theme.screenPadding : (width - columnWidth) / 2
    readonly property int headerClearance: 6
    // Issue #801: the Pinned strip's height, 0 with nothing pinned.
    readonly property real pinnedStripHeight: pinnedStrip.height

    // Issue #801 [r1-7]: the pins the strip shows, taken from listenPins only
    // while Home is shown. Hidden (another tab, a session, the wizard), it holds
    // the last list, so a rename or removal there raises nothing from the strip
    // and its height, and the body's scroll range, stay as they were; shown
    // again, it takes the list as it is then. The line they are laid out on is
    // held with them, its width and the pins' font, so a rotation or a text
    // size changed behind a hidden Home adds or removes no pin there either.
    property var heldPins: []
    property real heldStripWidth: 0
    property font heldPinFont: Theme.pinLabelFont
    function takePins() {
        if (!visible)
            return;
        heldPins = listenPins.entries;
        heldStripWidth = columnWidth;
        heldPinFont = Theme.pinLabelFont;
    }
    readonly property font pinLabelFont: Theme.pinLabelFont
    onVisibleChanged: {
        takePins();
        if (visible && restorePending)
            restoreTimer.restart();
    }
    onColumnWidthChanged: takePins()
    onPinLabelFontChanged: takePins()
    Component.onCompleted: {
        section = knownSection(prefsStore ? prefsStore.listenSection : "");
        takePins();
        placeTabs();
    }

    // Issue #646 D5: the Listen tab shown. It is read once from the tab this
    // device remembers (AppPrefs listen/section), anything unknown reading as
    // Systems, and is Home's own from then on: a choice writes the preference
    // but never waits on it, so a store that keeps nothing (a test's plain map)
    // still switches the tab. prefsStore is the app's AppPrefs; a test may
    // hand in its own.
    property var prefsStore: typeof prefs !== "undefined" ? prefs : null
    property string section: "systems"
    readonly property var sectionTabs: [
        {id: "systems", label: qsTr("Systems")},
        {id: "scanLists", label: qsTr("Scan lists")},
        {id: "explore", label: qsTr("Explore")}
    ]
    function knownSection(id) {
        return id === "scanLists" || id === "explore" ? id : "systems";
    }
    function sectionItem(id) {
        return id === "scanLists" ? listenScanListsSection : id === "explore" ? listenExploreSection : listenSystemsSection;
    }
    function holdsFocus(item) {
        var window = screen.Window.window;
        for (var at = window ? window.activeFocusItem : null; at; at = at.parent) {
            if (at === item)
                return true;
        }
        return false;
    }
    // Shows the section @a id names (`systems`, `scanLists` or `explore`);
    // any other id does nothing. Keyboard focus inside the section being
    // hidden moves first to the current tab, else to the body, so nothing
    // hidden keeps it; the tabs then hand it to the new current tab.
    // The body returns to where this tab was left (see restoreTimer).
    function showSection(id) {
        if (id !== knownSection(id) || id === section)
            return;
        // A fling stops with the tab it was on. The place left is kept unless
        // a restore is still owed: until then the body shows a range clamped
        // to the section before, which is no place the user left.
        scrollBody.cancelFlick();
        if (!restorePending)
            sectionScroll[section] = clampedScroll(scrollBody.contentY);
        restorePending = true;
        ++restoreGeneration;
        placeChase.active = false;
        revealFocusTimer.stop();
        focusRevealHeld = false;
        if (holdsFocus(sectionItem(section)) && !listenTabs.focusCurrent())
            scrollBody.forceActiveFocus();
        section = id;
        if (prefsStore && prefsStore.listenSection !== undefined)
            prefsStore.listenSection = id;
        restoreTimer.generation = restoreGeneration;
        restoreTimer.restart();
    }

    // Issue #646: each tab's place in the body, its contentY when it was left;
    // a tab not yet left opens at the top.
    property var sectionScroll: ({})
    // A tab's place is owed to the body from the choice until restoreTimer
    // puts it back; restoreGeneration names the latest choice, so a timer
    // armed for an earlier one drops itself.
    property bool restorePending: false
    property int restoreGeneration: 0
    function clampedScroll(y) {
        return Math.max(scrollBody.originY, Math.min(y, scrollBody.originY + scrollBody.contentHeight - scrollBody.height));
    }
    function cancelRestore() {
        restorePending = false;
        ++restoreGeneration;
        restoreTimer.stop();
        placeChase.active = false;
    }
    // Puts the shown tab's place back a turn after the choice, once Home is
    // shown (a hidden Home waits to be shown again). It lays the section and
    // the body's column out itself first, so the range it clamps to is the
    // new section's, not one a positioning pass still owes. With the
    // keyboard focus on the tabs in the body, they stay in view: the place
    // yields to them.
    function restoreSection() {
        if (!restorePending || restoreTimer.generation !== restoreGeneration || !visible)
            return;
        sectionItem(section).forceLayout();
        content.forceLayout();
        restorePending = false;
        var place = sectionScroll[section];
        if (place === undefined)
            place = scrollBody.originY;
        scrollBody.contentY = clampedScroll(place);
        // A keyboard choice with the tabs in the body: the place yields to
        // the focused tab, now and as the range grows, so it is not followed.
        var keyboardOnBodyTabs = !tabsFixed && holdsFocus(listenTabs);
        if (keyboardOnBodyTabs)
            revealFocus();
        placeChase.active = !keyboardOnBodyTabs && scrollBody.contentY < place - 0.5;
        placeChase.place = place;
        placeChase.applied = scrollBody.contentY;
        placeChase.frames = 0;
    }
    // Issue #646: the restore lays out the section and the body's column, but
    // not the columns nested in them (a card's labels), which a text-size
    // change in the same turn leaves to the next polish; the range can then
    // grow after the place was clamped short of it. While a restore stands
    // clamped short, each change of the body's range puts the place back
    // again. It never starts after a keyboard choice with the tabs in the
    // body (the place yields to the focused tab). It stops when the place is
    // reached; when the body was moved
    // some other way (user movement at once; a focus reveal or any other
    // write at the next range change, by contentY no longer being what was
    // put back); when a tab is chosen or revealTop() runs; or once two
    // frames have been swapped since (the owed polish runs before the next
    // frame; the second covers one already in flight), so later growth, a
    // card added minutes on, never pulls the view.
    QtObject {
        id: placeChase
        property bool active: false
        property real place: 0
        property real applied: 0
        property int frames: 0
    }
    function chaseSectionPlace() {
        if (!placeChase.active)
            return;
        if (scrollBody.moving || Math.abs(scrollBody.contentY - placeChase.applied) >= 0.5) {
            placeChase.active = false;
            return;
        }
        scrollBody.contentY = clampedScroll(placeChase.place);
        placeChase.applied = scrollBody.contentY;
        if (placeChase.applied >= placeChase.place - 0.5)
            placeChase.active = false;
    }
    Connections {
        target: screen.Window.window
        enabled: placeChase.active
        function onFrameSwapped() {
            if (++placeChase.frames >= 2)
                placeChase.active = false;
        }
    }
    Timer {
        id: restoreTimer
        interval: 0
        // The choice this restore is for (restoreGeneration).
        property int generation: 0
        onTriggered: screen.restoreSection()
    }
    // Forgets every tab's place and any scroll still owed, as a new Home
    // starts. For tests, which reuse one Home across cases.
    function resetScrollMemory() {
        cancelRestore();
        revealFocusTimer.stop();
        focusRevealHeld = false;
        sectionScroll = {};
        scrollBody.cancelFlick();
        scrollBody.contentY = scrollBody.originY;
    }
    Connections {
        target: listenPins
        function onEntriesChanged() {
            screen.takePins();
        }
    }

    // Issue #801: Pin to Listen as every source's menu offers it, keyed "pin":
    // Unpin once pinned, disabled with the reason while Pinned is full.
    function pinAction(kind, uid, objectName) {
        return Object.assign({key: "pin", objectName: objectName}, listenPins.action(kind, uid));
    }
    // Does what the row said when it was offered (@a offeredPinned: it read
    // Unpin), never the opposite. pin()'s refusal needs no notice here: the
    // menus are modal, so nothing fills the row while one is open, and a full
    // row's Pin is disabled with the reason.
    function togglePin(kind, uid, offeredPinned) {
        if (offeredPinned)
            listenPins.unpin(kind, uid);
        else
            listenPins.pin(kind, uid);
    }

    // B5: a session's ending or failure is reported at the top of the body.
    // Issue #646 B3: this is the strongest of Home's scroll writers. It
    // drops a tab's place still owed and a focus reveal still pending, keeps
    // the top as the shown tab's place, and holds off focus reveals until the
    // user scrolls or moves the focus by key, so a sheet handing the focus
    // back to its opener later cannot scroll the failure card away.
    function revealTop() {
        cancelRestore();
        revealFocusTimer.stop();
        scrollBody.cancelFlick();
        scrollBody.contentY = scrollBody.originY;
        sectionScroll[section] = scrollBody.originY;
        focusRevealHeld = true;
    }

    // Issue #646 B3: keyboard focus moving onto a control in the body scrolls
    // it into view once the move has settled. The timer is owned, so a scroll
    // with a stronger claim can cancel a reveal still pending: revealTop()
    // above all, and a tab's place being put back (which reveals the tabs
    // itself when they hold the focus in the body).
    function revealFocus() {
        var window = screen.Window.window;
        if (window && screen.visible)
            Theme.revealFocus(scrollBody, scrollBody.contentItem, window.activeFocusItem);
    }
    Timer {
        id: revealFocusTimer
        interval: 0
        onTriggered: screen.revealFocus()
    }
    property bool focusRevealHeld: false
    // The last item with the keyboard focus, to tell a move by Tab or Backtab
    // (to the next stop either way from a control on screen) from the focus
    // being handed somewhere.
    property var lastFocusItem: null
    function movedByKey(from, to) {
        return !!from && !!to && from.visible && (from.nextItemInFocusChain(true) === to || from.nextItemInFocusChain(false) === to);
    }
    Connections {
        target: screen.Window.window
        function onActiveFocusItemChanged() {
            var from = screen.lastFocusItem;
            var to = screen.Window.window.activeFocusItem;
            screen.lastFocusItem = to;
            if (!screen.visible || screen.restorePending)
                return;
            if (screen.focusRevealHeld) {
                if (!screen.movedByKey(from, to))
                    return;
                screen.focusRevealHeld = false;
            }
            revealFocusTimer.restart();
        }
    }

    DeviceStatus {
        id: deviceStatus
        host: decoderHost
        requestedSource: screen.requestedSource
    }

    component ListenTitle: ReadableText {
        heading: true
        text: qsTr("Listen")
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(24)
        font.weight: Font.Bold
        font.letterSpacing: -0.24
        color: Theme.textPrimary
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Item {
        id: listenHeader

        objectName: "listenHeader"
        visible: screen.fixedTitle
        enabled: !screen.managementSheetOpen
        x: screen.columnX
        y: Theme.screenPadding
        width: screen.columnWidth
        // One row while the title and the whole pill fit side by side; otherwise
        // the pill drops under the title, left-aligned, and its label wraps at
        // spaces rather than eliding.
        readonly property bool stacked: screen.pinnedHeader && headerTitle.implicitWidth + Theme.gap + dongleStatusPill.implicitWidth > width
        readonly property real titleHeight: Math.max(44, headerTitle.implicitHeight)
        // The pill as it is laid out, its label wrapped at the width it has,
        // never only its minimum (fixedExtent reads this too).
        readonly property real naturalHeight: !screen.pinnedHeader ? titleHeight : stacked ? titleHeight + 4 + dongleStatusPill.height : Math.max(titleHeight, dongleStatusPill.height)
        // Keyed on the host and the strip's held pins, not on `visible`: Main
        // hides Home on the other tabs, and a collapsing header would move the
        // body's scroll position.
        height: screen.fixedTitle ? naturalHeight : 0

        ListenTitle {
            id: headerTitle
            objectName: "listenHeaderTitle"
            y: listenHeader.stacked ? (listenHeader.titleHeight - height) / 2 : (listenHeader.height - height) / 2
        }

        Rectangle {
            id: dongleStatusPill

            objectName: "dongleStatusPill"
            // Only where the host brokers USB: the desktop's header holds the title alone.
            visible: screen.pinnedHeader
            readonly property bool actionable: deviceStatus.canAct && !screen.managementSheetOpen
            readonly property bool navigationAllowed: Navigation.allows(dongleStatusPill)
            function activate() {
                if (!actionable || !Navigation.allows(dongleStatusPill))
                    return;
                decoderHost.requestLocalDeviceAccess();
            }
            // A tab stop whatever its state: pressing it opens Android's prompt,
            // which makes it inert under the user's focus, and focus stays put
            // as on any control. The role and activate() say whether it acts.
            // Held while it has the focus (see OutlineButton).
            activeFocusOnTab: (visible && navigationAllowed) || activeFocus
            Accessible.role: deviceStatus.canAct ? Accessible.Button : Accessible.StaticText
            Accessible.name: deviceStatus.accessibleName
            Accessible.description: deviceStatus.detailText
            // Issue #782: focusable only while in the tree (see OutlineButton).
            Accessible.focusable: navigationAllowed && visible
            Accessible.ignored: !navigationAllowed || !visible
            Accessible.onPressAction: activate()
            Keys.onReturnPressed: activate()
            Keys.onEnterPressed: activate()
            Keys.onSpacePressed: activate()
            FocusFrame {}

            // The whole label on one line, which the one-row test measures.
            implicitWidth: Math.ceil(pillMetrics.advanceWidth) + 12 + 7 + 7 + 12
            implicitHeight: Math.max(Theme.minimumTouchSize, 30 * Theme.fontScale)
            x: listenHeader.stacked ? 0 : listenHeader.width - width
            y: listenHeader.stacked ? listenHeader.titleHeight + 4 : (listenHeader.height - height) / 2
            width: Math.min(implicitWidth, listenHeader.width)
            height: Math.max(implicitHeight, pillLabel.height + 16)
            radius: Theme.radiusButton
            color: pillTap.pressed && actionable ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
            border.width: 1
            border.color: Theme.panelBorder

            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }

            TextMetrics {
                id: pillMetrics
                font: pillLabel.font
                text: pillLabel.text
            }

            Rectangle {
                x: 12
                // Level with the first line when the label wraps.
                y: pillLabel.y + (pillMetrics.height - height) / 2
                width: 7
                height: 7
                radius: 3.5
                color: deviceStatus.dotColor
            }

            Text {
                id: pillLabel

                objectName: "dongleStatusLabel"
                x: 12 + 7 + 7
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - x - 12
                wrapMode: Text.WordWrap
                text: deviceStatus.pillText
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(11)
                font.letterSpacing: 1.4
                color: deviceStatus.state === "ready" || deviceStatus.alerting ? Theme.textPrimary : Theme.textSubdued
            }

            TapHandler {
                id: pillTap
                enabled: dongleStatusPill.actionable
                onTapped: dongleStatusPill.activate()
            }
        }
    }

    // Issue #801: the Pinned strip, one line under the fixed title header and
    // above the body, on both hosts (see fixedTitle). It holds the pins that
    // are found, in order, as many as fit, each at most the line wide less
    // More; More always ends it and opens the
    // Pinned sheet with every pin. Pins that do not fit are not made, and the
    // strip is no Flickable, so it adds no container node a swipe could stick on.
    Item {
        id: pinnedStrip

        objectName: "pinnedStrip"
        // Keyed on what is stored, not on `visible` (see listenHeader), and kept
        // while every pin is not found, so More still reaches them to unpin.
        readonly property bool shown: screen.heldPins.length > 0
        readonly property int pinGap: 8
        visible: shown
        enabled: !screen.managementSheetOpen
        x: screen.columnX
        y: listenHeader.y + listenHeader.height + pinGap
        width: screen.columnWidth
        // One line of label in a pin, at any text size.
        height: shown ? Math.max(Theme.minimumTouchSize, Math.ceil(pinMetrics.height) + 24) : 0

        // PinButton's label font as held, to measure pins before any is made.
        FontMetrics {
            id: pinMetrics
            font: screen.heldPinFont
        }
        // The narrowest a pin is shown elided at the end of the line: its
        // padding and about five characters of label, and never under two
        // touch targets, so a stub of a pin never stands in for a name.
        readonly property real elidedMinimum: Math.max(2 * Theme.minimumTouchSize, Math.ceil(5 * pinMetrics.averageCharacterWidth) + 2 * 15)
        // The found pins that fit, in order, each with its place: a pin is as
        // wide as its label (at most the room left of More). The first one that
        // does not fit ends the line: shown elided in the room left when that is
        // at least elidedMinimum (a long name otherwise leaves most of the line
        // blank, as if nothing else were pinned), else left to the sheet.
        readonly property var placed: {
            // Nothing to lay out before Home has first shown. Reading the held
            // font here also lays the line out again when it changes, which the
            // advanceWidth() calls below, being calls, would not.
            if (screen.heldStripWidth <= 0 || screen.heldPinFont.pixelSize <= 0)
                return [];
            var room = screen.heldStripWidth - moreButton.width - pinGap;
            var out = [];
            var at = 0;
            for (var entry of screen.heldPins) {
                if (!entry.found)
                    continue;
                // As PinButton sizes its label: the wider of the advance and
                // the ink, and a pixel, or a whole name can elide by a fraction.
                var label = Math.ceil(Math.max(pinMetrics.advanceWidth(entry.label), pinMetrics.boundingRect(entry.label).width)) + 1;
                var wide = Math.min(room, Math.max(Theme.minimumTouchSize, label + 2 * 15));
                var last = out.length > 0 && at + wide > room;
                if (last) {
                    wide = Math.floor(room - at);
                    if (wide < elidedMinimum)
                        break;
                }
                out.push({key: entry.key, kind: entry.kind, uid: entry.uid, label: entry.label,
                    accessibleName: entry.accessibleName, x: at, width: wide});
                if (last)
                    break;
                at += wide + pinGap;
            }
            return out;
        }

        // A button per pin, kept on its pin (StableRows): a rename renames it,
        // a pin that leaves the line or arrives on it adds or removes only it.
        Repeater {
            model: StableRows {
                id: stripRows
                entries: pinnedStrip.placed
                keyOf: function (entry) {
                    return entry.key;
                }
                lineOf: function (entry) {
                    return JSON.stringify([entry.label, entry.accessibleName]);
                }
            }
            PinButton {
                required property int index
                required property string key
                required property string line
                readonly property var parts: JSON.parse(line)
                readonly property var place: index >= 0 && index < pinnedStrip.placed.length ? pinnedStrip.placed[index] : null
                readonly property string pinKey: key

                objectName: "pinnedStripPin"
                x: place ? place.x : 0
                width: place ? place.width : 0
                height: pinnedStrip.height
                // A pin past the end of a shrinking line goes at the end of the
                // turn; until then it is not drawn, reads as nothing and takes
                // no focus or press (StableRows' `shown` and `live`).
                opacity: index >= 0 && index < stripRows.shown ? 1 : 0
                live: index >= 0 && index < stripRows.live
                text: parts[0]
                accessibleName: parts[1]
                enabled: !decoderHost.transitioning
                onClicked: {
                    for (var entry of pinnedStrip.placed) {
                        if (entry.key === pinKey) {
                            screen.startPin(entry.kind, entry.uid);
                            return;
                        }
                    }
                }
            }
        }

        IconButton {
            id: moreButton

            objectName: "pinnedStripMore"
            icon: "more"
            accessibleName: qsTr("All pinned shortcuts")
            x: pinnedStrip.width - width
            anchors.verticalCenter: parent.verticalCenter
            onClicked: pinnedSheet.open()
        }
    }

    // Issue #646: the Listen tabs, under the Pinned strip (or the title with
    // nothing pinned) on the content column, the last of the fixed region and
    // the last Tab stop before the body. Their own hairline marks where the
    // body's cards scroll away, so the body starts right under it. On a short
    // screen (not tabsFixed) the same tabs move into the body's first slot
    // instead, where they sit as they would here and scroll with the cards.
    Item {
        id: fixedTabsSlot

        visible: screen.tabsFixed
        x: screen.columnX
        y: (pinnedStrip.shown ? pinnedStrip.y + pinnedStrip.height : listenHeader.y + listenHeader.height) + pinnedStrip.pinGap
        width: screen.columnWidth
        height: screen.tabsFixed ? listenTabs.implicitHeight : 0

        ListenTabs {
            id: listenTabs

            parent: screen.tabsFixed ? fixedTabsSlot : bodyTabsSlot
            enabled: !screen.managementSheetOpen
            width: screen.columnWidth
            height: implicitHeight
            model: screen.sectionTabs
            currentId: screen.section
            onSelected: function (id) {
                screen.showSection(id);
            }
        }
    }

    PlexFlickable {
        id: scrollBody

        objectName: "dongleScrollBody"
        anchors.fill: parent
        // The user scrolling takes over from revealTop() (focusRevealHeld)
        // and from a restore still following the range (placeChase).
        onMovementStarted: {
            screen.focusRevealHeld = false;
            placeChase.active = false;
        }
        onContentHeightChanged: screen.chaseSectionPlace()
        onOriginYChanged: screen.chaseSectionPlace()
        // Pinned, the body is clipped a few pixels below the header, so cards
        // scroll away short of the pill; the rest of the header's gap to the
        // first card stays at the body's top, and nothing moves at rest.
        // The Pinned strip, when shown, sits between them on the same terms.
        // Under the fixed Listen tabs it starts at their hairline.
        anchors.topMargin: screen.tabsFixed ? fixedTabsSlot.y + fixedTabsSlot.height
            : pinnedStrip.shown ? pinnedStrip.y + pinnedStrip.height + screen.headerClearance
            : screen.fixedTitle ? listenHeader.y + listenHeader.height + screen.headerClearance : 0
        contentHeight: content.y + content.height + Theme.screenPadding
        clip: true
        // Menu rows use passive tap grabs, so a tap on a manage-menu button
        // would otherwise also reach whatever sits under the overlay — observed as
        // "Edit this system" opening the add wizard through the card list.
        enabled: !screen.managementSheetOpen

        Column {
            id: content

            x: screen.columnX
            // Under fixed tabs the cards start a gap below their hairline.
            // With the tabs in the body under a fixed title, the tabs start
            // where fixed ones would, the strip's 8 px under what is fixed.
            y: screen.tabsFixed ? Theme.gap : screen.fixedTitle ? pinnedStrip.pinGap - screen.headerClearance : Theme.screenPadding
            width: screen.columnWidth
            spacing: Theme.gap

            Item {
                visible: !screen.fixedTitle
                width: parent.width
                height: 44

                ListenTitle {
                    objectName: "listenTitle"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // Issue #646: where the Listen tabs go on a short screen (see
            // fixedTabsSlot), first in the body, after the title when the
            // title leads it.
            Item {
                id: bodyTabsSlot
                visible: !screen.tabsFixed
                width: parent.width
                height: screen.tabsFixed ? 0 : listenTabs.implicitHeight
            }

            // Issue #580: what this start's restore did, first thing, until dismissed.
            BackupResultCard {
                width: parent.width
                outcome: backup.startupOutcome
            }

            // WP-D5: keep the complete USB diagnostic next to the dongle pill.
            UiPanel {
                objectName: "dongleDetailsPanel"
                width: parent.width
                visible: screen.showDeviceDetails
                // Keyed on its own condition, not on `visible`, which also goes
                // false while Main shows another tab and would collapse the body.
                height: screen.showDeviceDetails ? dongleDetails.implicitHeight + 2 * Theme.cardPadding : 0
                Column {
                    id: dongleDetails
                    x: Theme.cardPadding
                    y: Theme.cardPadding
                    width: parent.width - 2 * Theme.cardPadding
                    spacing: 12
                    // Read as the status pill's description (issue #782).
                    Text {
                        objectName: "dongleStatusText"
                        width: parent.width
                        text: deviceStatus.detailText
                        // Can name the USB device as it names itself.
                        textFormat: Text.PlainText
                        Accessible.ignored: true
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(14)
                        color: Theme.textPrimary
                    }
                    OutlineButton {
                        objectName: "dongleRetry"
                        width: Math.max(110, implicitWidth)
                        visible: deviceStatus.actionText.length > 0
                        text: deviceStatus.actionText
                        enabled: deviceStatus.canAct
                        onClicked: decoderHost.requestLocalDeviceAccess()
                    }
                }
            }

            FailureCard {
                objectName: "homeFailureCard"
                width: parent.width
                visible: (screen.failure.message || "").length > 0
                message: screen.failure.message || ""
                source: screen.failure.source || ""
                canRetry: screen.failure.canRetry === true
                retryText: screen.failure.retryText || qsTr("Retry")
                onRetry: screen.retryFailure()
                onEdit: screen.editFailure()
                onDismiss: screen.dismissFailure()
                onDetails: screen.failureDetails()
            }
            UiPanel {
                objectName: "homeCompletionCard"
                width: parent.width
                visible: screen.completionMessage.length > 0
                height: completionBody.implicitHeight + 2 * Theme.cardPadding
                Column {
                    id: completionBody
                    x: Theme.cardPadding
                    y: Theme.cardPadding
                    width: parent.width - 2 * Theme.cardPadding
                    spacing: 10
                    ReadableText {
                        width: parent.width
                        text: screen.completionMessage
                        wrapMode: Text.Wrap
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSize(15)
                    }
                    OutlineButton {
                        objectName: "homeReplayAgain"
                        width: parent.width
                        text: qsTr("Play again")
                        enabled: !decoderHost.sessionActive
                        onClicked: screen.replayAgain()
                    }
                }
            }

            // Issue #646: then the selected tab's section alone. A hidden
            // section is also disabled, so nothing in it takes the focus, a
            // key or a press, and its cards keep following their models (a
            // decided limit). Each section's height is keyed on the tab, never
            // on `visible`, which also goes false while Main hides Home (B1).
            // The tab's label is the section's heading, and each section
            // starts with its creation action, in reach however long it is.
            Column {
                id: listenSystemsSection

                objectName: "listenSystemsSection"
                readonly property bool shown: screen.section === "systems"
                visible: shown
                enabled: shown
                width: parent.width
                height: shown ? implicitHeight : 0
                spacing: Theme.gap

                Column {
                    width: parent.width
                    spacing: 6
                    DashedActionButton {
                        objectName: "addSystemButton"
                        width: parent.width
                        text: qsTr("+ Add a system")
                        onClicked: radioReference.available ? addSystemMenu.open() : screen.addSystem()
                    }
                    ReadableText {
                        objectName: "addSystemHint"
                        width: parent.width
                        text: qsTr("USB, Airspy, network or file")
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                    }
                }

                Repeater {
                    model: savedSystems

                    UiPanel {
                        id: card

                        required property int index
                        required property string name
                        required property string sourceType
                        required property string host
                        required property int port
                        required property string freqMhz
                        required property string decodeFlag
                        required property bool trunking
                        required property string filePath
                        required property string encKeyType
                        required property string keyCsvPath
                        required property double lastHeard

                        width: content.width
                        property var siblings: (screen.siteRevision, savedSystems.siblingRows(index))
                        // One card per group of sites, on its first site. Keyed on
                        // its own condition, not on `visible`, which also goes false
                        // while Main hides Home and would collapse the body (issue #646 B1).
                        readonly property bool shown: siblings.length === 0 || siblings[0] === index
                        readonly property bool hasSites: siblings.length > 0
                        visible: shown
                        height: shown ? fullHeight : 0
                        readonly property var playableSites: siblings.filter(function (row) {
                            return !savedSystems.get(row).avoidSite;
                        })
                        // Play's own condition, which the site button lines up
                        // against: never Play's `visible`, which also goes false
                        // with the section or Home hidden (issue #646).
                        readonly property bool playShown: siblings.length === 0 || playableSites.length === 1
                        property bool refreshHint: (screen.siteRevision, screen.needsSiteRefresh(index))

                        // Issue #646 B8: as tall as what it holds, so larger text grows
                        // the card instead of running its labels into the hint or past
                        // its edges. At normal text that is the actions' 122 px, as
                        // before. Play counts even while it is hidden (several sites to
                        // play): the site button is centred on it.
                        readonly property real actionsBottom: Math.max(manageButton.y + manageButton.height, play.y + play.height,
                            hasSites ? siteButton.y + siteButton.height : 0)
                        readonly property real labelsBottom: labels.y + labels.height
                        // The hint takes the full width, so it starts below both.
                        readonly property real hintTop: Math.max(labelsBottom, actionsBottom) + 8
                        readonly property real fullHeight: (refreshHint ? hintTop + refreshHintText.height : Math.max(labelsBottom, actionsBottom)) + Theme.cardPadding

                        Column {
                            id: labels
                            anchors.left: parent.left
                            anchors.right: card.hasSites ? siteButton.left : play.left
                            anchors.leftMargin: Theme.cardPadding
                            anchors.rightMargin: 12
                            // Centred 10 px above the middle of the actions' box: the
                            // actions plus the bottom padding, 122 px at normal text,
                            // so they sit where they always have; never above the top
                            // padding.
                            y: Math.max(Theme.cardPadding, (card.actionsBottom + Theme.cardPadding - height) / 2 - 10)
                            spacing: 4

                            ReadableText {
                                width: parent.width
                                objectName: "savedSystemTitle"
                                text: card.name
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(17)
                                font.weight: Font.Bold
                                color: Theme.textPrimary
                                elide: Text.ElideRight
                            }

                            ReadableText {
                                width: parent.width
                                text: Util.systemMeta(card)
                                font.family: Theme.mono
                                font.pixelSize: Theme.fontSize(12)
                                color: Theme.textSubdued
                                elide: Text.ElideRight
                            }

                            ReadableText {
                                width: parent.width
                                // heardTick forces the minute-by-minute refresh.
                                objectName: "savedSystemMeta"
                                text: ((card.encKeyType.length > 0 || card.keyCsvPath.length > 0) ? qsTr("key configured") + " · " : "") + (screen.heardTick, Util.heardText(card.lastHeard, savedSystems.realtimeNowMs()))
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(13)
                                color: Theme.textSecondary
                                elide: Text.ElideRight
                            }
                        }

                        ReadableText {
                            id: refreshHintText
                            objectName: "savedSystemRefreshHint"
                            x: Theme.cardPadding
                            y: card.hintTop
                            width: parent.width - 2 * Theme.cardPadding
                            visible: card.refreshHint
                            text: qsTr("Refresh from RadioReference to enable site grouping")
                            wrapMode: Text.Wrap
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSize(12)
                        }
                        OutlineButton {
                            id: siteButton
                            objectName: "homeSiteChooserButton"
                            accessibleName: qsTr("Choose sites for %1").arg(card.name)
                            width: implicitWidth
                            anchors.right: parent.right
                            anchors.verticalCenter: play.verticalCenter
                            anchors.rightMargin: card.playShown ? Theme.cardPadding + play.width + 8 : Theme.cardPadding
                            text: card.siblings.length === 1 ? qsTr("1 site") : qsTr("%1 sites").arg(card.siblings.length)
                            visible: card.hasSites
                            onClicked: screen.chooseSites(card.index)
                        }
                        PlayCircle {
                            id: play
                            accessibleName: card.sourceType === "file" ? qsTr("Play %1").arg(card.name) : qsTr("Listen to %1").arg(card.name)
                            objectName: "savedSystemPlay"
                            visible: card.playShown
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.cardPadding
                            anchors.top: parent.top
                            anchors.topMargin: Theme.minimumTouchSize + 6
                            enabled: !decoderHost.transitioning
                            onClicked: screen.playSystem(card.playableSites.length === 1 ? card.playableSites[0] : card.index)
                        }

                        IconButton {
                            id: manageButton
                            objectName: "savedSystemManageButton"
                            icon: "more"
                            accessibleName: qsTr("More options for %1").arg(card.name)
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.cardPadding + (play.width - width) / 2
                            anchors.top: parent.top
                            onClicked: manageMenu.openFor(card.index, card.name)
                        }
                        // Long-press manages the card: the design keeps card faces clean,
                        // so destructive actions hide behind the press.
                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            onLongPressed: manageMenu.openFor(card.index, card.name)
                        }
                    }
                }
            }

            Column {
                id: listenScanListsSection

                objectName: "listenScanListsSection"
                readonly property bool shown: screen.section === "scanLists"
                visible: shown
                enabled: shown
                width: parent.width
                height: shown ? implicitHeight : 0
                spacing: Theme.gap

                Column {
                    width: parent.width
                    spacing: 6
                    DashedActionButton {
                        objectName: "addScanListButton"
                        width: parent.width
                        text: qsTr("+ Add a scan list")
                        onClicked: screen.addScanList()
                    }
                    // With no list yet, what one is for.
                    ReadableText {
                        objectName: "scanListsEmptyHint"
                        width: parent.width
                        visible: scanLists.count === 0
                        text: qsTr("A scan list steps through saved systems and frequencies one at a time.")
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                    }
                }

                // WP-S1: same play/edit gestures as saved systems.
                Repeater {
                    model: scanLists
                    ScanListCard {
                        required property int index
                        required property string name
                        required property var entries
                        required property string targetSource
                        required property var targetsCsvPath
                        required isDraft
                        width: content.width
                        listName: name
                        entryCount: {
                            var changed = importedFiles.revision;
                            return scanLists.entryCount({targetSource: targetSource, entries: entries, targetsCsvPath: targetsCsvPath || ""});
                        }
                        onPlay: screen.playScanList(index)
                        onEdit: scanMenu.openFor(index)
                    }
                }
            }

            // Issue #646: Signal search first, then Explore the band.
            Column {
                id: listenExploreSection

                objectName: "listenExploreSection"
                readonly property bool shown: screen.section === "explore"
                visible: shown
                enabled: shown
                width: parent.width
                height: shown ? implicitHeight : 0
                spacing: Theme.gap

                // Issue #801 D5: the button fills the row; its More holds Pin to Listen.
                Item {
                    width: parent.width
                    height: searchButton.height

                    OutlineButton {
                        id: searchButton
                        objectName: "homeSignalSearch"
                        width: searchMore.x - 8
                        text: qsTr("Signal search")
                        accessibleName: qsTr("Signal search across frequency ranges")
                        onClicked: screen.signalSearch()
                    }
                    IconButton {
                        id: searchMore
                        objectName: "signalSearchManageButton"
                        icon: "more"
                        accessibleName: qsTr("More options for Signal search")
                        anchors.right: parent.right
                        // On the column of the Explore card's More just below, the
                        // axis every card's More shares.
                        anchors.rightMargin: exploreCard.width - exploreMore.x - exploreMore.width
                        anchors.verticalCenter: parent.verticalCenter
                        onClicked: searchMenu.openFor()
                    }
                }

                // Deliberately not a system card: there is nothing saved here, nothing
                // named, and nothing to come back to. It is a door, so it carries a
                // chevron rather than a play button, and its meta line states what the
                // tap will actually do so that starting is never a surprise.
                UiPanel {
                    id: exploreCard

                    objectName: "exploreCard"
                    width: content.width
                    height: Math.max(96, exploreLabels.implicitHeight + 2 * Theme.cardPadding)

                    readonly property string sourceLabel: screen.exploreSource === "rtltcp" ? qsTr("RTL-TCP") : screen.exploreSource === "airspy" ? qsTr("Airspy") : qsTr("USB dongle")
                    readonly property bool configured: prefs.exploreSourceType.length > 0

                    // The whole card is the tap target, More's spot aside (More takes
                    // its own press). Long-press manages the card, as a system card's does.
                    TapHandler {
                        id: faceTap
                        enabled: exploreFace.enabled
                        onTapped: exploreFace.activate()
                        onLongPressed: exploreMenu.openFor()
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        radius: Theme.radiusPanel - 1
                        color: faceTap.pressed ? Qt.alpha(Theme.cyan, 0.08) : "transparent"

                        Behavior on color {
                            ColorAnimation {
                                duration: 120
                            }
                        }
                    }

                    // B3: the card's face is the button that starts Explore, beside
                    // More rather than around it: it stops short of More's column, so
                    // its focus ring and its bounds leave More out, and a key pressed
                    // on More never reaches it. Its name carries both lines, which
                    // read nothing on their own, so nothing is said twice.
                    Item {
                        id: exploreFace

                        objectName: "exploreStart"
                        width: exploreMore.x
                        height: parent.height
                        enabled: !decoderHost.transitioning
                        readonly property bool navigationAllowed: Navigation.allows(exploreFace)
                        // Held while focused, and the gate read first: see OutlineButton.
                        activeFocusOnTab: (navigationAllowed && enabled && visible) || activeFocus
                        Accessible.role: Accessible.Button
                        // No name while out of the tree, as ReadableText: a frequency
                        // changed in Explore's setup, Home hidden, renames nothing.
                        Accessible.name: Accessible.ignored ? "" : exploreCard.configured ? qsTr("Explore the band, start on %1 from %2 MHz").arg(exploreCard.sourceLabel).arg(prefs.exploreFreqMhz) : qsTr("Explore the band, set up a radio to tune around and find what is on the air")
                        Accessible.focusable: navigationAllowed && visible
                        Accessible.ignored: !navigationAllowed || !visible
                        Accessible.onPressAction: activate()
                        // Nothing remembered means nothing to start from, so the first
                        // press asks rather than guessing at a radio and a frequency.
                        function activate() {
                            if (!enabled || !Navigation.allows(exploreFace))
                                return;
                            if (exploreCard.configured)
                                screen.explore();
                            else
                                screen.exploreSetup();
                        }
                        Keys.onReturnPressed: activate()
                        Keys.onEnterPressed: activate()
                        Keys.onSpacePressed: activate()
                        FocusFrame {
                            radius: Theme.radiusPanel + 2
                        }

                        Column {
                            id: exploreLabels
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: Theme.cardPadding
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4

                            Text {
                                objectName: "exploreTitle"
                                width: parent.width
                                Accessible.ignored: true
                                text: qsTr("Explore the band")
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(17)
                                font.weight: Font.Bold
                                color: Theme.textPrimary
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width
                                objectName: "homeExploreSubtitle"
                                Accessible.ignored: true
                                text: exploreCard.configured ? qsTr("%1 · from %2 MHz").arg(exploreCard.sourceLabel).arg(prefs.exploreFreqMhz) : qsTr("Tune around and find what is on the air")
                                font.family: exploreCard.configured ? Theme.mono : Theme.sans
                                font.pixelSize: Theme.fontSize(exploreCard.configured ? 12 : 13)
                                color: Theme.textSubdued
                                // Two lines before it elides, so the frequency a tap
                                // starts on stays whole at large text; the card grows
                                // with the labels.
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // In More's column, under More: drawn, not read (the face's name
                    // says where a tap goes), and inside the card's tap target.
                    Caret {
                        id: exploreCaret

                        anchors.right: parent.right
                        anchors.rightMargin: Theme.cardPadding + 6
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: Theme.cardPadding
                        // Larger than the default: at the size a filter pill uses it
                        // reads as a speck against a full-width card, and this is the
                        // only thing saying the card goes somewhere.
                        width: 13
                        height: 9
                        // Points down at rotation 0; a quarter turn anticlockwise reads
                        // as forward navigation.
                        rotation: -90
                        color: Theme.cyan
                    }

                    // Issue #801 D5: a menu, as a system card's More opens: Edit
                    // connection and Pin to Listen. It never starts Explore.
                    IconButton {
                        id: exploreMore
                        objectName: "exploreManageButton"
                        icon: "more"
                        accessibleName: qsTr("More options for Explore")
                        anchors.right: parent.right
                        // Match the axis of the 52 px play circles on the cards.
                        anchors.rightMargin: Theme.cardPadding + 2
                        anchors.top: parent.top
                        onClicked: exploreMenu.openFor()
                        z: 2
                    }
                }
            }
        }
    }

    // Once cards pass under the pinned header or the Pinned strip, a hairline
    // marks where they go, as the bottom navigation's top edge does. Under the
    // fixed Listen tabs their own hairline does, so there is one line only;
    // with the tabs in the body, this one stands aside while theirs passes
    // the edge.
    function bodyTabsLineAtEdge() {
        var line = content.y + bodyTabsSlot.y + bodyTabsSlot.height - 1 - scrollBody.contentY;
        return line > -1 && line < 1;
    }
    Rectangle {
        objectName: "listenScrollDivider"
        visible: screen.fixedTitle && !screen.tabsFixed && scrollBody.contentY > scrollBody.originY + 0.5
            && !screen.bodyTabsLineAtEdge()
        y: scrollBody.y - 1
        width: screen.supportingPane ? screen.columnX + screen.columnWidth : screen.width
        height: 1
        color: Theme.divider
    }

    ActionMenu {
        id: addSystemMenu
        objectName: "addSystemMenu"
        title: qsTr("Add a system")
        actions: [
            {text: qsTr("Set up manually"), objectName: "addSystemManual"},
            {text: qsTr("Import from RadioReference"), objectName: "addSystemImport"}
        ]
        onTriggered: function (index) {
            if (index === 0)
                screen.addSystem();
            else
                screen.importSystem();
        }
    }

    // Resolve by uid when acting: rows can move while a menu is open.
    ActionMenu {
        id: manageMenu
        objectName: "savedSystemManageSheet"
        title: systemName
        property string systemUid: ""
        property string systemName: ""

        function openFor(row, name) {
            systemUid = savedSystems.get(row).uid;
            systemName = name;
            var items = [{text: qsTr("Edit"), key: "edit", objectName: "editSavedSystemButton"}];
            if (savedSystems.siblingRows(row).length > 1)
                items.push({text: qsTr("Sites"), key: "sites", objectName: "sitesSavedSystemButton"});
            items.push(screen.pinAction("system", systemUid, "pinSavedSystemButton"));
            items.push({text: qsTr("Remove…"), key: "remove", destructive: true, objectName: "removeSavedSystemButton"});
            actions = items;
            open();
        }
        onTriggered: function (index) {
            var row = savedSystems.rowForUid(systemUid);
            if (row < 0)
                return;
            var key = actions[index].key;
            if (key === "edit")
                screen.editSystem(row);
            else if (key === "sites")
                screen.chooseSites(row);
            else if (key === "pin")
                screen.togglePin("system", systemUid, actions[index].pinned);
            else if (key === "remove")
                removeSystem.openFor(row);
            // Any other key does nothing; it used to fall through to Remove.
        }
    }

    ActionMenu {
        id: scanMenu
        objectName: "scanListManageSheet"
        property string listUid: ""
        // Issue #697: a ready list can start with the other ready lists, so
        // the choice is offered only where there is one to make.
        function canScanWithOthers(list) {
            if (list.isDraft)
                return false;
            for (var i = 0; i < scanLists.count; ++i) {
                var other = scanLists.get(i);
                if (other.uid !== list.uid && !other.isDraft)
                    return true;
            }
            return false;
        }
        function openFor(row) {
            var list = scanLists.get(row);
            listUid = list.uid;
            title = list.name;
            var items = [{text: qsTr("Edit"), key: "edit", objectName: "editScanListButton"}];
            if (canScanWithOthers(list))
                items.push({text: qsTr("Scan with other lists…"), key: "together", objectName: "scanWithOtherListsButton"});
            items.push(screen.pinAction("scan", listUid, "pinScanListButton"));
            items.push({text: qsTr("Remove…"), key: "remove", destructive: true, objectName: "removeScanListButton"});
            actions = items;
            open();
        }
        onTriggered: function (index) {
            var row = scanLists.rowForUid(listUid);
            if (row < 0)
                return;
            var key = actions[index].key;
            if (key === "edit")
                screen.editScanList(row);
            else if (key === "together")
                scanTogether.openFor(row);
            else if (key === "pin")
                screen.togglePin("scan", listUid, actions[index].pinned);
            else if (key === "remove")
                removeScanList.openFor(row);
        }
    }

    ActionMenu {
        id: exploreMenu
        objectName: "exploreManageSheet"
        title: qsTr("Explore")
        function openFor() {
            actions = [{text: qsTr("Edit connection"), key: "edit", objectName: "editExploreConnectionButton"},
                screen.pinAction("explore", "", "pinExploreButton")];
            open();
        }
        onTriggered: function (index) {
            var key = actions[index].key;
            if (key === "edit")
                screen.exploreSetup();
            else if (key === "pin")
                screen.togglePin("explore", "", actions[index].pinned);
        }
    }

    ActionMenu {
        id: searchMenu
        objectName: "signalSearchManageSheet"
        title: qsTr("Signal search")
        function openFor() {
            actions = [screen.pinAction("search", "", "pinSignalSearchButton")];
            open();
        }
        onTriggered: function (index) {
            if (actions[index].key === "pin")
                screen.togglePin("search", "", actions[index].pinned);
        }
    }

    PinnedSheet {
        id: pinnedSheet
        onStartRequested: function (kind, uid) {
            screen.startPin(kind, uid);
        }
    }

    ScanTogetherSheet {
        id: scanTogether
        onStartRequested: function (primaryUid, companionUids) {
            var row = scanLists.rowForUid(primaryUid);
            if (row >= 0)
                screen.playScanLists(row, companionUids);
        }
    }

    ConfirmDialog {
        id: removeScanList
        objectName: "removeScanListConfirm"
        property string listUid: ""
        destructive: true
        confirmText: qsTr("Remove list")
        message: qsTr("This deletes the scan list and its settings. Imported files stay in the library.")
        function openFor(row) {
            var list = scanLists.get(row);
            listUid = list.uid;
            title = qsTr("Remove %1?").arg(list.name);
            open();
        }
        onConfirmed: {
            var row = scanLists.rowForUid(listUid);
            listUid = "";
            if (row >= 0 && !scanLists.remove(row)) {
                scanListRemovalError.open();
            }
        }
        onCancelled: listUid = ""
    }
    ConfirmDialog {
        id: scanListRemovalError
        objectName: "scanListRemovalError"
        title: qsTr("Scan list removal failed")
        message: qsTr("Could not remove the scan list.")
        confirmText: qsTr("OK")
        showCancel: false
    }

    ConfirmDialog {
        id: removeSystem
        objectName: "removeSystemConfirm"

        property string systemUid: ""
        destructive: true

        function openFor(row) {
            if (row < 0)
                return;
            var system = savedSystems.get(row);
            systemUid = system.uid;
            var grouped = savedSystems.siblingRows(row).length > 1;
            title = grouped
                ? qsTr("Remove site %1 of %2?").arg(system.siteName || system.name).arg(system.name)
                : qsTr("Remove %1?").arg(system.name);
            message = grouped
                ? qsTr("This deletes the saved site and its settings. Other sites remain. Imported files stay in the library.")
                : qsTr("This deletes the saved system and its settings. Imported files stay in the library.");
            confirmText = grouped ? qsTr("Remove site") : qsTr("Remove system");
            open();
        }
        onConfirmed: {
            var row = savedSystems.rowForUid(systemUid);
            systemUid = "";
            // Named before the removal is tried: a removal that cannot be
            // saved keeps the system, and the notice says which one.
            var system = savedSystems.get(row);
            var grouped = row >= 0 && savedSystems.siblingRows(row).length > 1;
            if (row >= 0 && !savedSystems.remove(row))
                systemRemovalError.openFor(system, grouped);
        }
        onCancelled: systemUid = ""
    }
    ConfirmDialog {
        id: systemRemovalError
        objectName: "systemRemovalError"
        confirmText: qsTr("OK")
        showCancel: false
        function openFor(system, grouped) {
            title = grouped ? qsTr("Site removal failed") : qsTr("System removal failed");
            message = grouped
                ? qsTr("Could not remove site %1 of %2. It is still saved. Try again.").arg(system.siteName || system.name).arg(system.name)
                : qsTr("Could not remove %1. It is still saved. Try again.").arg(system.name);
            open();
        }
    }

    UiPanel {
        visible: screen.supportingPane
        enabled: !screen.managementSheetOpen
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        width: 280
        PointerBarrier {}
        ReadableText {
            heading: true
            id: recentTitle
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: Theme.cardPadding
            text: qsTr("Recent activity")
            color: Theme.textPrimary
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSize(20)
        }
        ListView {
            id: recentList
            objectName: "homeRecentList"
            anchors.top: recentTitle.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.topMargin: 12
            clip: true
            // Set by recentPlace below, only while the list is visible (issue #782).
            model: null
            delegate: CallRow {
                // The row's identity, for recentPlace.
                readonly property var rowUid: model.uid
                width: recentList.width
                name: model.name
                metaText: model.systemName
                rightText: model.timeText
                enc: model.enc
                emergency: model.emergency
                hasRecording: model.hasRecording
                interactive: true
                onActivated: recentDetail.open(callHistory.recordFor(model.uid))
            }
            ReadableText {
                objectName: "homeRecentEmpty"
                anchors.centerIn: parent
                width: parent.width - 32
                // The store's count, not the view's: the view holds no rows while hidden.
                visible: callHistory.count === 0
                text: qsTr("Received activity will appear here.")
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSize(14)
            }
        }
        // Hidden, the list holds no rows (issue #782): a session runs on the
        // monitor with this screen under it, and every row built for a call
        // landing there is announced to accessibility clients as hidden.
        // Shown again, it lists the calls as they are then, and a reader who
        // had scrolled down is back on the same row.
        KeepPlace {
            list: recentList
            model: callHistory
            keyOf: function (row) {
                return row.rowUid;
            }
            rowOf: function (uid) {
                return callHistory.rowForUid(uid);
            }
        }
    }
    HistoryDetailSheet {
        id: recentDetail
        objectName: "homeRecentDetailSheet"
    }
}
