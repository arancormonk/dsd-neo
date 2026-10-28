// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import java.util.Locale

/**
 * Text rules for host diagnostics written from Kotlin (#782): the main-looper watchdog's incidents, the screen trace
 * and the teardown markers. Plain Kotlin, so it runs on the JVM.
 *
 * Native diagnostics store each record as `[host] [info] <text>`, cut at [RECORD_BYTES] bytes, and drop the whole
 * record when its text could carry a key (src/ui/qt/diagnostics_log.cpp, `DiagnosticsLog::redact`). A stack trace
 * names framework methods such as `dispatchKeyEvent`, which that rule would drop with everything correlated to it, so
 * [safe] breaks up each word the rule looks for with a `*` and keeps its other letters. Callers fit their records to
 * [MAX_BYTES] themselves, so nothing is lost to the cut.
 */
object HostRecordText {
    /** Native diagnostics cut each formatted record to this many UTF-8 bytes. */
    const val RECORD_BYTES = 512

    /** What a record's own text may take: the record limit less the `[host] [info] ` prefix native code adds. */
    const val MAX_BYTES = RECORD_BYTES - 14

    // The parts of the native key rule that ordinary text can trip; each match loses its second character.
    private val LABEL = Regex("(?i)key|secret|password|token")
    private val WORD = Regex("(?i)\\b(?:K[1-4]|RC4|AES|Hytera|scrambler)\\b")

    // An option-like dash, at the start or after a space, quote, bracket or comma; it becomes a tilde.
    private val DASH = Regex("(^|[\\s\"'\\[,])-")

    /** [text] with every word the native key rule matches broken up, so a record of it is kept. */
    fun safe(text: String): String {
        val words = WORD.replace(LABEL.replace(text, ::starred), ::starred)
        return DASH.replace(words) { it.groupValues[1] + "~" }
    }

    private fun starred(match: MatchResult): String = match.value.substring(0, 1) + "*" + match.value.substring(2)

    /** The UTF-8 size of [text]. */
    fun bytes(text: String): Int = text.toByteArray(Charsets.UTF_8).size

    /** [text] cut to at most [maxBytes] UTF-8 bytes, never inside a character; a cut ends with an ellipsis. */
    fun fit(text: String, maxBytes: Int = MAX_BYTES): String {
        if (bytes(text) <= maxBytes) {
            return text
        }
        val ellipsis = "..."
        val budget = maxBytes - ellipsis.length
        var end = 0
        var used = 0
        while (end < text.length) {
            val next = if (Character.isHighSurrogate(text[end]) && end + 1 < text.length) end + 2 else end + 1
            val size = bytes(text.substring(end, next))
            if (used + size > budget) {
                break
            }
            used += size
            end = next
        }
        return text.substring(0, end) + ellipsis
    }

    /**
     * An uptime reading in seconds with milliseconds, which is CLOCK_MONOTONIC on Android, the clock native code reads
     * too. In seconds, not milliseconds: ten or more digits in a row read as a key and are redacted.
     */
    fun uptime(ms: Long): String = String.format(Locale.ROOT, "%d.%03d s", ms / 1_000, ms % 1_000)
}
