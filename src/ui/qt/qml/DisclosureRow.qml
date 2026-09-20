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
    // The gate lives on the handler rather than on `enabled`, which is
    // hierarchical: binding it here would also disable everything the caller
    // nests inside the row.
    property bool tapEnabled: true
    signal tapped

    width: parent ? parent.width : 0
    height: Math.max(Theme.minimumTouchSize, labels.implicitHeight + 24)
    activeFocusOnTab: tapEnabled && Navigation.allows(row)
    Accessible.role: Accessible.Button
    Accessible.name: title
    Accessible.description: subtitle
    readonly property bool navigationAllowed: Navigation.allows(row)
    Accessible.ignored: !visible || !navigationAllowed
    Accessible.onPressAction: activate()
    function activate() {
        if (enabled && tapEnabled && Navigation.allows(row))
            tapped();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    FocusFrame {}

    Column {
        id: labels
        anchors.left: parent.left
        anchors.right: rowCaret.left
        anchors.leftMargin: Theme.cardPadding
        anchors.rightMargin: 12
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
            text: row.subtitle
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: row.subtitleColor
            wrapMode: Text.Wrap
        }
    }

    Caret {
        id: rowCaret

        anchors.right: parent.right
        anchors.rightMargin: Theme.cardPadding
        anchors.verticalCenter: parent.verticalCenter
        rotation: -90
        color: Theme.textSubdued
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
