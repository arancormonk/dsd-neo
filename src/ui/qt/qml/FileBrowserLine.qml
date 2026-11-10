// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #830: a line of the file browser under a row or a heading: a hint in
// the subdued colour, or, as a `problem`, what went wrong, in the alert colour
// and reported as an alert.
ReadableText {
    property bool problem: false
    width: parent ? parent.width : 0
    font.family: Theme.sans
    font.pixelSize: Theme.fontSize(problem ? 13 : 12)
    color: problem ? Theme.alert : Theme.textSubdued
    wrapMode: Text.Wrap
    Accessible.role: problem ? Accessible.AlertMessage : Accessible.StaticText
}
