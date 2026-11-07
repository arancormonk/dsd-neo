// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// A shortcut pinned to Listen (issue #801): FilterPill's fully rounded chip,
// with OutlineButton's gate and disabled look. The label is one line of plain
// text, elided at the width the caller gives, so a long system name never
// wraps the strip onto a second line.
Rectangle {
    id: control

    property string text: ""
    signal clicked

    property string accessibleName: text
    // Held while the button has active focus, as OutlineButton does: Qt refuses
    // to drop activeFocusOnTab from the active focus item.
    activeFocusOnTab: (Navigation.allows(control) && enabled && visible) || activeFocus
    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    // Issue #782: focusable only while in the tree (see OutlineButton).
    Accessible.focusable: navigationAllowed && visible
    readonly property bool navigationAllowed: Navigation.allows(control)
    // Gate first: see OutlineButton.
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !Navigation.allows(control))
            return;
        Navigation.commitComposing(true);
        clicked();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    Keys.onEnterPressed: activate()
    FocusFrame {}

    implicitWidth: Math.max(Theme.minimumTouchSize, Math.ceil(labelMetrics.advanceWidth) + 2 * 15)
    implicitHeight: Math.max(Theme.minimumTouchSize, label.height + 24)
    radius: height / 2
    color: tap.pressed && control.enabled ? Theme.chipSelectedFill : Theme.panel
    border.width: 1
    border.color: Theme.controlBorder
    opacity: enabled ? 1.0 : 0.5

    Behavior on color {
        ColorAnimation {
            duration: 120
        }
    }

    TextMetrics {
        id: labelMetrics
        font: label.font
        text: control.text
    }

    // The Pinned strip measures labels in this font (HomeScreen's pinMetrics)
    // to lay its pins out before any is made: change both together.
    Text {
        id: label
        objectName: "pinButtonLabel"
        x: 15
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, control.width - 2 * 15)
        text: control.text
        textFormat: Text.PlainText
        maximumLineCount: 1
        elide: Text.ElideRight
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        font.weight: Font.DemiBold
        color: Theme.buttonSecondaryText
    }

    TapHandler {
        id: tap
        enabled: control.enabled
        onTapped: control.activate()
    }
}
