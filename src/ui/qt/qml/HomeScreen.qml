// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Home: the saved systems, one tap to listen. The most recently heard system
// carries the gradient play button; everything else stays outlined.
Item {
    id: screen

    signal addSystem()
    signal playSystem(int row)
    signal editSystem(int row)
    signal networkSource()

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
                    font.pixelSize: 24
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
                            font.pixelSize: 11
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
                    font.pixelSize: 14
                    color: Theme.textPrimary
                }

                Text {
                    id: dismiss
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.cardPadding
                    anchors.verticalCenter: parent.verticalCenter
                    text: "✕"
                    font.pixelSize: 15
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
                    required property double lastHeard

                    width: content.width
                    height: 92

                    Column {
                        anchors.left: parent.left
                        anchors.right: play.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4

                        Text {
                            width: parent.width
                            text: card.name
                            font.family: Theme.sans
                            font.pixelSize: 17
                            font.weight: Font.Bold
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: Util.systemMeta(card)
                            font.family: Theme.mono
                            font.pixelSize: 12
                            color: Theme.textSubdued
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            // heardTick forces the minute-by-minute refresh.
                            text: (screen.heardTick, Util.heardText(card.lastHeard))
                            font.family: Theme.sans
                            font.pixelSize: 13
                            color: Theme.textSecondary
                            elide: Text.ElideRight
                        }
                    }

                    PlayCircle {
                        id: play
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        featured: index === savedSystems.mostRecentRow
                        enabled: !mainRoot.transitioning
                        onClicked: screen.playSystem(card.index)
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

            Item {
                width: parent.width
                height: 26
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Network or file source ›")
                font.family: Theme.sans
                font.pixelSize: 13
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
                    font.pixelSize: 17
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
