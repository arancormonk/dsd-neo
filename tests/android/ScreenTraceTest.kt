// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/**
 * A change record as tests/ui/test_ui_qt_diagnostics_log.cpp holds it (DIAGNOSTICS_GOLDEN_SCREEN), where native
 * redaction must keep it whole; run_jvm_tests.py checks the two are byte-identical.
 */
private const val GOLDEN_SCREEN =
    "screen: change 1: dim on, brightness 0.010 requested, 0.010 read back; mode dim_between_calls, delay 30 s, " +
        "session 7, visible, focused, top, not multi-window, screen on, wants dim and keep on, engaged by nothing; " +
        "activity 2 resumed, window 3 attached"

private class TraceRig {
    var now = 50_000L
    val records = mutableListOf<String>()
    var scheduled: Long? = null
    val trace = ScreenTrace({ now }, { scheduled = it }, { records += it })

    /** Moves the clock to [at], running the scheduled tick on the way if it is due. */
    fun advance(at: Long) {
        val due = scheduled
        if (due != null && due <= at) {
            now = due
            scheduled = null // The posted tick is spent once it runs, as a Handler's is.
            trace.tick()
        }
        now = at
    }

    fun take(): List<String> = records.toList().also { records.clear() }
}

private fun aWrittenChangeIsFollowedToItsDraw() {
    val rig = TraceRig()
    rig.trace.changed("dim on, written", written = true)
    check(rig.take() == listOf("screen: change 1: dim on, written"))
    check(rig.scheduled == 52_000L) { "the draw deadline" }
    // Draws before any change are nothing to follow; the first traversal and draw after it are its follow record.
    rig.now += 3
    rig.trace.preDraw()
    rig.now += 5
    rig.trace.preDraw()
    rig.now += 8
    rig.trace.draw(3)
    check(rig.take() == listOf("screen: change 1: traversal after 3 ms, draw after 16 ms on window 3"))
    check(rig.scheduled == null)
    rig.trace.draw(3)
    rig.trace.preDraw()
    check(rig.take().isEmpty()) { "later draws belong to no change" }
}

private fun aChangeThatNeverDrawsSaysSo() {
    val rig = TraceRig()
    rig.trace.changed("dim off, written", written = true)
    rig.now += 40
    rig.trace.preDraw()
    rig.advance(rig.now + 5_000)
    check(
        rig.take() == listOf(
            "screen: change 1: dim off, written",
            "screen: change 1: no draw within 2000 ms; traversal after 40 ms",
        ),
    )
    rig.trace.changed("dim on, written", written = true)
    rig.advance(rig.now + 5_000)
    check(rig.take().last() == "screen: change 2: no draw within 2000 ms; no traversal")
    // A change that was not written has nothing to follow.
    rig.trace.changed("dim off, already none, not written", written = false)
    check(rig.scheduled == null)
    rig.trace.draw(1)
    check(rig.take() == listOf("screen: change 3: dim off, already none, not written"))
}

private fun changesAreRecordedAtMostOncePerSecond() {
    val rig = TraceRig()
    val start = rig.now
    rig.trace.changed("a", written = true)
    rig.now = start + 100
    rig.trace.changed("b", written = true)
    rig.now = start + 300
    rig.trace.changed("c", written = true)
    rig.now = start + 600
    rig.trace.changed("d", written = true)
    // Change 1 was recorded; 2 was replaced before a draw and is followed no further. 3 and 4 wait.
    check(
        rig.take() == listOf(
            "screen: change 1: a",
            "screen: change 1: replaced by change 2 after 100 ms; no traversal, no draw",
        ),
    )
    check(rig.scheduled == start + 1_000) { "the waiting change goes out when the second is up" }
    // Change 4 draws before its record is out: the follow record waits for it.
    rig.now = start + 700
    rig.trace.preDraw()
    rig.trace.draw(2)
    check(rig.take().isEmpty()) { "a follow record never comes before its change record" }
    rig.advance(start + 1_000)
    check(
        rig.take() == listOf(
            "screen: change 4 (recorded 400 ms late, replacing 2 changes not recorded): d",
            "screen: change 4: traversal after 100 ms, draw after 100 ms on window 2",
        ),
    ) { rig.records.toString() }
    // The next change inside the new second waits again; one after it is recorded at once.
    rig.now = start + 1_500
    rig.trace.changed("e", written = false)
    check(rig.take().isEmpty())
    rig.advance(start + 2_100)
    check(rig.take() == listOf("screen: change 5 (recorded 500 ms late): e"))
    rig.now = start + 3_200
    rig.trace.changed("f", written = false)
    check(rig.take() == listOf("screen: change 6: f"))
}

