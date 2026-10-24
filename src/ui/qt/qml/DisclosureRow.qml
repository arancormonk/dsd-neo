// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// One disclosure row: title, helper line, trailing caret. The shape of every row
// that goes somewhere - the wizard's trunking-data pickers, the settings entry
// points, the RadioReference system list. The divider is the caller's to ask
// for, because only the caller knows whether the row is the last one in its
// panel.
Item {
    id: row

    property string title: ""
    property string subtitle: ""
    // Overridden where the helper line carries an answer rather than a hint.
    property color subtitleColor: Theme.textSubdued
    property bool showDivider: false
    property bool showCaret: true
    // The gate lives on the handler rather than on `enabled`, which is
    // hierarchical: binding it here would also disable everything the caller
    // nests inside the row.
    property bool tapEnabled: true
    // An expandable row shows or hides content below it rather than going
    // somewhere: its caret points down, turns up while expanded, and the row
    // reports itself as a checkable control whose checked state is `expanded`.
    property bool expandable: false
    property bool expanded: false
    property string accessibleName: title
    // A row whose helper line is a reading rather than a hint (the monitor's
    // "Signal & site" summary, issue #718): one elided line in the mono face
    // the readings use, optionally styled per token. The description stays
    // plain text for accessibility.
    property bool subtitleReading: false
    property int subtitleTextFormat: Text.PlainText
    property string accessibleDescription: subtitle
    // Space above and below the labels, together. A summary row on a crowded
    // screen asks for less than a row that goes somewhere.
    property int verticalPadding: 24
    signal tapped

    width: parent ? parent.width : 0
    height: Math.max(Theme.minimumTouchSize, labels.implicitHeight + verticalPadding)
    activeFocusOnTab: Navigation.allows(row) && tapEnabled
    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    Accessible.description: accessibleDescription
    Accessible.checkable: expandable
    Accessible.checked: expandable && expanded
    readonly property bool navigationAllowed: Navigation.allows(row)
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !tapEnabled || !Navigation.allows(row))
            return;
        // Issue #718: a row opened mid-word keeps the word; focus stays put.
        Navigation.commitComposing(false);
        tapped();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    FocusFrame {}

    Column {
        id: labels
        anchors.left: parent.left
        anchors.right: row.showCaret ? rowCaret.left : parent.right
        anchors.leftMargin: Theme.cardPadding
        anchors.rightMargin: row.showCaret ? 12 : Theme.cardPadding
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3

        Text {
            width: parent.width
            text: row.title
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }

        Text {
            width: parent.width
            // An expandable row may carry its whole label in the title. A reading
            // keeps its line while empty, so the row's height never follows it.
            visible: text.length > 0 || !row.expandable || row.subtitleReading
            text: row.subtitle
            textFormat: row.subtitleTextFormat
            font.family: row.subtitleReading ? Theme.mono : Theme.sans
            font.pixelSize: Theme.fontSize(row.subtitleReading ? 11 : 12)
            color: row.subtitleColor
            wrapMode: row.subtitleReading ? Text.NoWrap : Text.Wrap
            elide: row.subtitleReading ? Text.ElideRight : Text.ElideNone
            maximumLineCount: row.subtitleReading ? 1 : 1000
        }
    }

    Caret {
        id: rowCaret
        visible: row.showCaret

        anchors.right: parent.right
        anchors.rightMargin: Theme.cardPadding
        anchors.verticalCenter: parent.verticalCenter
        rotation: !row.expandable ? -90 : row.expanded ? 180 : 0
        color: Theme.textSubdued

        Behavior on rotation {
            NumberAnimation {
                duration: 150
                easing.type: Easing.OutCubic
            }
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.cardPadding
        height: 1
        visible: row.showDivider
        color: Theme.divider
    }

    TapHandler {
        enabled: row.tapEnabled
        onTapped: row.activate()
    }
}
