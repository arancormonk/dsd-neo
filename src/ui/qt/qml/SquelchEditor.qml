// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The squelch analog sessions start with (issue #899), for Settings: Off, Auto
// or Noise, and for the last two a margin of 3 to 30 dB stepped 1 dB at a time.
// It edits only what the app default can hold ("off", "auto+N", "noise+N");
// fixed levels stay with a session or a saved system. The caller owns the
// value: this never writes `setting`, it reports each choice and step as the
// setting text (`chosen`). Off keeps the margin it left, so Auto or Noise come
// back to it while the editor lives.
Column {
    id: editor

    property string title: qsTr("Squelch")
    property string setting: Util.SQUELCH_DEFAULT
    property string modeObjectName: ""
    property string marginObjectName: ""
    property string hintObjectName: ""
    property string accessibleName: title
    readonly property int minMargin: typeof commands !== "undefined" && commands ? commands.squelchMarginMinDb : 3
    readonly property int maxMargin: typeof commands !== "undefined" && commands ? commands.squelchMarginMaxDb : 30

    // The setting as shown: 0 Off, 1 Auto, 2 Noise, and its margin.
    readonly property var parsed: Util.parseSquelchSetting(setting) || Util.parseSquelchSetting(Util.SQUELCH_DEFAULT)
    readonly property int mode: parsed.mode
    property int keptMargin: 10
    readonly property int margin: mode > 0 ? parsed.margin : keptMargin
    onMarginChanged: keptMargin = margin
    Component.onCompleted: keptMargin = margin

    signal chosen(string setting)

    function choose(index) {
        if (index !== mode)
            chosen(Util.squelchSettingText(index, margin));
    }
    function step(delta) {
        var next = Math.max(minMargin, Math.min(maxMargin, margin + delta));
        if (mode > 0 && next !== margin)
            chosen(Util.squelchSettingText(mode, next));
    }

    spacing: 6

    MicroLabel {
        text: editor.title
    }
    SegmentedControl {
        objectName: editor.modeObjectName
        accessibleName: editor.accessibleName
        width: parent.width
        model: [qsTr("Off"), qsTr("Auto"), qsTr("Noise")]
        currentIndex: editor.mode
        onSelected: function (index) {
            editor.choose(index);
        }
    }
    // Auto and Noise open a margin over the channel's noise: the buttons step it.
    Row {
        width: parent.width
        visible: editor.mode > 0
        spacing: 10
        ReadableText {
            id: marginText
            objectName: editor.marginObjectName
            width: parent.width - 116
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("+%1 dB").arg(editor.margin)
            Accessible.name: reachable ? qsTr("Squelch margin %1 dB").arg(editor.margin) : ""
            color: Theme.textPrimary
            font.family: Theme.mono
            font.pixelSize: Theme.fontSize(14)
        }
        OutlineButton {
            objectName: editor.marginObjectName.length > 0 ? editor.marginObjectName + "Down" : ""
            width: 48
            text: "−"
            accessibleName: qsTr("Decrease Squelch Margin")
            enabled: editor.margin > editor.minMargin
            onClicked: editor.step(-1)
        }
        OutlineButton {
            objectName: editor.marginObjectName.length > 0 ? editor.marginObjectName + "Up" : ""
            width: 48
            text: "+"
            accessibleName: qsTr("Increase Squelch Margin")
            enabled: editor.margin < editor.maxMargin
            onClicked: editor.step(1)
        }
    }
    ReadableText {
        objectName: editor.hintObjectName
        width: parent.width
        text: editor.mode === 2 ? qsTr("Quiet until a signal comes in. Higher numbers need stronger signals. AM and narrow channels use Auto.")
            : editor.mode === 1 ? qsTr("Opens over each channel's own noise, learned in the first moments. Higher numbers need stronger signals.")
            : qsTr("Plays the receiver's hiss between transmissions.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSecondary
    }
    ReadableText {
        width: parent.width
        text: qsTr("For NFM and AM on a radio, unless a saved system has its own.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSecondary
    }
}
