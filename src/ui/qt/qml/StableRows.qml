// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #782: a Repeater model of `size` rows that grows and shrinks at its
// end, for a list handed over whole on every refresh. A JavaScript list (or a
// count) as the model rebuilds every row when it changes, and each row a
// screen reader is told about raises a show or hide as it goes; here a row
// is added or removed only when the count changes, and each delegate reads
// its entry from the list by `index`, in place.
ListModel {
    id: rows
    property int size: 0
    onSizeChanged: resize()
    Component.onCompleted: resize()
    function resize() {
        while (count < size)
            append({row: count});
        while (count > size)
            remove(count - 1);
    }
}
