// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #800: one of the map's floating controls: a 48 px square on the
// panel colour over the map, a drawn glyph (MapGlyph), named for its action.
// A toggle (Follow me) shows its on state as every checked control in the app
// does: the cyan outline and tint, with the glyph in cyan. Unframed, it draws
// no surface of its own: the zoom pair shares one frame.
Rectangle {
    id: control
    // Issue #902: its reach for the keyboard (PressGate).
    readonly property bool inReach: control.Accessible.focusable

    property string shape: "plus"
    property string accessibleName: ""
    property string accessibleDescription: ""
    property bool checkable: false
    property bool checked: false
    property bool framed: true
    // Issue #909: what a tap acts on (GatedTapHandler.pressKey): a tap whose
    // press began on another key is dropped.
    property var pressKey: null
    signal clicked

    implicitWidth: Theme.minimumTouchSize
    implicitHeight: Theme.minimumTouchSize
    radius: Theme.radiusButton
    color: checked || (tap.pressed && enabled) ? Theme.chipSelectedFill : framed ? Theme.mapOverlay : "transparent"
    border.width: framed || checked ? 1 : 0
    border.color: checked ? Theme.cyan : Theme.controlBorder
    opacity: enabled ? 1.0 : 0.45

    Behavior on color {
        enabled: !Theme.reducedMotion
        ColorAnimation {
            duration: 150
        }
    }

    // Held while it has the focus (see OutlineButton).
    activeFocusOnTab: (Navigation.allows(control) && enabled && visible) || activeFocus
    Accessible.role: Accessible.Button
    Accessible.name: !navigationAllowed || !visible ? "" : accessibleName
    Accessible.description: !navigationAllowed || !visible ? "" : accessibleDescription
    Accessible.checkable: checkable
    Accessible.checked: checkable && checked
    Accessible.focusable: navigationAllowed && visible
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !visible || !Navigation.allows(control))
            return;
        Navigation.commitComposing(true);
        clicked();
    }
    Keys.onPressed: function (event) {
        if (Navigation.activationPress(event, Window.activeFocusItem))
            activate();
    }
    Keys.onReleased: function (event) {
        Navigation.activationRelease(event);
    }
    FocusFrame {}

    MapGlyph {
        anchors.centerIn: parent
        shape: control.shape
        color: control.checked ? Theme.cyan : Theme.textPrimary
    }

    GatedTapHandler {
        id: tap
        enabled: control.enabled
        gesturePolicy: TapHandler.ReleaseWithinBounds
        pressKey: control.pressKey
        onAllowedTap: control.activate()
    }
}
