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

    readonly property bool dongleFailed: decoderHost && decoderHost.localDeviceFailureKind !== 0
    readonly property bool dongleReady: decoderHost && (!decoderHost.localDeviceBrokered || (decoderHost.localDeviceReady && !dongleFailed))

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // WP-D5: long diagnostics remain reachable on short screens.
    Flickable {
        objectName: "dongleScrollBody"
        id: body
        anchors.fill: parent
        clip: true
        contentHeight: bottomBlock.y + bottomBlock.height + 22

        Column {
            id: topBlock
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
                font.pixelSize: Theme.fontSize(30)
                font.weight: Font.Bold
                font.letterSpacing: -0.3
                color: Theme.textPrimary
                wrapMode: Text.Wrap
            }

            Text {
                width: parent.width
                text: qsTr("Police, fire, EMS and ham digital radio — decoded live on your phone.")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
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
                                font.pixelSize: Theme.fontSize(12)
                                color: Theme.cyan
                            }
                        }

                        Text {
                            text: modelData
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(15)
                            color: Theme.textPrimary
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
            }
        }

        Column {
            id: bottomBlock
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: Theme.screenPadding
            y: Math.max(topBlock.y + topBlock.height + 32, body.height - height - 22)
            spacing: 14

            UiPanel {
                width: parent.width
                height: dongleDetails.implicitHeight + 2 * Theme.cardPadding
                Column {
                    id: dongleDetails
                    x: Theme.cardPadding
                    y: Theme.cardPadding
                    width: parent.width - 2 * Theme.cardPadding
                    spacing: 12
                    Text {
                        width: parent.width
                        text: screen.dongleFailed ? qsTr("RTL-SDR needs attention")
                              : screen.dongleReady ? qsTr("RTL-SDR dongle connected") : qsTr("No dongle detected")
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(15)
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                    }
                    Text {
                        objectName: "dongleStatusText"
                        width: parent.width
                        text: decoderHost.localDeviceStatus || (screen.dongleReady
                              ? "RTL2832U · USB-OTG · " + qsTr("ready")
                              : qsTr("plug one in, then tap Connect"))
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(14)
                        color: Theme.textSecondary
                    }
                    OutlineButton {
                        objectName: "dongleRetry"
                        visible: !screen.dongleReady && decoderHost && decoderHost.localDeviceBrokered
                        width: 110
                        text: screen.dongleFailed ? qsTr("Retry") : qsTr("Connect")
                        enabled: !decoderHost.sessionActive
                        onClicked: decoderHost.requestLocalDeviceAccess()
                    }
                }
            }

            Text {
                width: parent.width
                text: qsTr("Long sessions? A powered OTG hub keeps the dongle fed and your battery out of it.")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(12)
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
                font.pixelSize: Theme.fontSize(14)
                color: Theme.textSecondary

                TapHandler {
                    onTapped: screen.networkSource()
                }
            }
        }
    }
}
