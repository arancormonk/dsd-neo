// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

ComboBox {
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
    leftPadding: 14
    rightPadding: 38
    contentItem: Text {
        text: control.displayText
        font: control.font
        color: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        Accessible.ignored: true
    }
    indicator: Caret {
        x: control.width - width - 14
        y: (control.height - height) / 2
        width: 12
        height: 8
        color: Theme.textSecondary
    }
    delegate: ItemDelegate {
        id: option
        required property int index
        width: control.width
        text: {
            var choices = control.model;
            return control.textAt(index);
        }
        highlighted: control.highlightedIndex === index
        implicitHeight: Math.max(48, optionLabel.implicitHeight + 24)
        contentItem: Text {
            id: optionLabel
            text: option.text
            font: control.font
            color: Theme.textPrimary
            wrapMode: Text.Wrap
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: option.highlighted ? Theme.chipSelectedFill : Theme.panel
        }
    }
    Item {
        id: popupLayer
        visible: false
        readonly property var navigationSurface: control.popup.contentItem
        function requestDismiss() {
            control.popup.close();
        }
        Component.onDestruction: Navigation.removeModal(popupLayer)
    }
    Connections {
        target: control.popup
        function onAboutToShow() {
            Navigation.addModal(popupLayer);
        }
        function onAboutToHide() {
            Navigation.removeModal(popupLayer);
        }
    }
    FocusFrame {}
}
