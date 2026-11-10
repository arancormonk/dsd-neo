// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The imported-file kind marker: mono uppercase in a quiet control-border chip,
// the same register as EncTag but deliberately neutral — a file's type is a
// fact, not a warning.
Rectangle {
    // "chan" | "group" | "keysDec" | "keysHex" | "p25Bandplan" | "src", as ImportedFilesModel stores it.
    property string type: ""
    // The kind as shown, for a row that reads it in its own words.
    readonly property string label: tag.text
    // Whether the kind is read where it sits. A row that is one button for a
    // screen reader folds it into its own description instead, so the badge
    // takes no stop of its own.
    property bool readAlone: true

    implicitWidth: tag.implicitWidth + 14
    implicitHeight: Math.max(20, tag.implicitHeight + 8)
    radius: 5
    color: "transparent"
    border.width: 1
    border.color: Theme.controlBorder

    // The kind is read where it sits (issue #782) unless the row reads it.
    ReadableText {
        id: tag
        objectName: "csvTypeBadgeText"
        anchors.centerIn: parent
        Accessible.ignored: !parent.readAlone || !navigationAllowed || !visible
        Accessible.name: parent.readAlone && navigationAllowed && visible ? text : ""
        text: parent.type === "trunkTargets" ? qsTr("TARGET CSV") : parent.type === "chan" ? qsTr("CHANNELS") : parent.type === "group" ? qsTr("TALKGROUPS") : parent.type === "keysDec" ? qsTr("KEYS · DEC") : parent.type === "keysHex" ? qsTr("KEYS · HEX") : parent.type === "vertexKeys" ? qsTr("VERTEX KEYS") : parent.type === "dmrTgKeys" ? qsTr("DMR KEY MAP") : parent.type === "p25Bandplan" ? qsTr("P25 BAND PLAN") : parent.type === "src" ? qsTr("RADIO IDS") : qsTr("FILE")
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(10)
        font.letterSpacing: 1
        color: Theme.textSecondary
    }
}
