// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Secondary action: 1px control-border outline, quiet fill, secondary text.
Rectangle {
    id: control

    property string text: ""
    signal clicked()

    implicitWidth: Math.max(44, label.implicitWidth + 2 * Theme.cardPadding)
    implicitHeight: Math.max(44, 30 * Theme.fontScale)
    radius: Theme.radiusButton
    color: tap.pressed && control.enabled ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
    border.width: 1
    border.color: Theme.controlBorder
    opacity: enabled ? 1.0 : 0.5

    Behavior on color {
        ColorAnimation { duration: 120 }
    }

    Text {
        id: label
        anchors.centerIn: parent
        text: control.text
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(15)
        font.weight: Font.DemiBold
        color: Theme.buttonSecondaryText
    }

    TapHandler {
        id: tap
        enabled: control.enabled
        onTapped: control.clicked()
    }
}
