// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import DsdNeo

// The band around the tuned frequency, and a way to move to a signal by
// touching it. Changing frequency otherwise means stopping the session, editing
// the system and starting again, which is hopeless when what you are doing is
// hunting for activity.
//
// The waterfall is the screen: everything else stays quiet so the eye goes to
// the one thing that shows where the traffic is.
Item {
    id: screen

    objectName: "spectrumScreen"

    signal closed()

    // While the trunking controller owns the tuner the engine refuses manual
    // retunes, so this is view-only rather than a control that does nothing.
    readonly property bool viewOnly: metrics ? metrics.trunkingEnabled : false
    // The frame's own center is what the bins were measured at; the options
    // reading is only a fallback for the moment before the first frame lands.
    readonly property real tunedHz: spectrum.hasData ? spectrum.centerFreqHz
                                                     : (metrics ? metrics.centerFreqHz : 0)

    // Producing frames is what costs battery, so it follows the screen being up
    // and a session running — not merely the object existing.
    Binding {
        target: spectrum
        property: "active"
        value: screen.visible && decoderHost.sessionActive
    }

    // Gesture state and the steps that act on it live here rather than inside
    // the handlers: a handler body cannot be called from a test, and these are
    // the parts that decide where the receiver ends up.
    property real pinchStartZoom: 1.0
    property real pinchAnchorX: 0.5
    property real panStartOffsetHz: 0

    /** Zoom to @a activeScale relative to where the pinch began, held at its anchor. */
    function applyPinch(activeScale) {
        spectrum.zoomToAnchored(screen.pinchStartZoom * activeScale, screen.pinchAnchorX)
    }

    /** Pan by @a dx pixels from where the drag began, across @a width pixels of view. */
    function applyPan(dx, width) {
        if (width <= 0)
            return
        // Dragging right walks the window down the band, as if pulling the
        // spectrum along under the finger.
        spectrum.viewOffsetHz = screen.panStartOffsetHz - ((dx / width) * spectrum.viewSpanHz)
    }

    /** Settle a finished pan: at most one retune, and only if it ran off the edge. */
    function endPan() {
        if (!screen.viewOnly && spectrum.edgeOvershootHz !== 0 && spectrum.hasData)
            commands.manualTuneHz(Math.round(spectrum.centerFreqHz + spectrum.edgeOvershootHz))
        spectrum.clearOvershoot()
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
        height: 48

        Item {
            id: backButton

            objectName: "spectrumBack"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 32
            height: 32

            Caret {
                anchors.centerIn: parent
                // Points down at rotation 0; a quarter turn clockwise reads as back.
                rotation: 90
                color: Theme.textPrimary
            }

            TapHandler {
                onTapped: screen.closed()
            }
        }

        Column {
            anchors.left: backButton.right
            anchors.leftMargin: 6
            anchors.right: statusPill.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            MicroLabel {
                text: qsTr("Spectrum")
            }

            Text {
                objectName: "spectrumCenterReadout"
                width: parent.width
                text: screen.tunedHz > 0 ? (screen.tunedHz / 1.0e6).toFixed(4) + " MHz" : "—"
                font.family: Theme.mono
                font.pixelSize: 21
                font.weight: Font.Medium
                color: Theme.textPrimary
                elide: Text.ElideRight
            }
        }

        Rectangle {
            id: statusPill

            objectName: "spectrumStatusPill"
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: statusLabel.implicitWidth + 22
            height: 30
            radius: Theme.radiusButton
            color: Theme.panel
            border.width: 1
            border.color: screen.viewOnly ? Theme.encBorder : Theme.panelBorder

            Text {
                id: statusLabel

                anchors.centerIn: parent
                text: screen.viewOnly ? qsTr("VIEW ONLY") : qsTr("TAP TO TUNE")
                font.family: Theme.mono
                font.pixelSize: 11
                font.letterSpacing: 1.4
                color: screen.viewOnly ? Theme.magenta : Theme.textSecondary
            }
        }
    }

    // ---- Trace, axis, waterfall ----
    Item {
        id: body

        objectName: "spectrumTapArea"

        anchors.top: header.bottom
        anchors.topMargin: Theme.gap
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        anchors.bottomMargin: Theme.screenPadding

        // Recomputed whenever the window moves. axisTicks() is a call, not a
        // property, so the window edges are read here to make the dependency
        // explicit — without them the labels would freeze on the first frame.
        readonly property var ticks: {
            var low = spectrum.viewLowHz
            var high = spectrum.viewHighHz
            if (!spectrum.hasData || high <= low)
                return []
            return spectrum.axisTicks(5)
        }

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

            Repeater {
                model: body.ticks

                MicroLabel {
                    // Anchored by its center on the tick, then nudged so the end
                    // labels stay inside the panel instead of hanging off it.
                    x: Math.round(Math.max(0, Math.min(axis.width - implicitWidth,
                                                       (modelData.xFraction * axis.width) - (implicitWidth / 2))))
                    y: Math.round((axis.height - implicitHeight) / 2)
                    text: modelData.label
                    font.capitalization: Font.MixedCase
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

        Text {
            anchors.centerIn: waterfall
            visible: !spectrum.hasData
            text: qsTr("Waiting for signal data…")
            font.family: Theme.mono
            font.pixelSize: 12
            font.letterSpacing: 0.8
            color: Theme.textSubdued
        }

        // The engine's answer to the last tap. The monitor shows this too, but it
        // is underneath this layer — without it here a refused tune (trunking
        // took the tuner, backend cannot retune) reads as a screen that ignored
        // the touch.
        Rectangle {
            objectName: "spectrumToast"

            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Theme.gap
            visible: metrics.uiMessage.length > 0
            width: Math.min(parent.width, toastLabel.implicitWidth + 24)
            height: 30
            radius: Theme.radiusButton
            color: Theme.panel
            border.width: 1
            border.color: Theme.panelBorder

            Text {
                id: toastLabel

                anchors.centerIn: parent
                width: Math.min(parent.width - 24, implicitWidth)
                text: metrics.uiMessage
                font.family: Theme.mono
                font.pixelSize: 12
                color: Theme.cyan
                elide: Text.ElideRight
            }
        }

        // A fingertip covers several channels, so the tap snaps to the strongest
        // peak near it. The gate is an affordance only — the engine refuses the
        // tune on its own if trunking took the tuner in the meantime.
        TapHandler {
            enabled: !screen.viewOnly && spectrum.hasData
            onTapped: function (eventPoint) {
                var hz = spectrum.tapFrequencyHz(eventPoint.position.x / body.width)
                if (hz > 0)
                    commands.manualTuneHz(Math.round(hz))
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
                    return
                screen.pinchStartZoom = spectrum.zoom
                screen.pinchAnchorX = body.width > 0 ? (pinch.centroid.position.x / body.width) : 0.5
            }
            onActiveScaleChanged: {
                if (pinch.active)
                    screen.applyPinch(pinch.activeScale)
            }
        }

        DragHandler {
            id: pan

            target: null
            xAxis.enabled: true
            yAxis.enabled: false

            onActiveChanged: {
                if (active) {
                    screen.panStartOffsetHz = spectrum.viewOffsetHz
                    return
                }
                // Exactly one retune per gesture, on release. Retuning per drag
                // frame would block the engine thread for up to 500 ms a time.
                screen.endPan()
            }
            onActiveTranslationChanged: {
                if (pan.active)
                    screen.applyPan(pan.activeTranslation.x, body.width)
            }
        }
    }
}
