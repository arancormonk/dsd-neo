// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Show / Hide beside a masked key field: an eye affordance plus a readable
// label, so it does not rely on emoji fonts. The caller owns `revealed` and
// flips it in onClicked.
OutlineButton {
    id: control
    property bool revealed: false
    width: 82
    text: revealed ? qsTr("Hide") : qsTr("Show")
    accessibleName: revealed ? qsTr("Hide key") : qsTr("Show key")
    Canvas {
        id: eye
        x: 5
        anchors.verticalCenter: parent.verticalCenter
        width: 14
        height: 10
        property color stroke: Theme.textSecondary
        onStrokeChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            ctx.strokeStyle = eye.stroke;
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(0, 5);
            ctx.quadraticCurveTo(7, -3, 14, 5);
            ctx.quadraticCurveTo(7, 13, 0, 5);
            ctx.stroke();
            ctx.beginPath();
            ctx.arc(7, 5, 2, 0, 2 * Math.PI);
            ctx.stroke();
        }
    }
}
