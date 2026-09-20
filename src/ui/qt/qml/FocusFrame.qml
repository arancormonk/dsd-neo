// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Rectangle {
    anchors.fill: parent
    anchors.margins: -2
    z: 10
    visible: parent.activeFocus
    color: "transparent"
    radius: Theme.radiusButton + 2
    border.width: 2
    border.color: Theme.textPrimary
    Accessible.ignored: true
}
