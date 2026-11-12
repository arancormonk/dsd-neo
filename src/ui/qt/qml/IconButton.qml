// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Item {
    id: control
    property string icon: "back"
    property string accessibleName: icon === "back" ? qsTr("Back") : icon === "close" ? qsTr("Close") : qsTr("More options")
    // Whether a press also finishes the field's editing (focus leaves it)
    // before the action, as OutlineButton's. False keeps the field in use and
    // finishes only the keyboard's word: a control inside a field that acts on
    // it, such as a search field's ×.
    property bool finishesEditing: true
    signal clicked
    implicitWidth: Theme.minimumTouchSize
    implicitHeight: Theme.minimumTouchSize
    // Held while it has the focus (see OutlineButton).
    activeFocusOnTab: (Navigation.allows(control) && enabled) || activeFocus
    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !Navigation.allows(control))
            return;
        // Issue #718: finish the keyboard's word and the field's editing before
        // the action reads the form.
        Navigation.commitComposing(control.finishesEditing);
        clicked();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    Keys.onEnterPressed: activate()
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
        onAllowedTap: control.activate()
    }
}