private fun aLateTickLetsTheNextChangeReplaceTheWaitingOne() {
    val rig = TraceRig()
    val start = rig.now
    rig.trace.changed("a", written = false)
    rig.now = start + 200
    rig.trace.changed("b", written = true)
    // The main thread was busy past the scheduled tick; the next change is due a record and replaces the waiting one.
    rig.now = start + 1_600
    rig.trace.changed("c", written = false)
    check(
        rig.take() == listOf("screen: change 1: a", "screen: change 3 (replacing 1 changes not recorded): c"),
    ) { rig.records.toString() }
    rig.advance(start + 10_000)
    check(rig.take().isEmpty()) { "the replaced change's follow record is dropped with it" }
}

private fun theRecordsFitAndKeepKeyWordsOut() {
    val rig = TraceRig()
    rig.trace.changed("keep on; " + "x".repeat(800), written = false)
    val record = rig.take().single()
    check(HostRecordText.bytes(record) <= HostRecordText.MAX_BYTES)
    check(record.endsWith("..."))
    check(ScreenTrace.brightness(-1f) == "none" && ScreenTrace.brightness(0.01f) == "0.010")
    check(ScreenTrace.brightness(Float.NaN) == "unset")
}

private fun theInputsNameWhatEngagesThePolicy() {
    var now = 1_000_000L
    val effects = object : ScreenController.Effects {
        override fun setKeepScreenOn(on: Boolean) = Unit
        override fun setDimmed(dimmed: Boolean) = Unit
        override fun renewLease() = Unit
        override fun releaseLease() = Unit
        override fun pulseWake() = ScreenWake.WOKE
        override fun schedule(atMs: Long?) = Unit
    }
    val controller = ScreenController({ now }, effects)
    controller.configure(ScreenPolicy.Mode.DIM_BETWEEN_CALLS.code, 30)
    controller.started()
    controller.topResumedChanged(true, true)
    controller.focusChanged(true, true)
    controller.sessionStarted(7)
    now += 1_200
    controller.touch(ScreenPolicy.ACTION_DOWN, false)
    controller.touch(ScreenPolicy.ACTION_UP, false)
    now += 300
    val engaged = ScreenTrace.inputs(controller.state, now)
    check(
        engaged == "mode dim_between_calls, delay 30 s, session 7, visible, focused, top, not multi-window, screen on, " +
            "wants keep on, engaged by touch 300 ms ago and start 1500 ms ago and setting 1500 ms ago",
    ) { engaged }
    now += 31_000
    controller.tick()
    val idle = ScreenTrace.inputs(controller.state, now)
    check(
        idle == "mode dim_between_calls, delay 30 s, session 7, visible, focused, top, not multi-window, screen on, " +
            "wants dim and keep on, engaged by nothing",
    ) { idle }
    // The golden record, which native redaction keeps whole.
    val rig = TraceRig()
    val description = "dim on, brightness ${ScreenTrace.brightness(0.01f)} requested, ${ScreenTrace.brightness(0.01f)} " +
        "read back; $idle; ${ActivityTrace.Snapshot(2, 3, "resumed", true)}"
    rig.trace.changed(description, written = true)
    check(rig.take().single() == GOLDEN_SCREEN) { rig.records.toString() }
}

// #782: a touch that comes down while the dim is on says whether the policy took it and what came of it, at most once
// per interval, the next record counting the ones left out.
private fun aTouchWhileDimmedIsRecorded() {
    val rig = TraceRig()
    rig.trace.touchWhileDimmed(taken = true, swallowed = true, dimAfter = false)
    check(rig.take() == listOf("screen: touch while dimmed: swallowed, dim off")) { "a tap that lifted the dim" }
    rig.now += 1_000
    rig.trace.touchWhileDimmed(taken = true, swallowed = false, dimAfter = true)
    rig.now += 1_000
    rig.trace.touchWhileDimmed(taken = false, swallowed = false, dimAfter = true)
    check(rig.take().isEmpty()) { "inside the interval, touches are only counted" }
    rig.now += ScreenTrace.MIN_TOUCH_INTERVAL_MS
    rig.trace.touchWhileDimmed(taken = true, swallowed = false, dimAfter = true)
    check(rig.take() == listOf("screen: touch while dimmed: passed on, dim still on; 2 more since, not recorded")) {
        "a tap the policy ignored, and the ones left out"
    }
    rig.now += ScreenTrace.MIN_TOUCH_INTERVAL_MS
    rig.trace.touchWhileDimmed(taken = false, swallowed = false, dimAfter = true)
    check(rig.take() == listOf("screen: touch while dimmed: not taken: the screen policy follows another activity")) {
        "a tap that reached an activity the policy does not follow"
    }
}

fun main() {
    aWrittenChangeIsFollowedToItsDraw()
    aChangeThatNeverDrawsSaysSo()
    changesAreRecordedAtMostOncePerSecond()
    aLateTickLetsTheNextChangeReplaceTheWaitingOne()
    theRecordsFitAndKeepKeyWordsOut()
    theInputsNameWhatEngagesThePolicy()
    aTouchWhileDimmedIsRecorded()
    println("PASS: screen trace records, rate limit and draw follow")
}
