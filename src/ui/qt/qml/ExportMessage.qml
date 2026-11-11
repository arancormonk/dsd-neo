// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The line under an export button (Signal search, History): the result, plain
// text (it can name a saved file), read out by its screen once the line can be
// reached. An alert a swipe reaches while in the tree; it leaves the tree, and
// holds no name, behind an open sheet (ReadableText).
ReadableText {
    // {text, error, heard}: the screen's record of the last result.
    property var message: ({})
    width: parent ? parent.width : 0
    visible: text.length > 0
    text: message.text || ""
    wrapMode: Text.Wrap
    color: message.error ? Theme.alert : Theme.textSecondary
    font.family: Theme.sans
    font.pixelSize: Theme.fontSize(14)
    Accessible.role: Accessible.AlertMessage
}
