// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/**
 * An incident as tests/ui/test_ui_qt_diagnostics_log.cpp holds it (DIAGNOSTICS_GOLDEN_WATCHDOG), where native
 * redaction must keep every line whole; run_jvm_tests.py checks the two are byte-identical. A stack that names
 * dispatchKeyEvent and AES, each broken up so the key rule does not drop the record, and a line number of -1, which the
 * rule leaves alone after a colon.
 */
private const val GOLDEN_WATCHDOG =
    "watchdog: stall 1.1 of 3 at uptime 12.345 s: main thread blocked 1501 ms, state BLOCKED; 2 touches delivered " +
        "since the probe; activity 2 resumed, window 3 attached; stack of 3 frames\n" +
        "watchdog: stall 1.2 of 3: screen as of 40 ms before: mode dim_between_calls, delay 30 s, session 7, " +
        "visible, focused, top, not multi-window, screen on, wants dim and keep on, engaged by nothing\n" +
        "watchdog: stall 1.3 of 3: main stack 1-3: android.view.ViewRootImpl.dispatchK*yEvent(ViewRootImpl.java:7012) " +
        "| javax.crypto.A*S.init(Native Method) | android.os.Looper.loop(Looper.java:-1)"

private val GOLDEN_CAPTURE = LooperWatchdog.Capture(
    stack = listOf(
        "android.view.ViewRootImpl.dispatchKeyEvent(ViewRootImpl.java:7012)",
        "javax.crypto.AES.init(Native Method)",
        "android.os.Looper.loop(Looper.java:-1)",
    ),
    activity = "activity 2 resumed, window 3 attached",
    screen = "as of 40 ms before: mode dim_between_calls, delay 30 s, session 7, visible, focused, top, " +
        "not multi-window, screen on, wants dim and keep on, engaged by nothing",
    state = "BLOCKED",
)

/** The watchdog against a fake clock, a fake watchdog thread and a fake main looper that runs only when told to. */
private class Rig(var stack: List<String> = listOf("android.os.MessageQueue.nativePollOnce(Native Method)")) {
    var now = 10_000L
    var touches = 0L
    val records = mutableListOf<String>()
    private val timers = mutableListOf<Pair<Long, Runnable>>()
    val mainQueue = ArrayDeque<Runnable>()
    var captures = 0
    var posted = 0

    /** When false, the main looper is blocked: probes queue up behind it and nothing runs. */
    var mainRuns = true

    val watchdog = LooperWatchdog(
        clock = { now },
        timer = { at, task -> timers += at to task },
        postToMain = {
            posted += 1
            mainQueue.addLast(it)
        },
        touches = { touches },
        capture = {
            captures += 1
            LooperWatchdog.Capture(stack, "activity 1 resumed, window 1 attached", "as of 0 ms before: mode off", "RUNNABLE")
        },
        emit = { records += it },
    )

    val pendingTimers: Int get() = timers.size

    /** Runs the watchdog's thread and the main looper up to [until], each task at its own instant. */
    fun advance(until: Long, lateBy: Long = 0L) {
        while (true) {
            if (mainRuns) {
                runMain()
            }
            val next = timers.minByOrNull { it.first } ?: break
            if (next.first > until) {
                break
            }
            timers.remove(next)
            now = maxOf(now, next.first + lateBy)
            next.second.run()
        }
        now = maxOf(now, until)
        if (mainRuns) {
            runMain()
        }
    }

    /** A thaw after [lateBy] ms in which the main looper runs before the watchdog's next check, which runs late. */
    fun thawMainFirst(lateBy: Long) {
        val next = timers.minByOrNull { it.first } ?: return
        timers.remove(next)
        now = next.first + lateBy
        runMain()
        next.second.run()
    }

    fun runMain() {
        while (mainQueue.isNotEmpty()) {
            mainQueue.removeFirst().run()
        }
    }

    fun take(): List<String> = records.toList().also { records.clear() }
}

