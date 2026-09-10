// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window

// A centred panel over a dimmed screen, dismissed by tapping outside it.
//
// The barrier stops delivery to controls behind every sheet. Panel controls
// appear above it; clicks on otherwise empty panel space do not dismiss it.
Rectangle {
    id: sheet

    /** Panel contents, laid out top to bottom. */
    default property alias content: column.data
    /** Gap between the panel's children. */
    property alias spacing: column.spacing
    /** Names the panel item itself, for tests that address it directly. */
    property alias panelObjectName: panelItem.objectName
    property string accessibleName: qsTr("Dialog")
    property var previousFocus: null
    property var dismissHandler: null
    property bool adjustingFocus: false
    readonly property bool isTopModal: Navigation.topModal === sheet
    Accessible.role: Accessible.Dialog
    Accessible.name: accessibleName
    Accessible.ignored: !visible || !isTopModal
    z: 100 + Math.max(0, Navigation.modals.indexOf(sheet))

    function requestDismiss() {
        if (dismissHandler) {
            dismissHandler();
            return;
        }
        visible = false;
        dismissed();
    }
    function focusInside() {
        if (!visible || !isTopModal || adjustingFocus)
            return;
        var window = sheet.Window.window;
        if (window && !Navigation.contains(sheet, window.activeFocusItem)) {
            adjustingFocus = true;
            sheet.forceActiveFocus();
            adjustingFocus = false;
        }
    }
    Connections {
        target: sheet
        function onVisibleChanged() {
            var window = sheet.Window.window;
            if (sheet.visible) {
                sheet.previousFocus = window ? window.activeFocusItem : null;
                Navigation.clearInput(window);
                Navigation.addModal(sheet);
                sheet.forceActiveFocus();
            } else {
                Navigation.removeModal(sheet);
                Navigation.clearInput(window);
                if (sheet.previousFocus && sheet.previousFocus !== sheet && Navigation.presented(sheet.previousFocus) && sheet.previousFocus.enabled && Navigation.allows(sheet.previousFocus))
                    sheet.previousFocus.forceActiveFocus();
                sheet.previousFocus = null;
            }
        }
    }
    Component.onCompleted: {
        if (visible)
            Navigation.addModal(sheet);
    }
    Component.onDestruction: Navigation.removeModal(sheet)
    Keys.onEscapePressed: Navigation.back(sheet.Window.window)
    Keys.onBackPressed: Navigation.back(sheet.Window.window)

    // keyboardRectangle is in window coordinates. Mapping it avoids subtracting
    // the keyboard twice when Android has already resized the usable window.
    property real keyboardTop: Theme.keyboardTop(sheet)
    property real maximumHeight: Math.max(0, Math.min(height, keyboardTop) - 2 * Theme.screenPadding)

    function revealFocus() {
        Theme.revealFocus(scroll, column, sheet.Window.window ? sheet.Window.window.activeFocusItem : null);
    }

    Connections {
        target: sheet.Window.window
        function onActiveFocusItemChanged() {
            Qt.callLater(sheet.focusInside);
            Qt.callLater(sheet.revealFocus);
        }
    }

    /** Emitted after a tap on the scrim has hidden the sheet. */
    signal dismissed

    /** Whether a point in this sheet's coordinates lies on the panel. */
    function hitsPanel(x, y) {
        var p = sheet.mapToItem(panelItem, x, y);
        return p.x >= 0 && p.y >= 0 && p.x <= panelItem.width && p.y <= panelItem.height;
    }

    anchors.fill: parent
    visible: false
    color: Qt.alpha("#000000", 0.5)

    PointerBarrier {
        onClicked: function (mouse) {
            if (sheet.hitsPanel(mouse.x, mouse.y))
                return;
            sheet.requestDismiss();
            // A sheet dismissed with a field still focused leaves the soft
            // keyboard standing over the screen it went back to.
            Qt.inputMethod.hide();
        }
    }

    UiPanel {
        id: panelItem

        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(0, (Math.min(sheet.height, sheet.keyboardTop) - height) / 2)
        width: Math.min(Theme.formWidth, parent.width - 2 * Theme.screenPadding)
        height: Math.min(sheet.maximumHeight, column.height + 2 * Theme.cardPadding)

        PlexFlickable {
            id: scroll
            objectName: "modalSheetScroll"
            x: Theme.cardPadding
            y: Theme.cardPadding
            width: Math.max(0, parent.width - 2 * Theme.cardPadding)
            height: Math.max(0, parent.height - 2 * Theme.cardPadding)
            clip: true
            contentWidth: width
            contentHeight: column.height
            boundsBehavior: Flickable.StopAtBounds
            onHeightChanged: Qt.callLater(sheet.revealFocus)

            Column {
                id: column
                width: scroll.width
                spacing: 12
            }
        }
    }
}
