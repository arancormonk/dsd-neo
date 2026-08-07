// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The toggle: cyan-tinted track and cyan knob when on, control-border track when off.
Item {
    id: control

    property bool checked: false
    property bool enabled: true
    signal toggled(bool checked)

    implicitWidth: 46
    implicitHeight: 26
    opacity: enabled ? 1.0 : 0.5

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: control.checked ? Theme.toggleOnTrack : Theme.toggleOffTrack

        Behavior on color {
            ColorAnimation { duration: 120 }
        }
    }

    Rectangle {
        width: 20
        height: 20
        radius: 10
        anchors.verticalCenter: parent.verticalCenter
        x: control.checked ? parent.width - width - 3 : 3
        color: control.checked ? Theme.cyan : Theme.toggleKnobOff
        // The light-mode off knob is white on a light track; the hairline keeps it visible.
        border.width: Theme.dark ? 0 : 1
        border.color: Theme.controlBorder

        Behavior on x {
            NumberAnimation { duration: 130; easing.type: Easing.OutCubic }
        }
        Behavior on color {
            ColorAnimation { duration: 120 }
        }
    }

    TapHandler {
        enabled: control.enabled
        // Report the request only — never assign checked here. A self-assignment
        // would destroy the instantiation site's `checked:` binding on the first
        // tap, leaving the switch permanently detached from its backing property.
        // Owners flip the backing property in onToggled and the binding follows.
        onTapped: control.toggled(!control.checked)
    }
}
