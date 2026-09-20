// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Text input in the house style: quiet surface, 1px control border that turns
// cyan with focus. `mono` switches the value to Plex Mono for data-shaped fields.
Rectangle {
    id: control

    property alias text: input.text
    property alias placeholderText: placeholder.text
    property alias inputMethodHints: input.inputMethodHints
    property alias input: input
    property bool mono: false
    property string label: placeholderText
    property string hint: ""
    property string error: ""
    property string unit: ""
    property var nextField: null
    property alias echoMode: input.echoMode
    signal accepted
    Accessible.ignored: true

    // Fires on Enter or focus loss — for fields whose consumer is too expensive
    // to run per keystroke (persisted preferences, argv rebuilds).
    signal editingFinished

    readonly property real labelHeight: label.length ? fieldLabel.implicitHeight + 8 : 0
    readonly property real helperHeight: helper.visible ? helper.implicitHeight + 8 : 0
    implicitHeight: Math.max(Theme.minimumTouchSize, input.implicitHeight + 24) + labelHeight + helperHeight
    radius: 10
    color: Theme.dark ? Theme.bg : Theme.panel
    border.width: 1
    border.color: error.length ? Theme.alert : input.activeFocus ? Theme.cyan : Theme.controlBorder

    Text {
        id: fieldLabel
        x: 14
        y: 8
        width: parent.width - 28
        visible: control.label.length > 0
        text: control.label + (control.unit.length ? " · " + control.unit : "")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        Accessible.ignored: true
    }

    TextInput {
        id: input

        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.topMargin: control.labelHeight
        anchors.bottomMargin: control.helperHeight
        verticalAlignment: TextInput.AlignVCenter
        font.family: control.mono ? Theme.mono : Theme.sans
        font.pixelSize: Theme.fontSize(15)
        color: Theme.textPrimary
        selectionColor: Qt.alpha(Theme.cyan, 0.35)
        selectedTextColor: Theme.textPrimary
        clip: true
        activeFocusOnTab: control.enabled && Navigation.allows(control)
        Accessible.role: Accessible.EditableText
        Accessible.name: control.label
        Accessible.description: [control.unit, control.hint, control.error].filter(function (s) {
            return s.length > 0;
        }).join(". ")
        Accessible.passwordEdit: echoMode === TextInput.Password
        readonly property bool navigationAllowed: Navigation.allows(control)
        Accessible.ignored: !visible || !navigationAllowed
        onAccepted: {
            control.accepted();
            if (control.nextField)
                control.nextField.input.forceActiveFocus();
            else {
                focus = false;
                Qt.inputMethod.hide();
            }
        }
        onEditingFinished: control.editingFinished()
    }

    Text {
        id: helper
        x: 14
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        width: parent.width - 28
        visible: text.length > 0
        text: control.error.length ? control.error : control.hint
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: control.error.length ? Theme.alert : Theme.textSecondary
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
    }

    Text {
        id: placeholder

        anchors.fill: input
        verticalAlignment: Text.AlignVCenter
        visible: input.text.length === 0 && !input.activeFocus && control.placeholderText !== control.label
        font: input.font
        color: Theme.textSubdued
        elide: Text.ElideRight
    }

    TapHandler {
        enabled: control.enabled && Navigation.allows(control)
        onTapped: input.forceActiveFocus()
    }
}
