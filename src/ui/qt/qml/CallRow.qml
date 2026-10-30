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
    activeFocusOnTab: Navigation.allows(row) && interactive && enabled
    Accessible.role: interactive ? Accessible.Button : Accessible.StaticText
    Accessible.name: hasRecording ? qsTr("%1, has a recording").arg(accessibleName) : accessibleName
    // What the row shows besides its name (issue #782): the meta line, its
    // tags and its time, unless the caller's name already reads the time.
    Accessible.description: [metaText, emergency ? Util.shownText(emergencyTag) : "", enc ? Util.shownText(encTag) : "",
        !enc && accessibleName.indexOf(rightText) < 0 ? rightText : ""].filter(function (part) {
        return part.length > 0;
    }).join(", ")
    readonly property bool navigationAllowed: Navigation.allows(row)
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (interactive && enabled && Navigation.allows(row))
            activated();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    FocusFrame {}
    TapHandler {
        onTapped: row.activate()
    }

    property string name: ""
    property string metaText: ""
    property string rightText: ""
    property bool enc: false
    property bool emergency: false
    property bool showDivider: true

    implicitHeight: Math.max(Theme.rowHeight, labels.implicitHeight + 24)

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
        // in them as markup instead of the characters received.
        Text {
            objectName: "callRowName"
            width: parent.width
            text: row.name
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
            text: row.metaText
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
