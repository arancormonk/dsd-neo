// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// "+ Add a system": a dashed outline with cyan text — an invitation, not a card.
Item {
    id: control

    property string text: ""
    signal clicked

    activeFocusOnTab: Navigation.allows(control) && enabled
    Accessible.role: Accessible.Button
    Accessible.name: text
    // Issue #782: focusable only while in the tree, so a copy Android still
    // holds of a covered or hidden control (no event tells it the control
    // left the tree) is not a swipe stop.
    Accessible.focusable: navigationAllowed
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !navigationAllowed
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !Navigation.allows(control))
            return;
        // Issue #718: finish the keyboard's word and the field's editing before
        // the action reads the form.
        Navigation.commitComposing(true);
        clicked();
    }
    Keys.onReturnPressed: activate()
    Keys.onSpacePressed: activate()
    FocusFrame {}
    implicitWidth: Math.max(Theme.minimumTouchSize, Math.ceil(label.implicitWidth) + 24)
    implicitHeight: Math.max(54, label.implicitHeight + 24)

    Canvas {
        id: outline
        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            ctx.strokeStyle = String(Theme.controlBorder);
            ctx.lineWidth = 1;
            ctx.setLineDash([5, 5]);
            ctx.beginPath();
            var r = Theme.radiusPanel;
            ctx.moveTo(r + 0.5, 0.5);
            ctx.lineTo(width - r - 0.5, 0.5);
            ctx.arcTo(width - 0.5, 0.5, width - 0.5, r + 0.5, r);
            ctx.lineTo(width - 0.5, height - r - 0.5);
            ctx.arcTo(width - 0.5, height - 0.5, width - r - 0.5, height - 0.5, r);
            ctx.lineTo(r + 0.5, height - 0.5);
            ctx.arcTo(0.5, height - 0.5, 0.5, height - r - 0.5, r);
            ctx.lineTo(0.5, r + 0.5);
            ctx.arcTo(0.5, 0.5, r + 0.5, 0.5, r);
            ctx.stroke();
        }

        Connections {
            target: Theme
            function onDarkChanged() {
                outline.requestPaint();
            }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    Text {
        id: label
        width: parent.width - 24
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        anchors.centerIn: parent
        text: control.text
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(15)
        font.weight: Font.DemiBold
        color: Theme.cyan
    }

    TapHandler {
        onTapped: control.activate()
    }
}
