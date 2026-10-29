// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * How Android says the app's recent processes ended, as diagnostics lines (issue #745).
 *
 * Plain Kotlin with no Android imports, so the JVM tests run it as is; [ProcessFacts] reads the
 * records off ActivityManager. The lines are facts with times and pids, not a verdict: the reader
 * matches them against the session lines around them. Android keeps no record when the phone
 * itself restarts, which is why the process line also carries the boot count.
 *
 * A process that set a state summary ([LaunchGuard]) carries it in parentheses at the end of its line (#782).
 */
object ExitFacts {
    /** Newest records read per process start; each is logged by the first start that reads it ([unlogged]). */
    const val MAX_LINES = 3

    /** Longest description kept, in characters; the diagnostics record bound is 512 bytes. */
    const val MAX_DESCRIPTION = 160

    /** Longest state summary kept, in characters: Android keeps at most [LaunchGuard.MAX_SUMMARY_BYTES] bytes. */
    const val MAX_SUMMARY = LaunchGuard.MAX_SUMMARY_BYTES

    /** One `ApplicationExitInfo`, reduced to what a line shows. */
    data class Record(
        val timestampMs: Long,
        val pid: Int,
        val reason: Int,
        val status: Int,
        val importance: Int,
        val pssKb: Long,
        val description: String?,
        /** `ApplicationExitInfo.getProcessStateSummary()`, as UTF-8 text; null when the process set none. */
        val summary: String? = null,
    )

    /** What tells one record from another across starts: its time and pid. */
    fun key(record: Record): String = "${record.timestampMs}/${record.pid}"

    /**
     * The [records] not in [logged] (keys an earlier start logged, [key]), so a start logs only exits it has not logged
     * before (#782): the host store keeps its records across starts, and repeats would crowd them out. A set rather
     * than the newest time, so a wall clock set back cannot hide a new exit.
     */
    fun unlogged(records: List<Record>, logged: Set<String>): List<Record> = records.filter { key(it) !in logged }

    /** `ApplicationExitInfo.REASON_*`, by value; a reason newer than this table prints as its number. */
    fun reasonName(reason: Int): String = when (reason) {
        0 -> "UNKNOWN"
        1 -> "EXIT_SELF"
        2 -> "SIGNALED"
        3 -> "LOW_MEMORY"
        4 -> "CRASH"
        5 -> "CRASH_NATIVE"
        6 -> "ANR"
        7 -> "INITIALIZATION_FAILURE"
        8 -> "PERMISSION_CHANGE"
        9 -> "EXCESSIVE_RESOURCE_USAGE"
        10 -> "USER_REQUESTED"
        11 -> "USER_STOPPED"
        12 -> "DEPENDENCY_DIED"
        13 -> "OTHER"
        14 -> "FREEZER"
        15 -> "PACKAGE_STATE_CHANGE"
        16 -> "PACKAGE_UPDATED"
        else -> "REASON_$reason"
    }

    /** The newest [MAX_LINES] records, newest first, with [formatTime] rendering the timestamps. */
    fun lines(records: List<Record>, formatTime: (Long) -> String): List<String> =
        records.sortedByDescending { it.timestampMs }.take(MAX_LINES).map { line(it, formatTime) }

    fun line(record: Record, formatTime: (Long) -> String): String {
        val base = "exit: ${formatTime(record.timestampMs)} pid ${record.pid} ${reasonName(record.reason)} " +
            "status ${record.status} importance ${record.importance} pss ${record.pssKb} kB"
        val description = printable(record.description, MAX_DESCRIPTION)
        val described = if (description.isEmpty()) base else "$base \"$description\""
        // The summary is the exited process's own words (LaunchGuard.SUMMARY in the build that wrote it), shown as is.
        val summary = printable(record.summary, MAX_SUMMARY)
        return if (summary.isEmpty()) described else "$described ($summary)"
    }

    /** [text] on one line, trimmed and cut to [max] characters; empty for null. */
    private fun printable(text: String?, max: Int): String =
        text?.map { if (it.isISOControl()) ' ' else it }?.joinToString("")?.trim()?.take(max) ?: ""
}
