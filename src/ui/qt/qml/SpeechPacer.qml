// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Issue #782: the words a screen reader is given for a reading that moves
// faster than anyone can listen. Qt's Android bridge speaks every rename of
// the node TalkBack is focused on, so a reading named by its live text would
// be read out on every tick. `spoken` catches up with the reading at most once
// a period (5 s), and soon after `step` changes (a call starts or ends, a new
// scan target or carrier, a lock or sync change) or the reading appears or
// disappears. The text on screen stays live. Nothing here runs per tick: the
// reading is read only when the period or a step comes round, and the timer
// runs only while the reading is shown.
//
// A step input can itself flap (a marginal lock, a squelch gate, a reading
// that comes and goes), so steps have a floor: a step is heard 1 ms after it
// when the last step was heard at least a floor ago (1 s), else at the end of
// that floor, with the state as it then is.
//
// The reading is `group`'s shown text (Util.shownText), whose children coming
// and going is itself a step, else what `read()` returns. The period and the
// floor are Navigation.speechPeriodMs and Navigation.speechStepFloorMs.
Timer {
    id: pacer
    property Item group: null
    property var read: null
    property string step: ""
    property bool shown: true
    property string spoken: ""
    property int period: Navigation.speechPeriodMs
    readonly property string groupKey: group ? Util.shownKey(group) : ""
    // A step that came while the floor ran, heard when it ends.
    property bool stepPending: false
    readonly property Timer floor: Timer {
        interval: Navigation.speechStepFloorMs
        onTriggered: {
            if (pacer.stepPending && pacer.shown) {
                pacer.stepPending = false;
                pacer.settle();
                start();
            }
        }
    }

    function current() {
        if (!shown)
            return "";
        if (group)
            return Util.shownText(group);
        return read ? String(read()) : "";
    }
    // Right after now, once all of a step's bindings have settled (a QML Timer
    // does not fire at an interval of 0): one rename for a step that changes
    // several readings, or rebuilds a group row by row.
    function settle() {
        interval = 1;
        restart();
    }
    function stepped() {
        if (!shown)
            return;
        if (floor.running) {
            stepPending = true;
            return;
        }
        settle();
        // A floor under 2 ms is none (tests that check each step on its own).
        if (floor.interval > 1)
            floor.start();
    }

    interval: period
    repeat: true
    onTriggered: {
        if (interval !== period)
            interval = period;
        var words = current();
        if (spoken !== words)
            spoken = words;
    }
    // A new period applies from now, not after the one already running.
    onPeriodChanged: {
        if (running && interval !== 1)
            interval = period;
    }
    onStepChanged: stepped()
    onGroupKeyChanged: stepped()
    onShownChanged: {
        if (shown) {
            spoken = current();
            settle();
        } else {
            stop();
            floor.stop();
            stepPending = false;
            spoken = "";
        }
    }
    Component.onCompleted: {
        if (shown) {
            spoken = current();
            settle();
        }
    }
}
