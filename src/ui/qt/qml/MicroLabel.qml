// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The mono micro-label that names every section: 10.5px IBM Plex Mono, wide
// tracking, uppercase, subdued.
Text {
    font.family: Theme.mono
    font.pixelSize: 11
    font.letterSpacing: 11 * 0.18
    font.capitalization: Font.AllUppercase
    color: Theme.textSubdued
}
