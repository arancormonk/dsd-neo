// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Settings: appearance, listening, decoding, and the advanced tuner defaults
// folded shut. Every row writes straight through to the persisted preference.
Item {
    id: screen

    property bool advancedOpen: false

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // One toggle row: title, helper, switch. Used by every panel below.
    component ToggleRow: Item {
        id: toggleRow

        property string title: ""
        property string subtitle: ""
        property bool checked: false
        property bool showDivider: false
        signal toggled(bool checked)

        width: parent ? parent.width : 0
        height: 62

        Column {
            anchors.left: parent.left
            anchors.right: rowSwitch.left
            anchors.leftMargin: Theme.cardPadding
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Text {
                width: parent.width
                text: toggleRow.title
                font.family: Theme.sans
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                elide: Text.ElideRight
            }

            Text {
                width: parent.width
                visible: text.length > 0
                text: toggleRow.subtitle
                font.family: Theme.sans
                font.pixelSize: 12
                color: Theme.textSubdued
                elide: Text.ElideRight
            }
        }

        PlexSwitch {
            id: rowSwitch
            anchors.right: parent.right
            anchors.rightMargin: Theme.cardPadding
            anchors.verticalCenter: parent.verticalCenter
            checked: toggleRow.checked
            onToggled: function (state) { toggleRow.toggled(state) }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.cardPadding
            height: 1
            visible: toggleRow.showDivider
            color: Theme.divider
        }
    }

    // Numeric advanced row: label left, small mono field + unit right.
    component ValueRow: Item {
        id: valueRow

        property string title: ""
        property string unit: ""
        property alias text: valueInput.text
        property bool showDivider: true
        signal edited(string text)

        width: parent ? parent.width : 0
        height: 52

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.cardPadding
            anchors.verticalCenter: parent.verticalCenter
            text: valueRow.title
            font.family: Theme.sans
            font.pixelSize: 15
            color: Theme.textPrimary
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: Theme.cardPadding
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            TextInput {
                id: valueInput

                width: Math.max(implicitWidth, 34)
                horizontalAlignment: TextInput.AlignRight
                font.family: Theme.mono
                font.pixelSize: 14
                color: Theme.textPrimary
                selectionColor: Qt.alpha(Theme.cyan, 0.35)
                selectedTextColor: Theme.textPrimary
                onEditingFinished: valueRow.edited(text)
            }

            Text {
                visible: valueRow.unit.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: valueRow.unit
                font.family: Theme.mono
                font.pixelSize: 14
                color: Theme.textSubdued
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.cardPadding
            height: 1
            visible: valueRow.showDivider
            color: Theme.divider
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: content.height + 2 * Theme.screenPadding
        clip: true

        Column {
            id: content

            x: Theme.screenPadding
            y: Theme.screenPadding
            width: parent.width - 2 * Theme.screenPadding
            spacing: Theme.gap

            Text {
                text: qsTr("Settings")
                font.family: Theme.sans
                font.pixelSize: 24
                font.weight: Font.Bold
                font.letterSpacing: -0.24
                color: Theme.textPrimary
            }

            // APPEARANCE
            UiPanel {
                width: parent.width
                height: appearanceColumn.height + 2 * Theme.cardPadding

                Column {
                    id: appearanceColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.cardPadding
                    spacing: 12

                    MicroLabel {
                        text: qsTr("Appearance")
                    }

                    SegmentedControl {
                        width: parent.width
                        model: [qsTr("System"), qsTr("Light"), qsTr("Dark")]
                        currentIndex: prefs.appearance
                        onSelected: function (index) { prefs.appearance = index }
                    }

                    Text {
                        width: parent.width
                        visible: prefs.appearance === 0
                        text: qsTr("Follows your phone's dark mode schedule.")
                        font.family: Theme.sans
                        font.pixelSize: 12
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }
                }
            }

            // LISTENING
            UiPanel {
                width: parent.width
                height: listeningColumn.height + Theme.cardPadding + 4

                Column {
                    id: listeningColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        text: qsTr("Listening")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    Item {
                        width: parent.width
                        height: 48

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.cardPadding
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Audio output")
                            font.family: Theme.sans
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.textPrimary
                        }

                        Row {
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.cardPadding
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Speaker")
                                font.family: Theme.sans
                                font.pixelSize: 14
                                color: Theme.textSecondary
                            }

                            Caret {
                                anchors.verticalCenter: parent.verticalCenter
                                color: Theme.textSecondary
                            }
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: Theme.cardPadding
                            height: 1
                            color: Theme.divider
                        }
                    }

                    ToggleRow {
                        title: qsTr("Keep listening in background")
                        subtitle: qsTr("Shows a persistent notification")
                        checked: prefs.backgroundListening
                        showDivider: true
                        onToggled: function (state) { prefs.backgroundListening = state }
                    }

                    ToggleRow {
                        title: qsTr("Keep screen awake")
                        checked: prefs.keepScreenAwake
                        onToggled: function (state) { prefs.keepScreenAwake = state }
                    }
                }
            }

            // DECODING
            UiPanel {
                width: parent.width
                height: decodingColumn.height + Theme.cardPadding + 4

                Column {
                    id: decodingColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        text: qsTr("Decoding")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    ToggleRow {
                        title: qsTr("Skip encrypted calls")
                        subtitle: qsTr("You'd only hear noise")
                        checked: prefs.skipEncrypted
                        showDivider: true
                        onToggled: function (state) { prefs.skipEncrypted = state }
                    }

                    ToggleRow {
                        title: qsTr("Auto tuner correction")
                        subtitle: qsTr("Fixes frequency drift on long runs")
                        checked: prefs.autoPpm
                        onToggled: function (state) { prefs.autoPpm = state }
                    }
                }
            }

            // ADVANCED (collapsible)
            UiPanel {
                width: parent.width
                height: advHeader.height + (screen.advancedOpen ? advColumn.height + 8 : 0) + Theme.cardPadding
                clip: true

                Behavior on height {
                    NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                }

                Item {
                    id: advHeader

                    width: parent.width
                    height: 44

                    MicroLabel {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Advanced")
                    }

                    Caret {
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        rotation: screen.advancedOpen ? 180 : 0
                        color: Theme.textSubdued

                        Behavior on rotation {
                            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                        }
                    }

                    TapHandler {
                        onTapped: screen.advancedOpen = !screen.advancedOpen
                    }
                }

                Column {
                    id: advColumn

                    anchors.top: advHeader.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    visible: screen.advancedOpen
                    spacing: 0

                    ValueRow {
                        title: qsTr("Tuner gain")
                        unit: "dB"
                        text: String(prefs.gainDb)
                        onEdited: function (value) {
                            var parsed = parseInt(value)
                            if (!isNaN(parsed))
                                prefs.gainDb = parsed
                        }
                    }

                    ValueRow {
                        title: qsTr("PPM correction")
                        text: String(prefs.ppm)
                        onEdited: function (value) {
                            var parsed = parseInt(value)
                            if (!isNaN(parsed))
                                prefs.ppm = parsed
                        }
                    }

                    ValueRow {
                        title: qsTr("Bandwidth")
                        unit: "kHz"
                        text: String(prefs.bandwidthKhz)
                        onEdited: function (value) {
                            var parsed = parseInt(value)
                            if (!isNaN(parsed) && parsed > 0)
                                prefs.bandwidthKhz = parsed
                        }
                    }

                    ToggleRow {
                        title: qsTr("Bias tee")
                        subtitle: qsTr("Powers an external LNA")
                        checked: prefs.biasTee
                        showDivider: true
                        onToggled: function (state) { prefs.biasTee = state }
                    }

                    Item {
                        width: parent.width
                        height: 76

                        Text {
                            id: extraLabel
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.cardPadding
                            anchors.top: parent.top
                            anchors.topMargin: 10
                            text: qsTr("Extra CLI args")
                            font.family: Theme.sans
                            font.pixelSize: 15
                            color: Theme.textPrimary
                        }

                        PlexTextField {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: extraLabel.bottom
                            anchors.margins: Theme.cardPadding
                            anchors.topMargin: 6
                            height: 38
                            mono: true
                            text: prefs.extraArgs
                            placeholderText: qsTr("e.g. -C chan.csv -G group.csv")
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            // Commit on Enter/focus loss, not per keystroke: every
                            // write lands in QSettings (disk on Android).
                            onEditingFinished: prefs.extraArgs = text
                        }
                    }
                }
            }

            Text {
                width: parent.width
                topPadding: 8
                horizontalAlignment: Text.AlignHCenter
                text: "DSD-neo " + appVersionText.replace(/^v/, "") + " · GPL-3.0 · " + qsTr("open source licenses")
                font.family: Theme.mono
                font.pixelSize: 11
                color: Theme.textSubdued
                wrapMode: Text.Wrap
            }
        }
    }
}
