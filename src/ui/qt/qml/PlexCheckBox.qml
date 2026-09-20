// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

CheckBox {
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
    indicator: Rectangle {
        x: control.leftPadding
        y: (control.height - height) / 2
        width: 24
        height: 24
        radius: 4
        color: control.checked ? Theme.cyan : "transparent"
        border.width: 2
        border.color: control.checked ? Theme.cyan : Theme.textSecondary
        Rectangle {
            visible: control.checked
            x: 5
            y: 12
            width: 7
            height: 2
            rotation: 45
            color: Theme.bg
        }
        Rectangle {
            visible: control.checked
            x: 9
            y: 10
            width: 11
            height: 2
            rotation: -45
            color: Theme.bg
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
