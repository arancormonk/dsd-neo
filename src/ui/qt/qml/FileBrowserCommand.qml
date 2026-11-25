// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #830: text to type on a computer (a folder's path, an adb line), each
// line in a selectable block of its own so a reader copying it by hand can tell
// where one ends and the next begins, and one Copy for all of them, one per
// line. Each block's text is named `<objectName>Text<index>` for tests.
Column {
    id: block
    property var lines: []
    readonly property string copyText: lines.join("\n")
    property string copyName: "fileBrowserCopy"
    // True for a while after a copy the clipboard took.
    property bool copied: false
    signal copyRequested(string text)
    width: parent ? parent.width : 0
    spacing: 6

    Repeater {
        model: block.lines
        Rectangle {
            id: lineBlock
            required property int index
            required property string modelData
            width: block.width
            height: lineText.implicitHeight + 20
            radius: Theme.radiusButton
            color: Theme.bg
            border.width: 1
            border.color: Theme.controlBorder
            TextEdit {
                id: lineText
                // Issue #902: its reach for the keyboard (PressGate).
                readonly property bool inReach: lineText.navigationAllowed && lineText.visible
                objectName: block.objectName + "Text" + lineBlock.index
                x: 10
                y: 10
                width: parent.width - 20
                text: lineBlock.modelData
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                // Breaks between words first, so a long command splits where
                // it is still readable.
                wrapMode: TextEdit.Wrap
                color: Theme.textPrimary
                selectionColor: Qt.alpha(Theme.cyan, 0.35)
                selectedTextColor: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
                readonly property bool navigationAllowed: Navigation.allows(lineText)
                activeFocusOnTab: false
                // Issue #902: Qt makes a text node focusable whatever its flags;
                // covered or hidden, a plain pane (see PlexTextField).
                Accessible.role: navigationAllowed && visible ? Accessible.StaticText : Accessible.Pane
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.name: !navigationAllowed || !visible ? "" : text
                Accessible.focusable: navigationAllowed && visible
                // Selectable, so it can hold the keyboard (a tap, a screen
                // reader's focus): hidden or covered, it gives it up.
            }
        }
    }
    Item {
        width: 1
        height: 2
    }
    OutlineButton {
        objectName: block.copyName
        text: block.copied ? qsTr("Copied") : qsTr("Copy")
        onClicked: block.copyRequested(block.copyText)
    }
}
