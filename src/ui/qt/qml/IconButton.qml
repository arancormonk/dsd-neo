// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Item {
    id: control
    property string icon: "back"
    property string accessibleName: icon === "back" ? qsTr("Back") : icon === "close" ? qsTr("Close") : qsTr("More options")
    signal clicked
    implicitWidth: Theme.minimumTouchSize
    implicitHeight: Theme.minimumTouchSize
    activeFocusOnTab: enabled && Navigation.allows(control)
    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !visible || !navigationAllowed
    Accessible.onPressAction: activate()
    function activate() {
        if (enabled && Navigation.allows(control))
            clicked();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
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
    TapHandler {
        id: tap
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: control.activate()
    }
}
