// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #782: lets go of the keyboard focus when the window loses it, so that
// TalkBack can come back to the control it was on.
//
// Qt's Android bridge carries out TalkBack's "focus this" as a keyboard focus
// on the control (forceActiveFocus, through the set-focus action) and tells
// TalkBack the focus landed only when that makes the control take focus. When
// a system dialog (the USB permission prompt) takes the window's focus and
// gives it back, Qt hands the keyboard focus back to the same control, and
// TalkBack, coming back to it, asks to focus a control that has focus already:
// Qt reports success, nothing changes, no "focused" event follows, and every
// swipe asks again, until a touch moves the focus elsewhere. With the focus
// let go of while the window is away, TalkBack's next focus is a real change.
// A text field keeps its focus, for the keyboard.
QtObject {
    id: release
    property var window: null
    property bool enabled: false
    // The control that last had the keyboard focus in the window.
    property Item held: null
    // A window that loses the focus clears its active focus item (Qt Quick
    // gives the content item's focus up); within the window a focus always
    // lands somewhere, at least on a focus scope.
    readonly property Connections watcher: Connections {
        target: release.window
        function onActiveFocusItemChanged() {
            if (release.window.activeFocusItem)
                release.held = release.window.activeFocusItem;
            else
                release.letGo();
        }
    }
    function letGo() {
        var item = held;
        if (!enabled || !item || item.cursorPosition !== undefined)
            return;
        item.focus = false;
        held = null;
    }
}
