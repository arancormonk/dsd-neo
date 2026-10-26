// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Home: saved systems and scan lists, with consistent listen and manage actions.
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
    Connections {
        target: importedFiles
        function onCountChanged() {
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
        || removeSystem.visible || removeScanList.visible || scanListRemovalError.visible

    // #647 WP6: where the host brokers USB access the title and the receiver
    // pill are pinned above the scrolling body, so the receiver's state and its
    // Connect or Retry never scroll away. The desktop opens its receiver itself
    // and keeps the title in the body, as before.
    readonly property bool pinnedHeader: !!decoderHost && decoderHost.localDeviceBrokered === true
    readonly property bool dongleReady: pinnedHeader && deviceStatus.state === "ready"
    readonly property bool showDeviceDetails: pinnedHeader && localDeviceRelevant && !dongleReady && !(failure.message || "").length
    // The content column, which the pinned header lines up with.
    readonly property real columnWidth: supportingPane ? width - 280 - 3 * Theme.screenPadding : Math.min(Theme.formWidth, width - 2 * Theme.screenPadding)
    readonly property real columnX: supportingPane ? Theme.screenPadding : (width - columnWidth) / 2
    readonly property int headerClearance: 6

    // B5: a session's ending or failure is reported at the top of the body.
    function revealTop() {
        scrollBody.contentY = scrollBody.originY;
    }

    DeviceStatus {
        id: deviceStatus
        host: decoderHost
        requestedSource: screen.requestedSource
    }

    component ListenTitle: Text {
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
        visible: screen.pinnedHeader
        enabled: !screen.managementSheetOpen
        x: screen.columnX
        y: Theme.screenPadding
        width: screen.columnWidth
        // One row while the title and the whole pill fit side by side; otherwise
        // the pill drops under the title, left-aligned, and its label wraps at
        // spaces rather than eliding.
        readonly property bool stacked: headerTitle.implicitWidth + Theme.gap + dongleStatusPill.implicitWidth > width
        readonly property real titleHeight: Math.max(44, headerTitle.implicitHeight)
        // Keyed on the host, not on `visible`: Main hides Home on the other
        // tabs, and a collapsing header would move the body's scroll position.
        height: !screen.pinnedHeader ? 0 : stacked ? titleHeight + 4 + dongleStatusPill.height : Math.max(titleHeight, dongleStatusPill.height)

        ListenTitle {
            id: headerTitle
            objectName: "listenHeaderTitle"
            y: listenHeader.stacked ? (listenHeader.titleHeight - height) / 2 : (listenHeader.height - height) / 2
        }

        Rectangle {
            id: dongleStatusPill

            objectName: "dongleStatusPill"
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
            activeFocusOnTab: visible && navigationAllowed
            Accessible.role: deviceStatus.canAct ? Accessible.Button : Accessible.StaticText
            Accessible.name: deviceStatus.accessibleName
            Accessible.description: deviceStatus.detailText
            Accessible.focusable: true
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

    PlexFlickable {
        id: scrollBody

        objectName: "dongleScrollBody"
        anchors.fill: parent
        // Pinned, the body is clipped a few pixels below the header, so cards
        // scroll away short of the pill; the rest of the header's gap to the
        // first card stays at the body's top, and nothing moves at rest.
        anchors.topMargin: screen.pinnedHeader ? listenHeader.y + listenHeader.height + screen.headerClearance : 0
        contentHeight: content.y + content.height + Theme.screenPadding
        clip: true
        // Menu rows use passive tap grabs, so a tap on a manage-menu button
        // would otherwise also reach whatever sits under the overlay — observed as
        // "Edit this system" opening the add wizard through the card list.
        enabled: !screen.managementSheetOpen

        Column {
            id: content

            x: screen.columnX
            y: screen.pinnedHeader ? Theme.gap - screen.headerClearance : Theme.screenPadding
            width: screen.columnWidth
            spacing: Theme.gap

            Item {
                visible: !screen.pinnedHeader
                width: parent.width
                height: 44

                ListenTitle {
                    objectName: "listenTitle"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                }
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
                    Text {
                        objectName: "dongleStatusText"
                        width: parent.width
                        text: deviceStatus.detailText
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
                width: parent.width
                visible: screen.completionMessage.length > 0
                height: completionBody.implicitHeight + 2 * Theme.cardPadding
                Column {
                    id: completionBody
                    x: Theme.cardPadding
                    y: Theme.cardPadding
                    width: parent.width - 2 * Theme.cardPadding
                    spacing: 10
                    Text {
                        width: parent.width
                        text: screen.completionMessage
                        wrapMode: Text.Wrap
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSize(15)
                    }
                    OutlineButton {
                        width: parent.width
                        text: qsTr("Play again")
                        enabled: !decoderHost.sessionActive
                        onClicked: screen.replayAgain()
                    }
                }
            }

            // Keep creation within reach regardless of either collection's size.
            Grid {
                id: addActions
                width: parent.width
                spacing: Theme.gap
                // Both equal cells must fit the wider complete label. Measure
                // the actual font so larger text and translations stack early.
                columns: width >= 2 * Math.max(addSystemAction.implicitWidth, addScanListAction.implicitWidth) + spacing ? 2 : 1
                readonly property real cellWidth: Math.floor((width - (columns - 1) * spacing) / columns)

                Column {
                    width: addActions.cellWidth
                    spacing: 6
                    DashedActionButton {
                        id: addSystemAction
                        objectName: "addSystemButton"
                        width: parent.width
                        text: qsTr("+ Add a system")
                        onClicked: radioReference.available ? addSystemMenu.open() : screen.addSystem()
                    }
                    Text {
                        objectName: "addSystemHint"
                        width: parent.width
                        text: qsTr("USB, Airspy, network or file")
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                    }
                }
                DashedActionButton {
                    id: addScanListAction
                    objectName: "addScanListButton"
                    width: addActions.cellWidth
                    text: qsTr("+ Add a scan list")
                    onClicked: screen.addScanList()
                }
            }

            MicroLabel {
                objectName: "savedSystemsHeading"
                visible: savedSystems.count > 0
                text: qsTr("Saved systems")
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
                    visible: siblings.length === 0 || siblings[0] === index
                    height: visible ? (refreshHint ? 154 : 122) : 0
                    readonly property var playableSites: siblings.filter(function (row) {
                        return !savedSystems.get(row).avoidSite;
                    })
                    property bool refreshHint: (screen.siteRevision, screen.needsSiteRefresh(index))

                    Column {
                        anchors.left: parent.left
                        anchors.right: siteButton.visible ? siteButton.left : play.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: card.refreshHint ? -20 : -10
                        spacing: 4

                        Text {
                            width: parent.width
                            objectName: "savedSystemTitle"
                            text: card.name
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(17)
                            font.weight: Font.Bold
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: Util.systemMeta(card)
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(12)
                            color: Theme.textSubdued
                            elide: Text.ElideRight
                        }

                        Text {
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

                    Text {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: Theme.cardPadding
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
                        anchors.rightMargin: play.visible ? Theme.cardPadding + play.width + 8 : Theme.cardPadding
                        text: card.siblings.length === 1 ? qsTr("1 site") : qsTr("%1 sites").arg(card.siblings.length)
                        visible: card.siblings.length > 0
                        onClicked: screen.chooseSites(card.index)
                    }
                    PlayCircle {
                        id: play
                        accessibleName: card.sourceType === "file" ? qsTr("Play %1").arg(card.name) : qsTr("Listen to %1").arg(card.name)
                        objectName: "savedSystemPlay"
                        visible: card.siblings.length === 0 || card.playableSites.length === 1
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.cardPadding
                        anchors.top: parent.top
                        anchors.topMargin: Theme.minimumTouchSize + 6
                        enabled: !decoderHost.transitioning
                        onClicked: screen.playSystem(card.playableSites.length === 1 ? card.playableSites[0] : card.index)
                    }

                    IconButton {
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

            // WP-S1: same play/edit gestures as saved systems.
            MicroLabel {
                objectName: "scanListsHeading"
                visible: scanLists.count > 0
                text: qsTr("Scan lists")
            }
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
                        var changed = importedFiles.count;
                        if (targetSource !== "csv") return entries.length;
                        var row = importedFiles.rowForPath(targetsCsvPath || "");
                        return row >= 0 ? importedFiles.get(row).accepted : 0;
                    }
                    onPlay: screen.playScanList(index)
                    onEdit: scanMenu.openFor(index)
                }
            }
            MicroLabel {
                text: qsTr("Or go looking")
            }
            OutlineButton {
                objectName: "homeSignalSearch"
                width: parent.width
                text: qsTr("Signal search")
                accessibleName: qsTr("Signal search across frequency ranges")
                onClicked: screen.signalSearch()
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

                IconButton {
                    objectName: "exploreManageButton"
                    icon: "more"
                    accessibleName: qsTr("Edit Explore connection")
                    anchors.right: parent.right
                    // Match the axis of the 52 px play circles above.
                    anchors.rightMargin: Theme.cardPadding + 2
                    anchors.top: parent.top
                    onClicked: screen.exploreSetup()
                    z: 2
                }
                readonly property string sourceLabel: screen.exploreSource === "rtltcp" ? qsTr("RTL-TCP") : screen.exploreSource === "airspy" ? qsTr("Airspy") : qsTr("USB dongle")
                readonly property bool configured: prefs.exploreSourceType.length > 0

                Column {
                    id: exploreLabels
                    anchors.left: parent.left
                    anchors.right: exploreCaret.left
                    anchors.leftMargin: Theme.cardPadding
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4

                    Text {
                        width: parent.width
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
                        text: exploreCard.configured ? qsTr("%1 · from %2 MHz").arg(exploreCard.sourceLabel).arg(prefs.exploreFreqMhz) : qsTr("Tune around and find what is on the air")
                        font.family: exploreCard.configured ? Theme.mono : Theme.sans
                        font.pixelSize: Theme.fontSize(exploreCard.configured ? 12 : 13)
                        color: Theme.textSubdued
                        elide: Text.ElideRight
                    }
                }

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

                TapHandler {
                    enabled: !decoderHost.transitioning
                    // Nothing remembered means nothing to start from, so the first
                    // tap asks rather than guessing at a radio and a frequency.
                    onTapped: exploreCard.configured ? screen.explore() : screen.exploreSetup()
                    onLongPressed: screen.exploreSetup()
                }
            }
        }
    }

    // Once cards pass under the pinned header, a hairline marks where they go,
    // as the bottom navigation's top edge does.
    Rectangle {
        visible: screen.pinnedHeader && scrollBody.contentY > scrollBody.originY + 0.5
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
            else
                removeSystem.openFor(row);
        }
    }

    ActionMenu {
        id: scanMenu
        objectName: "scanListManageSheet"
        property string listUid: ""
        actions: [
            {text: qsTr("Edit"), objectName: "editScanListButton"},
            {text: qsTr("Remove…"), destructive: true, objectName: "removeScanListButton"}
        ]
        function openFor(row) {
            var list = scanLists.get(row);
            listUid = list.uid;
            title = list.name;
            open();
        }
        onTriggered: function (index) {
            var row = scanLists.rowForUid(listUid);
            if (row < 0)
                return;
            if (index === 0)
                screen.editScanList(row);
            else
                removeScanList.openFor(row);
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
            if (row >= 0)
                savedSystems.remove(row);
        }
        onCancelled: systemUid = ""
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
        Text {
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
            anchors.top: recentTitle.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.topMargin: 12
            clip: true
            model: callHistory
            delegate: CallRow {
                width: recentList.width
                name: model.name
                metaText: model.systemName
                rightText: model.timeText
                enc: model.enc
                emergency: model.emergency
                interactive: true
                onActivated: recentDetail.open({
                    name: model.name,
                    when: model.when,
                    systemName: model.systemName,
                    systemUid: model.systemUid,
                    channel: model.channel,
                    tg: model.tg,
                    src: model.src,
                    sourceName: model.srcName,
                    enc: model.enc,
                    emergency: model.emergency,
                    detail: model.detail,
                    durationSecs: model.durationSecs,
                    freqHz: model.freqHz,
                    accessCode: model.accessCode,
                    accessCodeText: model.accessCodeText
                })
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - 32
                visible: recentList.count === 0
                text: qsTr("Received activity will appear here.")
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSize(14)
            }
        }
    }
    HistoryDetailSheet {
        id: recentDetail
        objectName: "homeRecentDetailSheet"
    }
}
