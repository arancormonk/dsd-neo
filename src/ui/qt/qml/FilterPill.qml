// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// History-screen filter pill: fully rounded, cyan-tinted when active.
Rectangle {
    id: control

    property string text: ""
    property bool active: false
    property bool caret: true
    signal clicked()

    implicitWidth: pillContent.implicitWidth + 30
    implicitHeight: 34
    radius: height / 2
    color: active ? Theme.chipSelectedFill : Theme.panel
    border.width: 1
    border.color: active ? Theme.cyan : Theme.controlBorder

    Row {
        id: pillContent
        anchors.centerIn: parent
        spacing: 6

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font.family: Theme.sans
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: control.active ? Theme.cyan : Theme.buttonSecondaryText
        }

        Caret {
            visible: control.caret
            anchors.verticalCenter: parent.verticalCenter
            color: label.color
        }
    }

    TapHandler {
        onTapped: control.clicked()
    }
}
