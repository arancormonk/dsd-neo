// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

private val format: (Long) -> String = { "t$it" }

private fun record(timestampMs: Long, reason: Int, description: String? = null) =
    ExitFacts.Record(timestampMs, pid = 4000 + timestampMs.toInt(), reason = reason, status = 9, importance = 125,
        pssKb = 51200, description = description)

private fun everyReasonHasItsName() {
    // ApplicationExitInfo.REASON_* by value, as Android 16 defines them.
    val names = listOf(
        "UNKNOWN", "EXIT_SELF", "SIGNALED", "LOW_MEMORY", "CRASH", "CRASH_NATIVE", "ANR", "INITIALIZATION_FAILURE",
        "PERMISSION_CHANGE", "EXCESSIVE_RESOURCE_USAGE", "USER_REQUESTED", "USER_STOPPED", "DEPENDENCY_DIED", "OTHER",
        "FREEZER", "PACKAGE_STATE_CHANGE", "PACKAGE_UPDATED",
    )
    names.forEachIndexed { value, name -> check(ExitFacts.reasonName(value) == name) { "reason $value is $name" } }
    check(ExitFacts.reasonName(99) == "REASON_99") { "a reason this table predates prints as its number" }
}

private fun aLineCarriesTheFacts() {
    val line = ExitFacts.line(record(7, 5, "Fatal signal 6\n(SIGABRT)\ttid 77"), format)
    check(line == "exit: t7 pid 4007 CRASH_NATIVE status 9 importance 125 pss 51200 kB \"Fatal signal 6 (SIGABRT) tid 77\"") {
        "unexpected line: $line"
    }
    check(ExitFacts.line(record(1, 2), format) == "exit: t1 pid 4001 SIGNALED status 9 importance 125 pss 51200 kB") {
        "a record with no description ends after its sizes"
    }
    check(ExitFacts.line(record(1, 2, "  "), format).endsWith("kB")) { "a blank description is dropped" }
    val long = ExitFacts.line(record(1, 4, "x".repeat(1000)), format)
    check(long.length < 300 && long.contains("x".repeat(ExitFacts.MAX_DESCRIPTION) + "\"")) { "descriptions are bounded" }
}

private fun theNewestThreeNewestFirst() {
    val lines = ExitFacts.lines(listOf(record(10, 1), record(40, 2), record(20, 3), record(30, 6)), format)
    check(lines.size == ExitFacts.MAX_LINES) { "at most ${ExitFacts.MAX_LINES} lines" }
    check(lines.map { it.substringAfter("exit: ").substringBefore(" ") } == listOf("t40", "t30", "t20")) {
        "newest first: $lines"
    }
    check(ExitFacts.lines(emptyList(), format).isEmpty()) { "no records, no lines" }
}

// #782: a start logs only the exits no earlier start logged, so the host store does not fill with the same history.
private fun eachExitIsLoggedOnce() {
    val first = listOf(record(30, 6), record(20, 3), record(10, 1))
    check(ExitFacts.unlogged(first, emptySet()) == first) { "a first start logs every exit" }
    val logged = first.map(ExitFacts::key).toSet()
    check(ExitFacts.unlogged(first, logged).isEmpty()) { "a later start logs the same exits again" }
    val next = listOf(record(40, 5), record(30, 6), record(20, 3))
    check(ExitFacts.unlogged(next, logged) == listOf(record(40, 5))) { "only the new exit is logged" }
    // A wall clock set back: an exit stamped before the newest logged one is still new.
    val earlier = listOf(record(30, 6), record(25, 2).copy(pid = 9999))
    check(ExitFacts.unlogged(earlier, logged) == listOf(earlier[1])) { "an exit stamped earlier is still logged" }
    // The same time from another process is another exit.
    check(ExitFacts.unlogged(listOf(record(30, 6).copy(pid = 1)), logged).size == 1) { "pid tells exits apart" }
}

fun main() {
    everyReasonHasItsName()
    aLineCarriesTheFacts()
    theNewestThreeNewestFirst()
    eachExitIsLoggedOnce()
    println("ExitFactsTest: OK")
}
