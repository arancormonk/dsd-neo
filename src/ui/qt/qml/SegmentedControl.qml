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

    implicitHeight: 38
    radius: Theme.radiusButton
    color: Theme.dark ? Theme.bg : Theme.panel
    border.width: 1
    border.color: Theme.controlBorder

    Row {
        anchors.fill: parent
        anchors.margins: 3

        Repeater {
            model: control.model

            Rectangle {
                required property int index
                required property var modelData

                readonly property bool active: control.currentIndex === index

                width: (control.width - 6) / control.model.length
                height: parent.height
                radius: Theme.radiusButton - 3
                color: active ? Theme.chipSelectedFill : "transparent"
                border.width: active ? 1 : 0
                border.color: Theme.cyan

                Text {
                    anchors.centerIn: parent
                    text: modelData
                    font.family: Theme.sans
                    font.pixelSize: 13
                    font.weight: parent.active ? Font.DemiBold : Font.Normal
                    color: parent.active ? Theme.cyan : Theme.textSecondary
                }

                TapHandler {
                    onTapped: control.selected(index)
                }
            }
        }
    }
}
