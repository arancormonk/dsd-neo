// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The primary action: a 1.5px cyan→magenta gradient border wrapping a quiet fill.
// The gradient is the accent; the inside stays calm so the label carries.
Item {
    id: control

    property string text: ""
    signal clicked

    property string accessibleName: text
    activeFocusOnTab: enabled && visible && Navigation.allows(control)
    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    Accessible.focusable: true
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !visible || !navigationAllowed
    Accessible.onPressAction: activate()
    function activate() {
        if (enabled && Navigation.allows(control))
            clicked();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    Keys.onEnterPressed: activate()
    FocusFrame {}

    implicitHeight: Math.max(Theme.minimumTouchSize, label.implicitHeight + 24)

    // Disabled dims the gradient stroke and label but keeps the fill opaque —
    // control-level opacity would let the gradient bleed through the inside.
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusButton
        opacity: control.enabled ? 1.0 : 0.35
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0.0
                color: Theme.cyan
            }
            GradientStop {
                position: 1.0
                color: Theme.magenta
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 1.5
        radius: Theme.radiusButton - 1.5
        color: Theme.dark ? Theme.bg : Theme.panel

        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: Qt.alpha(Theme.cyan, tap.pressed && control.enabled ? 0.10 : 0.0)

            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }
        }
    }

    Text {
        id: label
        anchors.centerIn: parent
        width: Math.max(0, control.width - 24)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: control.text
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(15)
        font.weight: Font.Bold
        opacity: control.enabled ? 1.0 : 0.45
        color: Theme.textPrimary
    }

    TapHandler {
        id: tap
        enabled: control.enabled
        onTapped: control.activate()
    }
}
