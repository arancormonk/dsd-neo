// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Controls

// Newest first, so the tail of a running system is at the top and no auto-scroll is
// needed to keep up with it. Shared by the monitoring view and the last-session sheet.
//
// An Item rather than the ListView itself: a child declared inside a ListView lands in
// its flickable content and scrolls away with the rows, which is not what an
// empty-state placeholder should do.
Item {
    id: root

    function positionViewAtBeginning() {
        view.positionViewAtBeginning()
    }

    ListView {
        id: view

        anchors.fill: parent
        clip: true
        model: eventLog
        spacing: 1

        delegate: Label {
            width: view.width
            font.family: monoFontFamily
            font.pixelSize: 12
            wrapMode: Text.Wrap
            text: model.text
        }

        ScrollBar.vertical: ScrollBar {}
    }

    Label {
        anchors.centerIn: parent
        width: parent.width - 24
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        opacity: 0.7
        visible: view.count === 0
        text: qsTr("No events yet.")
    }
}
