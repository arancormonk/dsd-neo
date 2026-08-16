// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Settings: appearance, listening, decoding, and the advanced tuner defaults
// folded shut. Every row writes straight through to the persisted preference.
Item {
    id: screen

    property bool advancedOpen: false

    signal openImports()
    signal openRadioReference()

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
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
                        showDivider: decoderHost.keepScreenAwakeSupported
                        onToggled: function (state) { prefs.backgroundListening = state }
                    }

                    ToggleRow {
                        // Hidden where the host cannot honor it: a switch that
                        // persists but changes nothing reads as broken.
                        visible: decoderHost.keepScreenAwakeSupported
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

            // TRUNKING DATA
            UiPanel {
                width: parent.width
                height: importsColumn.height + Theme.cardPadding + 4

                Column {
                    id: importsColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        text: qsTr("Trunking data")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    DisclosureRow {
                        title: qsTr("Imported files")
                        subtitle: qsTr("Channel maps, talkgroups, and keys")
                        onTapped: screen.openImports()
                    }
                }
            }

            // RADIOREFERENCE ACCOUNT
            // The username and application key persist; the password never does
            // — it is asked for once per app session on the import screen.
            UiPanel {
                width: parent.width
                visible: radioReference.available
                height: rrColumn.height + Theme.cardPadding + 4

                Column {
                    id: rrColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        text: qsTr("RadioReference account")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    DisclosureRow {
                        title: qsTr("Import a system")
                        subtitle: qsTr("Talkgroups and channel maps from the online database")
                        showDivider: true
                        onTapped: screen.openRadioReference()
                    }

                    Item {
                        width: parent.width
                        height: 76

                        Text {
                            id: rrUserLabel
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.cardPadding
                            anchors.top: parent.top
                            anchors.topMargin: 10
                            text: qsTr("Username")
                            font.family: Theme.sans
                            font.pixelSize: 15
                            color: Theme.textPrimary
                        }

                        PlexTextField {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: rrUserLabel.bottom
                            anchors.margins: Theme.cardPadding
                            anchors.topMargin: 6
                            height: 38
                            text: prefs.rrUsername
                            placeholderText: qsTr("radioreference.com username")
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            // Commit on Enter/focus loss, not per keystroke: every
                            // write lands in QSettings (disk on Android).
                            onEditingFinished: prefs.rrUsername = text
                        }
                    }

                    Item {
                        width: parent.width
                        height: 76

                        Text {
                            id: rrKeyLabel
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.cardPadding
                            anchors.top: parent.top
                            anchors.topMargin: 10
                            text: qsTr("Application key")
                            font.family: Theme.sans
                            font.pixelSize: 15
                            color: Theme.textPrimary
                        }

                        PlexTextField {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: rrKeyLabel.bottom
                            anchors.margins: Theme.cardPadding
                            anchors.topMargin: 6
                            height: 38
                            mono: true
                            text: prefs.rrAppKey
                            placeholderText: qsTr("leave empty to use this build's key")
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            onEditingFinished: prefs.rrAppKey = text
                        }
                    }

                    Text {
                        width: parent.width
                        leftPadding: Theme.cardPadding
                        rightPadding: Theme.cardPadding
                        bottomPadding: 6
                        text: qsTr("The password is asked for once per app session and is never saved. A RadioReference premium subscription is required.")
                        font.family: Theme.sans
                        font.pixelSize: 12
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
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
