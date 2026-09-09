// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The live session: who you are hearing now, the calls you just heard, and one
// way out. Replaces the home content while a session is active.
Item {
    id: screen

    onVisibleChanged: { if (!visible) keySheet.visible = false }

    readonly property bool compactHeight: height < 500

    onVisibleChanged: { if (!visible) networkSheet.visible = false; }

    // Raised by the header's spectrum button; Main.qml owns the layer.
    signal openSpectrum
    signal openTalkgroups

    // Raised by a long-press on the header title, the same gesture that edits a
    // card on Home. Main.qml opens the wizard on the running system — the only
    // way to change its imported CSVs while the session is live.
    signal editSystem

    // The saved-system map the session was started from (may be null for a
    // network/file quick start).
    property var system: null
    // After an Activity restart the reattached session has no saved-system map,
    // but the persisted session label still names what is playing — the header
    // should agree with the history rows it sits above.
    property string systemName: system ? system.name : callHistory.sessionLabel.length > 0 ? callHistory.sessionLabel : qsTr("Listening")

    // Which slot the hero shows: an active call wins, then a recently ended one, and
    // between equals the lower slot. Decided by dsd_app_lead_slot() rather than here, so
    // this panel and the Android notification cannot headline different slots.
    readonly property int heroSlot: metrics ? metrics.leadSlot : 0
    readonly property bool heroActive: heroSlot === 1 ? metrics.slot1CallState === 2 : heroSlot === 2 ? metrics.slot2CallState === 2 : false
    readonly property string heroName: heroSlot === 1 ? metrics.slot1CallName : heroSlot === 2 ? metrics.slot2CallName : ""
    // The scan channel the hero call was heard on (a -Y row name or a trunk-scan
    // target id); empty when the receiver is not scanning.
    readonly property string heroChannel: heroSlot === 1 ? metrics.slot1Channel : heroSlot === 2 ? metrics.slot2Channel : ""
    readonly property string heroTg: heroSlot === 1 ? metrics.slot1TgText : heroSlot === 2 ? metrics.slot2TgText : ""
    readonly property string heroSrc: heroSlot === 1 ? metrics.slot1SrcText : heroSlot === 2 ? metrics.slot2SrcText : ""
    readonly property double heroTgId: heroSlot === 1 ? metrics.slot1TgId : heroSlot === 2 ? metrics.slot2TgId : 0
    readonly property bool heroEmergency: heroSlot === 1 ? metrics.slot1CallEmergency : heroSlot === 2 ? metrics.slot2CallEmergency : false
    readonly property bool otherEmergency: otherSlot === 1 ? metrics.slot1CallEmergency : otherSlot === 2 ? metrics.slot2CallEmergency : false
    readonly property bool heroEnc: heroSlot === 1 ? metrics.slot1CallEnc : heroSlot === 2 ? metrics.slot2CallEnc : false
    readonly property string heroEncText: heroSlot === 1 ? metrics.slot1EncText : heroSlot === 2 ? metrics.slot2EncText : ""
    readonly property int heroSeconds: heroSlot === 1 ? metrics.slot1CallSeconds : heroSlot === 2 ? metrics.slot2CallSeconds : 0

    // The hero shows one slot, but TDMA carries two: when the other slot is also
    // live it gets a slim strip of its own, or that call is invisible and cannot
    // be skipped.
    readonly property int otherSlot: heroSlot === 1 ? 2 : heroSlot === 2 ? 1 : 0
    readonly property bool otherActive: otherSlot === 1 ? metrics.slot1CallState === 2 : otherSlot === 2 ? metrics.slot2CallState === 2 : false
    readonly property string otherName: otherSlot === 1 ? metrics.slot1CallName : otherSlot === 2 ? metrics.slot2CallName : ""
    readonly property string otherTg: otherSlot === 1 ? metrics.slot1TgText : otherSlot === 2 ? metrics.slot2TgText : ""
    readonly property bool otherEnc: otherSlot === 1 ? metrics.slot1CallEnc : otherSlot === 2 ? metrics.slot2CallEnc : false

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
    readonly property bool scanHeld: metrics ? metrics.scanHold : false

    onHeroNameChanged: heroText.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // Header
    Item {
        id: header
        enabled: !keySheet.visible

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 48

        Column {
            anchors.left: parent.left
            anchors.right: spectrumPill.visible ? spectrumPill.left : livePill.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Text {
                width: parent.width
                text: screen.systemName
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(20)
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                elide: Text.ElideRight
            }

            Text {
                width: parent.width
                text: screen.system ? Util.monitorMeta(screen.system) : ""
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(11)
                font.letterSpacing: 0.8
                color: Theme.textSubdued
                elide: Text.ElideRight
            }

            TapHandler {
                onLongPressed: screen.editSystem()
            }
        }

        // Only an RTL front end has a band around the tuned frequency to show;
        // a PCM feed or a file has nothing to draw.
        Rectangle {
            id: spectrumPill

            objectName: "openSpectrumButton"
            anchors.right: livePill.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            visible: metrics.radioInput
            width: spectrumLabel.implicitWidth + 22
            height: 30
            radius: Theme.radiusButton
            color: spectrumTap.pressed ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
            border.width: 1
            border.color: Theme.panelBorder

            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }

            Text {
                id: spectrumLabel

                anchors.centerIn: parent
                text: qsTr("SPECTRUM")
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(11)
                font.letterSpacing: 1.4
                color: Theme.textSecondary
            }

            TapHandler {
                id: spectrumTap
                onTapped: screen.openSpectrum()
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
                    font.pixelSize: Theme.fontSize(11)
                    font.letterSpacing: 1.4
                    color: Theme.textPrimary
                }
            }
        }
    }

    // Hero: now hearing.
    UiPanel {
        id: hero
        objectName: "monitorHero"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        anchors.topMargin: 12

        // Reserve space for scrolling even in landscape. The meter stands down
        // and duration moves to the title line so neither covers call identity.
        height: screen.compactHeight ? 110 + 40 * (Theme.fontScale - 1) : 170 * Theme.fontScale

        // Faint diagonal cyan→magenta wash over the panel.
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: Theme.radiusPanel - 1
            rotation: 0
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0.0
                    color: Qt.alpha(Theme.cyan, 0.07)
                }
                GradientStop {
                    position: 1.0
                    color: Qt.alpha(Theme.magenta, 0.06)
                }
            }
        }

        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.cardPadding
            spacing: screen.compactHeight ? 4 : 8

            MicroLabel {
                text: qsTr("Now hearing")
            }

            // Gradient-filled talkgroup name, drawn so the fill can follow the
            // cyan→magenta ramp per glyph run.
            Canvas {
                id: heroText

                width: parent.width
                height: (screen.compactHeight ? 30 : 40) * Theme.fontScale
                visible: screen.heroSlot !== 0

                onPaint: {
                    var ctx = getContext("2d");
                    ctx.reset();
                    var label = screen.heroName.length > 0 ? screen.heroName : screen.heroTg;
                    ctx.font = "bold " + ((screen.compactHeight ? 25 : 31) * Theme.fontScale) + "px \"" + Theme.sans + "\"";
                    ctx.textBaseline = "middle";
                    var gradient = ctx.createLinearGradient(0, 0, Math.max(ctx.measureText(label).width, 1), 0);
                    gradient.addColorStop(0, String(Theme.cyan));
                    gradient.addColorStop(1, String(Theme.magenta));
                    ctx.fillStyle = gradient;
                    ctx.fillText(label, 0, height / 2, width);
                }

                Connections {
                    target: Theme
                    function onDarkChanged() {
                        heroText.requestPaint();
                    }
                }
            }

            Text {
                visible: screen.heroSlot === 0
                height: (screen.compactHeight ? 30 : 40) * Theme.fontScale
                verticalAlignment: Text.AlignVCenter
                // A locked carrier means the site is there and quiet; only the
                // absence of one is "no signal".
                text: metrics.carrierLock ? qsTr("waiting for a call…") : qsTr("waiting for signal…")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(20)
                color: Theme.textSubdued
            }

            Row {
                id: heroSubline

                // Give encryption indicators priority, then fit the identity
                // and trailing channel into the remaining space.
                width: parent.width
                visible: screen.heroSlot !== 0
                spacing: 8

                // A zero id is "none decoded", not an identity: an encrypted
                // call on a conventional channel reports no talkgroup, and
                // "TG 0" under a headline that already names the channel would
                // only say the decoder saw nothing.
                Text {
                    objectName: "heroIds"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: text.length > 0
                    width: Math.max(0, Math.min(implicitWidth, heroSubline.width - (heroEmergencyTag.visible ? heroEmergencyTag.width + heroSubline.spacing : 0) - (heroEncTag.visible ? heroEncTag.width + heroSubline.spacing : 0) - (heroAlgorithm.visible ? heroAlgorithm.width + heroSubline.spacing : 0)))
                    elide: Text.ElideRight
                    text: {
                        var parts = [];
                        if (screen.heroTg.length > 0 && screen.heroTg !== "0")
                            parts.push("TG " + screen.heroTg);
                        if (screen.heroSrc.length > 0 && screen.heroSrc !== "0")
                            parts.push("SRC " + screen.heroSrc);
                        return parts.join(" · ");
                    }
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSecondary
                }

                // The hero must say when the call it is captioning is
                // encrypted — hearing silence over a normal-looking talkgroup
                // otherwise reads as the decoder failing.
                EmergencyTag {
                    id: heroEmergencyTag
                    objectName: "heroEmergencyTag"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: screen.heroEmergency
                }

                EncTag {
                    id: heroEncTag
                    objectName: "heroEncTag"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: screen.heroEnc
                }

                // The decoded algorithm and key id, when the header said:
                // AES and RC4 traffic should read differently at a glance.
                Text {
                    id: heroAlgorithm
                    objectName: "heroAlgorithm"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: screen.heroEnc && screen.heroEncText.length > 0
                    width: Math.max(0, Math.min(implicitWidth, heroSubline.width - (heroEmergencyTag.visible ? heroEmergencyTag.width + heroSubline.spacing : 0) - (heroEncTag.visible ? heroEncTag.width + heroSubline.spacing : 0)))
                    elide: Text.ElideRight
                    text: screen.heroEncText
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                // Where the call was heard, when that is not already the name
                // above: a listed talkgroup still says which scan channel it
                // came from. Last in the row and elided to what is left of it,
                // so an operator-length name never pushes the ENC tag off the
                // panel; x is laid out from the siblings before it, so the
                // width binding cannot loop.
                Text {
                    objectName: "heroChannel"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: screen.heroChannel.length > 0 && screen.heroChannel !== screen.heroName
                    width: Math.max(0, Math.min(implicitWidth, heroSubline.width - x))
                    elide: Text.ElideRight
                    text: "· " + screen.heroChannel
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSecondary
                }
            }
        }

        LevelMeter {
            visible: !screen.compactHeight
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: Theme.cardPadding
            active: screen.heroActive
        }

        Text {
            anchors.right: parent.right
            anchors.top: screen.compactHeight ? parent.top : undefined
            anchors.bottom: screen.compactHeight ? undefined : parent.bottom
            anchors.margins: Theme.cardPadding
            text: Util.fmtDuration(screen.heroSeconds)
            visible: screen.heroSlot !== 0
            font.family: Theme.mono
            font.pixelSize: Theme.fontSize(screen.compactHeight ? 14 : 24)
            font.weight: Font.Medium
            color: Theme.textPrimary
        }
    }

    // The hero stays visible while controls and recent calls scroll. A column
    // anchored at both ends assigned negative space to recent calls in landscape.
    Flickable {
        id: bodyScroll
        enabled: !keySheet.visible
        objectName: "monitorBody"
        anchors.top: hero.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: stopButton.top
        anchors.margins: Theme.screenPadding
        anchors.topMargin: Theme.gap
        anchors.bottomMargin: 14
        clip: true
        contentWidth: width
        contentHeight: body.height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: body
            width: bodyScroll.width
            spacing: Theme.gap

            // WP-F1: site identity sits between the hero and action row.
            UiPanel {
                id: siteRow
                objectName: "siteRow"
                width: parent.width
                height: Math.max(44, siteLabel.implicitHeight + 20)
                visible: metrics.siteLine.length > 0
                opacity: metrics.siteConfirmed ? 1 : 0.55
                Accessible.role: Accessible.Button
                Accessible.name: metrics.siteLine
                Accessible.onPressAction: siteSheet.visible = true
                Text {
                    id: siteLabel
                    anchors.centerIn: parent
                    width: parent.width - 24
                    text: metrics.siteLine + "  ›"
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: 12 * Theme.fontScale
                    color: Theme.textPrimary
                }
                TapHandler { onTapped: siteSheet.visible = true }
            }

            // Actions on the live engine.
            Row {
                width: parent.width
                spacing: 10

                OutlineButton {
                    width: (parent.width - 30) / 4
                    text: screen.muted ? qsTr("Unmute") : qsTr("Mute")
                    enabled: decoderHost.running
                    // The label follows metrics.audioMuted once the engine applies the
                    // command — the button never guesses at the outcome.
                    onClicked: commands.toggleMute()
                }

                OutlineButton {
                    width: (parent.width - 30) / 4
                    text: screen.holding ? qsTr("Release") : qsTr("Hold TG")
                    // Disabled, not a silent no-op, when the call has no numeric
                    // talkgroup (M17/D-STAR callsigns, dPMR dial strings).
                    enabled: decoderHost.running && (screen.holding || screen.heroTgId > 0)
                    border.color: screen.holding ? Theme.cyan : Theme.controlBorder
                    onClicked: commands.holdTalkgroup(screen.holding ? 0 : screen.heroTgId)
                }

                OutlineButton {
                    width: (parent.width - 30) / 4
                    text: qsTr("Skip")
                    enabled: decoderHost.running && screen.heroSlot !== 0
                    onClicked: commands.lockoutSlot(screen.heroSlot === 2 ? 1 : 0)
                }

                OutlineButton {
                    objectName: "talkgroupsButton"
                    width: (parent.width - 30) / 4
                    text: qsTr("TG list")
                    enabled: decoderHost.running
                    onClicked: screen.openTalkgroups()
                }
            }

            OutlineButton {
                objectName: "openKeySheetButton"
                width: parent.width
                text: qsTr("Encryption key…")
                enabled: decoderHost.running
                onClicked: keySheet.open()
            }

            // Actions on the scan rotation (#380): the -Y list or the trunk-scan
            // targets, whichever is running. Gated on the rotation, not on the
            // tuner gate: plain trunking owns the tuner too and has nothing to hold
            // or avoid at channel scope. Hold reads the engine back like Hold TG
            // does, and the engine's refusals ("Cannot avoid the last usable scan
            // channel") arrive through uiMessage below.
            Row {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "scanControlsRow"

                width: parent.width
                spacing: 10
                visible: metrics.scanRotationActive

                OutlineButton {
                    objectName: "scanHoldButton"

                    width: (parent.width - 20) / 3
                    text: screen.scanHeld ? qsTr("Release scan") : qsTr("Hold scan")
                    enabled: decoderHost.running
                    border.color: screen.scanHeld ? Theme.cyan : Theme.controlBorder
                    onClicked: commands.toggleScanHold()
                }

                OutlineButton {
                    objectName: "scanAvoidButton"

                    width: (parent.width - 20) / 3
                    text: qsTr("Avoid")
                    enabled: decoderHost.running
                    onClicked: commands.avoidCurrentChannel()
                }

                OutlineButton {
                    objectName: "scanNextButton"

                    width: (parent.width - 20) / 3
                    text: qsTr("Next")
                    enabled: decoderHost.running
                    onClicked: commands.nextChannel()
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
                font.pixelSize: Theme.fontSize(12)
                color: Theme.cyan
                elide: Text.ElideRight
            }

            // The concurrent TDMA call on the non-hero slot: identity plus its own
            // skip, so a second conversation is never invisible or untouchable.
            UiPanel {
                width: parent.width
                objectName: "otherSlotPanel"
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
                    anchors.right: otherEmergencyTag.visible ? otherEmergencyTag.left : otherEncTag.visible ? otherEncTag.left : otherSkip.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: screen.otherName.length > 0 ? screen.otherName + " · TG " + screen.otherTg : "TG " + screen.otherTg
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                }

                EmergencyTag {
                    id: otherEmergencyTag
                    objectName: "otherEmergencyTag"
                    visible: screen.otherEmergency
                    anchors.right: otherEncTag.visible ? otherEncTag.left : otherSkip.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
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
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.textSubdued
                    }

                    Text {
                        text: metrics.snrValid ? metrics.snrDb.toFixed(1) + " dB" : "—"
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: metrics.snrValid ? Theme.cyan : Theme.textSubdued
                    }
                }

                Row {
                    spacing: 5

                    Text {
                        text: qsTr("LOCK")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
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
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    text: qsTr("GAIN") + " " + metrics.tunerGainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                // Sample delivery from the tuner — "no samples" and "no signal" are
                // different faults, and the terminal UI always told them apart.
                Row {
                    spacing: 5

                    Text {
                        text: qsTr("STREAM")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.textSubdued
                    }

                    Text {
                        text: metrics.streamActive ? qsTr("ACTIVE") : qsTr("IDLE")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: metrics.streamActive ? Theme.cyan : Theme.textSubdued
                    }
                }
            }

            // Decode quality also applies to PCM, network and file sources.
            Flow {
                objectName: "decodeQualityRow"
                width: parent.width
                visible: metrics.qualityValid
                spacing: 10

                Text {
                    objectName: "ccFecQuality"
                    visible: metrics.ccFecValid
                    text: qsTr("CC FEC") + " " + metrics.ccFecOkPct.toFixed(0) + "%"
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }
                Text {
                    objectName: "voiceErrsQuality"
                    visible: metrics.voiceErrsValid
                    text: qsTr("VOICE") + " " + metrics.voiceErrsPerFrame.toFixed(1) + " err/fr"
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.cyan
                }
                Text {
                    objectName: "rsQuality"
                    visible: metrics.rsValid
                    text: qsTr("RS") + " " + metrics.rsOkPct.toFixed(0) + "%"
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }
                Text {
                    objectName: "lastFrameQuality"
                    visible: metrics.lastFrameErrsValid
                    text: qsTr("ERR") + " " + metrics.lastFrameErrs + "/" + metrics.lastFrameErrs2
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
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
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "encLockoutValue"

                    text: metrics.encLockoutCount.toString()
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.magenta
                }
            }

            // Why the scan stopped moving (#380): channels or targets the operator
            // avoided for the session, with the way to put them back. Hidden at zero
            // and outside any rotation, so an idle session never carries a 0. The
            // [avoided] marker is the trunk-scan fallback: every alternate failed to
            // retune and the receiver stayed on a target that was avoided.
            Row {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "scanAvoidRow"

                spacing: 8
                visible: metrics.scanRotationActive && metrics.scanAvoidCount > 0

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: metrics.scanTargetAvoided ? qsTr("SCAN AVOIDS [avoided]") : qsTr("SCAN AVOIDS")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "scanAvoidValue"

                    anchors.verticalCenter: parent.verticalCenter
                    text: metrics.scanAvoidCount.toString()
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.cyan
                }

                OutlineButton {
                    objectName: "scanAvoidClearButton"

                    width: 72
                    implicitHeight: 28
                    text: qsTr("Clear")
                    enabled: decoderHost.running
                    onClicked: commands.clearScanAvoids()
                }
            }

            // Recent calls.
            UiPanel {
                objectName: "recentCallsPanel"
                width: parent.width
                height: Math.max(120 * Theme.fontScale, bodyScroll.height - y)

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

                    delegate: CallRow {
                        width: ListView.view.width
                        name: model.name
                        metaText: {
                            if (model.kind === 1)
                                return model.detail.length > 0 ? model.detail : qsTr("data message");
                            // Same meta rules as the history row: a zero talkgroup is
                            // not printed, the channel closes the line unless it is
                            // already the name.
                            var meta = [];
                            if (model.tg > 0)
                                meta.push("TG " + model.tg);
                            if (model.src > 0 || model.srcName)
                                meta.push("SRC " + Util.sourceText(model.src, model.srcName));
                            if (model.enc)
                                meta.push(qsTr("encrypted"));
                            if (model.durationSecs >= 0)
                                meta.push(Util.fmtDuration(model.durationSecs));
                            if (model.channel.length > 0 && model.channel !== model.name)
                                meta.push(model.channel);
                            return meta.join(" · ");
                        }
                        // ageTick forces the minute-by-minute refresh; shortAge reads
                        // the clock, which is not a binding dependency by itself.
                        rightText: (screen.ageTick, Util.shortAge(model.when))
                        enc: model.enc
                        emergency: model.emergency
                    }
                }

                // Same rule as the history log: the pane keeps showing the call that
                // just ended, minus the new-calls pill, which four rows have no room
                // for and which the hero above already covers.
                FollowLatest {
                    list: recentList
                }

                // A sibling of the view, not a child: ListView reparents declared
                // children into its contentItem, where `parent.count` is undefined and
                // the placeholder would never show.
                Text {
                    anchors.centerIn: recentList
                    visible: recentList.count === 0
                    text: qsTr("Calls will appear here as they land.")
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSubdued
                }
            }
        }
    }

    // WP-D2: the modal consumes input above every monitor control.
    KeySheet {
        id: keySheet
        z: 10
    }

    GradientButton {
        id: stopButton

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: 22
        text: qsTr("Stop listening")
        enabled: !decoderHost.transitioning && !keySheet.visible
        onClicked: decoderHost.stop()
    }
    // WP-F1: owned by Monitor, above its scrolling body and stop control.
    SiteSheet {
        id: siteSheet
        z: 10
        onNetworkRequested: networkSheet.open()
    }

    // WP-F2: read-only network sheet, opened from SiteSheet.
    NetworkSheet { id: networkSheet; z: 100 }
}