private fun probesArePostedOneAtATimeEveryPeriod() {
    val rig = Rig()
    rig.watchdog.start()
    rig.advance(rig.now + 10_000)
    // One probe at the start and one per 500 ms check after it: 21 in 10 s, each run before the next went out.
    check(rig.posted == 21) { "probes posted: ${rig.posted}" }
    check(rig.pendingTimers == 1) { "the watchdog keeps one check armed" }
    check(rig.take().isEmpty()) { "an idle main thread records nothing" }
}

private fun onlyOneProbeIsOutstanding() {
    val rig = Rig()
    rig.mainRuns = false
    rig.watchdog.start()
    rig.advance(rig.now + 20_000)
    check(rig.mainQueue.size == 1) { "a blocked main looper must hold one probe, not ${rig.mainQueue.size}" }
    // The probe runs; the next one goes out only at the next check, not at once.
    rig.runMain()
    check(rig.mainQueue.isEmpty())
    rig.advance(rig.now + 499)
    check(rig.mainQueue.size <= 1)
    rig.mainRuns = true
    rig.advance(rig.now + 2_000)
    check(rig.mainQueue.isEmpty())
}

private fun aStallBecomesOneIncidentWithItsResume() {
    val rig = Rig(
        stack = listOf(
            "android.os.MessageQueue.nativePollOnce(Native Method)",
            "android.view.ViewRootImpl.dispatchKeyEvent(ViewRootImpl.java:7012)",
        ),
    )
    rig.watchdog.start()
    rig.advance(rig.now + 2_000)
    check(rig.take().isEmpty())
    // The main thread blocks right after a probe went out; two touches arrive before the capture, one after.
    rig.mainRuns = false
    rig.advance(rig.now + 500) // The next probe is posted at this check and then waits.
    val postedAt = rig.now
    rig.touches += 2
    rig.advance(postedAt + 1_500)
    check(rig.take().isEmpty()) { "1500 ms is not over the threshold" }
    rig.advance(postedAt + 1_501)
    val incident = rig.take()
    check(rig.captures == 1)
    check(incident.size == 3) { "header, screen and one stack record: $incident" }
    check(incident[0].startsWith("watchdog: stall 1.1 of 3 at uptime ")) { incident[0] }
    check(incident[0].contains("main thread blocked 1501 ms, state RUNNABLE; 2 touches delivered since the probe")) {
        incident[0]
    }
    check(incident[0].contains("activity 1 resumed, window 1 attached")) { incident[0] }
    check(incident[1] == "watchdog: stall 1.2 of 3: screen as of 0 ms before: mode off") { incident[1] }
    check(
        incident[2] == "watchdog: stall 1.3 of 3: main stack 1-2: android.os.MessageQueue.nativePollOnce(Native Method)" +
            " | android.view.ViewRootImpl.dispatchK*yEvent(ViewRootImpl.java:7012)",
    ) { incident[2] }
    // However long it lasts, one stall is one incident, even past the incident interval.
    rig.touches += 1
    rig.advance(postedAt + 40_000)
    check(rig.take().isEmpty())
    check(rig.captures == 1)
    // The probe runs at last; the next check reports the resume with the incident's next sequence number.
    rig.now = postedAt + 40_200
    rig.runMain()
    rig.mainRuns = true
    rig.advance(postedAt + 40_600)
    val resumed = rig.take()
    check(
        resumed == listOf(
            "watchdog: stall 1.4: main thread resumed after 40200 ms; 3 touches delivered while it waited, 2 of them by " +
                "the capture",
        ),
    ) { resumed.toString() }
    rig.advance(rig.now + 5_000)
    check(rig.take().isEmpty())
}

