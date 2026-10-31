// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Flickable {
    id: viewport
    property string accessibleName: qsTr("Scrollable content")
    Accessible.role: Accessible.Pane
    Accessible.name: accessibleName
    // Issue #782: takes the keyboard focus when TalkBack lands on it. Qt's
    // Android bridge carries out TalkBack's focus with the node's set-focus
    // action, and hears the focus landed only from the focus change it makes;
    // a container with none left the control before it holding the keyboard
    // focus, and TalkBack's focus back onto that control changed nothing and
    // was never heard. Not a Tab stop.
    Accessible.focusable: navigationAllowed && visible
    readonly property bool navigationAllowed: Navigation.allows(viewport)
    Accessible.ignored: !navigationAllowed || !visible
    function scrollPage(direction) {
        if (!enabled || !Navigation.allows(viewport))
            return;
        contentY = Math.max(0, Math.min(contentHeight - height, contentY + direction * Math.max(48, height * 0.8)));
    }
    Accessible.onScrollDownAction: scrollPage(1)
    Accessible.onScrollUpAction: scrollPage(-1)
    Accessible.onNextPageAction: scrollPage(1)
    Accessible.onPreviousPageAction: scrollPage(-1)
}
