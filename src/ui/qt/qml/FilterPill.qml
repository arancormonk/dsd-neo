// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// History-screen filter pill: fully rounded, cyan-tinted when active.
Rectangle {
    id: control

    property string text: ""
    property bool active: false
    property bool caret: true
    // Opt-in (#831 B8): the label wraps between words inside the width the
    // pill is given, and the pill grows taller, where a one-line pill would be
    // cut off by a narrow sheet or large text. The consumer bounds the width.
    property bool wrapLabel: false
    signal clicked

    property string accessibleName: text
    // Held while it has the focus (see OutlineButton).
    activeFocusOnTab: (Navigation.allows(control) && enabled && visible) || activeFocus
    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    // Issue #782: focusable only while in the tree, so a copy Android still
    // holds of a covered or hidden control (no event tells it the control
    // left the tree) is not a swipe stop.
    Accessible.focusable: navigationAllowed && visible
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !Navigation.allows(control))
            return;
        // Issue #718: a choice made mid-word keeps the word; focus stays put.
        Navigation.commitComposing(false);
        clicked();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    Keys.onEnterPressed: activate()
    FocusFrame {}
    Accessible.selected: active

    readonly property real caretSpace: caret ? caretMark.implicitWidth + pillContent.spacing : 0
    // A wrapping pill's natural width is its one-line label's, not the row's,
    // which follows the wrapped label's width.
    implicitWidth: (wrapLabel ? label.implicitWidth + caretSpace : pillContent.implicitWidth) + 30
    implicitHeight: Math.max(Theme.minimumTouchSize, (wrapLabel ? label.height : label.implicitHeight) + 24)
    // Wrapped over several lines, the ends keep a one-line pill's curve.
    radius: wrapLabel && label.lineCount > 1 ? Math.max(Theme.minimumTouchSize, label.height / label.lineCount + 24) / 2 : height / 2
    color: active ? Theme.chipSelectedFill : Theme.panel
    border.width: 1
    border.color: active ? Theme.cyan : Theme.controlBorder

    Row {
        id: pillContent
        anchors.centerIn: parent
        spacing: 6

        Text {
            id: label
            objectName: "filterPillLabel"
            anchors.verticalCenter: parent.verticalCenter
            width: control.wrapLabel ? Math.max(0, Math.min(implicitWidth, control.width - 30 - control.caretSpace)) : implicitWidth
            wrapMode: control.wrapLabel ? Text.WordWrap : Text.NoWrap
            horizontalAlignment: control.wrapLabel ? Text.AlignHCenter : Text.AlignLeft
            text: control.text
            // A pill can name a saved system or an imported category.
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            font.weight: Font.DemiBold
            color: control.active ? Theme.cyan : Theme.buttonSecondaryText
        }

        Caret {
            id: caretMark
            visible: control.caret
            anchors.verticalCenter: parent.verticalCenter
            color: label.color
        }
    }

    TapHandler {
        onTapped: control.activate()
    }
}
