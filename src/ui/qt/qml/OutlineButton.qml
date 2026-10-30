// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Secondary action: 1px control-border outline, quiet fill, secondary text.
Rectangle {
    id: control

    property string text: ""
    property color textColor: Theme.buttonSecondaryText
    signal clicked

    property string accessibleName: text
    // Issue #718: the part of the gate that depends on typed input, kept apart
    // from `enabled` (busy, permission and other real guards). While a field
    // holds the keyboard's composing word the button stays live, so a tap can
    // commit that word and then recheck this before acting; otherwise the
    // button is disabled exactly as before.
    property bool inputValid: true
    enabled: inputValid || Navigation.composing
    // A caller that sizes the button for a fixed number of label lines caps
    // them here; a label longer than that elides rather than overflowing.
    property alias maximumLineCount: label.maximumLineCount
    // Text.Wrap by default, which breaks a word that does not fit; a caller
    // that sizes its cells for whole words passes Text.WordWrap.
    property alias wrapMode: label.wrapMode
    // A toggle keeps one label and shows its on state as checked (issue #718):
    // the cyan border and the pressed tint, the same in light and dark. Off by
    // default, so a plain button looks and reports as before.
    property bool checkable: false
    property bool checked: false
    // A toggle whose `checked` reads back a command the decoder applies later
    // (Hold scan, Hold TG, Mute): it shows and reports the state asked for from
    // the moment it is pressed, until `checked` changes or a few seconds pass
    // (issue #782). Android's bridge builds the click event TalkBack reads out
    // as soon as the press returns, and the decoder's state reaches `checked`
    // only on a later refresh, so TalkBack said the state before the press.
    property bool commandToggle: false
    property bool asked: false
    readonly property bool shownChecked: pendingToggle.running ? asked : checked
    onCheckedChanged: pendingToggle.stop()
    Timer {
        id: pendingToggle
        interval: 2000
    }
    // Held while the button has active focus: Qt refuses to drop activeFocusOnTab
    // from the active focus item (and warns), so a focused button that disables or
    // hides keeps it until the focus has moved, then the gate applies again.
    activeFocusOnTab: (Navigation.allows(control) && enabled && inputValid && visible) || activeFocus
    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    Accessible.focusable: true
    Accessible.checkable: checkable
    Accessible.checked: checkable && shownChecked
    readonly property bool navigationAllowed: Navigation.allows(control)
    // The navigation gate is read before visible, here and in activeFocusOnTab:
    // its walk up the parents can settle an ancestor's pending `parent:` binding
    // (the monitor's tier groups), and a visibility read before that would be
    // changed under this binding while it runs, a binding loop.
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !Navigation.allows(control))
            return;
        // Finish the keyboard's word and the field's editing before the action
        // reads the form; the validity bindings re-evaluate on the spot.
        Navigation.commitComposing(true);
        if (!inputValid)
            return;
        if (checkable && commandToggle) {
            asked = !shownChecked;
            pendingToggle.restart();
        }
        clicked();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    Keys.onEnterPressed: activate()
    FocusFrame {}

    implicitWidth: Math.max(Theme.minimumTouchSize, label.implicitWidth + 2 * Theme.cardPadding)
    implicitHeight: Math.max(Theme.minimumTouchSize, label.implicitHeight + 24)
    radius: Theme.radiusButton
    color: control.shownChecked || (tap.pressed && control.enabled && control.inputValid) ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
    border.width: 1
    border.color: control.shownChecked ? Theme.cyan : Theme.controlBorder
    opacity: enabled && inputValid ? 1.0 : 0.5

    Behavior on color {
        ColorAnimation {
            duration: 120
        }
    }

    Text {
        id: label
        anchors.centerIn: parent
        width: Math.max(0, control.width - 24)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        elide: Text.ElideRight
        text: control.text
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(15)
        font.weight: Font.DemiBold
        color: control.textColor
    }

    TapHandler {
        id: tap
        enabled: control.enabled
        onTapped: control.activate()
    }
}
