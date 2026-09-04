// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The history screen's call log has to keep the latest call in view while the
// reader is parked at the top, and hold their place when they are reading further
// back. The bug this guards against is silent: a ListView prepend below the top
// moves the content rather than the view, so a list left off the top never
// returns on its own and every later call arrives above the viewport.
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
            verify(tc.list !== null, "the call log list is missing")
        }

        function init() {
            historyView.filterText = ""
            historyView.filterSystem = ""
            historyView.filterKind = 0
            callHistory.clearAll()
            callHistory.pushMany(24, "TODAY")
            tc.list.positionViewAtBeginning()
            tc.waitForRendering(tc.list)
            tryVerify(function () { return tc.atTop() })
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
        }

        function test_02_reading_further_back_holds_its_place() {
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

            // The new calls land above the viewport, out of sight — the whole
            // reason the list has to be pinned when the reader is at the top.
            // Waiting on the gap rather than on count: the model count changes
            // first, before the view has applied the insertions.
            tryVerify(function () { return tc.list.contentY - tc.list.originY > wasGap })

            // And the reader has not been moved.
            var stillThere = tc.rowAtViewportTop()
            compare(stillThere.name, anchorName)
            verify(Math.abs((stillThere.y - tc.list.contentY) - anchorScreenY) < 2)
            var newestRow = tc.list.itemAtIndex(0)
            verify(newestRow === null || newestRow.y + newestRow.height <= tc.list.contentY)
        }

        function test_03_an_offset_inside_one_row_still_counts_as_the_latest() {
            // What a stray touch, an overscroll bounce or the keyboard resizing
            // the view leaves behind. None of it means "I am reading back".
            tc.list.contentY = tc.list.originY + 20
            tc.waitForRendering(tc.list)
            verify(!tc.atTop())

            callHistory.push("TODAY")

            tryVerify(function () { return tc.atTop() })
        }

        function test_04_the_hidden_count_reads_as_a_sentence() {
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

            // The filter under the list is the production one, so the pill has to
            // pass a call it accepts, not merely hide the ones it does not: an
            // encrypted call shows while the four clear ones stay hidden. It is
            // stamped encrypted after it was logged, which is how a late ENC
            // header reaches an already-written row.
            callHistory.pushEncrypted("TODAY")

            tryCompare(tc.list, "count", 1)
            // On the binding, not on the property: tryCompare polls count by
            // reading it, while the view emits countChanged on its next layout
            // pass, which is when the empty state can go away.
            tryVerify(function () { return !detail.parent.visible }, 5000,
                      "the empty state is still showing over the encrypted call")
        }

        // A finger resting on the list is not a drag, so Flickable.moving stays
        // false throughout. Re-asserting the top under it would move the content
        // away from the press position the Flickable recorded when the finger went
        // down, and the reader's next drag would open by snapping back to it.
        function test_05_a_call_under_a_held_finger_waits_for_the_release() {
            tc.list.contentY = tc.list.originY + 20
            tc.waitForRendering(tc.list)
            verify(!tc.atTop())

            mousePress(tc.list, tc.list.width / 2, 20)
            callHistory.push("TODAY")
            // Asserting an absence, so the deferred pin has to be given the event
            // loop passes it would have needed to run.
            tc.waitForRendering(tc.list)
            tc.wait(50)
            verify(!tc.atTop(), "the list was pinned under a held finger")

            mouseRelease(tc.list, tc.list.width / 2, 20)

            tryVerify(function () { return tc.atTop() }, 5000,
                      "the deferred pin never ran after the release")
        }

        // A filter change is not a call landing: it is a new question, and where
        // the reader had scrolled to is the answer to the old one. The view holds
        // its scroll position across the change, so without a reposition the
        // reader lands in the middle of the new result with the newest matching
        // calls off-screen above and nothing on screen to say they exist.
        function test_06_a_narrowing_filter_is_answered_from_the_top() {
            // Enough matching rows that the filtered content still overflows the
            // viewport. A result shorter than the viewport is dragged back to the
            // top by the Flickable's own bounds, and would pass with the
            // reposition deleted.
            for (var i = 0; i < 20; i++) {
                callHistory.pushEncrypted("TODAY")
            }
            tryCompare(tc.list, "count", 44)
            tc.list.contentY = tc.list.originY + 1200
            tc.waitForRendering(tc.list)
            verify(!tc.atTop())

            historyView.filterKind = 2 // encrypted only: the 20 just pushed

            tryCompare(tc.list, "count", 20)
            tryVerify(function () { return tc.atTop() }, 5000,
                      "the filtered log did not open on its newest matching call")
            // The precondition, asserted rather than assumed: had the result fit
            // the viewport, the top would prove nothing.
            verify(tc.list.contentHeight > tc.list.height)
            var newestRow = tc.list.itemAtIndex(0)
            verify(newestRow !== null, "the newest matching call has no row")
            verify(newestRow.y + newestRow.height <= tc.list.contentY + tc.list.height)
        }

        // The same rule where no row moves at all: the pill cycles onto a filter
        // every logged call already passes. Nothing is inserted or removed, so the
        // view's own count never changes and the pin has no signal to fire on —
        // only the filter change itself can bring the log back.
        // A history row says which scan channel it was heard on, at the end of
        // its meta line, unless that is already the row's name.
        function test_06b_a_row_names_the_channel_it_was_heard_on() {
            var name = callHistory.pushOnChannel("TODAY", "Fire Dispatch")
            tryVerify(function () {
                var first = tc.list.itemAtIndex(0)
                return first !== null && first.name === name && first.metaText.indexOf(" · Fire Dispatch") > 0
            }, 5000, "the history row does not name its channel")
        }

        // A call that decoded no talkgroup is headlined by its channel, and the
        // meta line under it does not add "TG 0" — nothing was decoded to say.
        function test_06b_a_row_names_the_channel_it_was_heard_on_tg0() {
            var name = callHistory.pushUnnamedOnChannel("TODAY", "County EMS")
            tryVerify(function () {
                var first = tc.list.itemAtIndex(0)
                return first !== null && first.name === name
            }, 5000, "the channel-named row is not on top")
            var row = tc.list.itemAtIndex(0)
            verify(row.metaText.indexOf("TG 0") < 0, "the meta line still says TG 0: " + row.metaText)
            verify(row.metaText.indexOf("County EMS") < 0, "the meta line repeats the name: " + row.metaText)
        }

        function test_07_a_filter_that_hides_nothing_is_still_answered_from_the_top() {
            tc.scrollBack()

            historyView.filterKind = 1 // clear calls, which is all of them

            tryVerify(function () { return tc.atTop() }, 5000,
                      "a filter change that hid no rows left the log where it was")
            compare(tc.list.count, 24)
        }
    }
}
