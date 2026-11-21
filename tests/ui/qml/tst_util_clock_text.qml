// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml/Util.js" as Util
import "../../../src/ui/qt/qml/MapText.js" as MapText

// Issue #902: Util.clockText() says a moment the app saw as "hh:mm:ss" on the
// local day savedSystems.localDay names and "yyyy-MM-dd hh:mm:ss" on any other
// date, and nothing for a missing time; MapText.clockText() is the same text
// for a report's Unix seconds. The expected texts are literal local times in a
// zone west of UTC, so an instant at UTC midnight is still the day before.
// Util.heardText() says an age of a day or more in days (B4), never
// "yesterday", which an age cannot know.
TestCase {
    name: "UtilClockText"

    // 2026-11-21 00:00 in America/Los_Angeles (UTC-8 in November).
    readonly property double dayStart: Date.UTC(2026, 10, 21, 8, 0, 0)

    function init() {
        testContext.setLocalTimeZone("America/Los_Angeles")
    }

    function cleanup() {
        testContext.resetLocalDay()
    }

    function day() {
        return { startMs: dayStart, revision: 1 }
    }

    function test_zone_reaches_javascript() {
        compare(new Date(Date.UTC(2026, 0, 1)).getDate(), 31, "JavaScript dates use the zone the case set")
    }

    function test_clock_text_data() {
        return [
            { tag: "today", ms: Date.UTC(2026, 10, 21, 22, 32, 5), text: "14:32:05" },
            { tag: "today, first second", ms: Date.UTC(2026, 10, 21, 8, 0, 0), text: "00:00:00" },
            { tag: "today, last second", ms: Date.UTC(2026, 10, 22, 7, 59, 59), text: "23:59:59" },
            { tag: "milliseconds dropped", ms: Date.UTC(2026, 10, 21, 22, 32, 5) + 987, text: "14:32:05" },
            { tag: "yesterday", ms: Date.UTC(2026, 10, 21, 7, 59, 59), text: "2026-11-20 23:59:59" },
            { tag: "tomorrow", ms: Date.UTC(2026, 10, 22, 8, 0, 0), text: "2026-11-22 00:00:00" },
            { tag: "UTC midnight is the day before here", ms: Date.UTC(2026, 0, 1), text: "2025-12-31 16:00:00" },
            { tag: "zero", ms: 0, text: "" },
            { tag: "negative", ms: -5000, text: "" },
            { tag: "not a number", ms: NaN, text: "" },
            { tag: "infinite", ms: Infinity, text: "" },
            { tag: "missing", ms: undefined, text: "" },
            { tag: "a string", ms: "1763764325000", text: "" }
        ]
    }

    function test_clock_text(data) {
        compare(Util.clockText(data.ms, day()), data.text)
    }

    // With no day to compare against, a time always carries its date.
    function test_no_day_is_a_full_date() {
        var ms = Date.UTC(2026, 10, 21, 22, 32, 5)
        compare(Util.clockText(ms, null), "2026-11-21 14:32:05")
        compare(Util.clockText(ms, undefined), "2026-11-21 14:32:05")
        compare(Util.clockText(ms, {}), "2026-11-21 14:32:05")
        compare(Util.clockText(ms, { startMs: NaN }), "2026-11-21 14:32:05")
    }

    function test_map_text_is_the_same_in_seconds() {
        var secs = Date.UTC(2026, 10, 21, 22, 32, 5) / 1000
        compare(MapText.clockText(secs, day()), "14:32:05")
        compare(MapText.clockText(secs - 86400, day()), "2026-11-20 14:32:05")
        compare(MapText.clockText(String(secs), day()), "", "only numeric seconds")
        compare(MapText.clockText(0, day()), "")
        compare(MapText.clockText(secs, null), "2026-11-21 14:32:05")
    }

    function test_heard_text_data() {
        return [
            { tag: "just under a day", age: 86399, text: "Last listened 23 hours ago" },
            { tag: "a day", age: 86400, text: "Last listened a day ago" },
            { tag: "just under two days", age: 172799, text: "Last listened a day ago" },
            { tag: "two days", age: 172800, text: "Last listened 2 days ago" }
        ]
    }

    function test_heard_text(data) {
        var nowMs = Date.UTC(2026, 10, 21, 22, 0, 0)
        compare(Util.heardText(Math.floor(nowMs / 1000) - data.age, nowMs), data.text)
    }
}
