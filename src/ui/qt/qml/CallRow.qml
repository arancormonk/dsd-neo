// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// One call in a list — monitor's recent panel and the history screen share it.
// Encrypted rows carry the ENC tag while retaining readable text contrast. A
// call with a recording carries a small play glyph before the time or ENC tag
// (issue #708); a row without one is unchanged.
Item {
    id: row
    property string accessibleName: name
    property bool interactive: false
    property bool hasRecording: false
    signal activated
    // The keyboard lands on the node, the row's named stop (Tab, a sheet's
    // focus return): a focus given to the row itself is passed on to it.
    onActiveFocusChanged: {
        if (activeFocus)
            rowNode.forceActiveFocus();
    }
    // Issue #800 (F1): the row's accessible node is `node`, a child that
    // fills it, never the row itself. A list view writes Accessible.ignored on
    // the root of every row it lays out, with no change signal
    // (QQuickItemViewFxItem::setVisible): a call landing under a pushed screen
    // re-lays the Monitor's and History's rows, and a row whose own node was
    // the root came back into the tree under that screen, a stop with a press.
    // The view never writes a child's flag, so the node's gate holds: covered,
    // it leaves the tree with its name and its press. The root keeps the
    // keyboard (Tab, keys, the tap) and gates itself too, so a cover with no
    // re-layout leaves nothing of it; re-laid, it is a bare node that offers
    // nothing.
    readonly property bool navigationAllowed: Navigation.allows(row)
    readonly property bool reachable: navigationAllowed && visible
    readonly property Item node: rowNode
    Accessible.ignored: !reachable
    // What the row shows besides its name (issue #782): the meta line, its
    // tags and its time, each once: a tag the meta line already says (History
    // puts EMERGENCY and "encrypted" there) and a time the caller's name
    // already reads are left out.
    readonly property string emergencyWords: emergency ? Util.shownText(emergencyTag) : ""
    function activate() {
        if (interactive && enabled && Navigation.allows(row) && row.visible)
            activated();
    }
    Keys.onPressed: function (event) {
        if (Navigation.activationPress(event, Window.activeFocusItem))
            activate();
    }
    Keys.onReleased: function (event) {
        Navigation.activationRelease(event);
    }
    GatedTapHandler {
        onAllowedTap: row.activate()
    }

    property string name: ""
    property string metaText: ""
    property string rightText: ""
    property bool enc: false
    property bool emergency: false
    property bool showDivider: true

    implicitHeight: Math.max(Theme.rowHeight, labels.implicitHeight + 24)

    Item {
        id: rowNode
        // Issue #902: its reach for the keyboard (PressGate).
        readonly property bool inReach: Navigation.allows(rowNode) && rowNode.visible
        // Sized by binding, not anchors: a row the view builds is measured at
        // once (anchored, a list that put its place back found rows 58 high).
        width: row.width
        height: row.height
        Accessible.role: !row.reachable ? Accessible.Pane : (row.interactive ? Accessible.Button : Accessible.StaticText)
        Accessible.name: !row.reachable ? "" : row.hasRecording ? qsTr("%1, has a recording").arg(row.accessibleName) : row.accessibleName
        Accessible.description: !row.reachable ? "" : [row.metaText,
            row.emergencyWords.length > 0 && row.metaText.indexOf(row.emergencyWords) < 0 ? row.emergencyWords : "",
            row.enc && row.metaText.indexOf(qsTr("encrypted")) < 0 ? Util.shownText(encTag) : "",
            !row.enc && row.accessibleName.indexOf(row.rightText) < 0 ? row.rightText : ""].filter(function (part) {
            return part.length > 0;
        }).join(", ")
        Accessible.focusable: row.reachable
        Accessible.ignored: !row.reachable
        Accessible.onPressAction: row.activate()
        // The row's Tab stop, held while it has the focus (see OutlineButton);
        // keys reach the row's handlers from here.
        activeFocusOnTab: (row.navigationAllowed && row.interactive && row.enabled && row.visible) || activeFocus
        // The keyboard's and TalkBack's focus both land here: the frame shows it.
        FocusFrame {}

        // The row's text sits inside its node, so the node's words are its
        // own lines' (what a screen reader reaches is checked by that).
        Column {
            id: labels
            anchors.left: parent.left
            anchors.right: right.left
            anchors.leftMargin: 18
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            // Plain text: names and meta lines carry what the air delivered (aliases,
            // SMS bodies), and Qt's automatic rich-text detection would render a tag
            // in them as markup instead of the characters received. One line each:
            // a line break in that text is drawn as a space.
            Text {
                objectName: "callRowName"
                width: parent.width
                text: Util.oneLine(row.name)
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(16)
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                elide: Text.ElideRight
            }

            Text {
                objectName: "callRowMeta"
                width: parent.width
                text: Util.oneLine(row.metaText)
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
                color: Theme.textSubdued
                elide: Text.ElideRight
            }
        }

        Item {
            id: right

            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            width: (emergencyTag.visible ? emergencyTag.implicitWidth + 6 : 0) + (recordingGlyph.visible ? recordingGlyph.implicitWidth + 8 : 0) + Math.max(encTag.visible ? encTag.implicitWidth : 0, timeText.visible ? timeText.implicitWidth : 0)
            height: parent.height

            EmergencyTag {
                id: emergencyTag
                objectName: "rowEmergencyTag"
                visible: row.emergency
                anchors.right: recordingGlyph.visible ? recordingGlyph.left : encTag.visible ? encTag.left : timeText.left
                anchors.rightMargin: recordingGlyph.visible ? 8 : 6
                anchors.verticalCenter: parent.verticalCenter
            }

            PlayGlyph {
                id: recordingGlyph
                objectName: "rowRecordingGlyph"
                visible: row.hasRecording
                anchors.right: encTag.visible ? encTag.left : timeText.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
            }

            EncTag {
                id: encTag
                objectName: "rowEncTag"
                visible: row.enc
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                id: timeText
                objectName: "callRowTime"
                visible: !row.enc
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: row.rightText
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
                color: Theme.textSubdued
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 18
            height: 1
            visible: row.showDivider
            color: Theme.divider
        }
    }
}
