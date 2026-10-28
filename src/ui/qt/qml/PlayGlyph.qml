// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #708: the small drawn play triangle a call row carries when the call has
// a recording. Drawn like Caret, since the bundled faces have no geometric
// glyphs; nudged right of centre, as PlayCircle's is, so it does not read
// left-heavy.
Item {
    id: glyph

    property color color: Theme.cyan

    implicitWidth: 9
    implicitHeight: 10
    Accessible.ignored: true

    Canvas {
        id: canvas

        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.fillStyle = String(glyph.color)
            ctx.beginPath()
            ctx.moveTo(1, 0.5)
            ctx.lineTo(width - 0.5, height / 2)
            ctx.lineTo(1, height - 0.5)
            ctx.closePath()
            ctx.fill()
        }

        Connections {
            target: Theme
            function onDarkChanged() { canvas.requestPaint() }
        }
    }

    onColorChanged: canvas.requestPaint()
}
