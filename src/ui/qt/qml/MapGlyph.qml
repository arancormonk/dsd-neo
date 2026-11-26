// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #800: the map controls' drawn glyphs, in Caret's stroke (the bundled
// faces have no icon glyphs, and the app has no icon font). One 20 px box,
// one stroke weight, round caps:
//   "plus" / "minus"  zoom in and out;
//   "fit"             four frame corners: show every radio;
//   "locate"          a crosshair ring: find my position;
//   "follow"          the same ring with its centre filled: following it;
//   "map"             a folded map: open the map (History's top bar);
//   "play"            a triangle, nudged right of centre as PlayCircle's is:
//                     History's Play all (issue #909);
//   "previous" / "next"  a bar and a filled triangle: the now-playing bar's
//                     Previous call and Next call (issue #909).
Item {
    id: glyph

    property string shape: "plus"
    property color color: Theme.textPrimary

    implicitWidth: 20
    implicitHeight: 20
    Accessible.ignored: true

    Canvas {
        id: canvas

        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = String(glyph.color)
            ctx.fillStyle = String(glyph.color)
            ctx.lineWidth = 1.8
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            var w = width
            var h = height
            var c = w / 2
            ctx.beginPath()
            if (glyph.shape === "plus" || glyph.shape === "minus") {
                ctx.moveTo(3, h / 2)
                ctx.lineTo(w - 3, h / 2)
                if (glyph.shape === "plus") {
                    ctx.moveTo(c, 3)
                    ctx.lineTo(c, h - 3)
                }
                ctx.stroke()
            } else if (glyph.shape === "map") {
                ctx.moveTo(2, 5); ctx.lineTo(7, 3); ctx.lineTo(13, 5); ctx.lineTo(18, 3)
                ctx.lineTo(18, 15); ctx.lineTo(13, 17); ctx.lineTo(7, 15); ctx.lineTo(2, 17)
                ctx.closePath()
                ctx.moveTo(7, 3); ctx.lineTo(7, 15)
                ctx.moveTo(13, 5); ctx.lineTo(13, 17)
                ctx.stroke()
            } else if (glyph.shape === "play") {
                ctx.moveTo(6, 3.5); ctx.lineTo(16.5, 10); ctx.lineTo(6, 16.5)
                ctx.closePath()
                ctx.stroke()
            } else if (glyph.shape === "previous" || glyph.shape === "next") {
                // Drawn as "next", mirrored for "previous".
                if (glyph.shape === "previous") {
                    ctx.translate(w, 0)
                    ctx.scale(-1, 1)
                }
                ctx.moveTo(4, 4); ctx.lineTo(13, 10); ctx.lineTo(4, 16)
                ctx.closePath()
                ctx.fill()
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(16, 4); ctx.lineTo(16, 16)
                ctx.stroke()
            } else if (glyph.shape === "fit") {
                var a = 3, b = 8
                ctx.moveTo(a, b); ctx.lineTo(a, a); ctx.lineTo(b, a)
                ctx.moveTo(w - b, a); ctx.lineTo(w - a, a); ctx.lineTo(w - a, b)
                ctx.moveTo(w - a, h - b); ctx.lineTo(w - a, h - a); ctx.lineTo(w - b, h - a)
                ctx.moveTo(b, h - a); ctx.lineTo(a, h - a); ctx.lineTo(a, h - b)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(c, h / 2, 2, 0, 2 * Math.PI)
                ctx.fill()
            } else {
                ctx.arc(c, h / 2, 5.5, 0, 2 * Math.PI)
                ctx.moveTo(c, 1); ctx.lineTo(c, 4.5)
                ctx.moveTo(c, h - 4.5); ctx.lineTo(c, h - 1)
                ctx.moveTo(1, h / 2); ctx.lineTo(4.5, h / 2)
                ctx.moveTo(w - 4.5, h / 2); ctx.lineTo(w - 1, h / 2)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(c, h / 2, glyph.shape === "follow" ? 3 : 1.4, 0, 2 * Math.PI)
                ctx.fill()
            }
        }

        Connections {
            target: Theme
            function onDarkChanged() { canvas.requestPaint() }
        }
    }

    onColorChanged: canvas.requestPaint()
    onShapeChanged: canvas.requestPaint()
}
