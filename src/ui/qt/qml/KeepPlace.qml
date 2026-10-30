// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Gives a ListView its model only while the view is visible, and keeps the
// reader's place across the gap (issue #782).
//
// A hidden view still builds a row for every row its model gains, and each row
// made under a hidden screen is announced to accessibility clients as hidden;
// a session runs on the monitor with the other screens under it, so they would
// pay for every call. Setting a view's model puts it back at its start, though,
// so on hide this notes the row at the top of the viewport and how far it sat
// from the top, and on show it puts that row back there in the same handler,
// before the first frame: no frame shows the start first. A reader at the start,
// or whose row has gone meanwhile (cleared, trimmed or filtered away), comes back
// at the start; so does one whose model was reset while hidden, as a shown view
// is on a reset.
//
// Declared as a sibling of the list, as FollowLatest is: ListView reparents
// declared Item children into its contentItem.
Item {
    id: root

    required property ListView list
    // The model the list shows while it is visible.
    property var model: null
    // The identity a row delegate stands for, and the view row that identity is
    // at now (-1 when it is gone). Supplied by the screen, which knows its rows.
    property var keyOf: function (row) {
        return undefined;
    }
    property var rowOf: function (key) {
        return -1;
    }
    // Where coming back at the start loses the reader nothing.
    property bool atStart: root.list.contentY - root.list.originY < 1

    // {key, offset} while hidden away from the start; null otherwise.
    property var held: null

    // Nothing to draw and nothing to lay out.
    visible: false
    width: 0
    height: 0

    // The row at the top of the viewport. A section header can sit there, so
    // look a little further down for the row under it.
    function topRow() {
        for (var dy = 1; dy < 3 * Theme.rowHeight; dy += 4) {
            var at = root.list.indexAt(root.list.width / 2, root.list.contentY + dy);
            if (at >= 0)
                return root.list.itemAtIndex(at);
        }
        return null;
    }

    function release() {
        if (!root.list.model)
            return;
        root.held = null;
        if (!root.atStart) {
            var row = root.topRow();
            var key = row ? root.keyOf(row) : undefined;
            if (key !== undefined && key !== null)
                root.held = {key: key, offset: row.y - root.list.contentY};
        }
        root.list.model = null;
    }

    function attach() {
        if (root.list.model === root.model)
            return;
        var place = root.held;
        root.held = null;
        root.list.model = root.model;
        var at = place ? root.rowOf(place.key) : -1;
        var row = null;
        if (at >= 0) {
            root.list.positionViewAtIndex(at, ListView.Beginning);
            row = root.list.itemAtIndex(at);
        }
        if (!row) {
            root.list.positionViewAtBeginning();
            return;
        }
        var start = root.list.originY;
        var end = Math.max(start, root.list.originY + root.list.contentHeight - root.list.height);
        root.list.contentY = Math.max(start, Math.min(row.y - place.offset, end));
    }

    function follow() {
        if (root.list.visible)
            root.attach();
        else
            root.release();
    }

    Connections {
        target: root.list

        function onVisibleChanged() {
            root.follow();
        }
    }

    // A reset while hidden is a new list: start it from the top, as a shown view
    // does.
    Connections {
        target: root.held !== null ? root.model : null
        ignoreUnknownSignals: true

        function onModelReset() {
            root.held = null;
        }
    }

    Component.onCompleted: follow()
}
