// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Item {
    id: control
    // Issue #902: its reach for the keyboard (PressGate).
    readonly property bool inReach: control.Accessible.focusable
    property string icon: "back"
    property string accessibleName: icon === "back" ? qsTr("Back") : icon === "close" ? qsTr("Close") : qsTr("More options")
    // Whether a press also finishes the field's editing (focus leaves it)
    // before the action, as OutlineButton's. False keeps the field in use and
    // finishes only the keyboard's word: a control inside a field that acts on
    // it, such as a search field's ×.
    property bool finishesEditing: true
    // Issue #909: what a tap acts on (GatedTapHandler.pressKey): a tap whose
    // press began on another key is dropped.
    property var pressKey: null
    signal clicked
    implicitWidth: Theme.minimumTouchSize
    implicitHeight: Theme.minimumTouchSize
    // Held while it has the focus (see OutlineButton).
    activeFocusOnTab: (Navigation.allows(control) && visible && enabled) || activeFocus
    Accessible.role: Accessible.Button
    // Named and a stop only while in the tree, from the gate itself
    // (docs/code_map.md, issue #830); see OutlineButton.
    Accessible.name: !navigationAllowed || !visible ? "" : accessibleName
    Accessible.focusable: navigationAllowed && visible
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !Navigation.allows(control) || !control.visible)
            return;
        // Issue #718: finish the keyboard's word and the field's editing before
        // the action reads the form.
        Navigation.commitComposing(control.finishesEditing);
        clicked();
    }
    Keys.onPressed: function (event) {
        if (Navigation.activationPress(event, Window.activeFocusItem))
            activate();
    }
    Keys.onReleased: function (event) {
        Navigation.activationRelease(event);
    }
    FocusFrame {}
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusButton
        color: tap.pressed ? Theme.chipSelectedFill : "transparent"
    }
    Item {
        anchors.centerIn: parent
        width: 20
        height: 20
        Repeater {
            model: control.icon === "more" ? 3 : 2
            Rectangle {
                required property int index
                width: control.icon === "more" ? 4 : 15
                height: control.icon === "more" ? 4 : 2
                radius: 2
                color: Theme.textPrimary
                x: control.icon === "more" ? 8 : 2
                y: control.icon === "more" ? index * 7 + 1 : control.icon === "close" ? 9 : index * 10 + 4
                rotation: control.icon === "more" ? 0 : control.icon === "close" ? (index ? -45 : 45) : (index ? 45 : -45)
            }
        }
    }
    GatedTapHandler {
        id: tap
        gesturePolicy: TapHandler.ReleaseWithinBounds
        pressKey: control.pressKey
        onAllowedTap: control.activate()
    }
}
