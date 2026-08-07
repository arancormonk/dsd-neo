// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The base surface every card sits on: panel fill, 1px stroke, 12px radius.
Rectangle {
    color: Theme.panel
    border.width: 1
    border.color: Theme.panelBorder
    radius: Theme.radiusPanel
}
