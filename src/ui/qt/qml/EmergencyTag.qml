// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Emergency indication shared by live calls and history.
Rectangle {
    implicitWidth: tag.implicitWidth + 14
    implicitHeight: Math.max(20, tag.implicitHeight + 8)
    radius: 5
    color: "transparent"
    border.width: 1
    border.color: Theme.alertBorder

    Text {
        id: tag
        anchors.centerIn: parent
        text: qsTr("EMERGENCY")
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(10)
        font.letterSpacing: 1
        color: Theme.alert
    }
}
