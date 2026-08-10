// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Two answers stand between someone and a live band: what the radio is, and
// where to point it first. Everything else about exploring is decided on the
// spectrum itself, so this screen asks only those two and gets out of the way.
//
// It is not the add-system wizard with steps removed: there is nothing to name,
// nothing to decode-select (exploring listens for anything), and nothing saved
// at the end. A session that finds something worth keeping goes to the wizard
// afterwards, with the answer already filled in.
Item {
    id: screen

    objectName: "exploreSetupScreen"

    signal closed()
    signal start(string sourceType, string host, int port, string freqMhz)

    property string sourceType: "usb"
    property alias hostText: hostField.text
    property alias portText: portField.text
    property alias freqText: freqField.text

    readonly property bool needsHost: sourceType === "rtltcp"

    /** Fill the fields from what the last explore used, or from sane firsts. */
    function reset(prefSource, prefHost, prefPort, prefFreqMhz) {
        screen.sourceType = (prefSource === "rtltcp") ? "rtltcp" : "usb"
        hostField.text = prefHost && prefHost.length > 0 ? prefHost : "192.168.1.10"
        portField.text = String(prefPort > 0 ? prefPort : 1234)
        // 800 MHz is where most of the traffic this app decodes lives, so an
        // unconfigured first run opens on something rather than on dead air.
        freqField.text = prefFreqMhz && prefFreqMhz.length > 0 ? prefFreqMhz : "855.0000"
    }

    function portValid() {
        var p = parseInt(portText, 10)
        return !isNaN(p) && p >= 1 && p <= 65535
    }

    function ready() {
        if (needsHost && (hostText.length === 0 || !portValid()))
            return false
        return sessionArgs.freqValid(freqText)
    }

    function submit() {
        if (!ready())
            return
        screen.start(screen.sourceType, screen.needsHost ? screen.hostText : "",
                     screen.needsHost ? parseInt(screen.portText, 10) : 0, screen.freqText.trim())
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Item {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 46

        Text {
            id: back

            objectName: "exploreSetupBack"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "‹"
            font.pixelSize: 28
            color: Theme.textSecondary

            TapHandler {
                onTapped: screen.closed()
            }
        }

        Text {
            anchors.left: back.right
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Explore")
            font.family: Theme.sans
            font.pixelSize: 22
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
        }
    }

    Flickable {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: startButton.top
        anchors.topMargin: 14
        anchors.bottomMargin: 14
        contentHeight: content.height
        clip: true

        Column {
            id: content

            x: Theme.screenPadding
            width: parent.width - 2 * Theme.screenPadding
            spacing: Theme.gap

            Text {
                width: parent.width
                text: qsTr("Which radio?")
                font.family: Theme.sans
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }

            // Only the two tuner sources appear. A UDP or TCP audio feed and a
            // file have no front end to point anywhere, so offering them here
            // would be offering a session where every control on the next screen
            // is dead.
            Flow {
                width: parent.width
                spacing: 10

                Repeater {
                    model: [
                        { label: qsTr("USB dongle"), key: "usb" },
                        { label: qsTr("RTL-TCP"), key: "rtltcp" }
                    ]

                    DecodeChip {
                        required property var modelData

                        objectName: "exploreSource_" + modelData.key
                        text: modelData.label
                        selected: screen.sourceType === modelData.key
                        onClicked: screen.sourceType = modelData.key
                    }
                }
            }

            Column {
                width: parent.width
                visible: screen.needsHost
                spacing: 10

                Text {
                    text: qsTr("Host")
                    font.family: Theme.sans
                    font.pixelSize: 13
                    color: Theme.textSecondary
                }

                PlexTextField {
                    id: hostField

                    width: parent.width
                    mono: true
                    text: "192.168.1.10"
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                }

                Text {
                    text: qsTr("Port")
                    font.family: Theme.sans
                    font.pixelSize: 13
                    color: Theme.textSecondary
                }

                PlexTextField {
                    id: portField

                    width: parent.width
                    mono: true
                    text: "1234"
                    inputMethodHints: Qt.ImhDigitsOnly
                    input.validator: IntValidator {
                        bottom: 1
                        top: 65535
                    }
                }
            }

            Text {
                width: parent.width
                text: qsTr("Start where?")
                font.family: Theme.sans
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }

            UiPanel {
                width: parent.width
                height: freqColumn.height + 2 * Theme.cardPadding

                Column {
                    id: freqColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.cardPadding
                    spacing: 8

                    FrequencyField {
                        id: freqField

                        width: parent.width
                        fieldObjectName: "exploreStartFrequency"
                        text: "855.0000"
                    }

                    // For anyone who does not know a local frequency, this is the
                    // whole answer to "where do I even begin".
                    Flow {
                        width: parent.width
                        spacing: 8

                        Repeater {
                            model: Util.BANDS

                            FilterPill {
                                required property var modelData

                                objectName: "exploreBand_" + modelData.label
                                text: modelData.label
                                // The caret means "this opens a menu" on the history
                                // screen; these jump straight to a frequency.
                                caret: false
                                active: {
                                    var hz = parseFloat(screen.freqText) * 1.0e6
                                    return !isNaN(hz) && hz >= modelData.low && hz < modelData.high
                                }
                                onClicked: freqField.text = Util.mhzText(modelData.start)
                            }
                        }
                    }

                    Text {
                        width: parent.width
                        text: qsTr("Anywhere is fine — you can move around once the band is on screen.")
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    GradientButton {
        id: startButton

        objectName: "exploreStartButton"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: 22
        text: qsTr("Start exploring")
        enabled: screen.ready()
        onClicked: screen.submit()
    }
}
