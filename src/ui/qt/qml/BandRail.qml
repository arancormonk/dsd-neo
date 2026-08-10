// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Coarse tuning: a local window of band with the receiver in it, dragged.
//
// It used to be a readout with a track and a handle, which invited a drag it did
// not accept, and it was scoped to a hardcoded band the tuner reaches far
// outside of. Now the window simply travels with the receiver, so there is no
// boundary to explain — only the tuner's own ends.
//
// The arithmetic lives in functions rather than in the DragHandler's body
// because a handler body cannot be called from a test, and this is where the
// receiver's frequency is decided.
Item {
    id: rail

    property real tunedHz: 0
    // Full width of the window. One end-to-end drag moves twenty MHz, about
    // fourteen screenfuls of spectrum — coarse, which is the job; the picture
    // above does the fine work.
    property real windowHz: 20.0e6

    signal tuneRequested(double hz)

    property bool dragging: false
    property real pendingHz: 0
    property real dragStartHz: 0

    // Clamped in this order so that near a tuner limit the window stops sliding
    // and the knob moves off centre instead — the alternative is a window that
    // runs off the end of what the hardware can reach.
    readonly property real lowHz: {
        var lo = Math.max(Util.TUNER_LOW_HZ, rail.tunedHz - (rail.windowHz / 2))
        var hi = Math.min(Util.TUNER_HIGH_HZ, lo + rail.windowHz)
        return Math.max(Util.TUNER_LOW_HZ, hi - rail.windowHz)
    }
    readonly property real highHz: Math.min(Util.TUNER_HIGH_HZ, rail.lowHz + rail.windowHz)
    readonly property real displayHz: rail.dragging ? rail.pendingHz : rail.tunedHz

    implicitHeight: 32

    /** Take hold at the current frequency. */
    function beginDrag() {
        rail.dragStartHz = rail.tunedHz
        rail.pendingHz = rail.tunedHz
        rail.dragging = true
    }

    /**
     * Move @a dx pixels from where the drag began, across @a trackWidth pixels.
     *
     * Relative to the grab, never absolute to the touch: a knob that jumped to
     * the finger would turn a mis-aimed tap into a twenty-megahertz move. The
     * window cannot shift underneath this, because it derives from tunedHz and
     * nothing retunes until release.
     */
    function dragBy(dx, trackWidth) {
        if (!rail.dragging || !(trackWidth > 0))
            return
        var want = rail.dragStartHz + ((dx / trackWidth) * (rail.highHz - rail.lowHz))
        rail.pendingHz = Math.max(Util.TUNER_LOW_HZ, Math.min(Util.TUNER_HIGH_HZ, want))
    }

    /**
     * Let go: at most one retune, and only if the finger actually lifted.
     *
     * @a cancelled marks a drag that ended some other way. Those few pixels are
     * not a request to move the receiver.
     */
    function endDrag(cancelled) {
        var want = rail.pendingHz
        var moved = rail.dragging && Math.round(want) !== Math.round(rail.tunedHz)
        rail.dragging = false
        if (!cancelled && moved)
            rail.tuneRequested(want)
    }

    Text {
        id: railLow

        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: Math.round(rail.lowHz / 1.0e6)
        font.family: Theme.mono
        font.pixelSize: 10
        color: Theme.textSubdued
    }

    Text {
        id: railHigh

        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: Math.round(rail.highHz / 1.0e6)
        font.family: Theme.mono
        font.pixelSize: 10
        color: Theme.textSubdued
    }

    Item {
        id: track

        anchors.left: railLow.right
        anchors.right: railHigh.left
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.top: parent.top
        anchors.bottom: parent.bottom

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: Theme.divider
        }

        // The app's own knob, from PlexSwitch: round, cyan, with the hairline
        // light mode needs to keep it off a pale track. Borrowed rather than
        // invented because the whole defect here was a control that did not look
        // like one, and this is the shape the app already uses to say so.
        Rectangle {
            id: knob

            objectName: "spectrumBandMarker"
            width: 16
            height: 16
            radius: 8
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.cyan
            border.width: Theme.dark ? 0 : 1
            border.color: Theme.controlBorder
            x: {
                var span = rail.highHz - rail.lowHz
                if (!(span > 0))
                    return 0
                var t = (rail.displayHz - rail.lowHz) / span
                return Math.round(Math.max(0, Math.min(1, t)) * (track.width - width))
            }

            // Eases back to centre after a retune, which reads as the window
            // having come along. Off during a drag, where it would only lag the
            // finger.
            Behavior on x {
                enabled: !rail.dragging
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }
        }
    }

    DragHandler {
        target: null
        xAxis.enabled: true
        yAxis.enabled: false

        onActiveChanged: {
            if (active)
                rail.beginDrag()
            else
                rail.endDrag(false)
        }
        onActiveTranslationChanged: {
            if (active)
                rail.dragBy(activeTranslation.x, track.width)
        }
    }
}
