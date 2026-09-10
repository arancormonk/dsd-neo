// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Segmented selector (the Appearance setting): equal segments in one outlined
// track, selected segment tinted cyan.
Rectangle {
    id: control

    property var model: []
    property int currentIndex: 0
    signal selected(int index)

    property real labelHeight: 0
    function measureLabels() {
        var measured = 0;
        for (var i = 0; i < segments.count; ++i) {
            var item = segments.itemAt(i);
            if (item)
                measured = Math.max(measured, item.textHeight);
        }
        labelHeight = measured;
    }
    implicitHeight: Math.max(54, labelHeight + 30)
    radius: Theme.radiusButton
    color: Theme.dark ? Theme.bg : Theme.panel
    border.width: 1
    border.color: Theme.controlBorder

    Row {
        anchors.fill: parent
        anchors.margins: 3

        Repeater {
            id: segments
            model: control.model
            onItemAdded: Qt.callLater(control.measureLabels)
            onItemRemoved: Qt.callLater(control.measureLabels)

            Rectangle {
                required property int index
                required property var modelData
                readonly property real textHeight: segmentLabel.implicitHeight
                onTextHeightChanged: Qt.callLater(control.measureLabels)

                activeFocusOnTab: enabled && Navigation.allows(control)
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checkable: true
                Accessible.checked: active
                readonly property bool navigationAllowed: Navigation.allows(control)
                Accessible.ignored: !visible || !navigationAllowed
                Accessible.onPressAction: choose()
                function choose() {
                    if (enabled && Navigation.allows(control))
                        control.selected(index);
                }
                Keys.onSpacePressed: choose()
                Keys.onReturnPressed: choose()
                FocusFrame {}
                readonly property bool active: control.currentIndex === index

                width: (control.width - 6) / control.model.length
                height: parent.height
                radius: Theme.radiusButton - 3
                color: active ? Theme.chipSelectedFill : "transparent"
                border.width: active ? 1 : 0
                border.color: Theme.cyan

                Text {
                    id: segmentLabel
                    anchors.centerIn: parent
                    width: parent.width - 12
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    text: modelData
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(13)
                    font.weight: parent.active ? Font.DemiBold : Font.Normal
                    color: parent.active ? Theme.cyan : Theme.textSecondary
                }

                TapHandler {
                    onTapped: choose()
                }
            }
        }
    }
}
