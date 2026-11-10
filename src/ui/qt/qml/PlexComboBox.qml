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
    Accessible.ignored: !navigationAllowed || !visible
    // A choice given a name of its own ("Profile application scope") is read
    // by that name alone; the description adds what it is set to. Unnamed, Qt
    // names it by that value already, and the description stays empty. Which
    // one it is is settled once: Qt renames an unnamed choice only after its
    // value has changed, so comparing the two live would flicker.
    property bool ownAccessibleName: false
    Accessible.description: ownAccessibleName ? displayText : ""
    Component.onCompleted: ownAccessibleName = Accessible.name.length > 0 && Accessible.name !== displayText
    leftPadding: 14
    rightPadding: 38
    contentItem: Text {
        text: control.displayText
        // Choices can be saved system or file names: drawn as written, here
        // and in the list below.
        textFormat: Text.PlainText
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
        // A choice of one among several, each row a radio button checked
        // while it is the current one (#831). An ItemDelegate is a ListItem,
        // which carries no press action: Qt's Android bridge made each row a
        // TextView neither focusable nor clickable, so TalkBack could not
        // settle on a row, and its double tap picked the one at the screen's
        // centre. The press clicks the row as a tap does.
        Accessible.role: Accessible.RadioButton
        Accessible.checkable: true
        Accessible.checked: control.currentIndex === index
        Accessible.focusable: true
        Accessible.onPressAction: option.click()
        implicitHeight: Math.max(48, optionLabel.implicitHeight + 24)
        contentItem: Text {
            id: optionLabel
            text: option.text
            textFormat: Text.PlainText
            font: control.font
            color: Theme.textPrimary
            wrapMode: Text.Wrap
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: option.highlighted ? Theme.chipSelectedFill : Theme.panel
        }
    }
    // The list takes the press that dismisses it (#831). A ComboBox list
    // closes on a press outside it and lets the press through, so the same
    // tap acted on the control beneath (an entry's Remove, a sheet's Apply):
    // closed on the press, the list had left Navigation before that control's
    // press was judged. Modal but undimmed, it closes on the release instead,
    // and nothing beneath sees either.
    popup.modal: true
    popup.dim: false
    popup.closePolicy: Popup.CloseOnEscape | Popup.CloseOnReleaseOutside
    Item {
        id: popupLayer
        visible: false
        readonly property var navigationSurface: control.popup.contentItem
        // Back closes the list and hands focus back to the selector, so the
        // next Back reaches the sheet the selector sits in.
        function requestDismiss() {
            control.popup.close();
            if (control.visible && control.enabled)
                control.forceActiveFocus();
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
