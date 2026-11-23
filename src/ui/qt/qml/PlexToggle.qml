// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

Switch {
    id: control
    // Issue #902: its reach for the keyboard (PressGate).
    readonly property bool inReach: control.navigationAllowed && control.visible && control.Accessible.focusable
    palette.window: Theme.bg
    palette.windowText: Theme.textPrimary
    palette.base: Theme.panel
    palette.alternateBase: Theme.bg
    palette.text: Theme.textPrimary
    palette.button: Theme.panel
    palette.buttonText: Theme.textPrimary
    palette.highlight: Theme.cyan
    palette.highlightedText: Theme.bg
    palette.placeholderText: Theme.textSubdued
    palette.mid: Theme.controlBorder
    palette.dark: Theme.controlBorder
    palette.light: Theme.panelBorder
    font.family: Theme.sans
    font.pixelSize: Theme.fontSize(15)
    implicitHeight: Math.max(Theme.minimumTouchSize, contentItem.implicitHeight + topPadding + bottomPadding)
    // Dimmed while it cannot be changed, as PlexSwitch and OutlineButton are; the
    // style's own indicator otherwise looks the same enabled or not.
    opacity: enabled ? 1.0 : 0.5
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !navigationAllowed || !visible
    // Issue #902: named and a stop only while in the tree, and a press or
    // toggle a screen reader sends a covered or hidden node it still holds
    // does nothing (Qt's own would toggle it behind the sheet). The native
    // Tab focus is gated as well: it keeps a SetFocus action on the node.
    Accessible.name: !navigationAllowed || !visible ? "" : text
    Accessible.focusable: navigationAllowed && visible
    focusPolicy: (navigationAllowed && visible) || activeFocus ? Qt.StrongFocus : Qt.NoFocus
    Accessible.onPressAction: accessibleToggle()
    Accessible.onToggleAction: accessibleToggle()
    function accessibleToggle() {
        if (enabled && Navigation.allows(control) && visible)
            click();
    }
    FocusFrame {}
    // Issue #718: the word composing in a field is committed before this
    // takes the focus on a press, as a choice made mid-word keeps the word
    // (a field that saves on editingFinished would save it without it). A
    // pointer handler sees the press before the control does, and never
    // takes it.
    PointHandler {
        onActiveChanged: {
            if (active)
                Navigation.commitComposing(false);
        }
    }
    contentItem: Text {
        text: control.text
        font: control.font
        color: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        leftPadding: control.indicator.width + control.spacing
    }
}
