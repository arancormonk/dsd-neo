// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The encrypted-call marker: mono "ENC", magenta text in a 40%-alpha magenta
// outline. Deliberately not an error treatment.
Rectangle {
    implicitWidth: tag.implicitWidth + 14
    implicitHeight: 20
    radius: 5
    color: "transparent"
    border.width: 1
    border.color: Theme.encBorder

    Text {
        id: tag
        anchors.centerIn: parent
        text: "ENC"
        font.family: Theme.mono
        font.pixelSize: 10
        font.letterSpacing: 1
        color: Theme.magenta
    }
}
