// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Issue #782: a ReadableText whose value moves faster than every few seconds
// (a countdown, a duration, a readout being dragged). It shows its value live
// and names itself through a SpeechPacer: at most every 5 s, and at once when
// `step` changes (set it to what makes a change worth hearing at once, such as
// the call or the target). `speech`, when set, is a function giving the words
// it is named by, called each time the pacer reads it; else its text.
ReadableText {
    id: live
    property alias step: pacer.step
    property var speech: null
    Accessible.name: pacer.spoken
    SpeechPacer {
        id: pacer
        shown: live.reachable
        read: function () {
            return live.speech ? live.speech() : live.text;
        }
    }
}
