// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The toggle: cyan-tinted track and cyan knob when on, control-border track when off.
Item {
    id: control

    property bool checked: false
    property string accessibleName: ""
    property bool interactive: true
    signal toggled(bool checked)
    function activate() {
        if (enabled && interactive && Navigation.allows(control))
            toggled(!checked);
    }
    activeFocusOnTab: enabled && interactive && Navigation.allows(control)
    Accessible.role: Accessible.Switch
    Accessible.name: accessibleName
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.ignored: !visible || !interactive || !Navigation.allows(control)
    Accessible.onPressAction: activate()
    Accessible.onToggleAction: activate()
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    FocusFrame {}

    implicitWidth: Theme.minimumTouchSize
    implicitHeight: Theme.minimumTouchSize
    opacity: enabled ? 1.0 : 0.5

    Rectangle {
        anchors.centerIn: parent
        width: 46
        height: 26
        radius: height / 2
        color: control.checked ? Theme.toggleOnTrack : Theme.toggleOffTrack

        Behavior on color {
            ColorAnimation {
                duration: 120
            }
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
            NumberAnimation {
                duration: 130
                easing.type: Easing.OutCubic
            }
        }
        Behavior on color {
            ColorAnimation {
                duration: 120
            }
        }
    }

    TapHandler {
        enabled: control.enabled && control.interactive && Navigation.allows(control)
        // Report the request only — never assign checked here. A self-assignment
        // would destroy the instantiation site's `checked:` binding on the first
        // tap, leaving the switch permanently detached from its backing property.
        // Owners flip the backing property in onToggled and the binding follows.
        onTapped: control.activate()
    }
}
