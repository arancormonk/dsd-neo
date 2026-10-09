// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/** A tick with a fixed verdict, which the policy asks for as it would ask Android. */
private fun StatusFeed.onTick(running: Boolean, sessionId: Long, status: DecoderStatus?, wakeAllowed: Boolean) =
    onTick(running, sessionId, status) { wakeAllowed }

/** A sample with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenController.sample(stamp: ULong?, ageMs: Long, wakeAllowed: Boolean) =
    sample(stamp, ageMs) { wakeAllowed }

/** A stop with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenController.stopped(interactive: Boolean) = stopped(interactive) { true }

/** A screen-off broadcast with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenController.screenOff() = screenOff { true }

/** Records each delivery, with the verdict the sample would get if asked. */
private class RecordingScreen : StatusFeed.Screen {
    private val calls = mutableListOf<String>()

    override fun sessionStarted(id: Long) {
        calls += "start $id"
    }

    override fun sample(stamp: ULong?, ageMs: Long, wakeAllowed: () -> Boolean) {
        calls += "sample $stamp $ageMs ${wakeAllowed()}"
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
    // No readable record: a sample with no stamp, before any record and after one alike. The feed keeps no stamp of
    // its own to replay; the policy owns the last one.
    feed.onTick(true, 4L, null, true)
    check(screen.take() == listOf("start 4", "sample null -1 true"))
    feed.onTick(true, 4L, status(77uL, 300), true)
    check(screen.take() == listOf("sample 77 300 true"))
    feed.onTick(true, 4L, null, false)
    check(screen.take() == listOf("sample null -1 false"))
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

private fun theFeedHandsOnTheVerdictUnasked() {
    // The verdict costs the glue two binder calls, so the feed passes it on as it is and never asks it itself.
    var asked = 0
    val allowed = { asked++; true }
    var received: (() -> Boolean)? = null
    val feed = StatusFeed(
        object : StatusFeed.Screen {
            override fun sessionStarted(id: Long) {}

            override fun sample(stamp: ULong?, ageMs: Long, wakeAllowed: () -> Boolean) {
                received = wakeAllowed
            }

            override fun sessionEnded() {}
        },
    )
    feed.onTick(true, 6L, status(5uL, 0), allowed)
    feed.onTick(true, 6L, null, allowed)
    check(received === allowed && asked == 0)
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
    check(controller.state.lastAudio == now - 6_000) { "only the new stamp was activity" }
    feed.onTerminated()
    check(released == 1 && !controller.state.outputs.holdLease)
}

private fun aSynchronousSampleThenAnUnreadableTickReplaysNothing() {
    var now = 5_000_000L
    var pulses = 0
    val controller = ScreenController(
        { now },
        object : ScreenController.Effects {
            override fun setKeepScreenOn(on: Boolean) {}

            override fun setDimmed(dimmed: Boolean) {}

            override fun renewLease() {}

            override fun releaseLease() {}

            override fun pulseWake(): Boolean {
                pulses++
                return true
            }

            override fun schedule(atMs: Long?) {}
        },
    )
    val feed = StatusFeed(controller)
    controller.configure(3, 30)
    controller.started()
    controller.topResumedChanged(true, true)
    controller.focusChanged(true, true)
    feed.onTick(true, 1L, status(10uL, -1), true)
    feed.onTick(true, 1L, status(10uL, -1), true)
    now += 30_000
    controller.tick()
    // Asleep in front: armed.
    controller.topResumedChanged(false, false)
    controller.focusChanged(false, false)
    controller.paused(false)
    controller.stopped(false)
    controller.screenOff()
    check(controller.state.armed && pulses == 0)
    // During a phone call (wakes blocked) a call's audio starts; the glue's synchronous sample sees it first, straight
    // through the controller.
    now += 1_000
    controller.sample(11uL, 100, false)
    val heard = controller.state.lastAudio
    check(pulses == 0 && heard == now - 100)
    // The next poll's record is unreadable, and the one after reads that same audio again, with wakes allowed.
    now += 500
    feed.onTick(true, 1L, null, true)
    check(controller.state.lastStamp == 11uL) { "an unreadable tick must not put back an older stamp" }
    now += 500
    feed.onTick(true, 1L, status(11uL, 1_100), true)
    check(pulses == 0 && controller.state.lastAudio == heard) { "the same audio again is not a new call" }
}

fun main() {
    everyRunningTickSamplesEvenWhenOnlyTheAgeMoved()
    aDuplicateSessionIdStartsOnce()
    aNullStatusSamplesAsNoChange()
    everyTerminationEndsTheSessionOnce()
    theFeedHandsOnTheVerdictUnasked()
    theFeedDrivesTheControllerLease()
    aSynchronousSampleThenAnUnreadableTickReplaysNothing()
    println("PASS: status feed samples every running tick, starts each session once and ends it once")
}
