// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Keeps a newest-first ListView showing its newest row.
//
// The lists this guards are newest-first, so the latest call is the top row —
// and a reader parked at the top expects to keep seeing it as calls land. That
// has to be asserted: a prepend below the top does not move the view, it moves
// the content, holding the rows being read still and putting the new call above
// the viewport. A list left off the top therefore never comes back on its own,
// and every call after that lands out of sight — the log quietly stops tracking
// the latest call.
//
// Whether the reader is on the latest call is not a mode they manage — it is
// read off the list where they left it, each time a call lands. Nothing to get
// stuck, and one flick back to the top resumes it.
//
// Declared as a sibling of the list rather than inside it: ListView reparents
// declared Item children into its contentItem, where they scroll away with the
// rows.
Item {
    id: root

    required property ListView list

    // Nothing to draw and nothing to lay out; only the behaviour below matters.
    visible: false
    width: 0
    height: 0

    // Within a row of the top still counts as being on the latest call: a stray
    // touch, an overscroll bounce or the keyboard resizing the view leaves the
    // list a few pixels down, and none of those mean "I am reading further back".
    readonly property bool atLatest: root.list.contentY - root.list.originY < Theme.rowHeight

    // Set when a call landed while a finger was down, so the top is asserted once
    // the hand comes off instead of under the press. See the handler below.
    property bool deferred: false

    // Re-assert the exact top.
    function pinToLatest() {
        // Never mid-movement: that is a flick still settling, and yanking it
        // would fight the reader's hand.
        if (!root.atLatest || root.list.moving) {
            root.deferred = false
            return
        }
        // Flickable.moving covers a drag and a flick but not a finger resting on
        // the list, which is neither. Repositioning under a held press moves the
        // content away from the press position the Flickable recorded when the
        // finger went down, and the reader's next drag starts by snapping back to
        // it — so wait for the release instead.
        if (touch.active) {
            root.deferred = true
            return
        }
        root.deferred = false
        if (root.list.contentY !== root.list.originY)
            root.list.positionViewAtBeginning()
    }

    Connections {
        target: root.list

        // Deferred: inside the signal the view is still applying the model
        // change, and a position asserted there does not survive it.
        function onCountChanged() {
            Qt.callLater(root.pinToLatest)
        }

        // A drag that starts before the finger lifts is the reader deciding where
        // to be; the pin they never saw is theirs to drop.
        function onMovingChanged() {
            if (root.list.moving)
                root.deferred = false
        }
    }

    // Passive grabs only — PointHandler never takes an exclusive one, so this
    // neither steals the list's flick nor the rows' taps. Parented to the list so
    // it sees presses anywhere over the rows.
    PointHandler {
        id: touch

        parent: root.list

        onActiveChanged: {
            if (!touch.active && root.deferred)
                Qt.callLater(root.pinToLatest)
        }
    }
}
