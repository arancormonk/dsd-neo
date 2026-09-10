// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Flickable {
    id: viewport
    property string accessibleName: qsTr("Scrollable content")
    Accessible.role: Accessible.Pane
    Accessible.name: accessibleName
    Accessible.focusable: false
    readonly property bool navigationAllowed: Navigation.allows(viewport)
    Accessible.ignored: !visible || !navigationAllowed
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
