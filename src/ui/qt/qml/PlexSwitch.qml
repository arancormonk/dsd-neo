// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The toggle: cyan-tinted track and cyan knob when on, control-border track when off.
Item {
    id: control
    // Issue #902: its reach for the keyboard (PressGate).
    readonly property bool inReach: control.Accessible.focusable

    property bool checked: false
    property string accessibleName: ""
    // What a screen reader reads after the name (a card's rank and tags, a
    // switch's rule), gated as the name is.
    property string accessibleDescription: ""
    property bool interactive: true
    signal toggled(bool checked)
    function activate() {
        if (enabled && interactive && Navigation.allows(control) && control.visible) {
            // Issue #718: a choice made mid-word keeps the word.
            Navigation.commitComposing(false);
            toggled(!checked);
        }
    }
    // Held while it has the focus (see OutlineButton).
    activeFocusOnTab: (Navigation.allows(control) && visible && enabled && interactive) || activeFocus
    Accessible.role: Accessible.Switch
    Accessible.name: !Navigation.allows(control) || !visible || !interactive ? "" : accessibleName
    Accessible.focusable: Navigation.allows(control) && visible && interactive
    Accessible.description: !Navigation.allows(control) || !visible || !interactive ? "" : accessibleDescription
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.ignored: !Navigation.allows(control) || !visible || !interactive
    Accessible.onPressAction: activate()
    Accessible.onToggleAction: activate()
    Keys.onPressed: function (event) {
        if (Navigation.activationPress(event, Window.activeFocusItem))
            activate();
    }
    Keys.onReleased: function (event) {
        Navigation.activationRelease(event);
    }
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

    GatedTapHandler {
        enabled: Navigation.allows(control) && control.enabled && control.interactive
        // Report the request only — never assign checked here. A self-assignment
        // would destroy the instantiation site's `checked:` binding on the first
        // tap, leaving the switch permanently detached from its backing property.
        // Owners flip the backing property in onToggled and the binding follows.
        onAllowedTap: control.activate()
    }
}
