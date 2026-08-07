// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The per-system play control: a 52px circle, gradient-filled for the featured
// system and outlined for the rest, with a drawn triangle glyph.
Item {
    id: control

    property bool featured: false
    signal clicked()

    implicitWidth: 52
    implicitHeight: 52
    opacity: enabled ? 1.0 : 0.5

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: control.featured ? "transparent" : Theme.panel
        border.width: control.featured ? 0 : 1
        border.color: Theme.controlBorder
        gradient: control.featured ? fillGradient : null

        Gradient {
            id: fillGradient
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Theme.cyan }
            GradientStop { position: 1.0; color: Theme.magenta }
        }
    }

    Canvas {
        id: glyph
        anchors.centerIn: parent
        width: 18
        height: 18
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.fillStyle = control.featured
                    ? (Theme.dark ? "#0E1116" : "#FFFFFF")
                    : String(Theme.textSubdued)
            // Nudged right of center: an exactly centered triangle reads left-heavy.
            ctx.beginPath()
            ctx.moveTo(4, 1.5)
            ctx.lineTo(16, 9)
            ctx.lineTo(4, 16.5)
            ctx.closePath()
            ctx.fill()
        }

        Connections {
            target: Theme
            function onDarkChanged() { glyph.requestPaint() }
        }
        onVisibleChanged: requestPaint()
    }

    // Repaint when featured flips — the glyph color depends on it.
    onFeaturedChanged: glyph.requestPaint()

    TapHandler {
        enabled: control.enabled
        onTapped: control.clicked()
    }
}
