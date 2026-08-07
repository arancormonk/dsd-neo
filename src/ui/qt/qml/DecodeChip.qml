// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Single-select chip: outlined at rest, gradient-bordered with a faint cyan fill
// when selected.
Item {
    id: control

    property string text: ""
    property bool selected: false
    signal clicked()

    implicitWidth: label.implicitWidth + 32
    implicitHeight: 40

    // Gradient border only exists while selected; at rest a plain 1px outline.
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusChip
        visible: control.selected
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Theme.cyan }
            GradientStop { position: 1.0; color: Theme.magenta }
        }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: control.selected ? 1.5 : 0
        radius: control.selected ? Theme.radiusChip - 1.5 : Theme.radiusChip
        color: control.selected ? Theme.chipSelectedFill : Theme.panel
        border.width: control.selected ? 0 : 1
        border.color: Theme.controlBorder

        // The selected fill is translucent, so back it with the base surface —
        // otherwise the gradient behind shows through the tint.
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            z: -1
            visible: control.selected
            color: Theme.dark ? Theme.bg : Theme.panel
        }
    }

    Text {
        id: label
        anchors.centerIn: parent
        text: control.text
        font.family: Theme.sans
        font.pixelSize: 14
        font.weight: control.selected ? Font.Bold : Font.DemiBold
        color: control.selected ? Theme.textPrimary : Theme.buttonSecondaryText
    }

    TapHandler {
        onTapped: control.clicked()
    }
}
