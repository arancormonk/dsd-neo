// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The monitor's recent-calls pane is the same newest-first list as the history
// log, and drifts the same way if the top is not re-asserted. It carries no
// new-calls pill — four rows have no room for one — so what it owes the reader is
// only this: the call that just ended is on screen, and scrolling back holds.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: screenLoader

        anchors.fill: parent
        source: uiDir + "/MonitorScreen.qml"
    }

    TestCase {
        id: tc

        name: "MonitorRecentCallsFollowsLatest"
        when: windowShown

        property var list: null

        function atTop() {
            return Math.abs(tc.list.contentY - tc.list.originY) < 1
        }

        function initTestCase() {
            verify(screenLoader.item !== null, "MonitorScreen.qml failed to load")
            tc.list = findChild(screenLoader.item, "recentCallsList")
            verify(tc.list !== null, "the recent-calls list is missing")
        }

        function init() {
            monitorView.minWhen = 0
            callHistory.clearAll()
            callHistory.pushMany(12, "TODAY")
            tc.list.positionViewAtBeginning()
            tc.waitForRendering(tc.list)
            tryVerify(function () { return tc.atTop() })
        }

        function test_01_the_call_that_just_ended_is_on_screen() {
            var newest = ""
            for (var i = 0; i < 4; i++) {
                newest = callHistory.push("TODAY")
            }
            tryVerify(function () { return tc.atTop() })
            tryVerify(function () {
                var first = tc.list.itemAtIndex(0)
                return first !== null && first.name === newest
            }, 5000, "the newest call is not the first row")
        }

        // The case that actually needs the pin, and the one measured by hand on a
        // device: a drag that leaves the pane a fraction of a row down. ListView
        // holds an exact top by itself, so a test that only ever sits at the top
        // passes with the pin deleted.
        function test_02_an_offset_inside_one_row_still_counts_as_the_latest() {
            tc.list.contentY = tc.list.originY + 20
            tc.waitForRendering(tc.list)
            verify(!tc.atTop())

            callHistory.push("TODAY")

            tryVerify(function () { return tc.atTop() })
        }

        // The reading exists so a log that stays empty on an almost entirely
        // encrypted site does not read as a decoder that stopped. It must appear
        // once something is locked out and stay out of the way when nothing is.
        function test_03_the_lockout_count_shows_only_once_something_is_locked_out() {
            var row = findChild(screenLoader.item, "encLockoutRow")
            var value = findChild(screenLoader.item, "encLockoutValue")
            verify(row !== null, "the lockout row is missing")
            verify(value !== null, "the lockout value is missing")

            testContext.setMetric("encLockoutCount", 0)
            tryVerify(function () { return !row.visible })

            testContext.setMetric("encLockoutCount", 6)
            tryVerify(function () { return row.visible })
            compare(value.text, "6")

            testContext.setMetric("encLockoutCount", 0)
            tryVerify(function () { return !row.visible })
        }

        function test_04_scrolling_back_holds_the_reader_place() {
            // The pane is short, so scroll by a couple of rows rather than a screen.
            tc.list.contentY = tc.list.originY + 150
            tc.waitForRendering(tc.list)
            var anchorName = tc.list.itemAtIndex(tc.list.indexAt(tc.list.width / 2, tc.list.contentY + 20)).name
            // A prepend grows the gap from the start of the content by walking
            // originY backwards; contentY itself need not move.
            var wasGap = tc.list.contentY - tc.list.originY

            callHistory.pushMany(2, "TODAY")

            tryVerify(function () { return tc.list.contentY - tc.list.originY > wasGap })
            verify(!tc.atTop())
            compare(tc.list.itemAtIndex(tc.list.indexAt(tc.list.width / 2, tc.list.contentY + 20)).name, anchorName)
        }
    }
}
