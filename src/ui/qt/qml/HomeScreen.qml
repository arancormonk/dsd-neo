// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Home: the saved systems, one tap to listen. The most recently heard system
// carries the gradient play button; everything else stays outlined.
Item {
    id: screen

    signal addScanList()
    signal playScanList(int row)
    signal editScanList(int row)
    signal addSystem()
    signal chooseSites(int row)
    property int siteRevision: 0
    Connections { target: savedSystems; function onSitesChanged() { screen.siteRevision++ } }
    Connections { target: importedFiles; function onCountChanged() { screen.siteRevision++ } }
    function needsSiteRefresh(row) {
        var sys = savedSystems.get(row)
        if (sys.rrSid > 0 && sys.rrSiteId > 0) return false
        for (var path of [sys.chanCsvPath, sys.groupCsvPath]) {
            var libraryRow = importedFiles.rowForPath(path || "")
            if (libraryRow >= 0 && importedFiles.get(libraryRow).origin === "radioreference") return true
        }
        return false
    }
    signal playSystem(int row)
    signal editSystem(int row)
    signal networkSource()
    // Tap starts exploring with what was used last; long-press changes it. Same
    // split as a system card, where the face plays and the press manages.
    signal explore()
    signal exploreSetup()

    // Re-derives "Heard n minutes ago" once a minute so rows do not go stale.
    property int heardTick: 0

    Timer {
        interval: 60000
        running: screen.visible
        repeat: true
        onTriggered: screen.heardTick++
    }

    readonly property bool showDonglePill: decoderHost && decoderHost.localDeviceBrokered
    readonly property bool dongleReady: decoderHost && decoderHost.localDeviceReady

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Flickable {
        anchors.fill: parent
        contentHeight: content.height + 2 * Theme.screenPadding
        clip: true
        // TapHandlers never take exclusive grabs, so a tap on a manage-menu button
        // would otherwise also reach whatever sits under the overlay — observed as
        // "Edit this system" opening the add wizard through the card list.
        enabled: !manageMenu.visible

        Column {
            id: content

            x: Theme.screenPadding
            y: Theme.screenPadding
            width: parent.width - 2 * Theme.screenPadding
            spacing: Theme.gap

            Item {
                width: parent.width
                height: 44

                Text {
                    text: qsTr("Listen")
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(24)
                    font.weight: Font.Bold
                    font.letterSpacing: -0.24
                    color: Theme.textPrimary
                }

                Rectangle {
                    visible: screen.showDonglePill
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: pillRow.implicitWidth + 24
                    height: 30
                    radius: Theme.radiusButton
                    color: Theme.panel
                    border.width: 1
                    border.color: Theme.panelBorder

                    Row {
                        id: pillRow
                        anchors.centerIn: parent
                        spacing: 7

                        Rectangle {
                            width: 7
                            height: 7
                            radius: 3.5
                            anchors.verticalCenter: parent.verticalCenter
                            color: screen.dongleReady ? Theme.cyan : Theme.textSubdued
                        }

                        Text {
                            text: screen.dongleReady ? qsTr("DONGLE READY") : qsTr("NO DONGLE")
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(11)
                            font.letterSpacing: 1.4
                            color: screen.dongleReady ? Theme.textPrimary : Theme.textSubdued
                        }
                    }
                }
            }

            // A start that died reads as "nothing happened" without this.
            UiPanel {
                width: parent.width
                visible: failureBanner.text.length > 0
                height: visible ? failureBanner.implicitHeight + 26 : 0
                border.color: Theme.encBorder

                property alias text: failureBanner.text

                Text {
                    id: failureBanner
                    anchors.left: parent.left
                    anchors.right: dismiss.left
                    anchors.margins: Theme.cardPadding
                    anchors.verticalCenter: parent.verticalCenter
                    text: (typeof mainRoot !== "undefined" && mainRoot.showFailure) ? mainRoot.failureText : ""
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    color: Theme.textPrimary
                }

                Text {
                    id: dismiss
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.cardPadding
                    anchors.verticalCenter: parent.verticalCenter
                    text: "✕"
                    font.pixelSize: Theme.fontSize(15)
                    color: Theme.textSubdued

                    TapHandler {
                        onTapped: mainRoot.dismissedFailure = mainRoot.failureText
                    }
                }
            }

            MicroLabel {
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
                    property bool refreshHint: (screen.siteRevision, screen.needsSiteRefresh(index))

                    Column {
                        anchors.left: parent.left
                        anchors.right: play.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: card.refreshHint ? -20 : -10
                        spacing: 4

                        Text {
                            width: parent.width
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
                            text: ((card.encKeyType.length > 0 || card.keyCsvPath.length > 0)
                                   ? qsTr("key configured") + " · " : "")
                                  + (screen.heardTick, Util.heardText(card.lastHeard))
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
                        objectName: "homeSiteChooserButton"
                        width: implicitWidth
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 4
                        text: qsTr("%1 sites ›").arg(card.siblings.length)
                        visible: card.siblings.length > 0
                        onClicked: screen.chooseSites(card.index)
                    }
                    PlayCircle {
                        id: play
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        featured: index === savedSystems.mostRecentRow
                        enabled: !mainRoot.transitioning
                        onClicked: card.siblings.length > 0 ? screen.chooseSites(card.index) : screen.playSystem(card.index)
                    }

                    // Long-press manages the card: the design keeps card faces clean,
                    // so destructive actions hide behind the press.
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onLongPressed: manageMenu.openFor(card.index, card.name)
                    }
                }
            }

            DashedActionButton {
                width: parent.width
                text: qsTr("+ Add a system")
                onClicked: screen.addSystem()
            }

            // WP-S1: same play/edit gestures as saved systems.
            MicroLabel { text: qsTr("Scan lists") }
            Repeater {
                model: scanLists
                ScanListCard {
                    required property int index
                    required property string name
                    required property var entries
                    width: content.width
                    listName: name
                    entryCount: entries.length
                    onPlay: screen.playScanList(index)
                    onEdit: screen.editScanList(index)
                }
            }
            DashedActionButton {
                width: parent.width
                text: qsTr("+ Add a scan list")
                onClicked: screen.addScanList()
            }

            MicroLabel {
                text: qsTr("Or go looking")
            }

            // Deliberately not a system card: there is nothing saved here, nothing
            // named, and nothing to come back to. It is a door, so it carries a
            // chevron rather than a play button, and its meta line states what the
            // tap will actually do so that starting is never a surprise.
            UiPanel {
                id: exploreCard

                objectName: "exploreCard"
                width: content.width
                height: 78

                readonly property string sourceLabel: prefs.exploreSourceType === "rtltcp" ? qsTr("RTL-TCP")
                                                                                           : qsTr("USB dongle")
                readonly property bool configured: prefs.exploreSourceType.length > 0

                Column {
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
                        text: exploreCard.configured
                              ? qsTr("%1 · from %2 MHz").arg(exploreCard.sourceLabel).arg(prefs.exploreFreqMhz)
                              : qsTr("Tune around and find what is on the air")
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
                    anchors.verticalCenter: parent.verticalCenter
                    // Larger than the default: at the size a filter pill uses it
                    // reads as a speck against a full-width card, and this is the
                    // only thing saying the card goes somewhere.
                    width: 13
                    height: 9
                    // Points down at rotation 0; a quarter turn anticlockwise reads
                    // as forward, matching the "Network or file source ›" affordance.
                    rotation: -90
                    color: Theme.cyan
                }

                TapHandler {
                    enabled: !mainRoot.transitioning
                    // Nothing remembered means nothing to start from, so the first
                    // tap asks rather than guessing at a radio and a frequency.
                    onTapped: exploreCard.configured ? screen.explore() : screen.exploreSetup()
                    onLongPressed: screen.exploreSetup()
                }
            }

            Item {
                width: parent.width
                height: 26
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Network or file source ›")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSubdued

                TapHandler {
                    onTapped: screen.networkSource()
                }
            }
        }
    }

    // Remove-a-system sheet, reached by long-pressing a card.
    Rectangle {
        id: manageMenu

        property int row: -1
        property string systemName: ""

        function openFor(row, name) {
            manageMenu.row = row
            manageMenu.systemName = name
            visible = true
        }

        anchors.fill: parent
        visible: false
        color: Qt.alpha("#000000", 0.5)

        TapHandler {
            onTapped: manageMenu.visible = false
        }

        UiPanel {
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.screenPadding
            height: menuColumn.height + 2 * Theme.cardPadding

            Column {
                id: menuColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Theme.cardPadding
                spacing: 12

                Text {
                    width: parent.width
                    text: manageMenu.systemName
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(17)
                    font.weight: Font.Bold
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                }

                OutlineButton {
                    width: parent.width
                    text: qsTr("Edit this system")
                    onClicked: {
                        manageMenu.visible = false
                        screen.editSystem(manageMenu.row)
                    }
                }

                OutlineButton {
                    width: parent.width
                    text: qsTr("Remove this system")
                    onClicked: {
                        savedSystems.remove(manageMenu.row)
                        manageMenu.visible = false
                    }
                }

                OutlineButton {
                    width: parent.width
                    text: qsTr("Cancel")
                    onClicked: manageMenu.visible = false
                }
            }
        }
    }
}
