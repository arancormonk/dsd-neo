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
// button's label, a row named for its whole line) stays a plain Text. A line
// that reports a problem is one of these with the AlertMessage role, kept
// focusable so TalkBack's swipes reach it.
Text {
    id: readable
    // Issue #902: its reach for the keyboard (PressGate).
    property alias inReach: readable.reachable
    // Its words can be a typed, imported or received name (issue #802):
    // drawn as written, never as markup. A styled text sets its own format.
    textFormat: Text.PlainText
    // A screen or sheet title, which TalkBack can move between as a heading.
    property bool heading: false
    // Whether a screen reader reaches it: its navigation gate and its
    // effective visibility, and whatever a caller adds (a line kept but not
    // shown, a held row's). Its name, role, focusability and place in the
    // tree all follow this one predicate (issue #902): Qt keeps a static
    // text focusable, with SetFocus, whatever its flags, so an unreachable
    // one is a plain pane.
    property bool reachable: navigationAllowed && visible
    Accessible.role: !reachable ? Accessible.Pane : (heading ? Accessible.Heading : Accessible.StaticText)
    // Qt counts only StaticText and EditableText as text roles that are
    // focusable by themselves: a heading or an alert line is not unless told
    // (see MicroLabel), and TalkBack's swipes skip a text node that is not. So
    // it is told, for every role, only while in the tree (see OutlineButton).
    Accessible.focusable: reachable
    // Issue #902: a screen reader's SetFocus gives a reading the keyboard; when
    // it turns unreachable (hidden, covered, a caller's own gate) it hands it
    // back to the screen on top.
    readonly property bool navigationAllowed: Navigation.allows(readable)
    // Gate first: see OutlineButton.
    Accessible.ignored: !reachable
    // From the gate itself, never from Accessible.ignored read back: a list
    // view sets that flag on each delegate it lays out, with no change signal,
    // so a row made under a sheet came back into the tree with the empty name
    // it had then and kept it (#830).
    Accessible.name: reachable ? text : ""
}
