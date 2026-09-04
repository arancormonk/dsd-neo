// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The imported-file kind marker: mono uppercase in a quiet control-border chip,
// the same register as EncTag but deliberately neutral — a file's type is a
// fact, not a warning.
Rectangle {
    // "chan" | "group" | "keysDec" | "keysHex" | "p25Bandplan", as ImportedFilesModel stores it.
    property string type: ""

    implicitWidth: tag.implicitWidth + 14
    implicitHeight: 20
    radius: 5
    color: "transparent"
    border.width: 1
    border.color: Theme.controlBorder

    Text {
        id: tag
        anchors.centerIn: parent
        text: parent.type === "chan" ? qsTr("CHANNELS")
              : parent.type === "group" ? qsTr("TALKGROUPS")
              : parent.type === "keysDec" ? qsTr("KEYS · DEC")
              : parent.type === "keysHex" ? qsTr("KEYS · HEX")
              : parent.type === "p25Bandplan" ? qsTr("P25 BAND PLAN")
              : qsTr("FILE")
        font.family: Theme.mono
        font.pixelSize: 10
        font.letterSpacing: 1
        color: Theme.textSecondary
    }
}
