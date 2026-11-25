// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// One disclosure row: title, helper line, trailing caret. The shape of every row
// that goes somewhere - the wizard's trunking-data pickers, the settings entry
// points, the RadioReference system list. The divider is the caller's to ask
// for, because only the caller knows whether the row is the last one in its
// panel.
Item {
    id: row
    // Issue #902: its reach for the keyboard (PressGate).
    readonly property bool inReach: row.Accessible.focusable

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
    // Held while it has the focus (see OutlineButton).
    activeFocusOnTab: (Navigation.allows(row) && visible && tapEnabled) || activeFocus
    Accessible.role: Accessible.Button
    // Named, described and a stop only while in the tree, from the gate
    // itself (see OutlineButton).
    Accessible.name: !navigationAllowed || !visible ? "" : accessibleName
    Accessible.description: !navigationAllowed || !visible ? "" : accessibleDescription
    Accessible.focusable: navigationAllowed && visible
    Accessible.checkable: expandable
    Accessible.checked: expandable && expanded
    readonly property bool navigationAllowed: Navigation.allows(row)
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !tapEnabled || !Navigation.allows(row) || !row.visible)
            return;
        // Issue #718: a row opened mid-word keeps the word; focus stays put.
        Navigation.commitComposing(false);
        tapped();
    }
    Keys.onPressed: function (event) {
        if (Navigation.activationPress(event, Window.activeFocusItem))
            activate();
    }
    Keys.onReleased: function (event) {
        Navigation.activationRelease(event);
    }
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
            // Titles can be a typed or imported name (a RadioReference system,
            // a key label, a recordings folder): drawn as written.
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }

        Text {
            id: subtitleLine
            width: parent.width
            // An expandable row may carry its whole label in the title. A reading
            // keeps its line while empty, so the row's height never follows it.
            visible: text.length > 0 || !row.expandable || row.subtitleReading
            // A reading draws one line (issue #901 r2 B1): a line break in it
            // (an engine answer, a name in the summary) is a space, so the
            // words after it are not lost to maximumLineCount.
            text: row.subtitleReading ? Util.oneLine(row.subtitle) : row.subtitle
            textFormat: row.subtitleTextFormat
            font.family: row.subtitleReading ? Theme.mono : Theme.sans
            font.pixelSize: Theme.fontSize(row.subtitleReading ? 11 : 12)
            color: row.subtitleColor
            wrapMode: row.subtitleReading ? Text.NoWrap : Text.Wrap
            elide: row.subtitleReading ? Text.ElideRight : Text.ElideNone
            maximumLineCount: row.subtitleReading ? 1 : 1000
            // A reading's line fits its box (r5 H3): an answer in a taller
            // script is drawn smaller, never over the row's title.
            fontSizeMode: row.subtitleReading ? Text.VerticalFit : Text.FixedSize
            minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
            // A reading's line is a fixed box (LineBoxes, issue #901 D10): its
            // font's height, never its text's, so no script a reading or an
            // answer in its place shows (the Monitor's) changes the row's
            // height. A hint keeps its own wrapped height. subtitleReading is a
            // row's kind, set where the row is declared and never changed: a
            // Binding whose `when` turns false puts back the height it found
            // when it first applied, a fixed number, not the text's own.
            verticalAlignment: Text.AlignVCenter
            Binding on height {
                when: row.subtitleReading
                value: LineBoxes.lineHeight(subtitleLine.font.family, subtitleLine.font.pixelSize, subtitleLine.font.weight)
            }
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

    GatedTapHandler {
        enabled: row.tapEnabled
        onAllowedTap: row.activate()
    }
}
