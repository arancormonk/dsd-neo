// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// "+ Add a system": a dashed outline with cyan text — an invitation, not a card.
Item {
    id: control

    property string text: ""
    signal clicked()

    implicitHeight: 54

    Canvas {
        id: outline
        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = String(Theme.controlBorder)
            ctx.lineWidth = 1
            ctx.setLineDash([5, 5])
            ctx.beginPath()
            var r = Theme.radiusPanel
            ctx.moveTo(r + 0.5, 0.5)
            ctx.lineTo(width - r - 0.5, 0.5)
            ctx.arcTo(width - 0.5, 0.5, width - 0.5, r + 0.5, r)
            ctx.lineTo(width - 0.5, height - r - 0.5)
            ctx.arcTo(width - 0.5, height - 0.5, width - r - 0.5, height - 0.5, r)
            ctx.lineTo(r + 0.5, height - 0.5)
            ctx.arcTo(0.5, height - 0.5, 0.5, height - r - 0.5, r)
            ctx.lineTo(0.5, r + 0.5)
            ctx.arcTo(0.5, 0.5, r + 0.5, 0.5, r)
            ctx.stroke()
        }

        Connections {
            target: Theme
            function onDarkChanged() { outline.requestPaint() }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    Text {
        anchors.centerIn: parent
        text: control.text
        font.family: Theme.sans
        font.pixelSize: 15
        font.weight: Font.DemiBold
        color: Theme.cyan
    }

    TapHandler {
        onTapped: control.clicked()
    }
}
