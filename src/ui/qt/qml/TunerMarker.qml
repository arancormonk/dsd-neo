// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Where the receiver is, and how much of the band it is actually listening to.
//
// Neutral rather than cyan or magenta: the waterfall ramp runs background ->
// cyan -> magenta, so both accents already mean signal down there, and a
// coloured marker would read as a strong carrier. Value and form carry it
// instead — the highest-contrast token in either theme, in a shape no data
// element has.
//
// The cap is a flat square tick on purpose. Anything rounded or raised at the
// top of a vertical line reads as a grab handle, and this does not move: the way
// to retune is to tap the spectrum. The band rail's round knob is the thing you
// drag, and the contrast between the two shapes is doing real work.
Item {
    id: marker

    property real tunedHz: 0
    property real channelBwHz: 0
    property real viewLowHz: 0
    property real viewSpanHz: 0

    // Same mapping the trace and the axis labels use (SpectrumTraceItem::paint,
    // SpectrumModel::axisTicks). Anything else puts the marker off the carrier
    // it is naming.
    readonly property real xFraction: viewSpanHz > 0 ? (tunedHz - viewLowHz) / viewSpanHz : -1
    readonly property real bandWidthFraction: (viewSpanHz > 0 && channelBwHz > 0)
                                              ? channelBwHz / viewSpanHz : 0
    readonly property bool onScreen: xFraction >= 0 && xFraction <= 1

    // The one pixel column, hairline and tick are all centred on, so the three
    // cannot drift apart from each other by a rounding of their own.
    readonly property real centerX: Math.round(xFraction * width)

    // The picture's own edge, not the marker's: at high zoom near a view edge
    // the column's outer half would otherwise spill past it into the screen
    // padding this Item is not clipped by anywhere else.
    clip: true

    // No handlers anywhere in here: taps, pinches and drags belong to the
    // gesture area underneath, and an Item that accepted them would make the
    // spectrum untappable exactly where the receiver is.

    Rectangle {
        objectName: "spectrumTunerColumn"
        visible: marker.onScreen && marker.bandWidthFraction > 0
        // Lit on a dark ground, shaded on a light one. A fixed white wash is
        // invisible over the light theme's waterfall.
        color: Qt.alpha(Theme.textPrimary, Theme.dark ? 0.10 : 0.07)
        y: 0
        height: parent.height
        width: Math.max(2, Math.round(marker.bandWidthFraction * parent.width))
        x: marker.centerX - (width / 2)
    }

    Rectangle {
        id: hairline

        objectName: "spectrumTunerHairline"
        visible: marker.onScreen
        color: Theme.textPrimary
        width: 1
        y: 0
        height: parent.height
        x: marker.centerX - (width / 2)
    }

    Rectangle {
        objectName: "spectrumTunerTick"
        visible: marker.onScreen
        color: Theme.textPrimary
        width: 9
        height: 3
        y: 0
        x: marker.centerX - (width / 2)
    }

    // Off screen the marker is not drawn at all — clamping it to the edge would
    // sit it on a carrier it is not tuned to. This says which way it went.
    Caret {
        objectName: "spectrumTunerEdgeHint"
        visible: !marker.onScreen && marker.viewSpanHz > 0
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: marker.xFraction < 0 ? parent.left : undefined
        anchors.right: marker.xFraction > 1 ? parent.right : undefined
        anchors.margins: 2
        rotation: marker.xFraction < 0 ? 90 : -90
        color: Theme.textPrimary
    }
}
