// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The live session: who you are hearing now, the calls you just heard, and one
// way out. Replaces the home content while a session is active.
Item {
    id: screen

    // The saved-system map the session was started from (may be null for a
    // network/file quick start).
    property var system: null
    property string systemName: system ? system.name : qsTr("Listening")

    // Which slot the hero shows: an active call wins, then a recently ended one.
    readonly property int heroSlot: {
        if (!metrics)
            return 0
        if (metrics.slot1CallState === 2)
            return 1
        if (metrics.slot2CallState === 2)
            return 2
        if (metrics.slot1CallState === 3)
            return 1
        if (metrics.slot2CallState === 3)
            return 2
        return 0
    }
    readonly property bool heroActive: heroSlot === 1 ? metrics.slot1CallState === 2
                                                      : heroSlot === 2 ? metrics.slot2CallState === 2 : false
    readonly property string heroName: heroSlot === 1 ? metrics.slot1CallName
                                                      : heroSlot === 2 ? metrics.slot2CallName : ""
    readonly property string heroTg: heroSlot === 1 ? metrics.slot1TgText : heroSlot === 2 ? metrics.slot2TgText : ""
    readonly property string heroSrc: heroSlot === 1 ? metrics.slot1SrcText : heroSlot === 2 ? metrics.slot2SrcText : ""
    readonly property double heroTgId: heroSlot === 1 ? metrics.slot1TgId : heroSlot === 2 ? metrics.slot2TgId : 0
    readonly property int heroSeconds: heroSlot === 1 ? metrics.slot1CallSeconds
                                                      : heroSlot === 2 ? metrics.slot2CallSeconds : 0

    // The hero shows one slot, but TDMA carries two: when the other slot is also
    // live it gets a slim strip of its own, or that call is invisible and cannot
    // be skipped.
    readonly property int otherSlot: heroSlot === 1 ? 2 : heroSlot === 2 ? 1 : 0
    readonly property bool otherActive: otherSlot === 1 ? metrics.slot1CallState === 2
                                                        : otherSlot === 2 ? metrics.slot2CallState === 2 : false
    readonly property string otherName: otherSlot === 1 ? metrics.slot1CallName
                                                        : otherSlot === 2 ? metrics.slot2CallName : ""
    readonly property string otherTg: otherSlot === 1 ? metrics.slot1TgText
                                                      : otherSlot === 2 ? metrics.slot2TgText : ""
    readonly property bool otherEnc: otherSlot === 1 ? metrics.slot1CallEnc
                                                     : otherSlot === 2 ? metrics.slot2CallEnc : false

    property bool muted: false
    property bool holding: false

    // Each engine start rebuilds its options fresh — unmuted, no hold — so the
    // local toggles must follow, or the buttons run inverted against the new run.
    readonly property bool sessionOn: decoderHost ? decoderHost.sessionActive : false
    onSessionOnChanged: {
        if (sessionOn) {
            muted = false
            holding = false
        }
    }

    onHeroNameChanged: heroText.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // Header
    Item {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 48

        Column {
            anchors.left: parent.left
            anchors.right: livePill.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Text {
                width: parent.width
                text: screen.systemName
                font.family: Theme.sans
                font.pixelSize: 20
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                elide: Text.ElideRight
            }

            Text {
                width: parent.width
                text: screen.system ? Util.monitorMeta(screen.system) : ""
                font.family: Theme.mono
                font.pixelSize: 11
                font.letterSpacing: 0.8
                color: Theme.textSubdued
                elide: Text.ElideRight
            }
        }

        Rectangle {
            id: livePill

            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: liveRow.implicitWidth + 24
            height: 30
            radius: Theme.radiusButton
            color: Theme.panel
            border.width: 1
            border.color: Theme.panelBorder

            Row {
                id: liveRow
                anchors.centerIn: parent
                spacing: 7

                Item {
                    width: 8
                    height: 8
                    anchors.verticalCenter: parent.verticalCenter

                    Rectangle {
                        anchors.centerIn: parent
                        width: 18
                        height: 18
                        radius: 9
                        visible: Theme.dark && decoderHost.running
                        color: Qt.alpha(Theme.cyan, 0.25)
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: 8
                        height: 8
                        radius: 4
                        color: decoderHost.running ? Theme.cyan : Theme.textSubdued
                    }
                }

                Text {
                    text: decoderHost.running ? qsTr("LIVE") : decoderHost.statusText.toUpperCase()
                    font.family: Theme.mono
                    font.pixelSize: 11
                    font.letterSpacing: 1.4
                    color: Theme.textPrimary
                }
            }
        }
    }

    Column {
        id: body

        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: stopButton.top
        anchors.margins: Theme.screenPadding
        anchors.topMargin: 12
        anchors.bottomMargin: 14
        spacing: Theme.gap

        // Hero: now hearing.
        UiPanel {
            id: hero

            width: parent.width
            height: 170

            // Faint diagonal cyan→magenta wash over the panel.
            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                radius: Theme.radiusPanel - 1
                rotation: 0
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Qt.alpha(Theme.cyan, 0.07) }
                    GradientStop { position: 1.0; color: Qt.alpha(Theme.magenta, 0.06) }
                }
            }

            Column {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.cardPadding
                spacing: 8

                MicroLabel {
                    text: qsTr("Now hearing")
                }

                // Gradient-filled talkgroup name, drawn so the fill can follow the
                // cyan→magenta ramp per glyph run.
                Canvas {
                    id: heroText

                    width: parent.width
                    height: 40
                    visible: screen.heroSlot !== 0

                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        var label = screen.heroName.length > 0 ? screen.heroName : screen.heroTg
                        ctx.font = "bold 31px \"" + Theme.sans + "\""
                        ctx.textBaseline = "middle"
                        var gradient = ctx.createLinearGradient(0, 0, Math.max(ctx.measureText(label).width, 1), 0)
                        gradient.addColorStop(0, String(Theme.cyan))
                        gradient.addColorStop(1, String(Theme.magenta))
                        ctx.fillStyle = gradient
                        ctx.fillText(label, 0, height / 2, width)
                    }

                    Connections {
                        target: Theme
                        function onDarkChanged() { heroText.requestPaint() }
                    }
                }

                Text {
                    visible: screen.heroSlot === 0
                    height: 40
                    verticalAlignment: Text.AlignVCenter
                    // A locked carrier means the site is there and quiet; only the
                    // absence of one is "no signal".
                    text: metrics.carrierLock ? qsTr("waiting for a call…") : qsTr("waiting for signal…")
                    font.family: Theme.sans
                    font.pixelSize: 20
                    color: Theme.textSubdued
                }

                Text {
                    visible: screen.heroSlot !== 0
                    text: "TG " + screen.heroTg + " · SRC " + screen.heroSrc
                    font.family: Theme.mono
                    font.pixelSize: 13
                    color: Theme.textSecondary
                }
            }

            LevelMeter {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: Theme.cardPadding
                active: screen.heroActive
            }

            Text {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.cardPadding
                text: Util.fmtDuration(screen.heroSeconds)
                visible: screen.heroSlot !== 0
                font.family: Theme.mono
                font.pixelSize: 24
                font.weight: Font.Medium
                color: Theme.textPrimary
            }
        }

        // Actions on the live engine.
        Row {
            width: parent.width
            spacing: 10

            OutlineButton {
                width: (parent.width - 20) / 3
                text: screen.muted ? qsTr("Unmute") : qsTr("Mute")
                enabled: decoderHost.running
                onClicked: {
                    if (commands.toggleMute())
                        screen.muted = !screen.muted
                }
            }

            OutlineButton {
                width: (parent.width - 20) / 3
                text: screen.holding ? qsTr("Release") : qsTr("Hold TG")
                // Disabled, not a silent no-op, when the call has no numeric
                // talkgroup (M17/D-STAR callsigns, dPMR dial strings).
                enabled: decoderHost.running && (screen.holding || screen.heroTgId > 0)
                border.color: screen.holding ? Theme.cyan : Theme.controlBorder
                onClicked: {
                    if (screen.holding) {
                        commands.holdTalkgroup(0)
                        screen.holding = false
                    } else if (commands.holdTalkgroup(screen.heroTgId)) {
                        screen.holding = true
                    }
                }
            }

            OutlineButton {
                width: (parent.width - 20) / 3
                text: qsTr("Skip")
                enabled: decoderHost.running && screen.heroSlot !== 0
                onClicked: commands.lockoutSlot(screen.heroSlot === 2 ? 1 : 0)
            }
        }

        // The concurrent TDMA call on the non-hero slot: identity plus its own
        // skip, so a second conversation is never invisible or untouchable.
        UiPanel {
            width: parent.width
            visible: screen.otherActive
            height: 48

            MicroLabel {
                id: otherSlotLabel
                anchors.left: parent.left
                anchors.leftMargin: Theme.cardPadding
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("SLOT %1").arg(screen.otherSlot)
            }

            Text {
                anchors.left: otherSlotLabel.right
                anchors.leftMargin: 10
                anchors.right: otherEncTag.visible ? otherEncTag.left : otherSkip.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: screen.otherName.length > 0 ? screen.otherName + " · TG " + screen.otherTg
                                                  : "TG " + screen.otherTg
                font.family: Theme.sans
                font.pixelSize: 14
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                elide: Text.ElideRight
            }

            EncTag {
                id: otherEncTag
                visible: screen.otherEnc
                anchors.right: otherSkip.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
            }

            OutlineButton {
                id: otherSkip
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 70
                implicitHeight: 32
                height: 32
                text: qsTr("Skip")
                enabled: decoderHost.running
                onClicked: commands.lockoutSlot(screen.otherSlot === 2 ? 1 : 0)
            }
        }

        // Signal strip — tuner truths, only when a tuner exists.
        Row {
            visible: metrics.radioInput
            spacing: 6

            Text {
                text: qsTr("SNR")
                font.family: Theme.mono
                font.pixelSize: 11
                color: Theme.textSubdued
            }

            Text {
                text: metrics.snrValid ? metrics.snrDb.toFixed(1) + " dB" : "—"
                font.family: Theme.mono
                font.pixelSize: 11
                color: metrics.snrValid ? Theme.cyan : Theme.textSubdued
            }

            Text {
                text: "  " + qsTr("LOCK")
                font.family: Theme.mono
                font.pixelSize: 11
                color: Theme.textSubdued
            }

            Rectangle {
                width: 6
                height: 6
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: metrics.carrierLock ? Theme.cyan : Theme.textSubdued
            }

            Text {
                text: "  CFO " + metrics.cfoHz.toFixed(0) + " Hz  ·  " + qsTr("GAIN") + " " + metrics.tunerGainText
                font.family: Theme.mono
                font.pixelSize: 11
                color: Theme.textSubdued
            }
        }

        // Recent calls.
        UiPanel {
            width: parent.width
            height: body.height - y

            MicroLabel {
                id: recentLabel
                x: Theme.cardPadding
                y: Theme.cardPadding
                text: qsTr("Recent calls")
            }

            ListView {
                anchors.top: recentLabel.bottom
                anchors.topMargin: 10
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 6
                clip: true
                model: monitorView

                delegate: CallRow {
                    width: ListView.view.width
                    name: model.name
                    metaText: "TG " + model.tg
                              + (model.enc ? " · " + qsTr("encrypted") : "")
                              + (model.durationSecs >= 0 ? " · " + Util.fmtDuration(model.durationSecs) : "")
                    rightText: Util.shortAge(model.when)
                    enc: model.enc
                }

                Text {
                    anchors.centerIn: parent
                    visible: parent.count === 0
                    text: qsTr("Calls will appear here as they land.")
                    font.family: Theme.sans
                    font.pixelSize: 13
                    color: Theme.textSubdued
                }
            }
        }
    }

    GradientButton {
        id: stopButton

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: 22
        text: qsTr("Stop listening")
        enabled: !decoderHost.transitioning
        onClicked: decoderHost.stop()
    }
}
