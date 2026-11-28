// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
import QtQuick

// The app's TapHandler: it acts only on a tap whose press began while its item
// could take input (Navigation.allows()). With a mouse, a press on a control
// built on a TapHandler (which, unlike a MouseArea or a Qt Quick Control, does
// not take the press as an item) also reaches the tap handlers beneath it, a
// sheet's barrier notwithstanding; a touch press does not (measured, Qt 6.12).
// allows() refuses those while the cover is up, but a control that closes the
// cover on that release (Cancel, a menu row) has taken it away by the time the
// handler beneath reports its tap (#831). Read at the press, the answer is the
// one the press was made under; it is read again at the tap, for a cover that
// opened while the press was held.
//
// Callers handle allowedTap and allowedLongPress, not tapped and longPressed.
// Keyboard and accessibility presses do not come through here: they keep the
// control's own guarded activation. pressBegan() marks a press's start.
//
// Issue #909: a control whose tap depends on what it shows sets pressKey, a
// function naming what a tap would do now and what it would do it to (its
// action, the playlist, the item). The key is taken at the press, and the tap
// acts only when the key at the release is the same; otherwise it is dropped,
// never turned into another action on another target. Only that press's own
// tap uses the key: a keyboard or screen reader activation in between acts on
// the state at its moment and leaves the press alone, and a release with no
// press taken is dropped.
TapHandler {
    id: handler

    /** The item whose Navigation.allows() a press needs: the handler's parent. */
    property Item gate: parent
    /** Whether the gate item could take input when the press now held began. */
    property bool pressAllowed: false
    /** What a tap acts on now, as a string (null: a tap acts on whatever the control shows at the release). */
    property var pressKey: null
    /** The key the press now held began on; null with none taken. */
    property var keyAtPress: null

    /** tapped(), for a tap whose press began, and which ends, while the gate item can take input. */
    signal allowedTap(var eventPoint, int button)
    /** longPressed(), likewise. */
    signal allowedLongPress
    /** A press began (before any tap or drag it turns into). */
    signal pressBegan

    onPressedChanged: {
        if (pressed) {
            pressAllowed = !!gate && Navigation.allows(gate);
            keyAtPress = typeof pressKey === "function" ? String(pressKey()) : null;
            handler.pressBegan();
        }
    }
    // The press's key, taken by its tap: whether the control still shows what it began on.
    function keyHeld() {
        if (typeof pressKey !== "function")
            return true;
        var began = keyAtPress;
        keyAtPress = null;
        return began !== null && began === String(pressKey());
    }
    onTapped: function (eventPoint, button) {
        if (keyHeld() && pressAllowed && Navigation.allows(gate))
            handler.allowedTap(eventPoint, button);
    }
    onLongPressed: {
        if (pressAllowed && Navigation.allows(gate))
            handler.allowedLongPress();
    }
}
