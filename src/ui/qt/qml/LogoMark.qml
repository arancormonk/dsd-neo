// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The app mark: three signal bars stepping through the icon's cyan→magenta ramp,
// inside a panel square. Drawn, not an image, so both themes get it for free.
UiPanel {
    implicitWidth: 62
    implicitHeight: 62

    Row {
        anchors.centerIn: parent
        spacing: 5

        Rectangle { width: 6; height: 16; radius: 3; color: Theme.cyan; anchors.bottom: parent.bottom }
        Rectangle {
            width: 6
            height: 28
            radius: 3
            anchors.bottom: parent.bottom
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.cyan }
                GradientStop { position: 1.0; color: Theme.magenta }
            }
        }
        Rectangle { width: 6; height: 22; radius: 3; color: Theme.magenta; anchors.bottom: parent.bottom }
    }
}
