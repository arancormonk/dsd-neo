// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #782: text a screen reader reaches. A plain Text has no accessible
// interface, so Qt's Android bridge makes no node for it and TalkBack never
// reads it. This one is static text named by its own words, read where a
// swipe or touch reaches it and never announced on its own: Android speaks a
// rename only for the item it is focused on. It leaves the tree while hidden
// or behind the top layer, as MicroLabel does, and holds no name there, so a
// value that changes under a closed sheet raises nothing. Plain text only: a
// styled text sets its own name. A text an accessible parent already reads (a
// button's label, a row named for its whole line) stays a plain Text.
Text {
    id: readable
    // Its words can be a typed, imported or received name (issue #802):
    // drawn as written, never as markup. A styled text sets its own format.
    textFormat: Text.PlainText
    // A screen or sheet title, which TalkBack can move between as a heading.
    property bool heading: false
    Accessible.role: heading ? Accessible.Heading : Accessible.StaticText
    // A heading is not focusable unless told (see MicroLabel); static text is.
    // Only while in the tree (see OutlineButton).
    Accessible.focusable: heading && navigationAllowed && visible
    readonly property bool navigationAllowed: Navigation.allows(readable)
    // Gate first: see OutlineButton.
    Accessible.ignored: !navigationAllowed || !visible
    // From the gate itself, never from Accessible.ignored read back: a list
    // view sets that flag on each delegate it lays out, with no change signal,
    // so a row made under a sheet came back into the tree with the empty name
    // it had then and kept it (#830).
    Accessible.name: navigationAllowed && visible ? text : ""
}
