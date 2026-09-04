// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// A centred panel over a dimmed screen, dismissed by tapping outside it.
//
// One copy because the dismissal rule is subtle and had been written out three
// times: a TapHandler never takes an exclusive grab, so one placed on the panel
// does not stop the scrim's from firing as well — an unconditional dismiss on
// the scrim therefore closes the sheet on every press of every control inside
// it. The scrim has to decide by where the tap landed instead, which is what
// hitsPanel() is for, and a copy that got it wrong would be one sheet that
// closes on its own buttons while the others do not.
Rectangle {
    id: sheet

    /** Panel contents, laid out top to bottom. */
    default property alias content: column.data
    /** Gap between the panel's children. */
    property alias spacing: column.spacing
    /** Names the panel item itself, for tests that address it directly. */
    property alias panelObjectName: panelItem.objectName

    /** Emitted after a tap on the scrim has hidden the sheet. */
    signal dismissed

    /** Whether a point in this sheet's coordinates lies on the panel. */
    function hitsPanel(x, y) {
        var p = sheet.mapToItem(panelItem, x, y);
        return p.x >= 0 && p.y >= 0 && p.x <= panelItem.width && p.y <= panelItem.height;
    }

    anchors.fill: parent
    visible: false
    color: Qt.alpha("#000000", 0.5)

    TapHandler {
        onTapped: function (eventPoint) {
            if (sheet.hitsPanel(eventPoint.position.x, eventPoint.position.y))
                return;
            sheet.visible = false;
            // A sheet dismissed with a field still focused leaves the soft
            // keyboard standing over the screen it went back to.
            Qt.inputMethod.hide();
            sheet.dismissed();
        }
    }

    UiPanel {
        id: panelItem

        anchors.centerIn: parent
        width: parent.width - 2 * Theme.screenPadding
        height: column.height + 2 * Theme.cardPadding

        Column {
            id: column

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: Theme.cardPadding
            spacing: 12
        }
    }
}
