// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Accept the press before delivery reaches sibling controls behind an overlay.
// Put interactive overlay contents after this item so they remain reachable.
MouseArea {
    anchors.fill: parent
    acceptedButtons: Qt.AllButtons
    onWheel: function(wheel) { wheel.accepted = true; }
}