private fun incidentsAreRateLimited() {
    val rig = Rig()
    rig.watchdog.start()
    fun stall(forMs: Long) {
        rig.mainRuns = false
        rig.advance(rig.now + 500)
        rig.advance(rig.now + forMs)
        rig.mainRuns = true
        rig.advance(rig.now + 1_000)
    }
    stall(2_000)
    check(rig.take().count { it.contains(".1 of ") } == 1)
    // A second stall 10 s later is counted, not recorded, and its resume is not reported either.
    rig.advance(rig.now + 10_000)
    stall(2_000)
    check(rig.take().isEmpty()) { "within 30 s of the last incident" }
    // After 30 s the next stall is recorded and says how many were left out.
    rig.advance(rig.now + 25_000)
    stall(2_000)
    val next = rig.take()
    check(next.first().startsWith("watchdog: stall 2.1 of ")) { next.first() }
    check(next.first().endsWith("; 1 earlier stalls not recorded, within 30 s")) { next.first() }
    check(next.last().startsWith("watchdog: stall 2.4: main thread resumed after ")) { next.last() }
    check(rig.captures == 2)
}

private fun aFrozenProcessIsNotAStall() {
    val rig = Rig()
    rig.watchdog.start()
    rig.advance(rig.now + 1_000)
    // A probe goes out, and the whole process is frozen before the main looper gets to it.
    rig.mainRuns = false
    rig.advance(rig.now + 500)
    check(rig.mainQueue.size == 1)
    // Thawed 60 s later, the watchdog's check happens to run first: its own lateness is not the main thread's stall.
    rig.advance(rig.now + 500, lateBy = 60_000)
    check(rig.captures == 0 && rig.take().isEmpty()) { "a frozen process recorded a stall" }
    rig.mainRuns = true
    rig.advance(rig.now + 2_000)
    check(rig.take().isEmpty())
    check(rig.captures == 0)
    // A real stall is still caught, and a freeze inside it is named and left out of the resume's delay.
    rig.mainRuns = false
    rig.advance(rig.now + 500)
    val postedAt = rig.now
    rig.advance(postedAt + 2_000)
    check(rig.captures == 1)
    rig.advance(rig.now + 500, lateBy = 10_000)
    rig.advance(rig.now + 1_000)
    val ranAt = rig.now
    rig.runMain()
    rig.mainRuns = true
    rig.advance(rig.now + 1_000)
    val resumed = rig.take().last()
    check(resumed.contains("main thread resumed after ${ranAt - postedAt - 10_000} ms;")) { resumed }
    check(resumed.endsWith("; the watchdog itself was held for 10000 ms, not counted")) { resumed }
    // Frozen again inside a stall, and this time the main looper runs the probe before the late check after the thaw.
    rig.advance(rig.now + 31_000)
    rig.mainRuns = false
    rig.advance(rig.now + 500)
    val sentAt = rig.now
    rig.advance(sentAt + 2_000)
    check(rig.captures == 2)
    rig.thawMainFirst(lateBy = 20_000)
    val thawResume = rig.take().last()
    check(thawResume.contains("main thread resumed after ${rig.now - sentAt - 20_000} ms;")) { thawResume }
    check(thawResume.endsWith("; the watchdog itself was held for 20000 ms, not counted")) { thawResume }
    rig.mainRuns = true
}

// The main looper runs the probe, ending a real stall, and only then is the process frozen, before the watchdog's next
// check: the freeze came after the wait, so none of it is taken out of the stall the resume reports.
private fun aFreezeAfterTheProbeRanLeavesTheStallWhole() {
    val rig = Rig()
    rig.watchdog.start()
    rig.advance(rig.now + 1_000)
    rig.mainRuns = false
    rig.advance(rig.now + 500)
    val postedAt = rig.now
    rig.advance(postedAt + 3_000)
    check(rig.captures == 1)
    val ranAt = rig.now
    rig.runMain()
    rig.advance(rig.now + 1_000, lateBy = 10_000)
    val resumed = rig.take().last()
    check(resumed.contains("main thread resumed after ${ranAt - postedAt} ms;")) { resumed }
    check(!resumed.contains("held")) { resumed }
    rig.mainRuns = true
}

