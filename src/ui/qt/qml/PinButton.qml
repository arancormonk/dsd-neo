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
    // False for a button whose row StableRows is letting go of at the end of
    // the turn (index past `live`): it reads as nothing, is out of the tree and
    // takes no focus or press, as the house's StableRows views do.
    property bool live: true
    // Held while the button has active focus, as OutlineButton does: Qt refuses
    // to drop activeFocusOnTab from the active focus item.
    activeFocusOnTab: (Navigation.allows(control) && enabled && visible && live) || activeFocus
    Accessible.role: Accessible.Button
    Accessible.name: live ? accessibleName : ""
    // Issue #782: focusable only while in the tree (see OutlineButton).
    Accessible.focusable: navigationAllowed && visible && live
    readonly property bool navigationAllowed: Navigation.allows(control)
    // Gate first: see OutlineButton.
    Accessible.ignored: !navigationAllowed || !visible || !live
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !live || !Navigation.allows(control))
            return;
        Navigation.commitComposing(true);
        clicked();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    Keys.onEnterPressed: activate()
    FocusFrame {}

    // The label's room: the wider of its advance and its ink, and a pixel,
    // since a Text lays out a little wider than its advance and would elide a
    // whole name by a fraction. Home's Pinned strip measures pins the same way.
    implicitWidth: Math.max(Theme.minimumTouchSize, Math.ceil(Math.max(labelMetrics.advanceWidth, labelMetrics.boundingRect.width)) + 1 + 2 * 15)
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
        // The font the Pinned strip measures pins in (Theme.pinLabelFont).
        font: Theme.pinLabelFont
        color: Theme.buttonSecondaryText
    }

    TapHandler {
        id: tap
        enabled: control.enabled
        onTapped: control.activate()
    }
}
