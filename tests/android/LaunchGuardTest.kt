// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

// LaunchGuard is process state, so this file runs in its own JVM: the first call below is a fresh process's.

/** What the activity hands LaunchGuard: ProcessFacts.setStateSummary, here a list of the summaries set. */
private val summaries = mutableListOf<ByteArray>()
private val setSummary: (ByteArray) -> Unit = { summaries += it }

private fun aFreshProcessIsTheFirstInstance() {
    check(LaunchGuard.onCreate(setSummary) == LaunchGuard.Launch.FIRST) { "a fresh process is the first instance" }
    check(summaries.isEmpty()) { "the first instance records nothing" }
    // Until an afterQt, no instance has started Qt in this process.
    check(LaunchGuard.onCreate(setSummary) == LaunchGuard.Launch.FIRST) { "no instance has started Qt yet" }
    check(summaries.isEmpty()) { "nothing before Qt started" }
}

private fun anInstanceAfterQtStartedIsTheSecond() {
    LaunchGuard.afterQt()
    check(summaries.isEmpty()) { "starting Qt records nothing: a normal launch and its exit carry no summary" }
    check(LaunchGuard.onCreate(setSummary) == LaunchGuard.Launch.SECOND_IN_PROCESS) { "Qt started: a second instance" }
    check(summaries.size == 1) { "the second instance sets the summary once: ${summaries.size}" }
    val summary = summaries.single()
    check(summary.toString(Charsets.UTF_8) == "activity: second instance in a running process") {
        "unexpected summary: ${summary.toString(Charsets.UTF_8)}"
    }
    check(summary.size <= LaunchGuard.MAX_SUMMARY_BYTES) { "ActivityManager refuses ${summary.size} bytes" }
    check(LaunchGuard.MAX_SUMMARY_BYTES == 128) { "ActivityManager.setProcessStateSummary keeps 128 bytes" }
    // A later afterQt (one never runs after a second instance on Qt 6.11.2) leaves the process started.
    LaunchGuard.afterQt()
    check(LaunchGuard.onCreate(setSummary) == LaunchGuard.Launch.SECOND_IN_PROCESS) { "still a running process" }
    check(summaries.size == 2) { "each second instance records" }
}

// The next process reads the summary back off ApplicationExitInfo, as ProcessFacts does, onto the exit's line.
private fun theNextProcessPrintsIt() {
    val record = ExitFacts.Record(
        timestampMs = 7, pid = 4007, reason = 1, status = 0, importance = 100, pssKb = 51200, description = null,
        summary = summaries.first().toString(Charsets.UTF_8),
    )
    val line = ExitFacts.line(record) { "t$it" }
    check(
        line == "exit: t7 pid 4007 EXIT_SELF status 0 importance 100 pss 51200 kB " +
            "(activity: second instance in a running process; Qt 6.11.2 restarts the process)",
    ) { "unexpected line: $line" }
}

fun main() {
    aFreshProcessIsTheFirstInstance()
    anInstanceAfterQtStartedIsTheSecond()
    theNextProcessPrintsIt()
    println("LaunchGuardTest: OK")
}
