// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Text input in the house style: quiet surface, 1px control border that turns
// cyan with focus. `mono` switches the value to Plex Mono for data-shaped fields.
Rectangle {
    id: control

    property alias text: input.text
    property alias placeholderText: placeholder.text
    property alias inputMethodHints: input.inputMethodHints
    property alias input: input
    property bool mono: false

    // Fires on Enter or focus loss — for fields whose consumer is too expensive
    // to run per keystroke (persisted preferences, argv rebuilds).
    signal editingFinished()

    implicitHeight: 44
    radius: 10
    color: Theme.dark ? Theme.bg : Theme.panel
    border.width: 1
    border.color: input.activeFocus ? Theme.cyan : Theme.controlBorder

    TextInput {
        id: input

        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        verticalAlignment: TextInput.AlignVCenter
        font.family: control.mono ? Theme.mono : Theme.sans
        font.pixelSize: 15
        color: Theme.textPrimary
        selectionColor: Qt.alpha(Theme.cyan, 0.35)
        selectedTextColor: Theme.textPrimary
        clip: true
        onEditingFinished: control.editingFinished()
    }

    Text {
        id: placeholder

        anchors.fill: input
        verticalAlignment: Text.AlignVCenter
        visible: input.text.length === 0 && !input.activeFocus
        font: input.font
        color: Theme.textSubdued
        elide: Text.ElideRight
    }

    TapHandler {
        onTapped: input.forceActiveFocus()
    }
}
