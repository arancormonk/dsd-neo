// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

TextField {
    id: control
    // Issue #902: its reach for the keyboard (PressGate).
    property alias inReach: control.reachable
    palette.window: Theme.bg
    palette.windowText: Theme.textPrimary
    palette.base: Theme.panel
    palette.alternateBase: Theme.bg
    palette.text: Theme.textPrimary
    palette.button: Theme.panel
    palette.buttonText: Theme.textPrimary
    palette.highlight: Theme.cyan
    palette.highlightedText: Theme.bg
    palette.placeholderText: placeholderText === label ? "transparent" : Theme.textSubdued
    palette.mid: Theme.controlBorder
    palette.dark: Theme.controlBorder
    palette.light: Theme.panelBorder
    font.family: Theme.sans
    font.pixelSize: Theme.fontSize(15)
    implicitHeight: Math.max(Theme.minimumTouchSize, contentHeight + topPadding + bottomPadding)
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !navigationAllowed || !visible
    FocusFrame {}
    property string label: placeholderText
    Accessible.name: !navigationAllowed || !visible ? "" : label
    Accessible.focusable: navigationAllowed && visible
    // Issue #902: covered or hidden, no Tab stop and no editable-text role,
    // either of which keeps a SetFocus action on its accessible node (see
    // PlexTextField). Held while it has the focus (see OutlineButton).
    activeFocusOnTab: (navigationAllowed && visible) || activeFocus
    Accessible.role: navigationAllowed && visible ? Accessible.EditableText : Accessible.Pane
    // Issue #902: hidden or covered, it keeps no focus and takes no key (PressGate).
    readonly property bool reachable: navigationAllowed && visible
    color: Theme.textPrimary
    leftPadding: 14
    rightPadding: 14
    topPadding: label.length ? caption.implicitHeight + 16 : 12
    bottomPadding: 12
    Text {
        id: caption
        x: control.leftPadding
        y: 8
        width: control.width - control.leftPadding - control.rightPadding
        visible: control.label.length > 0
        text: control.label
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSecondary
        Accessible.ignored: true
    }
    background: Rectangle {
        color: Theme.panel
        radius: Theme.radiusButton
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.cyan : Theme.controlBorder
    }
}
