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
// A list with a header (the monitor's, issue #718) measures all of this against
// the top of its calls, where the header ends, not against the top of the
// content. While any of the header is in view a landing call never moves the
// view: ListView puts the new row at the top of the calls, directly under the
// header, so it is on screen and nothing being read scrolls away.
//
// Declared as a sibling of the list rather than inside it: ListView reparents
// declared Item children into its contentItem, where they scroll away with the
// rows.
Item {
    id: root

    required property ListView list
    // The row of the header that must hold still while the header changes height
    // (the monitor's "Signal & site" disclosure). A ListView header grows upward
    // from the first row, so without this a header growing below the row would
    // pull it up the screen.
    property Item headerAnchor: null

    // Nothing to draw and nothing to lay out; only the behaviour below matters.
    visible: false
    width: 0
    height: 0

    // Where the newest row starts, in content coordinates: the end of the header,
    // or the start of the content in a list without one.
    readonly property real latestY: root.list.headerItem ? root.list.headerItem.y + root.list.headerItem.height : root.list.originY
    // Some of the header is on screen. Position alone decides it, not focus.
    readonly property bool headerInView: root.list.headerItem !== null && root.list.contentY < root.latestY - 0.5

    // Within a row of the top still counts as being on the latest call: a stray
    // touch, an overscroll bounce or the keyboard resizing the view leaves the
    // list a few pixels down, and none of those mean "I am reading further back".
    readonly property bool atLatest: root.list.contentY - root.latestY < Theme.rowHeight

    // Set when a call landed while a finger was down, so the top is asserted once
    // the hand comes off instead of under the press. See the handler below.
    property bool deferred: false

    // Re-assert the exact top.
    function pinToLatest() {
        // Never mid-movement: that is a flick still settling, and yanking it
        // would fight the reader's hand. With the header in view the new row is
        // already on screen under it, and the header stays where the reader left
        // it.
        if (!root.atLatest || root.list.moving || root.headerInView) {
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
        if (root.list.headerItem === null) {
            if (root.list.contentY !== root.list.originY)
                root.list.positionViewAtBeginning()
        } else if (root.list.contentY !== root.latestY) {
            root.list.contentY = root.latestY
        }
    }

    // Header geometry the last time it changed: its height, and how far the
    // anchor row sits above the header's end. Plain values, not bindings: the
    // resize handler needs the height from before the change, and a binding
    // could already hold the new one.
    property real headerHeight: 0
    property real anchorDepth: 0
    // A correction owed while a finger is down, applied on release.
    property real pendingShift: 0

    function measureAnchorDepth() {
        var header = root.list.headerItem
        if (!header || !root.headerAnchor || !root.headerAnchor.visible)
            return 0
        return header.height - root.headerAnchor.mapToItem(header, 0, 0).y
    }

    // A ListView header keeps its end on the first row and grows or shrinks at its
    // top. While it is in view, move the view by what changed between the anchor
    // row and the header's end, so the anchor row keeps its place on screen and
    // what is below it moves instead. At the very start of the list the start is
    // kept, so whatever appeared above the anchor is shown rather than left above
    // the viewport.
    //
    // Measured from the header's end, read off the first row (or the footer of an
    // empty list), which a header resize does not move: the header's own y may or
    // may not have been updated yet when its height announces the change.
    //
    // Far down a long list the first row is recycled and there is nothing to read
    // the end off. The footer then sits at the far end of the calls, not under the
    // header, so it is only the boundary of an empty list. With the header in view
    // ListView places it against the first row's slot, so its own geometry gives
    // the end; with it off screen there is no end to measure and nothing to keep in
    // place — the reader is below the header, and the view stays where it is.
    function headerEnd() {
        if (root.list.count > 0) {
            var first = root.list.itemAtIndex(0)
            if (first)
                return first.y
            var header = root.list.headerItem
            if (header && root.headerInView)
                return header.y + header.height
            return null
        }
        if (root.list.footerItem)
            return root.list.footerItem.y
        return null
    }
    // Where the view stood against the header's end before the header last
    // changed: ListView may already have clamped contentY to the new bounds by
    // the time the header announces its new height, so the offset is kept from
    // the moves made between resizes.
    property real headerOffset: 0
    function noteOffset() {
        var header = root.list.headerItem
        if (!header || header.height !== root.headerHeight)
            return
        var end = root.headerEnd()
        if (end !== null)
            root.headerOffset = root.list.contentY - end
    }
    function onHeaderResized() {
        var header = root.list.headerItem
        if (!header)
            return
        var oldHeight = root.headerHeight
        var oldDepth = root.anchorDepth
        var offset = root.headerOffset
        root.headerHeight = header.height
        root.anchorDepth = root.measureAnchorDepth()
        var end = root.headerEnd()
        if (end === null || offset >= -0.5) {
            root.noteOffset()
            return
        }
        var atStart = offset <= -oldHeight + 0.5
        var next = end + (atStart ? -header.height : offset - (root.anchorDepth - oldDepth))
        if (touch.active) {
            // Applied on release; the view keeps whatever ListView left it at.
            root.pendingShift += root.list.contentY - next
            return
        }
        root.list.contentY = next
        root.noteOffset()
    }

    Connections {
        target: root.list

        // Deferred: inside the signal the view is still applying the model
        // change, and a position asserted there does not survive it.
        function onCountChanged() {
            afterCountChange.restart()
        }

        // A drag that starts before the finger lifts is the reader deciding where
        // to be; the pin they never saw, and a header correction owed from under
        // the press, are theirs to drop.
        function onMovingChanged() {
            if (root.list.moving) {
                root.deferred = false
                root.pendingShift = 0
            }
        }

        function onHeaderItemChanged() {
            root.noteHeader()
        }

        function onContentYChanged() {
            root.noteOffset()
        }
    }

    function noteHeader() {
        root.headerHeight = root.list.headerItem ? root.list.headerItem.height : 0
        root.anchorDepth = root.measureAnchorDepth()
        root.noteOffset()
    }
    Component.onCompleted: root.noteHeader()

    Connections {
        target: root.list.headerItem
        ignoreUnknownSignals: true

        function onHeightChanged() {
            root.onHeaderResized()
        }
    }

    // Passive grabs only — PointHandler never takes an exclusive one, so this
    // neither steals the list's flick nor the rows' taps. Parented to the list so
    // it sees presses anywhere over the rows.
    PointHandler {
        id: touch

        parent: root.list

        onActiveChanged: {
            if (touch.active)
                return
            if (root.pendingShift !== 0) {
                root.list.contentY -= root.pendingShift
                root.pendingShift = 0
                root.noteOffset()
            }
            if (root.deferred)
                afterRelease.restart()
        }
    }

    // The deferred calls run from timers this object owns rather than
    // Qt.callLater: a call queued that way still runs after the list and this
    // object are torn down (the monitor or a history screen closing as calls
    // land), in a context that no longer exists.
    Timer {
        id: afterCountChange
        interval: 0
        onTriggered: {
            root.pinToLatest()
            // A prepend above the viewport moves the first row, not contentY.
            root.noteOffset()
        }
    }
    Timer {
        id: afterRelease
        interval: 0
        onTriggered: root.pinToLatest()
    }
}
