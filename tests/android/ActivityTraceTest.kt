// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/** Stands in for an Activity instance; equal by value, so identity is what the trace must go by. */
private data class Instance(val name: String)

private fun generationsCountCreatesAndWindowAttaches() {
    val trace = ActivityTrace()
    check(trace.snapshot == ActivityTrace.Snapshot(0, 0, "not created", false))
    val first = Instance("main")
    trace.created(first)
    check(trace.snapshot == ActivityTrace.Snapshot(1, 0, "created", false))
    trace.phase(first, "started")
    trace.phase(first, "resumed")
    trace.windowAttached(first)
    check(trace.snapshot.toString() == "activity 1 resumed, window 1 attached") { trace.snapshot.toString() }
    trace.windowDetached(first)
    trace.windowAttached(first)
    check(trace.snapshot == ActivityTrace.Snapshot(1, 2, "resumed", true)) { "each attach of a window counts" }
    trace.touched()
    trace.touched()
    check(trace.touches == 2L)
}

private fun aRecreationKeepsTheGenerationsStraight() {
    val trace = ActivityTrace()
    val old = Instance("main")
    trace.created(old)
    trace.phase(old, "resumed")
    trace.windowAttached(old)
    // A configuration change: the new instance is created before the old one is torn down.
    val recreated = Instance("main")
    trace.created(recreated)
    check(trace.snapshot == ActivityTrace.Snapshot(2, 1, "created", false)) { trace.snapshot.toString() }
    // The old instance's late callbacks describe nothing current.
    trace.phase(old, "stopped")
    trace.windowDetached(old)
    trace.destroyed(old)
    check(trace.snapshot == ActivityTrace.Snapshot(2, 1, "created", false)) { trace.snapshot.toString() }
    trace.windowAttached(old)
    check(trace.snapshot.window == 1) { "a stale window attach does not move the generation" }
    trace.phase(recreated, "resumed")
    trace.windowAttached(recreated)
    check(trace.snapshot == ActivityTrace.Snapshot(2, 2, "resumed", true)) { trace.snapshot.toString() }
    // The usual order, a full teardown and a fresh start: the generations count on.
    trace.phase(recreated, "in Qt teardown")
    check(trace.snapshot.toString() == "activity 2 in Qt teardown, window 2 attached")
    trace.destroyed(recreated)
    check(trace.snapshot == ActivityTrace.Snapshot(2, 2, "destroyed", true))
    trace.phase(recreated, "resumed")
    check(trace.snapshot.phase == "destroyed") { "a destroyed instance is no longer current" }
    val next = Instance("main")
    trace.created(next)
    trace.windowAttached(next)
    check(trace.snapshot == ActivityTrace.Snapshot(3, 3, "created", true))
}

fun main() {
    generationsCountCreatesAndWindowAttaches()
    aRecreationKeepsTheGenerationsStraight()
    println("PASS: activity trace generations and phases")
}