private fun theIncidentSplitsIntoRecordsTopFramesFirst() {
    val frames = (1..120).map { "com.example.Deep.frame$it(Deep.java:$it)" } +
        listOf("android.os.λ.Unicode(λ.java:1)", "x".repeat(400))
    val capture = LooperWatchdog.Capture(frames, "activity 1 resumed, window 1 attached", "s".repeat(600), "WAITING")
    val records = LooperWatchdog.incident(7, 3_600_000L, 2_000L, 0L, capture, 0)
    val stack = records.drop(2)
    check(stack.size == LooperWatchdog.MAX_STACK_RECORDS) { "stack records: ${stack.size}" }
    records.forEachIndexed { index, record ->
        check(HostRecordText.bytes(record) <= HostRecordText.MAX_BYTES) { "record $index is ${HostRecordText.bytes(record)} B" }
        check(record.startsWith("watchdog: stall 7.${index + 1} of ${records.size}")) { record }
    }
    check(stack.first().startsWith("watchdog: stall 7.3 of 8: main stack 1-")) { stack.first() }
    check(stack.first().contains(": com.example.Deep.frame1(Deep.java:1) | com.example.Deep.frame2("))
    // Frames are contiguous across records and in order from the top.
    var expected = 1
    for (record in stack) {
        val range = Regex("main stack (\\d+)-(\\d+): ").find(record)!!.groupValues
        check(range[1].toInt() == expected) { record }
        expected = range[2].toInt() + 1
        val body = record.substringAfter(range[0])
        check(body.split(" | ").size == range[2].toInt() - range[1].toInt() + 1) { record }
    }
    val recorded = expected - 1
    check(recorded < frames.size)
    check(records[0].contains("stack of ${frames.size} frames, top $recorded recorded")) { records[0] }
    check(records[0].contains("at uptime 3600.000 s")) { records[0] }
    check(records[1].endsWith("...")) { "an over-long screen record is cut to fit" }
    // The golden incident, which native redaction keeps whole.
    val golden = LooperWatchdog.incident(1, 12_345L, 1_501L, 2L, GOLDEN_CAPTURE, 0)
    check(golden.joinToString("\n") == GOLDEN_WATCHDOG) { golden.joinToString("\n") }
}

private fun recordTextRules() {
    check(HostRecordText.safe("dispatchKeyEvent KEY token Secret PASSWORD") == "dispatchK*yEvent K*Y t*ken S*cret P*SSWORD")
    check(HostRecordText.safe("an AES key, K1, rc4 and Hytera") == "an A*S k*y, K*, r*4 and H*tera")
    check(HostRecordText.safe("-1 at start, a -b flag, [-5], '-R', ok-dash") == "~1 at start, a ~b flag, [~5], '~R', ok-dash")
    check(HostRecordText.safe("android.os.Looper.loop(Looper.java:224)") == "android.os.Looper.loop(Looper.java:224)")
    check(HostRecordText.fit("abc", 3) == "abc")
    check(HostRecordText.fit("abcdef", 5) == "ab...")
    // Never cut inside a character: λ is two bytes.
    val cut = HostRecordText.fit("λλλλ", 6)
    check(cut == "λ..." && HostRecordText.bytes(cut) <= 6) { cut }
    check(HostRecordText.uptime(12_345L) == "12.345 s")
    check(HostRecordText.uptime(9_999_999_999L) == "9999999.999 s") { "seconds stay under ten digits for centuries" }
    check(HostRecordText.MAX_BYTES == 512 - "[host] [info] ".length)
}

fun main() {
    recordTextRules()
    probesArePostedOneAtATimeEveryPeriod()
    onlyOneProbeIsOutstanding()
    aStallBecomesOneIncidentWithItsResume()
    incidentsAreRateLimited()
    aFrozenProcessIsNotAStall()
    aFreezeAfterTheProbeRanLeavesTheStallWhole()
    theIncidentSplitsIntoRecordsTopFramesFirst()
    println("PASS: main-looper watchdog probes, stalls, rate limit and records")
}
