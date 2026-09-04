// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// First run: from install to listening in three numbered steps, with the dongle's
// live status at the bottom. Everything routes through two exits — "Get started"
// and the network-source escape hatch.
Item {
    id: screen

    signal getStarted()
    signal networkSource()

    readonly property bool dongleReady: decoderHost && (!decoderHost.localDeviceBrokered || decoderHost.localDeviceReady)

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.screenPadding
        anchors.topMargin: 64
        spacing: 24

        LogoMark {}

        Text {
            width: parent.width
            text: qsTr("Hear your local airwaves")
            font.family: Theme.sans
            font.pixelSize: 30
            font.weight: Font.Bold
            font.letterSpacing: -0.3
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }

        Text {
            width: parent.width
            text: qsTr("Police, fire, EMS and ham digital radio — decoded live on your phone.")
            font.family: Theme.sans
            font.pixelSize: 15
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }

        Column {
            width: parent.width
            spacing: 16

            Repeater {
                model: [
                    qsTr("Plug an RTL-SDR dongle into USB"),
                    qsTr("Pick a system near you"),
                    qsTr("Listen")
                ]

                Row {
                    required property int index
                    required property var modelData

                    spacing: 14

                    Rectangle {
                        width: 26
                        height: 26
                        radius: 13
                        color: "transparent"
                        border.width: 1
                        border.color: Theme.controlBorder
                        anchors.verticalCenter: parent.verticalCenter

                        Text {
                            anchors.centerIn: parent
                            text: index + 1
                            font.family: Theme.mono
                            font.pixelSize: 12
                            color: Theme.cyan
                        }
                    }

                    Text {
                        text: modelData
                        font.family: Theme.sans
                        font.pixelSize: 15
                        color: Theme.textPrimary
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: 22
        spacing: 14

        UiPanel {
            width: parent.width
            height: 68

            Row {
                anchors.left: parent.left
                anchors.leftMargin: Theme.cardPadding
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12

                Item {
                    width: 10
                    height: 10
                    anchors.verticalCenter: parent.verticalCenter

                    // The glow: a soft halo behind the live dot. Light mode goes without.
                    Rectangle {
                        anchors.centerIn: parent
                        width: 22
                        height: 22
                        radius: 11
                        visible: Theme.dark && screen.dongleReady
                        color: Qt.alpha(Theme.cyan, 0.25)
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: 10
                        height: 10
                        radius: 5
                        color: screen.dongleReady ? Theme.cyan : Theme.textSubdued
                    }
                }

                Column {
                    spacing: 2
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        text: screen.dongleReady ? qsTr("RTL-SDR dongle connected") : qsTr("No dongle detected")
                        font.family: Theme.sans
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                    }

                    Text {
                        text: screen.dongleReady
                              ? "RTL2832U · USB-OTG · " + qsTr("ready")
                              : qsTr("plug one in, then tap Connect")
                        font.family: Theme.mono
                        font.pixelSize: 12
                        color: Theme.textSubdued
                    }
                }
            }

            OutlineButton {
                visible: !screen.dongleReady && decoderHost && decoderHost.localDeviceBrokered
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 96
                height: 38
                text: qsTr("Connect")
                onClicked: decoderHost.requestLocalDeviceAccess()
            }
        }

        Text {
            width: parent.width
            text: qsTr("Long sessions? A powered OTG hub keeps the dongle fed and your battery out of it.")
            font.family: Theme.sans
            font.pixelSize: 12
            color: Theme.textSubdued
            wrapMode: Text.Wrap
        }

        GradientButton {
            width: parent.width
            text: qsTr("Get started")
            onClicked: screen.getStarted()
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("I use a network source instead")
            font.family: Theme.sans
            font.pixelSize: 14
            color: Theme.textSecondary

            TapHandler {
                onTapped: screen.networkSource()
            }
        }
    }
}
