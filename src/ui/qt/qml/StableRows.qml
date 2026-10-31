// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #782: a Repeater model for a list handed over whole on every refresh.
// A JavaScript list (or a count) as the model rebuilds every row when it
// changes, and each row a screen reader is told about raises a show or hide as
// it goes. Here each row holds one entry, told apart by `keyOf(entry, index)`
// (by default its place), with the words `lineOf(entry, index)` gives it in
// its `line` role:
// - an entry that stays keeps its row, moved to its new place (a move raises
//   no show, hide or rename), and is renamed only when its own words change;
// - an entry that arrives takes the row of one that left, renamed, else a new
//   row; a row is added or removed only when the count changes;
// - rows are added and renamed at once, and let go at the end of the turn, so
//   a list emptied and filled again in one turn keeps its rows;
// - `shown` is the length of the list as it is now: a view draws rows from
//   `shown` on at no opacity, so a row that left is not drawn even for the rest
//   of the turn (an opacity raises no accessibility event);
// - `live` is the rows holding an entry, at once on a shrink and at the end of
//   the turn on emptying: a view names rows from `live` on as nothing;
// - with `holdRows`, the rows past the end of the list stay, held, whether the
//   list shrank or emptied: the view leaves rows from `live` on out of the tree
//   as well, and a longer list later reuses them rather than adding rows.
ListModel {
    id: rows
    property var entries: []
    property var keyOf: function (entry, index) {
        return String(index);
    }
    property var lineOf: function (entry, index) {
        return "";
    }
    property bool holdRows: false
    property int live: 0
    property int shown: 0
    onEntriesChanged: sync()
    Component.onCompleted: sync()

    function sync() {
        var list = entries || [];
        var n = list.length;
        var want = [];
        var words = [];
        var wanted = Object.create(null);
        for (var i = 0; i < n; ++i) {
            want.push(String(keyOf(list[i], i)));
            words.push(String(lineOf(list[i], i)));
            wanted[want[i]] = true;
        }
        var keys = [];
        var lines = [];
        for (i = 0; i < count; ++i) {
            var held = get(i);
            keys.push(held.key);
            lines.push(held.line);
        }
        // Rows before `i` hold their entries; the rest are searched.
        for (i = 0; i < n; ++i) {
            var at = keys.indexOf(want[i], i);
            if (at < 0) {
                // The first row further on whose entry has left.
                for (var j = i; j < keys.length && at < 0; ++j) {
                    if (!wanted[keys[j]])
                        at = j;
                }
                if (at < 0) {
                    insert(i, {key: want[i], line: words[i]});
                    keys.splice(i, 0, want[i]);
                    lines.splice(i, 0, words[i]);
                    continue;
                }
                setProperty(at, "key", want[i]);
                keys[at] = want[i];
            }
            if (at !== i) {
                move(at, i, 1);
                keys.splice(i, 0, keys.splice(at, 1)[0]);
                lines.splice(i, 0, lines.splice(at, 1)[0]);
            }
            if (lines[i] !== words[i]) {
                setProperty(i, "line", words[i]);
                lines[i] = words[i];
            }
        }
        shown = n;
        if (n > 0)
            live = n;
        if (n === 0 || count > n)
            Qt.callLater(rows.trim);
    }
    // The rows past the end of the list as it is at the end of the turn.
    function trim() {
        var n = entries ? entries.length : 0;
        shown = n;
        if (count > n && !holdRows)
            remove(n, count - n);
        live = n;
    }
}
