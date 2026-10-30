// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The mono micro-label that names every section: 10.5px IBM Plex Mono, wide
// tracking, uppercase, subdued.
Text {
    id: label
    Accessible.role: Accessible.Heading
    Accessible.name: text
    // Issue #782: Qt reports a Heading node focusable only when told to (its
    // text roles are focusable by themselves, Heading is not one), and TalkBack
    // skips a text node that is not focusable: without this a section's lines
    // were heard without the section they belong to.
    Accessible.focusable: true
    readonly property bool navigationAllowed: Navigation.allows(label)
    // Gate first: see OutlineButton.
    Accessible.ignored: !navigationAllowed || !visible
    font.family: Theme.mono
    font.pixelSize: Theme.fontSize(11)
    font.letterSpacing: Theme.fontSize(11) * 0.18
    font.capitalization: Font.AllUppercase
    color: Theme.textSubdued
}
