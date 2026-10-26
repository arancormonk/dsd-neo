// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

// What one stop handed the native side: the userRequested flag and the cause of each nativeStop call, in order.
private fun stopsFor(vararg causes: StopCause): List<Pair<Boolean, String>> {
    val seen = mutableListOf<Pair<Boolean, String>>()
    for (cause in causes) {
        val rc = EngineStop.request(cause) { userRequested, wire ->
            seen += userRequested to wire
            DsdNative_STATUS_OK
        }
        check(rc == DsdNative_STATUS_OK) { "$cause: request must return what nativeStop returned, got $rc" }
    }
    return seen
}

// DsdNative itself loads the app's native library, so the test does not touch it.
private const val DsdNative_STATUS_OK = 0

private fun onlyTheUsersStopAbandonsUploads() {
    check(stopsFor(StopCause.USER).map { it.first } == listOf(true)) { "the user's Stop must abandon pending uploads" }
    check(stopsFor(StopCause.DEVICE_DETACHED).map { it.first } == listOf(false)) {
        "a USB detach is a normal end and must drain"
    }
    check(stopsFor(StopCause.LIFECYCLE).map { it.first } == listOf(false)) { "a lifecycle teardown must drain" }
}

// #647: the native side latches the first cause of a session's ending, so every stop names its own on the wire.
private fun everyStopNamesItsCause() {
    check(stopsFor(StopCause.USER, StopCause.DEVICE_DETACHED, StopCause.LIFECYCLE).map { it.second } ==
        listOf("user", "detached", "lifecycle")) { "nativeStop must receive each stop's own wire cause" }
}

private fun theReturnCodeIsPassedThrough() {
    val rc = EngineStop.request(StopCause.USER) { _, _ -> -2 }
    check(rc == -2) { "nativeStop's bad-state code must reach the caller, got $rc" }
}

private fun theIntentCauseRoundTripsAndDefaultsToDrain() {
    for (cause in StopCause.entries) {
        check(StopCause.fromWire(cause.wire) == cause) { "$cause must survive the intent extra" }
    }
    // An ACTION_STOP that does not say who sent it must never abandon work.
    check(StopCause.fromWire(null) == StopCause.LIFECYCLE) { "a missing cause must drain" }
    check(StopCause.fromWire("") == StopCause.LIFECYCLE) { "an empty cause must drain" }
    check(StopCause.fromWire("USER") == StopCause.LIFECYCLE) { "the wire value is exact, not case-folded" }
    check(!StopCause.fromWire("bogus").abandonsUploads) { "an unknown cause must drain" }
}

fun main() {
    onlyTheUsersStopAbandonsUploads()
    everyStopNamesItsCause()
    theReturnCodeIsPassedThrough()
    theIntentCauseRoundTripsAndDefaultsToDrain()
    println("EngineStopTest: OK")
}
