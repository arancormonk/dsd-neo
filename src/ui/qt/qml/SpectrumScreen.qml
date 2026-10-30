// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import DsdNeo
import "Util.js" as Util

// The band around the tuned frequency. What you can do with it depends on why
// the session exists.
//
// Listening to a saved system, this watches: the card names a frequency, and a
// touch that quietly moved off it would make the card a lie and would file the
// calls heard afterwards under a system that was not on the air. Exploring, this
// *is* the session — tap a signal, step along the band, or let it sweep — and
// there is nothing to be untrue to.
//
// The waterfall is the screen either way: everything else stays quiet so the eye
// goes to the one thing that shows where the traffic is.
Item {
    id: screen

    objectName: "spectrumScreen"

    signal closed
    // Asks the session to stop following a system and let the tuner be driven by
    // hand. Main.qml owns what that costs; this screen only asks.
    signal exploreFromHere
    signal saveAsSystem(double freqHz)

    // Whether this session may be retuned at all. Set from the session's intent,
    // defaulted to the safe answer so the screen is inert wherever it is loaded
    // without one.
    property bool exploring: false
    // Whether the session was started as a scan list. Its spectrum watches like
    // a saved system's, but the scan owning the tuner is the only thing in the
    // way of a typed frequency (issue #671).
    property bool scanList: false

    // Two different refusals, and the difference matters to the user. An
    // automatic controller holding the tuner is temporary and has a name; a saved
    // system is a choice they made, and it has a way out.
    readonly property bool tunerHeld: metrics ? metrics.tunerControlled : false
    // A third with no way out at all (issue #575): an I/Q replay plays the
    // tuning its capture recorded, and the engine refuses every other tune and
    // the tuner release, whatever this session's intent.
    readonly property bool replayInput: metrics ? metrics.replayInput : false
    readonly property bool viewOnly: !screen.exploring || screen.tunerHeld || screen.replayInput

    // A scan running, and one the operator paused (issue #671). Read apart from
    // tunerHeld because a scan gives way to a typed frequency: Go to pauses it,
    // and Resume scan hands the tuner back.
    readonly property bool scanOwnsTuner: metrics ? metrics.scanRotationActive : false
    readonly property bool scanPaused: metrics ? metrics.scanPaused : false
    // Go to has its own answer, wider than the picture's: a frequency typed on
    // purpose wins over a scan (it pauses), where a tap or a drag does not. A
    // saved system's scan is still that system and stays watched until Explore
    // from here; anything else holding the tuner, or a replay, refuses it.
    readonly property bool goToAllowed: !screen.replayInput && ((screen.exploring && !screen.tunerHeld) || (screen.scanList && screen.scanOwnsTuner) || screen.scanPaused)

    // The frame's own center is what the bins were measured at; the options
    // reading is only a fallback for the moment before the first frame lands.
    readonly property real tunedHz: spectrum.hasData ? spectrum.centerFreqHz : (metrics ? metrics.centerFreqHz : 0)

    // What the big readout shows. While the rail is being dragged, or waiting for
    // a drag's retune to land, that is where it is heading, not where the
    // receiver still is — it is the number being steered. The rail owns that
    // preview; this just reads it, rather than restating the same expression.
    readonly property real readoutHz: bandRail.displayHz

    // Which band the tuner is currently inside, or null off-band. The Go-to
    // sheet reads this to highlight the matching pill; nextStepHz() reaches the
    // same answer through its own Util.bandFor() lookup to decide where a step
    // wraps. Neither the rail nor its window cares about bands at all.
    readonly property var band: Util.bandFor(screen.tunedHz)

    // A step overlaps the last one, so a signal sitting on a seam is not missed
    // by both screens it straddles.
    readonly property real stepHz: spectrum.hasData ? spectrum.spanHz * 0.9 : 0

    // Answers this screen has to give itself, which the engine never sends: an
    // empty band, a sweep that wrapped. Preferred over the engine's own message
    // so the newer answer wins.
    property string hint: ""

    // Whether a sheet is over the picture. TapHandlers never take exclusive
    // grabs, so a tap meant for a control on a sheet also reaches the spectrum
    // underneath and retunes the radio — the panel closes and the receiver has
    // moved. Disabling the picture while a sheet is up is what stops that; the
    // sheets are siblings of it, so they stay live.
    // Main supplies the session sheet; standalone hosts keep a local fallback.
    property var sharedRadioSheet: null
    readonly property var radioSheet: sharedRadioSheet || localRadioLoader.item
    readonly property bool sheetOpen: (radioSheet !== null && radioSheet.visible) || goToSheet.visible || confirmExplore.visible

    // Producing frames is what costs battery, so it follows the screen being up
    // and a session running — not merely the object existing.
    Binding {
        target: spectrum
        property: "active"
        value: screen.visible && decoderHost.sessionActive
    }

    onVisibleChanged: {
        if (!visible)
            screen.stopSweep();
    }

    onViewOnlyChanged: {
        if (viewOnly)
            screen.stopSweep();
    }

    // A replay that begins under a sheet leaves it nothing to do: the release
    // the confirm would ask for and the tune Go to would submit are both ones
    // the engine refuses, and Go to would answer "Cannot tune to ..." rather
    // than say why.
    onReplayInputChanged: {
        if (replayInput) {
            confirmExplore.visible = false;
            goToSheet.visible = false;
        }
    }

    // The same for anything else that takes the tuner while Go to is open: a
    // Tune that could only be refused is closed now, with the reason, rather
    // than answered "Cannot tune to ..." later. A replay says why on its own.
    onGoToAllowedChanged: {
        if (goToAllowed || !goToSheet.visible)
            return;
        goToSheet.visible = false;
        if (!replayInput)
            screen.hint = screen.scanOwnsTuner ? qsTr("Go to closed: the scan has the tuner") : metrics.trunkingEnabled ? qsTr("Go to closed: trunking has the tuner") : qsTr("Go to closed: the tuner is in use");
    }

    // Gesture state and the steps that act on it live here rather than inside
    // the handlers: a handler body cannot be called from a test, and these are
    // the parts that decide where the receiver ends up.
    property real pinchStartZoom: 1.0
    property real pinchAnchorX: 0.5
    property real panStartOffsetHz: 0

    /** Zoom to @a activeScale relative to where the pinch began, held at its anchor. */
    function applyPinch(activeScale) {
        spectrum.zoomToAnchored(screen.pinchStartZoom * activeScale, screen.pinchAnchorX);
    }

    /** Pan by @a dx pixels from where the drag began, across @a width pixels of view. */
    function applyPan(dx, width) {
        if (width <= 0)
            return;
        // Dragging right walks the window down the band, as if pulling the
        // spectrum along under the finger.
        spectrum.viewOffsetHz = screen.panStartOffsetHz - ((dx / width) * spectrum.viewSpanHz);
    }

    /**
     * Settle a finished pan: at most one retune, and only if it ran off the edge.
     *
     * @a cancelled marks a drag that ended for a reason other than the finger
     * lifting — a pinch taking the grab. The few pixels of drift before the
     * second finger landed are not a request to move the receiver, and at 1x
     * every drift is overshoot, so acting on it would retune on the way into a
     * zoom and drop the decode session.
     *
     * The retune target is where the viewport was asking to be, not merely how
     * far past the edge it reached: the granted offset is already carrying the
     * user toward one end of the span, and tuning to the overshoot alone would
     * land short and snap the view backwards away from what they dragged to.
     */
    function endPan(cancelled) {
        var wantHz = spectrum.centerFreqHz + spectrum.viewOffsetHz + spectrum.edgeOvershootHz;
        // Through tuneTo() like every other retune on this screen, rather than
        // straight to the command: it owns the view-only gate, the uint32 bound
        // its own comment explains, and the stale-hint clear — and a pan is the
        // one path that had its own half of those written out again.
        if (!cancelled && spectrum.edgeOvershootHz !== 0 && spectrum.hasData)
            screen.tuneTo(wantHz);
        spectrum.clearOvershoot();
    }

    /**
     * Ask for @a hz, if this session is allowed to ask at all.
     *
     * The upper bound is not a tuner limit — the engine owns those and refuses
     * with its own message. It is the point past which the number stops meaning
     * itself: manualTuneHz takes a uint, and QML converts a JS number to one by
     * ECMAScript ToUint32, so 5 GHz typed into "Go to" arrives as 705 MHz and
     * retunes the receiver somewhere plausible that nobody asked for.
     */
    function tuneTo(hz) {
        if (screen.viewOnly || !(hz > 0) || !(hz < 4294967296))
            return false;
        screen.hint = "";
        commands.manualTuneHz(Math.round(hz));
        return true;
    }

    /**
     * Go to @a hz: the frequency entry every frontend shares (issue #671), not
     * a tap's tune. It pauses a running scan, which stays paused for the next
     * one; tuneTo()'s uint bound holds for the same reason.
     */
    function goTo(hz) {
        if (!screen.goToAllowed || !(hz > 0) || !(hz < 4294967296))
            return false;
        screen.hint = "";
        commands.tuneHz(Math.round(hz));
        return true;
    }

    /**
     * Where a step of @a direction (+1 up the band, -1 down) from @a fromHz lands.
     *
     * Wraps inside the band it started in rather than running off the end: past
     * the top of 800 MHz there is nothing this app decodes, and a control that
     * keeps going teaches the user nothing about where they are. Off-band there
     * is nothing to wrap against and the walk is unbounded.
     *
     * Separate from stepBy() because this is the part worth pinning: the wrap
     * only happens at a band edge, which is not somewhere a test can drive the
     * receiver to.
     */
    function nextStepHz(fromHz, direction) {
        if (!(screen.stepHz > 0))
            return 0;
        var next = fromHz + (direction >= 0 ? screen.stepHz : -screen.stepHz);
        var band = Util.bandFor(fromHz);
        if (!band)
            return next;
        // Half a step in from the far edge, so the first screenful after a wrap
        // is spectrum rather than half a screen of nothing.
        if (next >= band.high)
            return band.low + (screen.stepHz / 2);
        if (next <= band.low)
            return band.high - (screen.stepHz / 2);
        return next;
    }

    /** Walk one screen of spectrum in @a direction (+1 up the band, -1 down). */
    function stepBy(direction) {
        if (!spectrum.hasData)
            return;
        var from = screen.tunedHz;
        var next = screen.nextStepHz(from, direction);
        if (!(next > 0))
            return;
        // Moving against the direction asked for can only mean the walk came back
        // round; say so, or the readout appears to jump for no reason.
        var wrapped = (direction >= 0) ? (next < from) : (next > from);
        if (!screen.tuneTo(next))
            return;
        if (wrapped)
            screen.hint = qsTr("Wrapped to %1").arg(Util.fmtMhz(next));
    }

    /** Jump to the next signal above (+1) or below (-1) the current center. */
    function hopToSignal(direction) {
        var hz = spectrum.nextPeakHz(direction);
        if (!(hz > 0)) {
            screen.hint = qsTr("Nothing else on this screen");
            return;
        }
        screen.tuneTo(hz);
    }

    // ---- Sweep ----
    // Stepping, repeated, until something is found. It is the same action as the
    // chevrons either side of it, which is why it lives between them rather than
    // as a control of its own.
    property bool sweeping: false

    function startSweep() {
        if (screen.viewOnly || !spectrum.hasData)
            return;
        screen.hint = "";
        // The band is decided once, at the start: wrapping against a boundary the
        // sweep itself crossed would make it stop somewhere it never chose.
        screen.sweeping = true;
        sweepTimer.restart();
    }

    function stopSweep() {
        if (!screen.sweeping)
            return;
        screen.sweeping = false;
        sweepTimer.stop();
    }

    function toggleSweep() {
        if (screen.sweeping)
            screen.stopSweep();
        else
            screen.startSweep();
    }

    /** One dwell has elapsed: keep what was found, or move on. */
    function sweepTick() {
        if (!screen.sweeping)
            return;
        if (metrics.syncedHere) {
            screen.stopSweep();
            screen.hint = qsTr("Found something at %1").arg(Util.fmtMhz(screen.tunedHz));
            return;
        }
        screen.stepBy(1);
    }

    Timer {
        id: sweepTimer

        // Long enough for the tuner to settle and the decoder to find sync, and
        // no shorter: each retune blocks the engine thread for up to half a
        // second, so a fast sweep spends its time stopping and starting.
        //
        // It must also outlast metrics' sync hold (kSyncHoldSeconds, 3 s in
        // metrics_model.cpp), which decays rather than clearing on a retune: a
        // lock the decoder found just before the previous step still reads as
        // locked for that long, and a shorter dwell stops the sweep on the
        // frequency it has already moved to and reports that one as the find.
        interval: 3500
        repeat: true
        running: screen.sweeping
        onTriggered: screen.sweepTick()
    }

    // This layer sits above the monitor, which stays visible underneath it.
    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // ---- Header ----
    Item {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        // The readout is never elided, so where large text leaves no room for
        // the status pill beside it, the pill takes a line of its own under it.
        readonly property bool pillBeside: backButton.width + 6 + readoutRow.implicitWidth + 12 + statusPill.width <= width
        readonly property real titleHeight: Math.max(48, titleColumn.implicitHeight)
        height: pillBeside ? titleHeight : titleHeight + 6 + statusPill.height
        // Same shield the picture carries, for the same reason: the sheets are
        // siblings anchored over the whole screen, so their scrim covers this
        // strip too, and a TapHandler never takes an exclusive grab. Without
        // this, dismissing a sheet by tapping the scrim over the header also
        // fires the back caret underneath and closes the whole view — or
        // re-opens the Go-to sheet and overwrites what was typed into it.
        enabled: !screen.sheetOpen

        IconButton {
            id: backButton
            icon: "back"
            objectName: "spectrumBack"
            anchors.left: parent.left
            y: (header.titleHeight - height) / 2
            onClicked: screen.closed()
        }

        Column {
            id: titleColumn

            anchors.left: backButton.right
            anchors.leftMargin: 6
            anchors.right: header.pillBeside ? statusPill.left : parent.right
            anchors.rightMargin: header.pillBeside ? 12 : 0
            y: (header.titleHeight - height) / 2
            spacing: 2

            // Names what the receiver is tuned to, which a scan changes on every
            // hop: read at the readings' pace (issue #782, SpeechPacer).
            MicroLabel {
                id: spectrumTitle
                objectName: "spectrumTitle"
                width: parent.width
                elide: Text.ElideRight
                Accessible.name: spectrumTitlePace.spoken
                SpeechPacer {
                    id: spectrumTitlePace
                    shown: !spectrumTitle.Accessible.ignored
                    read: function () {
                        return spectrumTitle.text;
                    }
                }
                text: {
                    if (typeof knownFrequencies === "undefined")
                        return qsTr("Spectrum");
                    var revision = knownFrequencies.revision;
                    var names = knownFrequencies.tunedNames(screen.tunedHz, metrics ? metrics.channelBandwidthHz : 0);
                    return names.length ? qsTr("Spectrum · %1").arg(names.join(" / ")) : qsTr("Spectrum");
                }
            }

            Row {
                id: readoutRow

                spacing: 6

                LiveText {
                    objectName: "spectrumCenterReadout"
                    // A drag moves it on every frame; where the radio lands is heard at once.
                    step: String(screen.tunedHz)
                    // Never elided: this is the frequency the radio is on, and a
                    // readout that drops a digit is worse than one that is tight.
                    text: screen.readoutHz > 0 ? Util.fmtMhz(screen.readoutHz) : "—"
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(21)
                    font.weight: Font.Medium
                    // Cyan while it is a request rather than a fact, the same way
                    // every other in-flight thing on this screen reads — through
                    // the retune settling, not just through the drag itself.
                    color: (bandRail.dragging || bandRail.settling) ? Theme.cyan : Theme.textPrimary
                }

                // The readout doubles as the way to leave the neighbourhood
                // entirely. Only marked as a control when it is one.
                Caret {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: screen.goToAllowed
                    color: Theme.cyan
                }
            }

            TapHandler {
                objectName: "spectrumGoToOpen"
                enabled: screen.goToAllowed
                onTapped: goToSheet.open(screen.tunedHz)
            }
        }

        Rectangle {
            id: statusPill

            objectName: "spectrumStatusPill"
            anchors.right: parent.right
            y: header.pillBeside ? (header.titleHeight - height) / 2 : header.titleHeight + 6
            width: statusLabel.implicitWidth + 22
            height: 30
            radius: Theme.radiusButton
            color: Theme.panel
            border.width: 1
            border.color: screen.viewOnly ? Theme.encBorder : screen.sweeping ? Theme.cyan : Theme.panelBorder

            ReadableText {
                id: statusLabel

                anchors.centerIn: parent
                text: screen.viewOnly ? qsTr("VIEW ONLY") : screen.sweeping ? qsTr("SWEEPING") : qsTr("TAP TO TUNE")
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(11)
                font.letterSpacing: 1.4
                color: screen.viewOnly ? Theme.magenta : screen.sweeping ? Theme.cyan : Theme.textSecondary
            }
        }
    }

    // ---- Trace, axis, waterfall ----
    Item {
        id: body

        anchors.top: header.bottom
        anchors.topMargin: Theme.gap
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        anchors.bottomMargin: Theme.screenPadding
        enabled: !screen.sheetOpen

        // Most labels the axis will carry. Shared with the Repeater below so the
        // delegate count and the request can never drift apart.
        readonly property int maxTicks: 5

        // Recomputed whenever the window moves. axisTicks() is a call, not a
        // property, so the window edges are read here to make the dependency
        // explicit — without them the labels would freeze on the first frame.
        readonly property var ticks: {
            var low = spectrum.viewLowHz;
            var high = spectrum.viewHighHz;
            if (!spectrum.hasData || high <= low)
                return [];
            return spectrum.axisTicks(body.maxTicks);
        }

        // What the decoder is making of all this. The waterfall says where the
        // energy is and says nothing at all about whether any of it is being
        // decoded — which, on a band being swept, is the only question. Sync
        // first because it is the answer; SNR second because it explains a
        // missing sync; the call last because it is the payoff. The line under
        // it says which system that is: the colour code, NAC or RAN that tells
        // one site from the next while discovering what is on a band.
        Item {
            id: statusStrip

            objectName: "spectrumStatusStrip"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            // The button's height until large text needs more for two lines.
            height: Math.max(radioButton.height, statusLines.implicitHeight)

            Column {
                id: statusLines

                objectName: "spectrumStatusLines"
                anchors.left: parent.left
                anchors.right: radioButton.visible ? radioButton.left : parent.right
                anchors.rightMargin: radioButton.visible ? Theme.gap : 0
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                // One item for a screen reader (issue #782): the sync, SNR, offset
                // and talkgroup as shown, read where it is reached. Its readings
                // rename it as they move, which Android speaks only while it is
                // focused; the line's own place never moves with them.
                Row {
                    id: statusLine
                    objectName: "spectrumStatusLine"

                    Accessible.role: Accessible.StaticText
                    readonly property bool navigationAllowed: Navigation.allows(statusLine)
                    Accessible.ignored: !navigationAllowed || !visible
                    Accessible.name: statusSpeech.spoken
                    SpeechPacer {
                        id: statusSpeech
                        group: statusLine
                        shown: !statusLine.Accessible.ignored
                        // A new sync or call is heard at once; the SNR and offset at the readings' pace.
                        step: [metrics.syncedHere, metrics.syncLabel, metrics.carrierLock, metrics.centerFreqHz,
                            callLabel.text].join("|")
                    }

                    // Wider than the line, the talkgroup gives way first, then
                    // the offset; the sync and SNR are what the strip is for.
                    readonly property real lockedWidth: syncRow.width + spacing + snrLabel.width
                    readonly property bool cfoWanted: metrics.syncedHere && Math.abs(metrics.cfoHz) >= 50
                    readonly property real callMinimum: callLabel.text.length > 0 ? Math.min(callLabel.implicitWidth, callMetrics.advanceWidth) + spacing : 0

                    width: parent.width
                    spacing: 14

                    Row {
                        id: syncRow

                        spacing: 6

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 7
                            height: 7
                            radius: 3.5
                            color: metrics.syncedHere ? Theme.cyan : Theme.textSubdued
                        }

                        Text {
                            objectName: "spectrumSyncLabel"
                            anchors.verticalCenter: parent.verticalCenter
                            // Named when there is one, because "DMR" where P25 was
                            // expected is the whole explanation for silence.
                            text: metrics.syncedHere ? metrics.syncLabel.toUpperCase() : qsTr("NO SYNC")
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(11)
                            font.letterSpacing: 1.0
                            color: metrics.syncedHere ? Theme.textPrimary : Theme.textSubdued
                        }
                    }

                    Text {
                        id: snrLabel
                        objectName: "spectrumSnr"

                        anchors.verticalCenter: parent.verticalCenter
                        text: metrics.snrValid ? Util.fixed(metrics.snrDb, 1) + " dB" : "— dB"
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: metrics.snrValid ? Theme.textSecondary : Theme.textSubdued
                    }

                    // How far off frequency the decoder is finding the signal. Only
                    // meaningful once something is locked, and only interesting when
                    // it is not ~0 — a standing offset of a few hundred Hz is the
                    // symptom of a PPM correction that does not match this dongle,
                    // which is a thing the panel one tap away can fix.
                    Text {
                        id: cfoLabel

                        objectName: "spectrumCfoLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: statusLine.cfoWanted && statusLine.lockedWidth + statusLine.spacing + implicitWidth + statusLine.callMinimum <= statusLine.width
                        text: Math.round(metrics.cfoHz) + " Hz"
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        // Loud once it is large enough to be why nothing decodes.
                        color: Math.abs(metrics.cfoHz) >= 500 ? Theme.magenta : Theme.textSecondary
                    }

                    Text {
                        id: callLabel

                        objectName: "spectrumCallLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: text.length > 0
                        width: Math.max(0, Math.min(implicitWidth, statusLine.width - statusLine.lockedWidth - (cfoLabel.visible ? statusLine.spacing + cfoLabel.width : 0) - statusLine.spacing))
                        elide: Text.ElideRight
                        // A target that has not decoded has no text (the call view
                        // leaves it empty, never "0"), so nothing is named for it.
                        text: metrics.slot1CallState === 2 ? metrics.slot1TgText : metrics.slot2CallState === 2 ? metrics.slot2TgText : ""
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.cyan

                        // Room for a few characters and the ellipsis: less than
                        // this and the offset gives up its place instead.
                        TextMetrics {
                            id: callMetrics

                            font: callLabel.font
                            text: "0000…"
                        }
                    }
                }

                // The system on this carrier, as it said so itself: only what a
                // checked message carried, never a typed-in seed or what the last
                // carrier left. Dimmed once it is what was last heard here rather
                // than what is being received. Whole tokens or none, because a
                // "SYS" cut from its value is worse than no "SYS".
                Text {
                    id: identityLabel

                    readonly property bool live: metrics.syncedHere && metrics.carrierIdentityLive
                    // Under NO SYNC, or a sync hold that still names another
                    // protocol, the line names its own.
                    readonly property var tokens: {
                        var list = metrics.carrierIdentityTokens.slice();
                        if (list.length > 0 && !live && metrics.carrierIdentityProtocol.length > 0)
                            list.unshift(metrics.carrierIdentityProtocol);
                        return list;
                    }

                    objectName: "spectrumIdentityLabel"
                    width: parent.width
                    // A line's height whether or not there is anything to say, so
                    // the line above never moves.
                    height: identityFont.height
                    // The font is read here so a text size change refits.
                    text: font.pixelSize > 0 ? Util.fitTokens(tokens, width, identityFont, " · ", " …") : ""
                    // Only for a first token wider than the whole line.
                    elide: Text.ElideRight
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    font.letterSpacing: 1.0
                    color: live ? Theme.textPrimary : Theme.textSubdued
                    Accessible.role: Accessible.StaticText
                    Accessible.name: metrics.carrierIdentityText.length === 0 ? "" : live ? qsTr("System identity: %1").arg(metrics.carrierIdentityText) : qsTr("System identity, last heard: %1").arg(metrics.carrierIdentityText)

                    FontMetrics {
                        id: identityFont

                        font: identityLabel.font
                    }
                }
            }

            // The settings that decide whether any of the above happens. Here
            // rather than in the header because the header has no room left, and
            // here rather than with the tuning controls because a saved system
            // that is view-only still needs its gain fixed.
            OutlineButton {
                id: radioButton
                objectName: "spectrumRadioButton"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: metrics.radioInput
                width: Math.max(80, implicitWidth)
                text: qsTr("Radio")
                onClicked: radioSheet.open()
            }
        }

        // The gesture surface is exactly the picture — trace, axis and waterfall —
        // and nothing else. The controls below sit outside it, so a tap that
        // misses a button cannot fall through and retune the radio.
        Item {
            id: gestureArea

            objectName: "spectrumTapArea"
            anchors.top: statusStrip.bottom
            anchors.topMargin: 4
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: resumeButton.visible ? resumeButton.top : tuning.visible ? tuning.top : (exploreButton.visible ? exploreButton.top : (replayReason.visible ? replayReason.top : parent.bottom))
            anchors.bottomMargin: (resumeButton.visible || tuning.visible || exploreButton.visible || replayReason.visible) ? Theme.gap : 0

            SpectrumTrace {
                id: trace

                objectName: "spectrumTrace"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: Math.round(parent.height * 0.32)

                model: spectrum
                lineColor: Theme.cyan
                areaColor: Qt.alpha(Theme.cyan, 0.14)
                gridColor: Theme.divider
            }

            Item {
                id: axis

                anchors.top: trace.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 18

                // Hairline where the live trace hands over to its own history.
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 1
                    color: Theme.panelBorder
                }

                // A fixed count, not the tick list itself: binding a Repeater to a
                // freshly built list destroys and recreates every label each time
                // the viewport moves, which during a drag is fifteen times a
                // second. The delegates stay put and only their bindings re-evaluate.
                //
                // The ticks are a scale, drawn in the section label's type and
                // left out of the accessibility tree (issue #782): they move and
                // rename on every retune, a scan's every hop, and the readout
                // already says where the receiver is.
                Repeater {
                    model: body.maxTicks

                    Text {
                        required property int index

                        objectName: "spectrumScaleLabel"

                        readonly property var tick: index < body.ticks.length ? body.ticks[index] : null

                        visible: tick !== null
                        // Anchored by its center on the tick, then nudged so the end
                        // labels stay inside the panel instead of hanging off it.
                        x: tick ? Math.round(Math.max(0, Math.min(axis.width - implicitWidth, (tick.xFraction * axis.width) - (implicitWidth / 2)))) : 0
                        y: Math.round((axis.height - implicitHeight) / 2)
                        text: tick ? tick.label : ""
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        font.letterSpacing: Theme.fontSize(11) * 0.18
                        color: Theme.textSubdued
                    }
                }
            }

            Waterfall {
                id: waterfall

                objectName: "spectrumWaterfall"
                anchors.top: axis.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                model: spectrum
                // The page color, not the panel: history takes ~16 s to fill, and
                // a panel-colored fresh waterfall is a bright empty slab in light
                // mode. Sharing the background makes "nothing heard yet" read as
                // nothing rather than as a hole.
                coldColor: Theme.bg
                midColor: Theme.cyan
                hotColor: Theme.magenta
            }

            // Above trace, axis and waterfall as one continuous column. Correct
            // over waterfall history only because a retune clears it, so every
            // visible row shares this frame's center — if that ever changes, so
            // must this.
            TunerMarker {
                objectName: "spectrumTunerMarker"
                anchors.fill: parent
                visible: spectrum.hasData

                tunedHz: screen.tunedHz
                channelBwHz: metrics ? metrics.channelBandwidthHz : 0
                viewLowHz: spectrum.viewLowHz
                viewSpanHz: spectrum.viewSpanHz
            }

            ReadableText {
                anchors.centerIn: waterfall
                visible: !spectrum.hasData
                text: qsTr("Waiting for signal data…")
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
                font.letterSpacing: 0.8
                color: Theme.textSubdued
            }

            // A fingertip covers several channels, so the tap snaps to the strongest
            // peak near it. The gate is an affordance only — the engine refuses the
            // tune on its own if trunking took the tuner in the meantime.
            TapHandler {
                enabled: !screen.viewOnly && spectrum.hasData
                onTapped: function (eventPoint) {
                    // Touching the spectrum is taking over; a sweep that carried on
                    // underneath would move the radio off what was just chosen.
                    screen.stopSweep();
                    screen.tuneTo(spectrum.tapFrequencyHz(eventPoint.position.x / gestureArea.width));
                }
            }

            PinchHandler {
                id: pinch

                target: null
                // Wider than the model's 1x-8x so the clamp lives in one place.
                minimumScale: 0.1
                maximumScale: 20.0

                onActiveChanged: {
                    if (!active)
                        return;
                    screen.stopSweep();
                    screen.pinchStartZoom = spectrum.zoom;
                    screen.pinchAnchorX = gestureArea.width > 0 ? (pinch.centroid.position.x / gestureArea.width) : 0.5;
                }
                onActiveScaleChanged: {
                    if (pinch.active)
                        screen.applyPinch(pinch.activeScale);
                }
            }

            DragHandler {
                id: pan

                target: null
                xAxis.enabled: true
                yAxis.enabled: false

                onActiveChanged: {
                    if (active) {
                        screen.stopSweep();
                        screen.panStartOffsetHz = spectrum.viewOffsetHz;
                        return;
                    }
                    // Exactly one retune per gesture, on release. Retuning per drag
                    // frame would block the engine thread for up to 500 ms a time.
                    // A pinch stealing the grab deactivates this handler too, and
                    // that is not a release.
                    screen.endPan(pinch.active);
                }
                onActiveTranslationChanged: {
                    if (pan.active)
                        screen.applyPan(pan.activeTranslation.x, gestureArea.width);
                }
            }
        }

        // ---- Tuning controls (exploring only) ----
        Column {
            id: tuning

            objectName: "spectrumTuning"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            spacing: 8
            visible: !screen.viewOnly

            // Where in the neighbourhood this screenful sits, and the way to move
            // it. A readout until now, which is why it invited a drag it did not
            // take.
            BandRail {
                id: bandRail

                objectName: "spectrumBandRail"
                width: parent.width
                visible: screen.tunedHz > 0

                tunedHz: screen.tunedHz
                onDraggingChanged: {
                    if (dragging)
                        screen.stopSweep();
                }
                // A refused tune has nothing to settle on: the rail holds its
                // preview until the receiver lands on what it asked for, and
                // tuneTo() turns the request down outright when something else
                // owns the tuner — so the header would read a frequency nobody is
                // on until the rail's own timeout gave up on it.
                onTuneRequested: function (hz) {
                    if (!screen.tuneTo(hz))
                        bandRail.cancelSettle();
                }
            }

            Flow {
                width: parent.width
                spacing: 10

                // Step, and the same step repeated. Putting the sweep between the
                // chevrons says what it does without a word of explanation.
                Rectangle {
                    id: stepPill

                    width: Math.max(186, stepRow.implicitWidth + 24)
                    height: Math.max(48, Theme.fontSize(14) + 24)
                    radius: Theme.radiusButton
                    color: Theme.panel
                    border.width: 1
                    border.color: screen.sweeping ? Theme.cyan : Theme.controlBorder
                    opacity: spectrum.hasData ? 1.0 : 0.5

                    Behavior on border.color {
                        ColorAnimation {
                            duration: 120
                        }
                    }

                    Row {
                        id: stepRow

                        anchors.centerIn: parent
                        spacing: 0

                        Item {
                            id: spectrumStepDownAction
                            objectName: "spectrumStepDown"
                            activeFocusOnTab: Navigation.allows(spectrumStepDownAction) && spectrum.hasData
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Tune down")
                            Accessible.onPressAction: activate()
                            Keys.onReturnPressed: activate()
                            Keys.onSpacePressed: activate()
                            FocusFrame {}
                            function activate() {
                                if (spectrum.hasData && Navigation.allows(spectrumStepDownAction)) {
                                    screen.stopSweep();
                                    screen.stepBy(-1);
                                }
                            }
                            width: 48
                            height: Math.max(48, Theme.fontSize(14) + 24)

                            Caret {
                                anchors.centerIn: parent
                                rotation: 90
                                color: Theme.buttonSecondaryText
                            }

                            TapHandler {
                                enabled: spectrum.hasData
                                onTapped: {
                                    screen.stopSweep();
                                    screen.stepBy(-1);
                                }
                            }
                        }

                        Item {
                            id: spectrumSweepToggleAction
                            objectName: "spectrumSweepToggle"
                            activeFocusOnTab: Navigation.allows(spectrumSweepToggleAction) && spectrum.hasData
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Start or stop frequency sweep")
                            // The step it sweeps by, which its label shows.
                            Accessible.description: sweepLabel.text
                            Accessible.onPressAction: activate()
                            Keys.onReturnPressed: activate()
                            Keys.onSpacePressed: activate()
                            FocusFrame {}
                            function activate() {
                                if (spectrum.hasData && Navigation.allows(spectrumSweepToggleAction)) {
                                    screen.toggleSweep();
                                }
                            }
                            width: Math.max(94, sweepLabel.implicitWidth + 26)
                            height: Math.max(48, Theme.fontSize(14) + 24)

                            Row {
                                anchors.centerIn: parent
                                spacing: 7

                                // A triangle and a square, drawn: the font's own
                                // play and stop glyphs are missing on the device.
                                Canvas {
                                    id: sweepGlyph

                                    width: 10
                                    height: 10
                                    anchors.verticalCenter: parent.verticalCenter

                                    onPaint: {
                                        var ctx = getContext("2d");
                                        ctx.reset();
                                        ctx.fillStyle = screen.sweeping ? Theme.cyan : Theme.buttonSecondaryText;
                                        if (screen.sweeping) {
                                            ctx.fillRect(1, 1, 8, 8);
                                        } else {
                                            ctx.beginPath();
                                            ctx.moveTo(1, 0);
                                            ctx.lineTo(10, 5);
                                            ctx.lineTo(1, 10);
                                            ctx.closePath();
                                            ctx.fill();
                                        }
                                    }

                                    Connections {
                                        target: screen
                                        function onSweepingChanged() {
                                            sweepGlyph.requestPaint();
                                        }
                                    }

                                    Connections {
                                        target: Theme
                                        function onDarkChanged() {
                                            sweepGlyph.requestPaint();
                                        }
                                    }
                                }

                                Text {
                                    id: sweepLabel

                                    anchors.verticalCenter: parent.verticalCenter
                                    text: spectrum.hasData ? (screen.stepHz / 1.0e6).toFixed(2) + " MHz" : "—"
                                    font.family: Theme.mono
                                    font.pixelSize: Theme.fontSize(12)
                                    color: screen.sweeping ? Theme.cyan : Theme.buttonSecondaryText
                                }
                            }

                            TapHandler {
                                enabled: spectrum.hasData
                                onTapped: screen.toggleSweep()
                            }
                        }

                        Item {
                            id: spectrumStepUpAction
                            objectName: "spectrumStepUp"
                            activeFocusOnTab: Navigation.allows(spectrumStepUpAction) && spectrum.hasData
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Tune up")
                            Accessible.onPressAction: activate()
                            Keys.onReturnPressed: activate()
                            Keys.onSpacePressed: activate()
                            FocusFrame {}
                            function activate() {
                                if (spectrum.hasData && Navigation.allows(spectrumStepUpAction)) {
                                    screen.stopSweep();
                                    screen.stepBy(1);
                                }
                            }
                            width: 48
                            height: Math.max(48, Theme.fontSize(14) + 24)

                            Caret {
                                anchors.centerIn: parent
                                rotation: -90
                                color: Theme.buttonSecondaryText
                            }

                            TapHandler {
                                enabled: spectrum.hasData
                                onTapped: {
                                    screen.stopSweep();
                                    screen.stepBy(1);
                                }
                            }
                        }
                    }
                }

                // The finer move: to the next carrier actually on screen.
                Rectangle {
                    width: 96
                    height: Math.max(48, Theme.fontSize(14) + 24)
                    radius: Theme.radiusButton
                    color: Theme.panel
                    border.width: 1
                    border.color: Theme.controlBorder
                    opacity: spectrum.hasData ? 1.0 : 0.5

                    Row {
                        anchors.centerIn: parent
                        spacing: 0

                        Item {
                            id: spectrumSignalDownAction
                            objectName: "spectrumSignalDown"
                            activeFocusOnTab: Navigation.allows(spectrumSignalDownAction) && spectrum.hasData
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Previous signal")
                            Accessible.onPressAction: activate()
                            Keys.onReturnPressed: activate()
                            Keys.onSpacePressed: activate()
                            FocusFrame {}
                            function activate() {
                                if (spectrum.hasData && Navigation.allows(spectrumSignalDownAction)) {
                                    screen.stopSweep();
                                    screen.hopToSignal(-1);
                                }
                            }
                            width: 48
                            height: Math.max(48, Theme.fontSize(14) + 24)

                            Row {
                                anchors.centerIn: parent
                                spacing: 3

                                Caret {
                                    anchors.verticalCenter: parent.verticalCenter
                                    rotation: 90
                                    color: Theme.buttonSecondaryText
                                }

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 2
                                    height: 12
                                    color: Theme.cyan
                                }
                            }

                            TapHandler {
                                enabled: spectrum.hasData
                                onTapped: {
                                    screen.stopSweep();
                                    screen.hopToSignal(-1);
                                }
                            }
                        }

                        Item {
                            id: spectrumSignalUpAction
                            objectName: "spectrumSignalUp"
                            activeFocusOnTab: Navigation.allows(spectrumSignalUpAction) && spectrum.hasData
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Next signal")
                            Accessible.onPressAction: activate()
                            Keys.onReturnPressed: activate()
                            Keys.onSpacePressed: activate()
                            FocusFrame {}
                            function activate() {
                                if (spectrum.hasData && Navigation.allows(spectrumSignalUpAction)) {
                                    screen.stopSweep();
                                    screen.hopToSignal(1);
                                }
                            }
                            width: 48
                            height: Math.max(48, Theme.fontSize(14) + 24)

                            Row {
                                anchors.centerIn: parent
                                spacing: 3

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 2
                                    height: 12
                                    color: Theme.cyan
                                }

                                Caret {
                                    anchors.verticalCenter: parent.verticalCenter
                                    rotation: -90
                                    color: Theme.buttonSecondaryText
                                }
                            }

                            TapHandler {
                                enabled: spectrum.hasData
                                onTapped: {
                                    screen.stopSweep();
                                    screen.hopToSignal(1);
                                }
                            }
                        }
                    }
                }

                OutlineButton {
                    objectName: "spectrumSaveSystem"
                    width: 96
                    height: Math.max(48, Theme.fontSize(14) + 24)
                    text: qsTr("Save")
                    enabled: screen.tunedHz > 0
                    onClicked: {
                        screen.stopSweep();
                        screen.saveAsSystem(screen.tunedHz);
                    }
                }

                // The inverse of "Explore from here": hand the tuner to trunking
                // and let it follow the system off this control channel. Inside
                // `tuning`, so !viewOnly is already answered — what is left is
                // whether there is anything here worth following. No confirm
                // sheet, unlike the way out: this loses nothing that was not
                // already the user's next tap, and the moment trunking takes the
                // tuner the escape appears in its place.
                OutlineButton {
                    objectName: "spectrumFollowSystem"
                    width: 200
                    height: Math.max(48, Theme.fontSize(14) + 24)
                    text: qsTr("Follow this system")
                    // A trunk scan keeps its targets while paused and has no
                    // trunking of its own to hand over: Resume scan is the way back.
                    visible: decoderHost.sessionActive && metrics && metrics.trunkableSync && metrics.scanKind !== 2
                    onClicked: {
                        screen.stopSweep();
                        commands.setTrunking(true);
                    }
                }
            }
        }

        // The way back to a scan the operator paused (issue #671), over whichever
        // controls the session has below it: the scan's list, cursor and avoids
        // are all still there.
        OutlineButton {
            id: resumeButton

            objectName: "spectrumResumeScan"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: tuning.visible ? tuning.top : (exploreButton.visible ? exploreButton.top : parent.bottom)
            anchors.bottomMargin: tuning.visible || exploreButton.visible ? Theme.gap : 0
            visible: screen.scanPaused && !screen.replayInput && decoderHost.sessionActive
            text: qsTr("Resume scan")
            enabled: metrics.scanResumable
            onClicked: {
                screen.stopSweep();
                commands.resumeScan();
            }
        }

        // The way out of view-only, when there is one. A saved system is a choice
        // the user made, so this offers to undo it rather than silently allowing
        // a touch to do the same thing.
        OutlineButton {
            id: exploreButton

            objectName: "spectrumExploreFromHere"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: screen.viewOnly && !screen.replayInput && decoderHost.sessionActive
            text: qsTr("Explore from here")
            onClicked: {
                // Nothing is holding the tuner, so there is nothing to warn about
                // and nothing to confirm.
                if (!screen.tunerHeld)
                    screen.exploreFromHere();
                else
                    confirmExplore.visible = true;
            }
        }

        // Where the way out would be, why there is none: a replay is a recording
        // of where the receiver went, and nothing here can move it. The engine's
        // own words, so this and a refused command never disagree.
        ReadableText {
            id: replayReason

            objectName: "spectrumReplayReason"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: screen.replayInput && decoderHost.sessionActive
            text: qsTr("An I/Q replay cannot retune.")
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            color: Theme.textSecondary
        }

        // The engine's answer to the last tap, or this screen's own. Anchored
        // above the controls rather than to the bottom of the body, which the
        // tuning row now occupies.
        Rectangle {
            objectName: "spectrumToast"

            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: resumeButton.visible ? resumeButton.top : tuning.visible ? tuning.top : (exploreButton.visible ? exploreButton.top : (replayReason.visible ? replayReason.top : parent.bottom))
            anchors.bottomMargin: Theme.gap
            // Suppressed while sweeping: every step draws an "Applied" message
            // with a three-second life, so at this cadence it would never leave
            // the screen and would read as a fault rather than as progress.
            visible: !screen.sweeping && toastLabel.text.length > 0
            width: Math.min(parent.width, toastLabel.implicitWidth + 24)
            // Two lines at most: a tune's answer names the frequency and what it did to the scan ("; scan paused",
            // issue #671), which one line on a phone cuts before the part that matters.
            height: Math.max(30, toastLabel.height + 12)
            radius: Theme.radiusButton
            color: Theme.panel
            border.width: 1
            border.color: Theme.panelBorder

            ReadableText {
                id: toastLabel

                anchors.centerIn: parent
                width: Math.min(parent.width - 24, implicitWidth)
                // This screen's own answer wins: it is always the newer one, and
                // the engine has nothing to say about an empty band.
                text: screen.hint.length > 0 ? screen.hint : metrics.uiMessage
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
                color: Theme.cyan
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
        }
    }

    // A hint is this screen talking, and it should not outlive the answer it was
    // about. The engine's own messages carry their own expiry.
    Timer {
        id: hintTimer

        interval: 4000
        running: screen.hint.length > 0
        onTriggered: screen.hint = ""
    }

    // Restarted explicitly, because `running` does not change when one hint
    // replaces another: without this the second hint inherits whatever is left
    // of the first one's four seconds, and a "Wrapped to ..." followed by
    // "Nothing else on this screen" flashes the second one for a moment.
    onHintChanged: {
        if (screen.hint.length > 0)
            hintTimer.restart();
    }

    // ---- Go to ----
    ModalSheet {
        id: goToSheet

        objectName: "spectrumGoToSheet"
        // A form sheet (issue #718): an outside tap keeps a typed frequency;
        // Cancel, Back and Escape ask first.
        closesOnOutsideTap: false

        // The frequency the sheet opened on: a dismissal asks only once it differs.
        property string openedText: ""

        function open(hz) {
            goToField.text = Util.mhzValueText(hz);
            openedText = goToField.text;
            visible = true;
            goToField.forceActiveFocus();
        }

        function requestClose() {
            if (goToField.text !== openedText)
                goToDiscard.ask(function () {
                    goToSheet.visible = false;
                });
            else
                visible = false;
        }
        dismissHandler: requestClose

        DiscardDialog {
            id: goToDiscard
            parent: goToSheet
        }

        function submit() {
            var mhz = parseFloat(goToField.text);
            if (isNaN(mhz) || !(mhz > 0))
                return;
            goToSheet.visible = false;
            Qt.inputMethod.hide();
            screen.stopSweep();
            // The field's validator accepts up to 99999 MHz and goTo() refuses
            // anything past its own uint bound, so a number this sheet took can
            // still be turned down. Reported rather than dropped: the sheet
            // vanishing with the receiver where it was reads as the app ignoring
            // the entry. Said after the dismissal because the toast sits under
            // the sheet.
            if (!screen.goTo(mhz * 1.0e6))
                screen.hint = qsTr("Cannot tune to %1 MHz").arg(Util.mhzText(mhz * 1.0e6));
        }

        // Read in the field's name (issue #782), not as a heading of its own.
        MicroLabel {
            objectName: "spectrumGoToCaption"
            text: qsTr("Go to")
            Accessible.ignored: true
        }

        FrequencyField {
            id: goToField
            accessibleName: qsTr("Go to frequency in MHz")

            width: parent.width
            fieldObjectName: "spectrumGoToField"
            // A shade smaller than the setup screens': this panel also carries
            // the band pills and the button, over a live waterfall.
            pixelSize: 30
            spacing: 12
            onAccepted: goToSheet.submit()
        }

        // Typing a frequency and jumping to a band are the same intent at
        // different precisions, so they share one panel.
        Flow {
            width: parent.width
            spacing: 8

            Repeater {
                model: Util.BANDS

                FilterPill {
                    required property var modelData

                    objectName: "spectrumBand_" + modelData.label
                    text: modelData.label
                    caret: false
                    active: screen.band ? screen.band.label === modelData.label : false
                    onClicked: goToField.text = Util.mhzValueText(modelData.start)
                }
            }
        }

        // What Tune costs while a scan has the tuner, said before it is paid.
        Text {
            objectName: "spectrumGoToScanNote"
            width: parent.width
            visible: screen.scanOwnsTuner
            text: qsTr("Tuning pauses the scan until you resume it.")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }

        OutlineButton {
            width: parent.width
            objectName: "spectrumGoToConfirm"
            text: qsTr("Tune")
            onClicked: goToSheet.submit()
        }

        OutlineButton {
            width: parent.width
            objectName: "spectrumGoToCancel"
            text: qsTr("Cancel")
            onClicked: goToSheet.requestClose()
        }
    }

    // ---- Radio settings ----
    Loader {
        id: localRadioLoader
        anchors.fill: parent
        active: screen.sharedRadioSheet === null
        sourceComponent: RadioSheet {}
    }

    // ---- Explore from here, confirmed ----
    ModalSheet {
        id: confirmExplore

        objectName: "spectrumExploreConfirm"

        ReadableText {
            heading: true
            width: parent.width
            text: qsTr("Explore from here?")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(17)
            font.weight: Font.Bold
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }

        // Names what stops, and — the part people actually worry about —
        // what does not. A scan pauses rather than stops (issue #671), and
        // Resume scan is the way back.
        ReadableText {
            objectName: "spectrumExploreConfirmText"
            width: parent.width
            text: metrics.scanKind === 2 ? qsTr("This pauses the scan. Resume scan picks it up again; the scan list itself is unchanged.") : metrics.scanKind === 1 ? qsTr("This pauses stepping through the channel list. Resume scan picks it up again; the saved system itself is unchanged.") : qsTr("This stops following calls across channels for now. The saved system itself is unchanged.")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }

        OutlineButton {
            objectName: "spectrumExploreConfirmAccept"
            width: parent.width
            text: qsTr("Explore from here")
            onClicked: {
                confirmExplore.visible = false;
                screen.exploreFromHere();
            }
        }

        OutlineButton {
            width: parent.width
            text: qsTr("Cancel")
            onClicked: confirmExplore.visible = false
        }
    }
}
