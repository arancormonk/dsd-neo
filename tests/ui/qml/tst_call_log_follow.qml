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

    TextMetrics {
        id: partWidth
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

        // Issue #799: the details open on the call's site and channel, in the
        // Monitor site row's words, as one line after the frequency, so the
        // pinned lines (talkgroup 4, radio ID 6, its code and frequency after
        // it) stay where they are.
        function openDetails(name) {
            var row = tc.rowNamed(name)
            waitForRendering(row)
            mouseClick(row, row.width / 2, row.height / 2)
            var sheet = findChild(screenLoader.item, "historyDetailSheet")
            verify(sheet !== null, "the details sheet is missing")
            tryCompare(sheet, "visible", true)
            return sheet
        }

        function test_06j_details_show_site_line() {
            var name = callHistory.pushTuned(4001, 7001, "Fire Dispatch", 851012500, "CC 1", "Color code 1")
            verify(callHistory.updateRow(callHistory.newestUid(),
                                         {siteText: "P25 · SYS 1A3 · RFSS 3 · SITE 21 · WACN BEE00"}))
            var sheet = tc.openDetails(name)
            compare(sheet.record.siteText, "P25 · SYS 1A3 · RFSS 3 · SITE 21 · WACN BEE00")
            compare(sheet.detailLines[4], "Talkgroup 4001")
            compare(sheet.detailLines[6], "Radio ID 7001")
            compare(sheet.detailLines[7], "Color code 1")
            compare(sheet.detailLines[8], "Frequency 851.0125 MHz")
            compare(sheet.detailLines[9], "P25 · SYS 1A3 · RFSS 3 · SITE 21 · WACN BEE00")
        }

        // A call whose site was never heard, only its channel, names the channel.
        function test_06k_channel_only_line() {
            var name = callHistory.pushTuned(4001, 7001, "", 0, "CC 1", "Color code 1")
            verify(callHistory.updateRow(callHistory.newestUid(), {siteText: "DMR · LPCN 12"}))
            var sheet = tc.openDetails(name)
            compare(sheet.detailLines[6], "Radio ID 7001")
            compare(sheet.detailLines[7], "Color code 1")
            compare(sheet.detailLines[8], "")
            compare(sheet.detailLines[9], "DMR · LPCN 12")
            // A call with neither adds no line.
            sheet.visible = false
            var plain = callHistory.pushTuned(4002, 7002, "", 0, "", "")
            sheet = tc.openDetails(plain)
            compare(sheet.detailLines[9], "")
        }

        // A fragment folded into the row after the details opened (a retune a
        // few seconds on) shows there: the sheet reads its row again by uid.
        function test_06l_details_follow_row_update() {
            var name = callHistory.pushTuned(4001, 7001, "Fire Dispatch", 851012500, "CC 1", "Color code 1")
            var uid = callHistory.newestUid()
            var sheet = tc.openDetails(name)
            compare(sheet.detailLines[9], "")
            verify(callHistory.updateRow(uid, {durationSecs: 9, freqHz: 851025000, srcName: "Engine 5",
                                               siteText: "DMR · Con+ · Net 3 · Site 5 · Connect Plus · LCN 3"}))
            tryVerify(function () {
                return sheet.detailLines[9] === "DMR · Con+ · Net 3 · Site 5 · Connect Plus · LCN 3"
            }, 5000, "the details did not follow the row: " + sheet.detailLines)
            compare(sheet.detailLines[5], "Engine 5")
            compare(sheet.detailLines[8], "Frequency 851.0250 MHz")
            verify(sheet.detailLines.indexOf("Duration: 9 seconds") >= 0, sheet.detailLines)
            // Another row changing leaves this one as it is.
            callHistory.push("TODAY")
            verify(callHistory.updateRow(callHistory.newestUid(), {siteText: "DMR · LPCN 1"}))
            compare(sheet.record.uid, uid)
            compare(sheet.detailLines[9], "DMR · Con+ · Net 3 · Site 5 · Connect Plus · LCN 3")
        }

        // A row that leaves the log (cleared, aged out) leaves the details as
        // they last read, not empty.
        function test_06m_details_keep_last_record_when_row_leaves() {
            var name = callHistory.pushTuned(4001, 7001, "Fire Dispatch", 851012500, "CC 1", "Color code 1")
            verify(callHistory.updateRow(callHistory.newestUid(), {siteText: "DMR · LPCN 12"}))
            var sheet = tc.openDetails(name)
            var before = sheet.detailLines.slice()
            callHistory.clearAll()
            tc.waitForRendering(sheet)
            verify(sheet.visible)
            compare(sheet.record.name, name)
            compare(sheet.detailLines, before)
        }

        // A row a frequency search found leads its meta line, which a screen
        // reader hears as the row's description, with the frequency: the name
        // does not say it again.
        function test_06n_frequency_match_read_once() {
            var voice = callHistory.pushTuned(4001, 7001, "Fire Dispatch", 851012500, "CC 1", "Color code 1")
            historyView.filterText = "851.0125"
            tryCompare(tc.list, "count", 1)
            var row = tc.rowNamed(voice)
            var spoken = row.Accessible.name + ", " + row.Accessible.description
            compare(spoken.split("851.0125 MHz").length - 1, 1, spoken)
            verify(tc.startsWith(row.Accessible.description, "851.0125 MHz · "), row.Accessible.description)
        }

        // Review focus: a long DMR line on a 360 px phone wraps between its
        // parts, never inside one ("Net | 3", "Conn|ect"), nothing is clipped,
        // and a screen reader reads the line whole.
        function test_06o_site_line_wraps_and_reads() {
            var width = root.width
            try {
                root.width = 360
                var line = "DMR · TIII Large · Net 12 · Site 3.1 · SYS 1170 · Motorola Capacity Max · LCN 1234"
                var name = callHistory.pushTuned(4001, 7001, "Fire Dispatch", 851012500, "CC 1", "Color code 1")
                verify(callHistory.updateRow(callHistory.newestUid(), {siteText: line}))
                var sheet = tc.openDetails(name)
                var text = findChild(sheet, "historyDetailSite")
                verify(text !== null, "the site line is missing")
                verify(text.visible)
                waitForRendering(text)
                compare(text.wrapMode, Text.Wrap)
                compare(text.Accessible.name, line)
                verify(text.lineCount >= 2, "a long line does not wrap at 360 px: " + text.lineCount)
                verify(text.width <= sheet.width, "the line is wider than the sheet")
                verify(text.contentWidth <= text.width + 0.5, "the line is clipped: " + text.contentWidth)
                // Laid out part by part, the lines hold exactly the parts that
                // fit, each but the last ending on its separator's dot. Each
                // candidate is spelled as the delegate shows it: no-break spaces
                // inside a part and before a dot.
                var parts = line.split(" · ").map(function (part) {
                    return part.replace(/ /g, "\u00a0")
                })
                partWidth.font = text.font
                var lines = 1
                var current = ""
                for (var i = 0; i < parts.length; ++i) {
                    var tail = i + 1 < parts.length ? "\u00a0·" : ""
                    partWidth.text = parts[i] + tail
                    verify(partWidth.advanceWidth <= text.width, parts[i] + " alone is wider than the line")
                    var candidate = current.length > 0 ? current + "\u00a0· " + parts[i] : parts[i]
                    partWidth.text = candidate + tail
                    if (current.length > 0 && partWidth.advanceWidth > text.width) {
                        lines++
                        current = parts[i]
                    } else {
                        current = candidate
                    }
                }
                compare(text.lineCount, lines, "the line breaks inside a part")
                var image = grabImage(sheet)
                verify(image !== null)
            } finally {
                root.width = width
            }
        }


        // A row a frequency search found leads with that frequency, on voice and
        // notice rows alike (its name never adds it: test_06n). A row the same text
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

        // Issue #667: an analog reception has no ids and no access code. Its
        // tone takes the code's place on the meta line, the frequency still
        // closes it, and the details read the tone alone on the code's line and
        // what the call was heard in on the Encrypted line's. No site line.
        function test_06p_analog_rows_data() {
            return [
                {tag: "nfm-ctcss", name: "Fire", mode: "NFM", tone: "CTCSS 100.0 Hz", channel: "Fire Dispatch", freq: 154430000, mhz: "154.4300",
                    meta: "CTCSS 100.0 Hz · 0:04 · Fire Dispatch · 154.4300 MHz", kind: "Analog NFM"},
                {tag: "nfm-dcs", name: "NFM", mode: "NFM", tone: "DCS D023N / D047I", channel: "", freq: 154445000, mhz: "154.4450",
                    meta: "DCS D023N / D047I · 0:04 · 154.4450 MHz", kind: "Analog NFM"},
                {tag: "am", name: "AM", mode: "AM", tone: "", channel: "", freq: 119100000, mhz: "119.1000",
                    meta: "0:04 · 119.1000 MHz", kind: "Analog AM"},
                {tag: "unknown-mode", name: "Analog", mode: "", tone: "CTCSS 100.0 Hz", channel: "", freq: 0, mhz: "",
                    meta: "CTCSS 100.0 Hz · 0:04", kind: "Analog"}
            ]
        }

        function test_06p_analog_rows(data) {
            var name = callHistory.pushAnalog(data.name, data.mode, data.tone, data.channel, data.freq)
            compare(tc.rowNamed(name).metaText, data.meta)
            var sheet = tc.openDetails(name)
            verify(sheet.record.analog, "the details lost the analog flag")
            compare(sheet.record.analogMode, data.mode)
            compare(sheet.detailLines[0], data.name)
            compare(sheet.detailLines[4], "")
            compare(sheet.detailLines[6], "")
            compare(sheet.detailLines[7], data.tone)
            compare(sheet.detailLines[8], data.freq > 0 ? "Frequency " + data.mhz + " MHz" : "")
            compare(sheet.detailLines[9], "")
            compare(sheet.detailLines[11], data.kind)
            verify(sheet.detailLines.indexOf("Unencrypted") < 0, "an analog call reads as unencrypted: " + sheet.detailLines)
            verify(sheet.detailLines.join("|").indexOf("Received tone") < 0, "the tone is listed twice: " + sheet.detailLines)
            // The tone line is an ordinary line; the site line's own rendering
            // stays on the site's place, empty here.
            var site = findChild(sheet, "historyDetailSite")
            verify(site !== null, "the site line is missing")
            compare(site.text, "")
            verify(!site.visible, "an analog call shows a site line")
            sheet.visible = false
            // A frequency search still leads with the frequency it found.
            if (data.freq > 0) {
                historyView.filterText = data.mhz
                tryCompare(tc.list, "count", 1)
                verify(tc.startsWith(tc.rowNamed(name).metaText, data.mhz + " MHz · "),
                       "the frequency the search found does not lead: " + tc.rowNamed(name).metaText)
            }
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

        // A filter chosen from the keyboard (Return, Enter, keypad Enter or
        // Space) leaves the shared choice sheet closed and the keyboard on the
        // pill: the choice acts on the press and hands the keyboard back while
        // the key is down, so its repeats and release reach the pill, which
        // repeats only a key whose first press it took (TalkBack's Alt+Enter
        // passed on late: tst_a11y_scan_churn.qml, test_m5_*).
        function test_06q_a_keyboard_choice_leaves_the_filter_sheet_closed_data() {
            return [{tag: "Return", key: Qt.Key_Return, modifiers: Qt.NoModifier},
                {tag: "Enter", key: Qt.Key_Enter, modifiers: Qt.NoModifier},
                {tag: "keypad Enter", key: Qt.Key_Enter, modifiers: Qt.KeypadModifier},
                {tag: "Space", key: Qt.Key_Space, modifiers: Qt.NoModifier}]
        }
        function test_06q_a_keyboard_choice_leaves_the_filter_sheet_closed(data) {
            var pill = findPill(screenLoader.item, "All activity")
            verify(pill !== null, "the activity pill")
            pill.forceActiveFocus()
            keyClick(data.key, data.modifiers)
            var sheet = Ui.Navigation.topModal
            verify(sheet !== null && sheet.visible, "the activity sheet opened")
            var choice = findPill(sheet, "Encrypted")
            verify(choice !== null, "the Encrypted choice")
            choice.forceActiveFocus()
            keyPress(data.key, data.modifiers)
            verify(!sheet.visible, "the choice closed the sheet")
            compare(historyView.filterKind, 2)
            verify(pill.activeFocus, "the keyboard is back on the pill")
            for (var i = 0; i < 3; ++i) {
                verify(testContext.sendKey(pill, data.key, data.modifiers, true, true), "repeat " + i + " is taken")
                verify(!sheet.visible, "repeat " + i + " opened the sheet again")
            }
            keyRelease(data.key, data.modifiers)
            wait(0)
            verify(!sheet.visible, "the release opened the sheet again")
            verify(pill.activeFocus, "the keyboard stays on the pill")
            compare(pill.text, "Encrypted")
            keyClick(data.key, data.modifiers)
            tryCompare(sheet, "visible", true)
            sheet.visible = false
        }
        // The control under @p under, shown, whose text is @p text.
        function findPill(under, text) {
            for (var i = 0; i < under.children.length; ++i) {
                var child = under.children[i]
                if (child.visible && child.text === text && typeof child.activate === "function")
                    return child
                var found = findPill(child, text)
                if (found !== null)
                    return found
            }
            return null
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

        // A message can hold line breaks, and so can a name the air
        // delivered. The row draws each on one line, a space for each break,
        // clear of the time; the row keeps the text as received (the details
        // and the export read it from the model).
        function test_06q_line_breaks_stay_on_one_line() {
            var name = callHistory.pushTunedNotice(4006, 7006, "first line\nsecond line\r\nthird line", "", 0, "", "")
            var row = tc.rowNamed(name)
            verify(callHistory.updateRow(callHistory.newestUid(), {name: "Unit\nNorth"}))
            tryCompare(row, "name", "Unit\nNorth")
            waitForRendering(row)
            verify(row.metaText.indexOf("first line\nsecond line\r\nthird line") >= 0, "the row keeps the text: " + row.metaText)
            var meta = findChild(row, "callRowMeta")
            var title = findChild(row, "callRowName")
            var time = findChild(row, "callRowTime")
            var timeLeft = time.mapToItem(row, 0, 0).x
            for (var text of [meta, title]) {
                compare(text.lineCount, 1, text.objectName + " is one line")
                var at = text.mapToItem(row, 0, 0)
                verify(at.x + Math.max(text.width, text.contentWidth) <= timeLeft,
                       text.objectName + " clear of the time: " + (at.x + text.contentWidth) + " > " + timeLeft)
                verify(at.y >= 0 && at.y + text.contentHeight <= row.height, text.objectName + " inside its row")
            }
            compare(meta.text, "TG 4006 · SRC 7006 · first line second line third line")
            compare(title.text, "Unit North")
        }

        // A row with no start time (the details leave its time out, the export
        // its time cells empty) shows no made-up time, and its name reads
        // without one.
        function test_06r_a_row_without_a_start_time_shows_no_time() {
            var name = callHistory.push("TODAY")
            verify(callHistory.updateRow(callHistory.newestUid(), {when: 0, dayLabel: "TIME UNKNOWN", timeText: ""}))
            var row = tc.rowNamed(name)
            tryCompare(row, "rightText", "")
            compare(findChild(row, "callRowTime").text, "")
            compare(row.accessibleName, name, "no time in the name")
            compare(row.Accessible.description.indexOf(", ,"), -1, row.Accessible.description)
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

        // Issue #782: with a filter that hides rows above the reader, the
        // reader's row sits at a different index in the view than in the store,
        // so the restore has to map it. The filter is set before the scroll-back,
        // so no filter change happens while the log is hidden.
        function test_08b_a_filtered_log_keeps_the_readers_place() {
            var screen = screenLoader.item
            callHistory.clearAll()
            for (var i = 0; i < 30; i++) {
                callHistory.pushEncrypted("TODAY")
                callHistory.push("TODAY")
            }
            historyView.filterKind = 2
            tryCompare(historyView, "count", 30)
            tc.waitForRendering(tc.list)
            tryVerify(function () { return tc.atTop() })
            tc.scrollBack()
            var anchorRow = tc.rowAtViewportTop()
            var anchorName = anchorRow.name
            var anchorScreenY = anchorRow.y - tc.list.contentY
            screen.visible = false
            for (var j = 0; j < 3; j++) {
                callHistory.pushEncrypted("TODAY")
                callHistory.push("TODAY")
                tc.waitForRendering(screen.parent)
            }
            screen.visible = true
            tc.waitForRendering(tc.list)
            compare(tc.list.count, 33)
            var row = tc.rowAtViewportTop()
            compare(row.name, anchorName, "the reader's row is back at the top of the viewport")
            verify(Math.abs((row.y - tc.list.contentY) - anchorScreenY) <= 2,
                   "at the same height: " + (row.y - tc.list.contentY) + " vs " + anchorScreenY)
        }

        // A search set while the log is hidden is a new question, answered from
        // the top, even when the reader's row still matches it: here through the
        // filter both History screens share.
        function test_11_a_filter_set_while_hidden_reopens_at_the_top() {
            var screen = screenLoader.item
            tc.scrollBack()
            var anchorName = tc.rowAtViewportTop().name
            screen.visible = false
            historyView.filterText = "call"
            tc.waitForRendering(screen.parent)
            screen.visible = true
            tc.waitForRendering(tc.list)
            compare(historyView.count, 24, "the reader's row still matches")
            verify(tc.atTop(), "a new search starts at the top, not at " + anchorName)
        }

        // The same for a search typed just before the log is hidden: the
        // debounce has not applied it yet, and hiding flushes it, after the log
        // has noted its place.
        function test_12_a_search_flushed_on_hide_reopens_at_the_top() {
            var screen = screenLoader.item
            var field = findChild(screen, "historySearch")
            tc.scrollBack()
            field.input.text = "call"
            compare(historyView.filterText, "", "still debounced")
            screen.visible = false
            compare(historyView.filterText, "call", "hiding applied the typed search")
            tc.waitForRendering(screen.parent)
            screen.visible = true
            tc.waitForRendering(tc.list)
            verify(tc.atTop(), "a new search starts at the top")
        }
    }
}
