// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

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

    TextMetrics {
        id: frequencyWidth

        text: "851.0125 MHz"
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

        function cleanup() {
            screenLoader.item.visible = true
            var sheet = findChild(screenLoader.item, "historyDetailSheet")
            if (sheet !== null)
                sheet.visible = false
        }

        function test_source_alias_is_visible() {
            var newest = callHistory.pushWithSourceName("Radio 1201")
            tryVerify(function () {
                var first = tc.list.itemAtIndex(0)
                return first !== null && first.name === newest && first.metaText.indexOf("SRC Radio 1201 (") >= 0
            }, 5000, "the source alias and ID are missing from the call row")
            historyView.filterText = "radio 1201"
            tryCompare(historyView, "count", 1)
            historyView.filterText = ""
        }

        // Issue #782: hidden, the log holds no rows, so calls landing under the
        // monitor build none; shown again it lists them as they are then, with
        // the search it had still applied and no flash of the empty state. Where
        // it reopens is test_08 to test_10's.
        function test_00_a_hidden_log_holds_no_rows() {
            var screen = screenLoader.item
            var empty = findChild(screen, "callLogEmptyState")
            verify(empty !== null, "the empty state is missing")
            callHistory.pushWithSourceName("Radio 7777")
            historyView.filterText = "radio 7777"
            tryCompare(historyView, "count", 1)
            screen.visible = false
            for (var i = 0; i < 5; i++) {
                callHistory.push("TODAY")
                tc.waitForRendering(screen.parent)
            }
            callHistory.pushWithSourceName("Radio 7777")
            tc.waitForRendering(screen.parent)
            compare(tc.list.count, 0, "a hidden log holds no rows")
            compare(tc.list.contentItem.children.filter(function (child) { return child.name !== undefined }).length, 0)
            screen.visible = true
            compare(tc.list.count, 2, "the search still applies to what landed while hidden")
            compare(historyView.count, 2)
            verify(!empty.visible, "a log with calls never shows its empty state")
            tc.waitForRendering(tc.list)
            verify(!empty.visible)
            historyView.filterText = "no such call"
            tryVerify(function () { return empty.visible }, 5000, "an empty result still says so")
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
            // On the binding, as below: the clear removes rows, and the view
            // announces the empty count on its next layout pass.
            tryVerify(function () { return detail.parent.visible }, 5000, "the empty state is not showing")
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

        // Issue #575: the call's access code follows its ids and the frequency it
        // was heard on closes the line, on voice and notice rows alike, so a
        // narrow phone elides the frequency first. The details carry all three.
        function test_06c_a_row_shows_the_access_code_and_frequency() {
            var name = callHistory.pushTuned(4001, 7001, "Fire Dispatch", 851012500, "CC 1", "Color code 1")
            var row = null
            tryVerify(function () { row = tc.list.itemAtIndex(0); return row !== null && row.name === name },
                      5000, "the tuned row is not on top")
            compare(row.metaText, "TG 4001 · SRC 7001 · CC 1 · 0:04 · Fire Dispatch · 851.0125 MHz")

            waitForRendering(row)
            mouseClick(row, row.width / 2, row.height / 2)
            var sheet = findChild(screenLoader.item, "historyDetailSheet")
            verify(sheet !== null, "the details sheet is missing")
            tryCompare(sheet, "visible", true)
            compare(sheet.record.freqHz, 851012500)
            compare(sheet.record.accessCode, "CC 1")
            compare(sheet.record.accessCodeText, "Color code 1")
            verify(sheet.detailLines.indexOf("Color code 1") >= 0, "no code line: " + sheet.detailLines)
            verify(sheet.detailLines.indexOf("Frequency 851.0125 MHz") >= 0, "no frequency line: " + sheet.detailLines)
        }

        function test_06d_a_notice_row_shows_the_access_code_and_frequency() {
            var name = callHistory.pushTunedNotice(4001, 7001, "GPS 40.1 -75.2", "Fire Dispatch", 851012500,
                                                   "NAC 293", "Network access code 293")
            var row = null
            tryVerify(function () { row = tc.list.itemAtIndex(0); return row !== null && row.name === name },
                      5000, "the notice row is not on top")
            compare(row.metaText, "TG 4001 · SRC 7001 · NAC 293 · GPS 40.1 -75.2 · Fire Dispatch · 851.0125 MHz")
        }

        // Without either, both kinds read exactly as they did.
        function test_06e_rows_without_a_code_or_frequency_are_unchanged() {
            var voice = callHistory.pushTuned(4001, 7001, "Fire Dispatch", 0, "", "")
            var row = null
            tryVerify(function () { row = tc.list.itemAtIndex(0); return row !== null && row.name === voice },
                      5000, "the voice row is not on top")
            compare(row.metaText, "TG 4001 · SRC 7001 · 0:04 · Fire Dispatch")
            var notice = callHistory.pushTunedNotice(4001, 7001, "GPS 40.1 -75.2", "", 0, "", "")
            tryVerify(function () { row = tc.list.itemAtIndex(0); return row !== null && row.name === notice },
                      5000, "the notice row is not on top")
            compare(row.metaText, "TG 4001 · SRC 7001 · GPS 40.1 -75.2")
        }

        // The delegate of the row named @p name, once the view lists it.
        function rowNamed(name) {
            var found = null
            tryVerify(function () {
                for (var i = 0; i < tc.list.count; ++i) {
                    var item = tc.list.itemAtIndex(i)
                    if (item !== null && item.name === name) {
                        found = item
                        return true
                    }
                }
                return false
            }, 5000, "the row " + name + " is not listed")
            return found
        }

        function startsWith(text, prefix) {
            return text.substring(0, prefix.length) === prefix
        }

        // The field says what it searches, and how a frequency is told from an
        // ID, in text a screen reader announces with the field.
        function test_06f_the_search_field_names_the_frequency_rule() {
            var field = findChild(screenLoader.item, "historySearch")
            verify(field !== null, "the search field is missing")
            compare(field.label, "Search talkgroup, unit or frequency")
            compare(field.hint, "Frequencies need a decimal point: 851.0125")
            verify(field.input.Accessible.description.indexOf(field.hint) >= 0,
                   "the rule is not in the accessible description: " + field.input.Accessible.description)
        }

        // A row a frequency search found leads with that frequency, on voice and
        // notice rows alike, and names it to a screen reader. A row the same text
        // found by its channel keeps its order, and clearing the search restores it.
        function test_06g_a_frequency_search_leads_with_the_frequency() {
            var voice = callHistory.pushTuned(4001, 7001, "Fire Dispatch", 851012500, "CC 1", "Color code 1")
            var notice = callHistory.pushTunedNotice(4001, 7001, "GPS 40.1 -75.2", "Fire Dispatch", 851012500,
                                                     "NAC 293", "Network access code 293")
            var byText = callHistory.pushTuned(4002, 7002, "851.0125 backup", 460000000, "", "")
            historyView.filterText = "851.0125"
            tryCompare(tc.list, "count", 3)

            compare(tc.rowNamed(voice).metaText, "851.0125 MHz · TG 4001 · SRC 7001 · CC 1 · 0:04 · Fire Dispatch")
            compare(tc.rowNamed(notice).metaText,
                    "851.0125 MHz · TG 4001 · SRC 7001 · NAC 293 · GPS 40.1 -75.2 · Fire Dispatch")
            compare(tc.rowNamed(byText).metaText, "TG 4002 · SRC 7002 · 0:04 · 851.0125 backup · 460.0000 MHz")
            verify(tc.rowNamed(voice).accessibleName.indexOf("851.0125 MHz") >= 0,
                   "a row found by its frequency does not name it: " + tc.rowNamed(voice).accessibleName)
            verify(tc.rowNamed(byText).accessibleName.indexOf("MHz") < 0,
                   "a row found by its channel names its frequency: " + tc.rowNamed(byText).accessibleName)

            historyView.filterText = ""
            tryCompare(tc.list, "count", 27)
            compare(tc.rowNamed(voice).metaText, "TG 4001 · SRC 7001 · CC 1 · 0:04 · Fire Dispatch · 851.0125 MHz")
            verify(tc.rowNamed(voice).accessibleName.indexOf("MHz") < 0, "the frequency stayed in the name")
        }

        // One row, two frequency searches: found by its frequency under the
        // first, by its channel under the second, and ordered by which.
        function test_06h_the_order_follows_why_the_row_is_listed() {
            var name = callHistory.pushTuned(4003, 7003, "851.9 backup", 851012500, "", "")
            historyView.filterText = "851."
            verify(tc.startsWith(tc.rowNamed(name).metaText, "851.0125 MHz · "),
                   "the frequency the search found does not lead: " + tc.rowNamed(name).metaText)
            historyView.filterText = "851.9"
            tryCompare(tc.list, "count", 1)
            verify(historyView.frequencyQuery)
            compare(tc.rowNamed(name).metaText, "TG 4003 · SRC 7003 · 0:04 · 851.9 backup · 851.0125 MHz")
        }

        // On an emergency call the frequency leads ahead of EMERGENCY, which the
        // badge also shows, and it fits on a phone at the larger text sizes: the
        // meta line is only as wide as the badge and the time leave it.
        function test_06i_an_emergency_row_keeps_the_frequency_in_view_data() {
            return [{tag: "1.0", scale: 1.0}, {tag: "1.3", scale: 1.3}]
        }
        function test_06i_an_emergency_row_keeps_the_frequency_in_view(data) {
            var width = root.width
            var scale = Ui.Theme.fontScale
            try {
                root.width = 360
                Ui.Theme.fontScale = data.scale
                var name = callHistory.pushTunedEmergency(4004, 7004, "Fire Dispatch", 851012500)
                historyView.filterText = "851.0125"
                var row = tc.rowNamed(name)
                verify(tc.startsWith(row.metaText, "851.0125 MHz · EMERGENCY · "), row.metaText)
                waitForRendering(row)
                var meta = findChild(row, "callRowMeta")
                verify(meta !== null, "the meta line is missing")
                frequencyWidth.font = meta.font
                verify(frequencyWidth.advanceWidth <= meta.width,
                       "the frequency is elided: " + frequencyWidth.advanceWidth + " > " + meta.width)
            } finally {
                Ui.Theme.fontScale = scale
                root.width = width
            }
        }

        // Typing is kept when a pill changes before the debounce applies it: the
        // field mirrors the search text alone, not every filter.
        function test_06j_a_pill_change_keeps_the_text_being_typed() {
            var field = findChild(screenLoader.item, "historySearch")
            historyView.filterText = "dispatch"
            compare(field.text, "dispatch")
            field.input.text = "851.0125"
            historyView.filterKind = 1
            compare(field.text, "851.0125", "the pill change wrote the applied text over the typing")
            tryCompare(historyView, "filterText", "851.0125")
        }

        // Clear filters clears the typing too, which the debounce would otherwise
        // apply after the button had cleared the applied search.
        function test_06k_clear_filters_clears_the_text_being_typed() {
            var field = findChild(screenLoader.item, "historySearch")
            historyView.filterKind = 3 // messages: none logged
            tryCompare(tc.list, "count", 0)
            field.input.text = "999.0"
            var clear = findChild(screenLoader.item, "historyClearFilters")
            verify(clear !== null, "Clear filters is missing")
            tryVerify(function () { return clear.visible }, 5000, "Clear filters is not offered")
            clear.clicked()
            compare(field.text, "")
            wait(350)
            compare(historyView.filterText, "", "the debounce applied the cleared typing")
            compare(historyView.filterKind, 0)
            compare(tc.list.count, 24)
        }

        // What the air delivered is shown as received: a tag in an alias or an
        // SMS body is text, not markup.
        function test_06l_rows_show_markup_as_text() {
            var name = callHistory.pushTunedNotice(4005, 7005, "<b>851.0125</b>", "", 460000000, "", "")
            var row = tc.rowNamed(name)
            compare(findChild(row, "callRowMeta").textFormat, Text.PlainText)
            compare(findChild(row, "callRowName").textFormat, Text.PlainText)
            verify(row.metaText.indexOf("<b>851.0125</b>") >= 0, row.metaText)
        }

        function test_07_a_filter_that_hides_nothing_is_still_answered_from_the_top() {
            tc.scrollBack()

            historyView.filterKind = 1 // clear calls, which is all of them

            tryVerify(function () { return tc.atTop() }, 5000,
                      "a filter change that hid no rows left the log where it was")
            compare(tc.list.count, 24)
        }

        // Issue #782: the log lets its rows go while hidden, and a reader who was
        // reading back comes back to the same row at the same height, on the
        // first frame: no deferred restore, so compare() right after the show.
        function test_08_a_hidden_log_keeps_the_readers_place() {
            var screen = screenLoader.item
            tc.scrollBack()
            var anchorRow = tc.rowAtViewportTop()
            var anchorName = anchorRow.name
            var anchorScreenY = anchorRow.y - tc.list.contentY
            screen.visible = false
            for (var i = 0; i < 5; i++) {
                callHistory.push("TODAY")
                tc.waitForRendering(screen.parent)
            }
            compare(tc.list.count, 0, "a hidden log holds no rows")
            screen.visible = true
            tc.waitForRendering(tc.list)
            compare(tc.list.count, historyView.count)
            var row = tc.rowAtViewportTop()
            compare(row.name, anchorName, "the reader's row is back at the top of the viewport")
            verify(Math.abs((row.y - tc.list.contentY) - anchorScreenY) <= 2,
                   "at the same height: " + (row.y - tc.list.contentY) + " vs " + anchorScreenY)
            var first = tc.list.itemAtIndex(0)
            verify(first === null || first.y + first.height <= tc.list.contentY + 1,
                   "the newest call stays above the viewport")
        }

        // A reader on the latest call is on it again, newest first.
        function test_09_a_hidden_log_at_the_latest_reopens_at_the_newest() {
            var screen = screenLoader.item
            verify(tc.atTop())
            screen.visible = false
            var newest = ""
            for (var i = 0; i < 5; i++) {
                newest = callHistory.push("TODAY")
                tc.waitForRendering(screen.parent)
            }
            screen.visible = true
            tc.waitForRendering(tc.list)
            verify(tc.atTop(), "back on the latest call")
            var first = tc.list.itemAtIndex(0)
            verify(first !== null)
            compare(first.name, newest)
        }

        // A reader whose row went while the log was hidden (here, History was
        // cleared) has no place to come back to, and lands on the newest call.
        function test_10_a_reader_whose_row_is_gone_lands_at_the_top() {
            var screen = screenLoader.item
            var empty = findChild(screen, "callLogEmptyState")
            tc.scrollBack()
            screen.visible = false
            callHistory.clearAll()
            var newest = ""
            // Longer than the viewport, so landing anywhere but the top shows.
            for (var i = 0; i < 24; i++) {
                newest = callHistory.push("TODAY")
                tc.waitForRendering(screen.parent)
            }
            screen.visible = true
            tc.waitForRendering(tc.list)
            verify(tc.atTop(), "no place to keep: the top")
            verify(!empty.visible)
            var first = tc.list.itemAtIndex(0)
            verify(first !== null)
            compare(first.name, newest)
        }
    }
}
