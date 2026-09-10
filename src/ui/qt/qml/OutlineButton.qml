// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Secondary action: 1px control-border outline, quiet fill, secondary text.
Rectangle {
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

    implicitWidth: Math.max(Theme.minimumTouchSize, label.implicitWidth + 2 * Theme.cardPadding)
    implicitHeight: Math.max(Theme.minimumTouchSize, label.implicitHeight + 24)
    radius: Theme.radiusButton
    color: tap.pressed && control.enabled ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
    border.width: 1
    border.color: Theme.controlBorder
    opacity: enabled ? 1.0 : 0.5

    Behavior on color {
        ColorAnimation {
            duration: 120
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
        font.weight: Font.DemiBold
        color: Theme.buttonSecondaryText
    }

    TapHandler {
        id: tap
        enabled: control.enabled
        onTapped: control.activate()
    }
}
