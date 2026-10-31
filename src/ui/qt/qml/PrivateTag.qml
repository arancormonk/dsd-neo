// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The private-call marker (issue #773): one radio calling another rather than a
// talkgroup. Mono "PRIVATE" in the secondary text colour inside a neutral outline,
// the ENC and EMERGENCY tags' metrics: a kind of call, not a warning.
Rectangle {
    // The marker's words, for a reader that names the call whether or not the tag fits on screen (issue #782).
    readonly property string label: qsTr("PRIVATE")
    implicitWidth: tag.implicitWidth + 14
    implicitHeight: Math.max(20, tag.implicitHeight + 8)
    radius: 5
    color: "transparent"
    border.width: 1
    border.color: Theme.privateBorder

    Text {
        id: tag
        anchors.centerIn: parent
        text: parent.label
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(10)
        font.letterSpacing: 1
        color: Theme.textSecondary
    }
}
