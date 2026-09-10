// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// One toggle row: title, helper line, switch. The shape every panel that asks a
// yes/no question uses - the settings panels and the RadioReference import
// options. The divider is the caller's to ask for, because only the caller knows
// whether the row is the last one in its panel.
Item {
    id: row

    property string title: ""
    property string subtitle: ""
    property bool checked: false
    property bool showDivider: false
    signal toggled(bool checked)

    width: parent ? parent.width : 0
    height: Math.max(Theme.minimumTouchSize, labels.implicitHeight + 24)
    activeFocusOnTab: enabled && Navigation.allows(row)
    Accessible.role: Accessible.Switch
    Accessible.name: title
    Accessible.description: subtitle
    Accessible.checkable: true
    Accessible.checked: checked
    readonly property bool navigationAllowed: Navigation.allows(row)
    Accessible.ignored: !visible || !navigationAllowed
    Accessible.onPressAction: activate()
    Accessible.onToggleAction: activate()
    function activate() {
        if (enabled && Navigation.allows(row))
            toggled(!checked);
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    FocusFrame {}
    TapHandler {
        onTapped: row.activate()
    }

    Column {
        id: labels
        anchors.left: parent.left
        anchors.right: rowSwitch.left
        anchors.leftMargin: Theme.cardPadding
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3

        Text {
            width: parent.width
            Accessible.ignored: true
            text: row.title
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }

        Text {
            width: parent.width
            visible: text.length > 0
            Accessible.ignored: true
            text: row.subtitle
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSubdued
            // Wrapped rather than elided: the helper line is what makes the
            // question answerable, and 62px holds the two 12px lines a wrapped
            // one needs. Eliding instead truncates the longer helpers on a
            // narrow screen, which is where they matter most.
            wrapMode: Text.Wrap
        }
    }

    PlexSwitch {
        id: rowSwitch

        anchors.right: parent.right
        anchors.rightMargin: Theme.cardPadding
        anchors.verticalCenter: parent.verticalCenter
        checked: row.checked
        interactive: false
        onToggled: function (state) {
            row.toggled(state);
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
}
