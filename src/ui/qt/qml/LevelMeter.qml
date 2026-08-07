// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The 7-bar level meter under the hero talkgroup, colored along the cyan→magenta
// ramp. There is no per-sample audio level at this boundary, so while a call is
// live the bars breathe on desynchronized loops — activity, not measurement.
Row {
    id: meter

    property bool active: false

    spacing: 4
    height: 26

    Repeater {
        model: 7

        Rectangle {
            required property int index

            width: 4
            radius: 2
            anchors.bottom: parent.bottom
            color: Qt.rgba(
                       Theme.cyan.r + (Theme.magenta.r - Theme.cyan.r) * index / 6,
                       Theme.cyan.g + (Theme.magenta.g - Theme.cyan.g) * index / 6,
                       Theme.cyan.b + (Theme.magenta.b - Theme.cyan.b) * index / 6,
                       1)
            height: meter.active ? 8 : 5

            SequentialAnimation on height {
                running: meter.active
                loops: Animation.Infinite
                alwaysRunToEnd: true

                NumberAnimation {
                    to: 6 + ((index * 7) % 17)
                    duration: 260 + index * 40
                    easing.type: Easing.InOutQuad
                }
                NumberAnimation {
                    to: 10 + ((index * 11) % 13)
                    duration: 300 + ((index * 53) % 90)
                    easing.type: Easing.InOutQuad
                }
                NumberAnimation {
                    to: 5 + ((index * 5) % 9)
                    duration: 240 + ((index * 31) % 70)
                    easing.type: Easing.InOutQuad
                }
            }
        }
    }
}
