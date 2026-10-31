// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window

// A centred panel over a dimmed screen.
//
// Only a real tap outside the panel dismisses it: press and release both off
// the panel, with no more movement than a drag threshold. A press that starts
// on the panel and ends beside it, or a drag in the gutter, is not a tap.
// Form sheets (typed input or an in-progress choice) set closesOnOutsideTap to
// false and leave only through their own Cancel/close control, Back or Escape,
// each of which goes through their discard check (issue #718). Menus,
// confirmations, choice sheets, read-only and live-apply sheets keep the
// outside tap.
//
// The barrier accepts every press, so nothing behind the sheet receives
// presses, drags or releases. Panel controls appear above it; taps on
// otherwise empty panel space do not dismiss it.
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
    /** Whether a tap outside the panel asks to dismiss; false for form sheets. */
    property bool closesOnOutsideTap: true
    property bool adjustingFocus: false
    readonly property bool isTopModal: Navigation.topModal === sheet
    Accessible.role: Accessible.Dialog
    // Issue #782: nameless while covered or closed, like the rest of it.
    Accessible.name: isTopModal && visible ? accessibleName : ""
    Accessible.ignored: !visible || !isTopModal
    z: 100 + Math.max(0, Navigation.modals.indexOf(sheet))

    // Subclasses with their own open(...) overload bypass this pre-visible focus capture.
    function open() {
        if (visible)
            return;
        // A caller may disable its opener when the sheet becomes visible.
        // Capture focus before those bindings move it to the window root.
        var window = sheet.Window.window;
        var opener = window ? window.activeFocusItem : null;
        visible = true;
        previousFocus = opener;
    }
    // Escape, Back and an outside tap. The composing word is committed first,
    // so a discard check sees what was typed.
    function requestDismiss() {
        Navigation.commitComposing(false);
        if (dismissHandler) {
            dismissHandler();
            return;
        }
        visible = false;
        dismissed();
    }
    function restoreFocus(opener) {
        if (visible)
            return false;
        var targets = [opener];
        for (var surface = sheet.parent; surface; surface = surface.parent)
            targets.push(surface);
        if (Navigation.rootSurfaces.length)
            targets.push(Navigation.rootSurfaces[Navigation.rootSurfaces.length - 1]);
        for (var target of targets) {
            if (target && target !== sheet && Navigation.presented(target) && target.enabled && Navigation.allows(target)) {
                target.forceActiveFocus();
                return true;
            }
        }
        return false;
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
                // A reused sheet opens at its top, not where its last visit was
                // scrolled to (issue #718). Before the focus moves below, so the
                // focus reveal that follows still scrolls to a focused field.
                scroll.contentX = 0;
                scroll.contentY = 0;
                sheet.previousFocus = window ? window.activeFocusItem : null;
                Navigation.clearInput(window);
                Navigation.addModal(sheet);
                sheet.forceActiveFocus();
            } else {
                Navigation.removeModal(sheet);
                Navigation.clearInput(window);
                var opener = sheet.previousFocus;
                sheet.previousFocus = null;
                sheet.restoreFocus(opener);
                // Bindings may settle or the confirmed action may hide the opener after dismissal.
                Qt.callLater(sheet.restoreFocus, opener);
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
    /** Scroll @p item, one of the panel's children, into view: an error that appeared below the fold. */
    function reveal(item) {
        column.forceLayout();
        Theme.revealFocus(scroll, column, item);
    }

    Connections {
        target: sheet.Window.window
        function onActiveFocusItemChanged() {
            Qt.callLater(sheet.focusInside);
            Qt.callLater(sheet.revealFocus);
        }
    }

    /** Emitted after an outside tap, Escape or Back has hidden a sheet without a dismissHandler. */
    signal dismissed

    /** Whether a point in this sheet's coordinates lies on the panel. */
    function hitsPanel(x, y) {
        var p = sheet.mapToItem(panelItem, x, y);
        return p.x >= 0 && p.y >= 0 && p.x <= panelItem.width && p.y <= panelItem.height;
    }

    anchors.fill: parent
    visible: false
    color: Qt.alpha("#000000", 0.5)

    // Issue #782: tells Android's bridge to read the window's tree again each
    // time this sheet becomes the top layer or stops being it (opened, covered
    // by another sheet, uncovered, closed). Covered or closed, a sheet leaves
    // the accessibility tree, and leaving it raises nothing the bridge passes
    // on, so TalkBack went on swiping through the copy it held of a covered
    // sheet. This item, under the window's content item and never left out of
    // the tree, shows and hides with the sheet's turn on top; the bridge
    // passes a show or hide on as a change to the whole subtree under its
    // parent, the window. It has no size, so it is reported invisible, and no
    // name: nothing reads or reaches it.
    Item {
        objectName: sheet.objectName.length ? sheet.objectName + "TreeRefresh" : "modalSheetTreeRefresh"
        parent: sheet.Window.contentItem
        width: 0
        height: 0
        visible: sheet.visible && sheet.isTopModal
        Accessible.role: Accessible.Grouping
        Accessible.focusable: false
    }

    PointerBarrier {
        // A tap, not a click: MouseArea.clicked fires for any press and release
        // inside the area however far the pointer travelled, so a press on the
        // panel's padding that slid off, or a drag in the gutter, would close it.
        property real pressX: 0
        property real pressY: 0
        property bool pressedOutside: false
        property bool moved: false
        onPressed: function (mouse) {
            pressX = mouse.x;
            pressY = mouse.y;
            pressedOutside = !sheet.hitsPanel(mouse.x, mouse.y);
            moved = false;
        }
        onPositionChanged: function (mouse) {
            if (Math.abs(mouse.x - pressX) > Qt.styleHints.startDragDistance || Math.abs(mouse.y - pressY) > Qt.styleHints.startDragDistance)
                moved = true;
        }
        onCanceled: pressedOutside = false
        onReleased: function (mouse) {
            var tapped = pressedOutside && !moved && !sheet.hitsPanel(mouse.x, mouse.y);
            pressedOutside = false;
            if (!tapped || !sheet.closesOnOutsideTap)
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
