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

        // The hero's subline says which scan channel the call was heard on, but
        // only when that is not already the name above it: a talkgroup-0 call is
        // headlined by its channel, so repeating it underneath would say nothing.
        function test_03b_the_hero_subline_names_the_channel_only_when_the_name_is_something_else() {
            var channel = findChild(screenLoader.item, "heroChannel")
            verify(channel !== null, "the hero channel text is missing")

            testContext.setMetric("leadSlot", 1)
            testContext.setMetric("slot1CallState", 2)
            testContext.setMetric("slot1CallName", "Metro Fire")
            testContext.setMetric("slot1Channel", "Fire Dispatch")
            tryVerify(function () { return channel.visible }, 5000, "a named call does not show its channel")
            compare(channel.text, "· Fire Dispatch")

            testContext.setMetric("slot1CallName", "Fire Dispatch")
            tryVerify(function () { return !channel.visible }, 5000, "the channel repeats the name")

            testContext.setMetric("slot1Channel", "")
            testContext.setMetric("slot1CallName", "Metro Fire")
            tryVerify(function () { return !channel.visible }, 5000, "an empty channel still shows")

            testContext.setMetric("slot1CallName", "")
            testContext.setMetric("slot1CallState", 0)
            testContext.setMetric("leadSlot", 0)
        }

        // A zero talkgroup is "none decoded": the hero headline is already the
        // channel for such a call, and a subline reading "TG 0" under it would
        // only say the decoder saw nothing. The source still shows when known.
        function test_03c_the_hero_subline_hides_a_zero_talkgroup() {
            var ids = findChild(screenLoader.item, "heroIds")
            verify(ids !== null, "the hero ids text is missing")

            testContext.setMetric("leadSlot", 1)
            testContext.setMetric("slot1CallState", 2)
            testContext.setMetric("slot1TgText", "4001")
            testContext.setMetric("slot1SrcText", "7001")
            tryVerify(function () { return ids.text === "TG 4001 · SRC 7001" }, 5000, "a decoded call names both ids")

            testContext.setMetric("slot1TgText", "0")
            tryVerify(function () { return ids.text === "SRC 7001" }, 5000, "a zero talkgroup still prints")

            testContext.setMetric("slot1SrcText", "0")
            tryVerify(function () { return !ids.visible }, 5000, "an id-less call keeps an empty subline")

            testContext.setMetric("slot1TgText", "")
            testContext.setMetric("slot1SrcText", "")
            testContext.setMetric("slot1CallState", 0)
            testContext.setMetric("leadSlot", 0)
        }

        // A name longer than the panel is cut, not spilled: the channel is the
        // last thing on the subline and elides to the room that is left.
        function test_03d_a_long_channel_name_elides_inside_the_panel() {
            var channel = findChild(screenLoader.item, "heroChannel")
            verify(channel !== null, "the hero channel text is missing")

            testContext.setMetric("leadSlot", 1)
            testContext.setMetric("slot1CallState", 2)
            testContext.setMetric("slot1CallName", "Metro Fire")
            testContext.setMetric("slot1TgText", "4001")
            testContext.setMetric("slot1SrcText", "7001")
            testContext.setMetric("slot1Channel", "County Fire and Rescue Dispatch North Zone Simulcast Alternate")
            tryVerify(function () { return channel.visible && channel.truncated }, 5000, "a long name is not elided")
            verify(channel.x + channel.width <= channel.parent.width + 0.5, "the channel runs past the subline")

            testContext.setMetric("slot1Channel", "")
            testContext.setMetric("slot1TgText", "")
            testContext.setMetric("slot1SrcText", "")
            testContext.setMetric("slot1CallName", "")
            testContext.setMetric("slot1CallState", 0)
            testContext.setMetric("leadSlot", 0)
        }

        // The recent-calls row answers "where was this heard" the way the hero
        // does: the channel closes the meta line when it is not already the name.
        function test_03e_a_recent_row_names_its_channel() {
            var name = callHistory.pushOnChannel("TODAY", "Fire Dispatch")
            tryVerify(function () {
                var first = tc.list.itemAtIndex(0)
                return first !== null && first.name === name && first.metaText.indexOf(" · Fire Dispatch") > 0
            }, 5000, "the recent row does not name its channel")
        }

        // A call that decoded no talkgroup is headlined by its channel, and the
        // meta line under it does not add "TG 0" — nothing was decoded to say.
        function test_03e_a_recent_row_names_its_channel_tg0() {
            var name = callHistory.pushUnnamedOnChannel("TODAY", "County EMS")
            tryVerify(function () {
                var first = tc.list.itemAtIndex(0)
                return first !== null && first.name === name
            }, 5000, "the channel-named row is not on top")
            var row = tc.list.itemAtIndex(0)
            verify(row.metaText.indexOf("TG 0") < 0, "the meta line still says TG 0: " + row.metaText)
            verify(row.metaText.indexOf("County EMS") < 0, "the meta line repeats the name: " + row.metaText)
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

        // Same rule as the history log: a finger resting on the pane is not a
        // drag, so Flickable.moving stays false for it, and repositioning under
        // the press would leave the reader's next drag snapping back to where the
        // finger went down.
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
            verify(!tc.atTop(), "the pane was pinned under a held finger")

            mouseRelease(tc.list, tc.list.width / 2, 20)

            tryVerify(function () { return tc.atTop() }, 5000,
                      "the deferred pin never ran after the release")
        }
    }
}
