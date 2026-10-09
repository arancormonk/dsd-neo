// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

private class RecordingScreen : StatusFeed.Screen {
    private val calls = mutableListOf<String>()

    override fun sessionStarted(id: Long) {
        calls += "start $id"
    }

    override fun sample(stamp: ULong, ageMs: Long, wakeAllowed: Boolean) {
        calls += "sample $stamp $ageMs $wakeAllowed"
    }

    override fun sessionEnded() {
        calls += "end"
    }

    fun take(): List<String> = calls.toList().also { calls.clear() }
}

/** A status whose only screen-relevant fields are the audible stamp and its age. */
private fun status(stamp: ULong, ageMs: Long) = DecoderStatus(
    protocol = "DMR",
    radioInput = true,
    trunking = false,
    trunkTuned = false,
    ccFreqHz = 0L,
    vcFreqHz = 0L,
    centerFreqHz = 0L,
    slots = emptyList(),
    leadSlotIndex = -1,
    audibleStamp = stamp,
    audibleAgeMs = ageMs,
)

private fun everyRunningTickSamplesEvenWhenOnlyTheAgeMoved() {
    val screen = RecordingScreen()
    val feed = StatusFeed(screen)
    feed.onTick(true, 3L, status(50uL, 100), true)
    check(screen.take() == listOf("start 3", "sample 50 100 true"))
    feed.onTick(true, 3L, status(50uL, 1_100), false)
    check(screen.take() == listOf("sample 50 1100 false")) { "an age-only change still feeds the policy" }
    val same = status(50uL, 2_100)
    feed.onTick(true, 3L, same, true)
    feed.onTick(true, 3L, same, true)
    check(screen.take() == listOf("sample 50 2100 true", "sample 50 2100 true")) { "an unchanged record still feeds" }
}

private fun aDuplicateSessionIdStartsOnce() {
    val screen = RecordingScreen()
    val feed = StatusFeed(screen)
    repeat(3) { feed.onTick(true, 9L, status(0uL, -1), true) }
    check(screen.take().count { it.startsWith("start") } == 1)
    feed.onTick(true, 10L, status(0uL, -1), true)
    check(screen.take() == listOf("start 10", "sample 0 -1 true")) { "a new id is a new session" }
}

private fun aNullStatusSamplesAsNoChange() {
    val screen = RecordingScreen()
    val feed = StatusFeed(screen)
    // Before any readable record: the wire's "nothing audible" (stamp 0, age -1).
    feed.onTick(true, 4L, null, true)
    check(screen.take() == listOf("start 4", "sample 0 -1 true"))
    feed.onTick(true, 4L, status(77uL, 300), true)
    check(screen.take() == listOf("sample 77 300 true"))
    // After one: the last stamp again, with no age, so the policy sees no change but still renews a held lease.
    feed.onTick(true, 4L, null, false)
    check(screen.take() == listOf("sample 77 -1 false"))
    feed.onTick(true, 4L, status(77uL, 2_300), true)
    check(screen.take() == listOf("sample 77 2300 true"))
}

private fun everyTerminationEndsTheSessionOnce() {
    val screen = RecordingScreen()
    val feed = StatusFeed(screen)
    feed.onTerminated()
    feed.onTick(false, 5L, status(1uL, 0), true)
    check(screen.take().isEmpty()) { "nothing to end before a session was fed" }
    feed.onTick(true, 5L, status(1uL, 0), true)
    screen.take()
    // The not-RUNNING tick, then every teardown path the service has: one end between them.
    feed.onTick(false, 5L, status(1uL, 0), true)
    feed.onTerminated()
    feed.onTerminated()
    feed.onTick(false, 5L, null, true)
    feed.onTerminated()
    check(screen.take() == listOf("end"))
    // A later run, even under the same id, starts over.
    feed.onTick(true, 5L, status(1uL, 0), true)
    check(screen.take() == listOf("start 5", "sample 1 0 true"))
    feed.onTerminated()
    check(screen.take() == listOf("end"))
}

private fun theFeedDrivesTheControllerLease() {
    var now = 4_000_000L
    val renewals = mutableListOf<Long>()
    var released = 0
    val controller = ScreenController(
        { now },
        object : ScreenController.Effects {
            override fun setKeepScreenOn(on: Boolean) {}

            override fun setDimmed(dimmed: Boolean) {}

            override fun renewLease() {
                renewals += now
            }

            override fun releaseLease() {
                released++
            }

            override fun pulseWake(): Boolean = true

            override fun schedule(atMs: Long?) {}
        },
    )
    controller.configure(3, 30)
    controller.started()
    controller.topResumedChanged(true, true)
    controller.focusChanged(true, true)
    val feed = StatusFeed(controller)
    feed.onTick(true, 1L, status(10uL, -1), true)
    feed.onTick(true, 1L, status(11uL, 0), true)
    // The call's audio record ages each second without a new stamp; each poll renews the 10 s lease.
    for (age in 1_000L..5_000L step 1_000L) {
        now += 1_000
        feed.onTick(true, 1L, status(11uL, age), true)
    }
    now += 1_000
    feed.onTick(true, 1L, null, true)
    // Taken at the start, then renewed by each of the eight polls.
    check(renewals.size == 9 && renewals.last() == now) { "renewed on every poll: $renewals" }
    check(controller.policy.lastAudio == now - 6_000) { "only the new stamp was activity" }
    feed.onTerminated()
    check(released == 1 && !controller.policy.outputs.holdLease)
}

fun main() {
    everyRunningTickSamplesEvenWhenOnlyTheAgeMoved()
    aDuplicateSessionIdStartsOnce()
    aNullStatusSamplesAsNoChange()
    everyTerminationEndsTheSessionOnce()
    theFeedDrivesTheControllerLease()
    println("PASS: status feed samples every running tick, starts each session once and ends it once")
}
