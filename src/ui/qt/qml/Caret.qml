// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// A small drawn caret. The bundled faces have no geometric-triangle glyphs, so
// every disclosure marker draws this instead of a character. Points down at
// rotation 0; rotate 180 for up, -90 for right.
Item {
    id: caret

    property color color: Theme.textSubdued

    implicitWidth: 9
    implicitHeight: 6

    Canvas {
        id: canvas

        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = String(caret.color)
            ctx.lineWidth = 1.6
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.beginPath()
            ctx.moveTo(1, 1.2)
            ctx.lineTo(width / 2, height - 1.2)
            ctx.lineTo(width - 1, 1.2)
            ctx.stroke()
        }

        Connections {
            target: Theme
            function onDarkChanged() { canvas.requestPaint() }
        }
    }

    onColorChanged: canvas.requestPaint()
}
