// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The history screen's call log has to keep the latest call in view while the
// reader is parked at the top, hold their place when they are reading further
// back, and say what landed while they were away. The bug this guards against is
// silent: a ListView prepend below the top moves the content rather than the
// view, so a list left off the top never returns on its own and every later call
// arrives above the viewport with nothing on screen to say so.
//
// Positions are set directly rather than flicked. A flick's momentum and its
// boundary bounce are timing-dependent, and the behaviour under test is keyed on
// where the list came to rest, not on how it got there.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: screenLoader

        anchors.fill: parent
        source: uiDir + "/HistoryScreen.qml"
    }

    TestCase {
        id: tc

        name: "CallLogFollowsLatest"
        when: windowShown

        property var list: null
        property var pill: null

        function atTop() {
            // Sub-pixel: positionViewAtBeginning can land on -0.0.
            return Math.abs(tc.list.contentY - tc.list.originY) < 1
        }

        function scrollBack() {
            // Far enough that the top row is well outside the viewport, which is
            // what "reading further back" means to the list.
            tc.list.contentY = tc.list.originY + 400
            tc.waitForRendering(tc.list)
        }

        function rowAtViewportTop() {
            var idx = tc.list.indexAt(tc.list.width / 2, tc.list.contentY + 40)
            verify(idx >= 0, "no row under the top of the viewport")
            return tc.list.itemAtIndex(idx)
        }

        function initTestCase() {
            verify(screenLoader.item !== null, "HistoryScreen.qml failed to load")
            tc.list = findChild(screenLoader.item, "callLogList")
            tc.pill = findChild(screenLoader.item, "newCallsPill")
            verify(tc.list !== null, "the call log list is missing")
            verify(tc.pill !== null, "the new-calls pill is missing")
        }

        function init() {
            historyView.filterText = ""
            historyView.filterSystem = ""
            historyView.filterKind = 0
            callHistory.clearAll()
            callHistory.pushMany(24, "TODAY")
            tc.list.positionViewAtBeginning()
            tc.waitForRendering(tc.list)
            tryVerify(function () { return tc.atTop() && tc.list.unseen === 0 })
        }

        function test_01_calls_landing_at_the_top_stay_in_view() {
            var newest = ""
            for (var i = 0; i < 5; i++) {
                newest = callHistory.push("TODAY")
            }
            tryVerify(function () { return tc.atTop() })
            tryVerify(function () {
                var first = tc.list.itemAtIndex(0)
                return first !== null && first.name === newest
            }, 5000, "the newest call is not the first row")

            // On screen, not merely first: the promise is that the reader sees it.
            var newestRow = tc.list.itemAtIndex(0)
            verify(newestRow.y >= tc.list.contentY - 1)
            verify(newestRow.y + newestRow.height <= tc.list.contentY + tc.list.height)

            compare(tc.list.unseen, 0)
            verify(!tc.pill.shown)
        }

        function test_02_reading_further_back_holds_its_place_and_tallies() {
            tc.scrollBack()
            var anchorRow = tc.rowAtViewportTop()
            var anchorName = anchorRow.name
            var anchorScreenY = anchorRow.y - tc.list.contentY
            // The gap from the start of the content, which is what the list reads
            // to decide the reader is not on the latest call. A prepend grows it
            // by walking originY backwards rather than by moving contentY, so
            // contentY alone says nothing here.
            var wasGap = tc.list.contentY - tc.list.originY

            callHistory.pushMany(3, "TODAY")

            // One deferred reconcile for three inserts: the tally is a delta, so
            // coalescing must not lose calls.
            tryCompare(tc.list, "unseen", 3)
            verify(tc.pill.shown)

            // The reader has not been moved.
            var stillThere = tc.rowAtViewportTop()
            compare(stillThere.name, anchorName)
            verify(Math.abs((stillThere.y - tc.list.contentY) - anchorScreenY) < 2)

            // And the new calls landed above the viewport, out of sight — the
            // whole reason the list has to be pinned when the reader is at the top.
            verify(tc.list.contentY - tc.list.originY > wasGap)
            var newestRow = tc.list.itemAtIndex(0)
            verify(newestRow === null || newestRow.y + newestRow.height <= tc.list.contentY)
        }

        function test_03_the_pill_returns_to_the_latest_call() {
            tc.scrollBack()
            var newest = ""
            for (var i = 0; i < 3; i++) {
                newest = callHistory.push("TODAY")
            }
            tryVerify(function () { return tc.pill.shown })

            mouseClick(tc.pill)

            tryVerify(function () { return tc.atTop() })
            tryCompare(tc.list, "unseen", 0)
            verify(!tc.pill.shown)
            tryVerify(function () {
                var first = tc.list.itemAtIndex(0)
                return first !== null && first.name === newest
            }, 5000, "the pill did not land on the newest call")
        }

        function test_04_an_offset_inside_one_row_still_counts_as_the_latest() {
            // What a stray touch, an overscroll bounce or the keyboard resizing
            // the view leaves behind. None of it means "I am reading back".
            tc.list.contentY = tc.list.originY + 20
            tc.waitForRendering(tc.list)
            verify(!tc.atTop())

            callHistory.push("TODAY")

            tryVerify(function () { return tc.atTop() })
            compare(tc.list.unseen, 0)
            verify(!tc.pill.shown)
        }

        function test_05_a_filter_change_is_answered_from_the_top() {
            tc.scrollBack()
            callHistory.pushMany(3, "TODAY")
            tryCompare(tc.list, "unseen", 3)

            historyView.filterKind = 1 // clear calls, which is all of them

            tryVerify(function () { return tc.atTop() })
            tryCompare(tc.list, "unseen", 0)
            verify(!tc.pill.shown)
        }

        function test_06_clearing_the_log_leaves_no_stale_tally() {
            tc.scrollBack()
            callHistory.pushMany(3, "TODAY")
            tryCompare(tc.list, "unseen", 3)

            callHistory.clearAll()

            tryCompare(tc.list, "count", 0)
            tryCompare(tc.list, "unseen", 0)
            verify(!tc.pill.shown)
        }

        function test_07_the_hidden_count_reads_as_a_sentence() {
            // %n plural forms need a translation catalogue the app does not ship,
            // so a %n string renders its own "(s)" and never agrees with its verb.
            var detail = findChild(screenLoader.item, "logEmptyDetail")
            verify(detail !== null, "the empty-state detail line is missing")

            callHistory.clearAll()
            callHistory.push("TODAY")
            historyView.filterKind = 2 // encrypted only: hides the clear call

            tryCompare(tc.list, "count", 0)
            verify(detail.parent.visible, "the empty state is not showing")
            compare(detail.text.indexOf("(s)"), -1, detail.text)
            compare(detail.text.indexOf("1 logged call is hidden"), 0, detail.text)

            callHistory.pushMany(3, "TODAY")

            tryVerify(function () { return detail.text.indexOf("4 logged calls are hidden") === 0 },
                      5000, "the plural form did not follow the count")
            compare(detail.text.indexOf("(s)"), -1, detail.text)
        }
    }
}
