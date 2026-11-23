// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

ComboBox {
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
    palette.placeholderText: Theme.textSubdued
    palette.mid: Theme.controlBorder
    palette.dark: Theme.controlBorder
    palette.light: Theme.panelBorder
    font.family: Theme.sans
    font.pixelSize: Theme.fontSize(15)
    implicitHeight: Math.max(Theme.minimumTouchSize, contentItem.implicitHeight + topPadding + bottomPadding)
    readonly property bool navigationAllowed: Navigation.allows(control)
    // Issue #902: its gate. Its own open list is the top modal but no cover:
    // the selector keeps the keyboard, its name and its stop while that list
    // is on top, and its own key handling chooses in the list (Up, Down,
    // Home, End, a letter, Return, Space, Escape, Back, Tab), as Qt's combo
    // does. Anything else over it gates it.
    readonly property bool reachable: (navigationAllowed || Navigation.topModal === popupLayer) && visible
    Accessible.ignored: !reachable
    // A choice given a name of its own (`accessibleName`, "Profile application
    // scope") is read by that name alone; the description adds what it is set
    // to. Unnamed, it is named by that value, and the description stays empty.
    // Both, and whether it is a stop, come from the gate itself
    // (docs/code_map.md, issue #830): covered or hidden, the choice is nameless.
    property string accessibleName: ""
    readonly property bool ownAccessibleName: accessibleName.length > 0
    Accessible.name: !reachable ? "" : ownAccessibleName ? accessibleName : displayText
    Accessible.description: !reachable || !ownAccessibleName ? "" : displayText
    Accessible.focusable: reachable
    // Issue #902: the native Tab focus too, which keeps a SetFocus action on
    // the accessible node of a covered choice. Held while it has the focus
    // (see OutlineButton).
    focusPolicy: reachable || activeFocus ? Qt.StrongFocus : Qt.NoFocus
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
        // centre. The row's node is a child of its own (rowNode), as the
        // button writes its own unchecked, uncheckable state over any
        // Accessible.checkable and checked set on it. The delegate stays a
        // node all the same (Qt clears its ignored flag once the list opens),
        // so it goes nameless: one named node per row, one swipe stop.
        Accessible.ignored: true
        Accessible.name: ""
        /** The row's choice, as a tap makes it, while its list shows. */
        function choose() {
            if (option.visible && option.enabled && control.popup.visible && Navigation.allows(option))
                option.click();
        }
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
        // Rows stay built once the list closes, and Android can keep its copy
        // of a node, so a row is a stop, and its press chooses, only while
        // the list shows.
        Item {
            id: rowNode
            // Issue #902: its reach for the keyboard (PressGate).
            property alias inReach: rowNode.reachable
            objectName: "plexComboRow"
            anchors.fill: parent
            // Its own gate (issue #902): a sheet over the open list, or the
            // list closed, leaves the row nameless, out of the tree and no
            // stop, and its press chooses nothing (choose()).
            readonly property bool reachable: Navigation.allows(rowNode) && option.visible && control.popup.visible
            Accessible.role: Accessible.RadioButton
            Accessible.name: reachable ? option.text : ""
            Accessible.ignored: !reachable
            Accessible.checkable: true
            Accessible.checked: control.currentIndex === option.index
            Accessible.focusable: reachable
            Accessible.onPressAction: option.choose()
            // A radio button is toggled too: a desktop reader may send that.
            Accessible.onToggleAction: option.choose()
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
    // Issue #718: the word composing in a field is committed before the combo
    // takes the focus on a press (a field that saves on editingFinished would
    // save it without that word), and before the list opens by any other way
    // (a screen reader's press, the keyboard). A pointer handler sees the press
    // before the combo does, and never takes it.
    PointHandler {
        onActiveChanged: {
            if (active)
                Navigation.commitComposing(false);
        }
    }
    Connections {
        target: control.popup
        function onAboutToShow() {
            Navigation.commitComposing(false);
            Navigation.addModal(popupLayer);
        }
        function onAboutToHide() {
            Navigation.removeModal(popupLayer);
        }
    }
    FocusFrame {}
}
