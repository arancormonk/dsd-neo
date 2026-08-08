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
    // After an Activity restart the reattached session has no saved-system map,
    // but the persisted session label still names what is playing — the header
    // should agree with the history rows it sits above.
    property string systemName: system ? system.name
                                       : callHistory.sessionLabel.length > 0 ? callHistory.sessionLabel
                                                                             : qsTr("Listening")

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
    readonly property bool heroEnc: heroSlot === 1 ? metrics.slot1CallEnc
                                                   : heroSlot === 2 ? metrics.slot2CallEnc : false
    readonly property string heroEncText: heroSlot === 1 ? metrics.slot1EncText
                                                         : heroSlot === 2 ? metrics.slot2EncText : ""
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

    // Ticks the recent-calls age labels ("now", "1m", "2h") once a minute:
    // Util.shortAge reads the clock, which is not a binding dependency, so
    // without this a row's age freezes at whatever it said when its delegate
    // was created. Same device as HomeScreen's heardTick.
    property int ageTick: 0

    Timer {
        interval: 60000
        running: screen.visible
        repeat: true
        onTriggered: screen.ageTick++
    }

    // Engine truth, not local mirrors: commands only enqueue a request, and on
    // Android the service (which owns both states) outlives the Activity — a
    // relaunched UI must show where mute and hold actually stand, or its buttons
    // run inverted against the live session.
    readonly property bool muted: metrics ? metrics.audioMuted : false
    readonly property bool holding: metrics ? metrics.heldTg > 0 : false

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

                Row {
                    visible: screen.heroSlot !== 0
                    spacing: 8

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "TG " + screen.heroTg + " · SRC " + screen.heroSrc
                        font.family: Theme.mono
                        font.pixelSize: 13
                        color: Theme.textSecondary
                    }

                    // The hero must say when the call it is captioning is
                    // encrypted — hearing silence over a normal-looking talkgroup
                    // otherwise reads as the decoder failing.
                    EncTag {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: screen.heroEnc
                    }

                    // The decoded algorithm and key id, when the header said:
                    // AES and RC4 traffic should read differently at a glance.
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: screen.heroEnc && screen.heroEncText.length > 0
                        text: screen.heroEncText
                        font.family: Theme.mono
                        font.pixelSize: 11
                        color: Theme.textSubdued
                    }
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
                // The label follows metrics.audioMuted once the engine applies the
                // command — the button never guesses at the outcome.
                onClicked: commands.toggleMute()
            }

            OutlineButton {
                width: (parent.width - 20) / 3
                text: screen.holding ? qsTr("Release") : qsTr("Hold TG")
                // Disabled, not a silent no-op, when the call has no numeric
                // talkgroup (M17/D-STAR callsigns, dPMR dial strings).
                enabled: decoderHost.running && (screen.holding || screen.heroTgId > 0)
                border.color: screen.holding ? Theme.cyan : Theme.controlBorder
                onClicked: commands.holdTalkgroup(screen.holding ? 0 : screen.heroTgId)
            }

            OutlineButton {
                width: (parent.width - 20) / 3
                text: qsTr("Skip")
                enabled: decoderHost.running && screen.heroSlot !== 0
                onClicked: commands.lockoutSlot(screen.heroSlot === 2 ? 1 : 0)
            }
        }

        // The engine's answer to the last command ("Output: Muted", "Output:
        // open failed") — without it a tap that failed inside the engine reads
        // as a button that did nothing.
        Text {
            width: parent.width
            visible: metrics.uiMessage.length > 0
            text: metrics.uiMessage
            font.family: Theme.mono
            font.pixelSize: 12
            color: Theme.cyan
            elide: Text.ElideRight
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
        //
        // Flow, not Row: every reading's width moves with its value (a CFO can
        // run to "-1234 Hz", an SNR to "-10.0 dB"), and an unconstrained Row ran
        // the last reading off the side of a 411 dp phone with no way to reach
        // it. Each reading is one child, so a wrap drops a whole reading to the
        // next line and never splits a label from its value. Whitespace groups
        // them, as in the design — separator glyphs would strand at a line head.
        Flow {
            width: parent.width
            visible: metrics.radioInput
            spacing: 10

            Row {
                spacing: 5

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
            }

            Row {
                spacing: 5

                Text {
                    text: qsTr("LOCK")
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
            }

            Text {
                text: "CFO " + metrics.cfoHz.toFixed(0) + " Hz"
                font.family: Theme.mono
                font.pixelSize: 11
                color: Theme.textSubdued
            }

            Text {
                text: qsTr("GAIN") + " " + metrics.tunerGainText
                font.family: Theme.mono
                font.pixelSize: 11
                color: Theme.textSubdued
            }

            // Sample delivery from the tuner — "no samples" and "no signal" are
            // different faults, and the terminal UI always told them apart.
            Row {
                spacing: 5

                Text {
                    text: qsTr("STREAM")
                    font.family: Theme.mono
                    font.pixelSize: 11
                    color: Theme.textSubdued
                }

                Text {
                    text: metrics.streamActive ? qsTr("ACTIVE") : qsTr("IDLE")
                    font.family: Theme.mono
                    font.pixelSize: 11
                    color: metrics.streamActive ? Theme.cyan : Theme.textSubdued
                }
            }
        }

        // Why an empty log can still be a working decoder. On an almost entirely
        // encrypted site the control channel decodes, every grant is declined and
        // no call is ever logged, which is indistinguishable from a decoder that
        // stopped. Outside the strip above and its tuner gate on purpose: this is
        // decode truth, not tuner truth, and a network source meets it just as
        // often.
        //
        // The ledger's size, not a tally of refusals: a control channel repeats a
        // grant update every few hundred ms while a call is up, so counting
        // refused grants read 150 where about a dozen transmissions had happened.
        // The ledger counts targets, and only from voice confirmed undecryptable.
        // Magenta, not cyan: cyan is signal health here, magenta is encryption,
        // same as the ENC tags on the rows below. Hidden at zero, so a site with
        // no encrypted traffic never carries a permanent 0.
        Row {
            // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
            objectName: "encLockoutRow"

            spacing: 5
            visible: metrics.encLockoutCount > 0

            Text {
                text: qsTr("ENC LOCKOUT")
                font.family: Theme.mono
                font.pixelSize: 11
                color: Theme.textSubdued
            }

            Text {
                objectName: "encLockoutValue"

                text: metrics.encLockoutCount.toString()
                font.family: Theme.mono
                font.pixelSize: 11
                color: Theme.magenta
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
                id: recentList

                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "recentCallsList"

                anchors.top: recentLabel.bottom
                anchors.topMargin: 10
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 6
                clip: true
                model: monitorView

                // Newest-first, and held at the top so the pane keeps showing the
                // call that just ended. A prepend below the top moves the content,
                // not the view: it holds the rows being read still and puts the
                // new call above the viewport, so a pane left scrolled never comes
                // back on its own. Same rule as the history screen — the top is
                // re-asserted when a call lands and the pane is at rest within a
                // row of it — minus the new-calls pill, which four rows have no
                // room for and which the hero above already covers.
                readonly property bool atLatest: contentY - originY < Theme.rowHeight

                function pinToLatest() {
                    if (atLatest && !moving && contentY !== originY)
                        positionViewAtBeginning()
                }

                // Deferred: inside the signal the view is still applying the
                // model change, and a position asserted there does not survive it.
                onCountChanged: Qt.callLater(pinToLatest)

                delegate: CallRow {
                    width: ListView.view.width
                    name: model.name
                    metaText: {
                        if (model.kind === 1)
                            return model.detail.length > 0 ? model.detail : qsTr("data message")
                        return "TG " + model.tg
                               + (model.enc ? " · " + qsTr("encrypted") : "")
                               + (model.durationSecs >= 0 ? " · " + Util.fmtDuration(model.durationSecs) : "")
                    }
                    // ageTick forces the minute-by-minute refresh; shortAge reads
                    // the clock, which is not a binding dependency by itself.
                    rightText: (screen.ageTick, Util.shortAge(model.when))
                    enc: model.enc
                }
            }

            // A sibling of the view, not a child: ListView reparents declared
            // children into its contentItem, where `parent.count` is undefined and
            // the placeholder would never show.
            Text {
                anchors.centerIn: recentList
                visible: recentList.count === 0
                text: qsTr("Calls will appear here as they land.")
                font.family: Theme.sans
                font.pixelSize: 13
                color: Theme.textSubdued
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
