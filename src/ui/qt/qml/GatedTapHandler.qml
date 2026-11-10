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
// control's own guarded activation.
TapHandler {
    id: handler

    /** The item whose Navigation.allows() a press needs: the handler's parent. */
    property Item gate: parent
    /** Whether the gate item could take input when the press now held began. */
    property bool pressAllowed: false

    /** tapped(), for a tap whose press began, and which ends, while the gate item can take input. */
    signal allowedTap(var eventPoint, int button)
    /** longPressed(), likewise. */
    signal allowedLongPress

    onPressedChanged: {
        if (pressed)
            pressAllowed = !!gate && Navigation.allows(gate);
    }
    onTapped: function (eventPoint, button) {
        if (pressAllowed && Navigation.allows(gate))
            handler.allowedTap(eventPoint, button);
    }
    onLongPressed: {
        if (pressAllowed && Navigation.allows(gate))
            handler.allowedLongPress();
    }
}
