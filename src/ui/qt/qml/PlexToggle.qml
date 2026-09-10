// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

Switch {
    id: control
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
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !visible || !navigationAllowed
    FocusFrame {}
    contentItem: Text {
        text: control.text
        font: control.font
        color: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        leftPadding: control.indicator.width + control.spacing
    }
}
