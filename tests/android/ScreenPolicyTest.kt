// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import io.github.arancormonk.dsdneo.ScreenPolicy.Mode
import io.github.arancormonk.dsdneo.ScreenPolicy.Outputs

// Mode codes as Qt's AppPrefs::ScreenMode sends them.
private const val SYSTEM = 0
private const val ALWAYS_ON = 1
private const val DIM = 2
private const val OFF = 3

private const val DELAY_S = 30
private const val DELAY_MS = 30_000L
private const val SESSION = 7L

// The phone's screen-off timeout as the glue reads it at a wake.
private const val TIMEOUT_MS = 30_000L

private val NOTHING = Outputs(keepScreenOn = false, dimmed = false, holdLease = false)

/** A sample with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenPolicy.sample(stamp: ULong?, ageMs: Long, wakeAllowed: Boolean) =
    sample(stamp, ageMs) { wakeAllowed }

/** A stop with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenPolicy.stopped(interactive: Boolean) = stopped(interactive) { true }

/** A screen-off broadcast with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenPolicy.screenOff() = screenOff { true }

/** One policy on a hand-driven clock, with the event sequences the histories share. */
private class PolicyRig(mode: Int = OFF, delaySeconds: Int = DELAY_S) {
    var now = 1_000_000L
    val policy = ScreenPolicy { now }
    private var nextStamp = 1_000uL

    init {
        policy.configure(mode, delaySeconds)
    }

    val wakes: Long get() = policy.wakeSerial

    fun advance(ms: Long) {
        now += ms
    }

    /** Lets every engagement so far run out and delivers the tick the deadline asked for. */
    fun quiet() {
        advance(policy.delaySeconds * 1_000L)
        policy.tick()
    }

    /** DSD-neo started, top-resumed and focused, with the screen on. */
    fun front() {
        policy.started()
        policy.topResumedChanged(true, true)
        policy.focusChanged(true, true)
    }

    /** A decoder session whose first poll has been taken (stamp 0: nothing audible yet). */
    fun session(id: Long = SESSION) {
        policy.sessionStarted(id)
        policy.sample(0uL, -1, true)
    }

    /** A poll that sees new audible audio, heard [ageMs] before it. */
    fun audio(ageMs: Long = 0, wakeAllowed: Boolean = true): ULong {
        nextStamp++
        policy.sample(nextStamp, ageMs, wakeAllowed)
        return nextStamp
    }

    /** The power button with DSD-neo in front: the foreground goes, then the screen-off broadcast arrives. */
    fun sleep() {
        policy.topResumedChanged(false, false)
        policy.focusChanged(false, false)
        policy.paused(false)
        policy.stopped(false)
        policy.screenOff()
    }

    /** Armed and asleep in front of a quiet session: the next fresh call may wake. */
    fun armedAsleep(): PolicyRig {
        front()
        session()
        quiet()
        sleep()
        check(policy.armed && !policy.snoozed && !policy.interactive && !policy.visible)
        return this
    }

    /** Asleep after a power press during audio: armed but snoozed. */
    fun snoozedAsleep(): PolicyRig {
        front()
        session()
        audio()
        sleep()
        check(policy.armed && policy.snoozed)
        return this
    }

    /** The screen-on broadcast for a wake DSD-neo caused. */
    fun ownWake(timeoutMs: Long = TIMEOUT_MS) = policy.screenOn(true, timeoutMs)
}

// ---- The transition table, one test per row ----

private fun row01ConfigureUnchangedIsNoChange() {
    val rig = PolicyRig(mode = SYSTEM)
    check(rig.policy.lastConfig == null) { "configuring the defaults (System, 30 s) changes nothing" }
    // An unknown mode and an off-list delay read as System and 30 s: still no change.
    rig.policy.configure(9, 45)
    check(rig.policy.lastConfig == null && rig.policy.mode == Mode.SYSTEM && rig.policy.delaySeconds == 30)
    rig.policy.configure(DIM, 60)
    val configuredAt = rig.now
    rig.advance(5_000)
    rig.policy.configure(DIM, 60)
    check(rig.policy.lastConfig == configuredAt) { "an unchanged configure must not restart the delay" }
}

private fun row02ConfigureChangedSetsAndEngages() {
    val rig = PolicyRig(mode = SYSTEM)
    rig.advance(1_000)
    rig.policy.configure(OFF, 30)
    check(rig.policy.mode == Mode.OFF_BETWEEN_CALLS && rig.policy.lastConfig == rig.now)
    rig.advance(1_000)
    rig.policy.configure(OFF, 120)
    check(rig.policy.delaySeconds == 120 && rig.policy.lastConfig == rig.now)
    rig.advance(1_000)
    rig.policy.configure(DIM, 120)
    check(rig.policy.mode == Mode.DIM_BETWEEN_CALLS && rig.policy.lastConfig == rig.now)
    // A setting change brightens a dimmed screen for a full (new) delay.
    val dim = PolicyRig(mode = DIM)
    dim.front()
    dim.session()
    dim.quiet()
    check(dim.policy.outputs.dimmed)
    dim.policy.configure(DIM, 60)
    check(!dim.policy.outputs.dimmed && dim.policy.deadline == dim.now + 60_000L)
}

private fun row03DuplicateSessionStartIsNoChange() {
    val rig = PolicyRig()
    rig.front()
    rig.session()
    val startedAt = rig.policy.lastStart
    rig.advance(5_000)
    rig.policy.sessionStarted(SESSION)
    check(rig.policy.session == SESSION && rig.policy.lastStart == startedAt) { "a duplicate START is no new start" }
    // Not primed again: the next new stamp is activity rather than a first sample.
    rig.audio()
    check(rig.policy.lastAudio == rig.now)
}

private fun row04NewSessionStartsAndPrimesOnItsFirstSample() {
    val rig = PolicyRig()
    rig.front()
    rig.advance(1_000)
    rig.policy.sessionStarted(SESSION)
    check(rig.policy.session == SESSION && rig.policy.lastStart == rig.now)
    // The native stamp resets at every session end, so audio the first look reports was played in this session.
    rig.policy.sample(500uL, 0, true)
    check(rig.policy.lastStamp == 500uL && rig.policy.lastAudio == rig.now) { "the first sample counts its audio" }
    rig.advance(1_000)
    rig.policy.sample(501uL, 0, true)
    check(rig.policy.lastAudio == rig.now)
    // A first look with no age records the stamp and nothing else.
    val none = PolicyRig()
    none.front()
    none.policy.sessionStarted(SESSION)
    none.policy.sample(500uL, -1, true)
    check(none.policy.lastStamp == 500uL && none.policy.lastAudio == null) { "a first sample with no age is no audio" }
    // A new id clears a snooze, restarts the delay and primes again, even with the screen off.
    val snoozed = PolicyRig().snoozedAsleep()
    snoozed.advance(2_000)
    snoozed.policy.sessionStarted(SESSION + 1)
    check(snoozed.policy.session == SESSION + 1 && !snoozed.policy.snoozed && snoozed.policy.lastStart == snoozed.now)
    snoozed.audio()
    check(snoozed.policy.lastAudio == snoozed.now && snoozed.wakes == 0L) { "a new session's first sample never wakes" }
    snoozed.audio()
    check(snoozed.policy.lastAudio == snoozed.now && snoozed.wakes == 1L)
}

private fun row05SessionEndRestoresEverythingButArming() {
    // Off after an own wake: the lease and its deadline go at once, the arming stays.
    val off = PolicyRig().armedAsleep()
    off.audio()
    off.ownWake()
    check(off.policy.outputs.holdLease && off.policy.deadline != null)
    off.advance(1_000)
    off.policy.sessionEnded()
    check(off.policy.session == null && off.policy.outputs == NOTHING && off.policy.deadline == null)
    check(off.policy.armed) { "ending a session keeps the arming" }
    check(off.policy.leaseReleasedAt == off.now)
    // Dim: brightness and the keep-on flag are restored.
    val dim = PolicyRig(mode = DIM)
    dim.front()
    dim.session()
    dim.quiet()
    check(dim.policy.outputs == Outputs(keepScreenOn = true, dimmed = true, holdLease = false))
    dim.policy.sessionEnded()
    check(dim.policy.outputs == NOTHING && dim.policy.deadline == null)
    // A snooze ends with the session.
    val snoozed = PolicyRig().snoozedAsleep()
    snoozed.policy.sessionEnded()
    check(!snoozed.policy.snoozed && snoozed.policy.armed)
}

private fun row06NewActivityRecordsAudioAndWakes() {
    val rig = PolicyRig().armedAsleep()
    val stamp = rig.audio(ageMs = 400)
    check(rig.policy.lastStamp == stamp && rig.policy.lastAudio == rig.now - 400)
    check(rig.wakes == 1L) { "fresh audio, armed and asleep in Off mode wakes" }
    // lastAudio only moves forward: a new stamp that reports older audio leaves it.
    val front = PolicyRig()
    front.front()
    front.session()
    front.audio()
    val heard = front.policy.lastAudio
    front.advance(1_000)
    front.audio(ageMs = 5_000)
    check(front.policy.lastAudio == heard)
    // Each wake condition alone blocks it.
    check(PolicyRig().armedAsleep().apply { audio(ageMs = 2_000) }.wakes == 1L) { "2 s is still fresh" }
    check(PolicyRig().armedAsleep().apply { audio(ageMs = 2_001) }.wakes == 0L) { "older than 2 s" }
    check(PolicyRig(mode = DIM).armedAsleep().apply { audio() }.wakes == 0L) { "not Off mode" }
    // Visible: the focus went first and armed, the screen is off, but the activity has not stopped yet.
    val visible = PolicyRig()
    visible.front()
    visible.session()
    visible.quiet()
    visible.policy.focusChanged(false, false)
    visible.policy.screenOff()
    visible.audio()
    check(visible.policy.armed && visible.policy.visible && visible.wakes == 0L) { "visible" }
    check(PolicyRig().armedAsleep().apply { policy.userPresent(); audio() }.wakes == 0L) { "not armed" }
    check(PolicyRig().snoozedAsleep().apply { advance(1_000); audio() }.wakes == 0L) { "snoozed" }
    check(PolicyRig().armedAsleep().apply { ownWake(); audio() }.wakes == 0L) { "interactive" }
    val blocked = PolicyRig().armedAsleep()
    blocked.audio(wakeAllowed = false)
    check(blocked.wakes == 0L && blocked.policy.lastAudio == blocked.now) { "a blocked wake still engages" }
    val ended = PolicyRig().armedAsleep()
    ended.policy.sessionEnded()
    val stampBefore = ended.policy.lastStamp
    ended.audio()
    check(ended.wakes == 0L && ended.policy.lastStamp == stampBefore && ended.policy.lastAudio == null) {
        "a sample outside a session is ignored"
    }
}

private fun row06aChangedStampWithoutAgeOnlyMovesTheStamp() {
    val rig = PolicyRig().armedAsleep()
    rig.policy.sample(9_999uL, -1, true)
    check(rig.policy.lastStamp == 9_999uL && rig.policy.lastAudio == null && rig.wakes == 0L)
    check(rig.policy.deadline == null)
}

private fun row07SameStampIsNoChange() {
    val rig = PolicyRig().armedAsleep()
    val stamp = rig.audio(ageMs = 100)
    check(rig.wakes == 1L)
    val heard = rig.policy.lastAudio
    rig.advance(1_000)
    rig.policy.sample(stamp, 1_100, true)
    check(rig.wakes == 1L && rig.policy.lastAudio == heard && rig.policy.lastStamp == stamp)
}

private fun row08RegainingTheForegroundDisarmsAndUnsnoozes() {
    val regains = listOf<Pair<String, (ScreenPolicy) -> Unit>>(
        "started" to { it.started() },
        "top resumed" to { it.topResumedChanged(true, true) },
    )
    for ((name, regain) in regains) {
        val rig = PolicyRig().snoozedAsleep()
        check(rig.policy.lossSequenceOpen && !rig.policy.visible && !rig.policy.top)
        rig.advance(1_000)
        regain(rig.policy)
        check(rig.policy.top && rig.policy.visible) { "$name: top and visible" }
        check(!rig.policy.armed && !rig.policy.snoozed) { "$name: back in front clears arming and snooze" }
        check(rig.policy.lastStart == rig.now && !rig.policy.lossSequenceOpen) { "$name: restarts the delay" }
    }
}

private fun row09FirstLossWhileEligibleWithTheScreenOffArms() {
    val losses = listOf<Pair<String, (ScreenPolicy) -> Unit>>(
        "focus" to { it.focusChanged(false, false) },
        "top resumed" to { it.topResumedChanged(false, false) },
        "paused" to { it.paused(false) },
        "stopped" to { it.stopped(false) },
    )
    for ((name, loss) in losses) {
        val quiet = PolicyRig()
        quiet.front()
        quiet.session()
        quiet.quiet()
        loss(quiet.policy)
        check(quiet.policy.armed && !quiet.policy.snoozed && quiet.policy.lossSequenceOpen) { "$name: arms" }
        val loud = PolicyRig()
        loud.front()
        loud.session()
        loud.audio()
        loud.advance(DELAY_MS - 1)
        loss(loud.policy)
        check(loud.policy.armed && loud.policy.snoozed) { "$name: audio within the delay snoozes" }
    }
    // Snoozing needs a between-calls mode: Always on arms but never snoozes.
    val always = PolicyRig(mode = ALWAYS_ON)
    always.front()
    always.session()
    always.audio()
    always.sleep()
    check(always.policy.armed && !always.policy.snoozed)
}

private fun row10FirstLossOtherwiseDisarms() {
    // The screen is still on: Home, Recents, another app.
    val home = PolicyRig()
    home.front()
    home.session()
    home.policy.topResumedChanged(false, true)
    check(!home.policy.armed && home.policy.lossSequenceOpen)
    // Top-resumed but without the focus just before (a dialog or the other split-screen app has it).
    val unfocused = PolicyRig()
    unfocused.policy.started()
    unfocused.policy.topResumedChanged(true, true)
    unfocused.session()
    unfocused.policy.paused(false)
    check(!unfocused.policy.armed)
    // Focused but not top-resumed just before.
    val notTop = PolicyRig()
    notTop.policy.focusChanged(true, true)
    notTop.session()
    notTop.policy.stopped(false)
    check(!notTop.policy.armed)
    // An arming left from an earlier sequence goes when a new sequence starts ineligible.
    val stale = PolicyRig()
    stale.front()
    stale.session()
    stale.policy.focusChanged(false, false)
    check(stale.policy.armed)
    stale.policy.focusChanged(true, true) // Focus back while still top: the sequence closes.
    stale.policy.topResumedChanged(false, true)
    check(!stale.policy.armed)
}

private fun row11LaterLossesOnlyKeepBooks() {
    val armed = PolicyRig()
    armed.front()
    armed.session()
    armed.quiet()
    armed.policy.topResumedChanged(false, false)
    check(armed.policy.armed && !armed.policy.top && armed.policy.focused && armed.policy.visible)
    armed.policy.focusChanged(false, true) // Even with the screen back on, a later loss does not judge again.
    check(armed.policy.armed && !armed.policy.focused)
    armed.policy.paused(true)
    armed.policy.stopped(true)
    check(armed.policy.armed && !armed.policy.visible && !armed.policy.top)
    val disarmed = PolicyRig()
    disarmed.front()
    disarmed.session()
    disarmed.policy.focusChanged(false, true)
    disarmed.policy.topResumedChanged(false, false)
    disarmed.policy.paused(false)
    disarmed.policy.stopped(false)
    check(!disarmed.policy.armed && !disarmed.policy.top && !disarmed.policy.visible && !disarmed.policy.focused)
}

private fun row12OwnScreenOnRecordsTheWake() {
    val rig = PolicyRig().armedAsleep()
    rig.audio()
    rig.advance(300)
    rig.policy.screenOn(true, 15_000)
    check(rig.policy.interactive && rig.policy.wakeAt == rig.now && rig.policy.wakeTimeoutMs == 15_000L)
    check(rig.policy.armed && rig.policy.outputs.holdLease)
    // With nothing armed (DSD-neo already back in front, on a phone with no lock screen) the wake carries no arming,
    // so it keeps no window.
    val front = PolicyRig().armedAsleep()
    front.audio()
    front.front()
    front.policy.screenOn(true, 15_000)
    check(front.policy.interactive && !front.policy.armed)
    check(front.policy.wakeAt == null && front.policy.wakeTimeoutMs == 0L)
}

private fun row13UserScreenOnDisarmsAtOnce() {
    val rig = PolicyRig().armedAsleep()
    rig.audio(wakeAllowed = false) // Engaged, but nothing woke.
    rig.policy.screenOn(false, TIMEOUT_MS)
    check(rig.policy.interactive && !rig.policy.armed && rig.policy.wakeAt == null)
    check(!rig.policy.outputs.holdLease) { "no lease over a screen the user turned on" }
    // The same screen-on caused by DSD-neo would hold it.
    val ours = PolicyRig().armedAsleep()
    ours.audio(wakeAllowed = false)
    ours.ownWake()
    check(ours.policy.outputs.holdLease)
}

private fun row14UnlockDisarmsAndReleasesAtOnce() {
    val rig = PolicyRig().armedAsleep()
    rig.audio()
    rig.ownWake()
    check(rig.policy.outputs.holdLease)
    rig.advance(2_000)
    rig.policy.userPresent()
    check(!rig.policy.armed && !rig.policy.outputs.holdLease && rig.policy.interactive)
    check(rig.policy.leaseReleasedAt == rig.now)
}

private fun row15ScreenOffJudgesTheOwnWakeWindow() {
    /** An own wake whose lease ends after [DELAY_MS], then the screen off [offAfterMs] after the wake. */
    fun offAfterOwnWake(timeoutMs: Long, offAfterMs: Long): PolicyRig {
        val rig = PolicyRig().armedAsleep()
        rig.audio()
        val wakeAt = rig.now
        rig.ownWake(timeoutMs)
        rig.advance(DELAY_MS)
        rig.policy.tick()
        check(!rig.policy.outputs.holdLease && rig.policy.leaseReleasedAt == wakeAt + DELAY_MS)
        rig.advance(offAfterMs - DELAY_MS)
        rig.policy.screenOff()
        check(!rig.policy.interactive && rig.policy.wakeAt == null)
        return rig
    }
    // The phone's 60 s timeout outlasts the 30 s lease: the off is due at wake + 60 s, plus 5 s of grace.
    check(offAfterOwnWake(60_000, 65_000).policy.armed)
    check(!offAfterOwnWake(60_000, 65_001).policy.armed)
    // The lease outlasts a 15 s timeout: the off is due at its release, wake + 30 s.
    check(offAfterOwnWake(15_000, 35_000).policy.armed)
    check(!offAfterOwnWake(15_000, 35_001).policy.armed)
    // Without an own wake there is no window to judge: arming survives a late off.
    val slept = PolicyRig().armedAsleep()
    slept.advance(10 * 60_000L)
    slept.policy.screenOff()
    check(slept.policy.armed)
    // The off during audio snoozes (or keeps a snooze); without audio it adds none.
    val loud = PolicyRig().armedAsleep()
    loud.audio(wakeAllowed = false)
    loud.policy.screenOff()
    check(loud.policy.snoozed && loud.policy.armed)
    val dim = PolicyRig(mode = DIM).armedAsleep()
    check(!dim.policy.snoozed)
    // An off only adds to a snooze: one still running survives an off that would not start one itself.
    val kept = PolicyRig().snoozedAsleep()
    kept.policy.configure(SYSTEM, DELAY_S)
    kept.policy.screenOff()
    check(kept.policy.snoozed)
    val system = PolicyRig(mode = SYSTEM).armedAsleep()
    system.audio()
    system.policy.screenOff()
    check(!system.policy.snoozed) { "only a between-calls mode snoozes" }
}

private fun row16FocusGainCountsAsATouch() {
    val rig = PolicyRig(mode = DIM)
    rig.front()
    rig.session()
    rig.quiet()
    rig.policy.focusChanged(false, true)
    rig.advance(1_000)
    rig.policy.focusChanged(true, true)
    check(rig.policy.focused && rig.policy.lastTouch == rig.now && !rig.policy.outputs.dimmed)
}

private fun row17MultiWindowNeverDims() {
    val rig = PolicyRig(mode = DIM)
    rig.front()
    rig.session()
    rig.quiet()
    check(rig.policy.outputs.dimmed)
    rig.policy.multiWindowChanged(true)
    check(rig.policy.multiWindow && !rig.policy.outputs.dimmed && rig.policy.outputs.keepScreenOn)
    rig.policy.multiWindowChanged(false)
    check(!rig.policy.multiWindow && rig.policy.outputs.dimmed)
}

private fun row18InteractionCountsOnlyWhileVisible() {
    val rig = PolicyRig(mode = DIM)
    rig.front()
    rig.session()
    rig.quiet()
    rig.policy.userInteraction()
    check(rig.policy.lastTouch == rig.now && !rig.policy.outputs.dimmed && rig.policy.deadline == rig.now + DELAY_MS)
    val hidden = PolicyRig()
    hidden.front()
    hidden.session()
    hidden.policy.stopped(true)
    val touched = hidden.policy.lastTouch
    hidden.advance(1_000)
    hidden.policy.userInteraction()
    check(hidden.policy.lastTouch == touched)
}

private fun rows19To21TouchLatchesTheGesture() {
    val rig = PolicyRig(mode = DIM)
    rig.front()
    rig.session()
    rig.quiet()
    // Row 19: DOWN reads the dimming before it brightens, then counts as a touch.
    check(rig.policy.touch(ScreenPolicy.ACTION_DOWN, false))
    check(rig.policy.swallowing && !rig.policy.outputs.dimmed && rig.policy.lastTouch == rig.now)
    // Row 20: the gesture's middle returns the latch and still counts.
    rig.advance(50)
    check(rig.policy.touch(ScreenPolicy.ACTION_MOVE, false) && rig.policy.lastTouch == rig.now)
    check(rig.policy.touch(ScreenPolicy.ACTION_POINTER_DOWN, false))
    check(rig.policy.touch(ScreenPolicy.ACTION_POINTER_UP, false))
    // Row 21: UP returns the latch and clears it.
    rig.advance(50)
    check(rig.policy.touch(ScreenPolicy.ACTION_UP, false) && !rig.policy.swallowing && rig.policy.lastTouch == rig.now)
    check(!rig.policy.touch(ScreenPolicy.ACTION_DOWN, false)) { "bright now: the next gesture reaches the app" }
    check(!rig.policy.touch(ScreenPolicy.ACTION_CANCEL, false) && !rig.policy.swallowing)
}

private fun row22RefusedWakeSnoozes() {
    val rig = PolicyRig().armedAsleep()
    rig.audio()
    check(rig.wakes == 1L)
    rig.policy.wakeRefused()
    check(!rig.policy.interactive && rig.policy.snoozed && rig.policy.armed)
    rig.advance(5_000)
    rig.audio()
    check(rig.wakes == 1L) { "no retry while the same run of calls continues" }
    rig.advance(DELAY_MS)
    rig.policy.tick()
    check(!rig.policy.snoozed)
    rig.audio()
    check(rig.wakes == 2L)
}

private fun row23TickAtTheDeadlineDimsAndReleases() {
    val rig = PolicyRig()
    rig.front()
    rig.session()
    rig.quiet()
    rig.audio()
    val deadline = rig.now + DELAY_MS
    check(rig.policy.deadline == deadline && rig.policy.outputs.holdLease && !rig.policy.outputs.dimmed)
    rig.advance(DELAY_MS - 1)
    rig.policy.tick()
    check(rig.policy.outputs.holdLease && rig.policy.deadline == deadline) { "not before the deadline" }
    rig.advance(1)
    rig.policy.tick()
    check(!rig.policy.outputs.holdLease && rig.policy.outputs.dimmed)
    check(rig.policy.leaseReleasedAt == deadline && rig.policy.deadline == null)
}

private fun row24DestroyedForgetsTheWindow() {
    val rig = PolicyRig(mode = DIM)
    rig.front()
    rig.session()
    rig.quiet()
    check(rig.policy.touch(ScreenPolicy.ACTION_DOWN, false) && rig.policy.swallowing)
    rig.policy.destroyed()
    check(!rig.policy.visible && !rig.policy.top && !rig.policy.swallowing)
    check(rig.policy.outputs == NOTHING)
    val armed = PolicyRig()
    armed.front()
    armed.session()
    armed.quiet()
    armed.policy.focusChanged(false, false)
    check(armed.policy.armed)
    armed.policy.destroyed()
    check(!armed.policy.armed)
}

private fun housekeepingClearsTheSnoozeAfterAFullQuietDelay() {
    val rig = PolicyRig().snoozedAsleep()
    rig.advance(DELAY_MS - 1)
    rig.policy.multiWindowChanged(false)
    check(rig.policy.snoozed)
    rig.advance(1)
    rig.policy.multiWindowChanged(false)
    check(!rig.policy.snoozed)
    // A call right at the end of the quiet delay wakes, though no other input came between.
    val late = PolicyRig().snoozedAsleep()
    late.advance(DELAY_MS)
    late.audio()
    check(late.wakes == 1L && !late.policy.snoozed)
}

private fun aSnoozeEndsOnlyWhenTheNewAudioFollowsAFullQuietDelay() {
    // The last audio and the off at L. The next poll comes 200 ms after L + delay, but the audio it reports was heard
    // 900 ms before it: at L + 29.3 s, inside the delay. The run of calls never went quiet, so the snooze holds.
    val rig = PolicyRig().snoozedAsleep()
    val lastHeard = rig.policy.lastAudio!!
    rig.advance(DELAY_MS + 200)
    rig.audio(ageMs = 900)
    check(rig.wakes == 0L && rig.policy.snoozed) { "audio heard inside the delay continues the snooze" }
    check(rig.policy.lastAudio == lastHeard + DELAY_MS - 700)
    // Audio heard a full delay after that ends it, and wakes.
    rig.advance(DELAY_MS)
    rig.audio(ageMs = 900)
    check(rig.wakes == 1L && !rig.policy.snoozed)
}

private fun aSampleWithoutARecordChangesNothing() {
    val rig = PolicyRig().armedAsleep()
    val stamp = rig.audio(ageMs = 100)
    check(rig.wakes == 1L)
    val heard = rig.policy.lastAudio
    rig.advance(1_000)
    rig.policy.sample(null, -1, true)
    check(rig.policy.lastStamp == stamp && rig.policy.lastAudio == heard && rig.wakes == 1L)
    // Nor does it take a new session's first look: the first readable record is still the one that never wakes.
    rig.policy.sessionStarted(SESSION + 1)
    rig.policy.sample(null, -1, true)
    rig.policy.sample(stamp + 1uL, 0, true)
    check(rig.policy.lastStamp == stamp + 1uL && rig.policy.lastAudio == rig.now && rig.wakes == 1L)
    rig.advance(1_000)
    rig.policy.sample(stamp + 2uL, 0, true)
    check(rig.policy.lastAudio == rig.now && rig.wakes == 2L)
}

private fun aPowerPressBeforeTheFirstReadableSampleSnoozesTheCall() {
    // The service polls before the engine publishes, so the session's first running poll finds no record. A call is
    // already playing when Power is pressed, before any readable sample: the glue's synchronous sample before the first
    // loss callback is the session's first readable one. It never wakes, but the audio it reports is this session's
    // (the native stamp resets at every session end), so the sleep it ends is engaged and snoozes, and the call's next
    // block cannot wake the screen.
    val rig = PolicyRig()
    rig.front()
    rig.policy.sessionStarted(SESSION)
    rig.policy.sample(null, -1, true) // The first running poll: no record yet.
    rig.advance(2_000)
    val call = 5_000uL
    var asked = 0
    val verdict = { asked++; true }
    rig.policy.sample(call, 300, verdict) // Before the first loss callback: the first readable sample.
    val heardAt = rig.now - 300
    rig.policy.topResumedChanged(false, false)
    rig.policy.sample(call, 300, verdict) // Each later synchronous sample sees the same stamp.
    rig.policy.focusChanged(false, false)
    rig.policy.sample(call, 300, verdict)
    rig.policy.paused(false)
    rig.policy.sample(call, 300, verdict)
    rig.policy.stopped(false, verdict)
    rig.policy.sample(call, 300, verdict)
    rig.policy.screenOff()
    check(rig.policy.armed && rig.policy.snoozed) { "Power during the call snoozes, though the first sample saw it" }
    check(rig.wakes == 0L && asked == 0) { "the first readable sample never wakes, and asks nothing" }
    check(rig.policy.lastStamp == call && rig.policy.lastAudio == heardAt) { "its audio counts from when it was heard" }
    rig.advance(1_000)
    rig.audio() // The call's next block, at the next poll.
    check(rig.wakes == 0L && rig.policy.snoozed) { "the continuing call does not wake the screen" }
    rig.advance(DELAY_MS)
    rig.audio()
    check(rig.wakes == 1L && !rig.policy.snoozed) { "a call after a full quiet delay wakes" }
    // A first readable sample with no age (no stamp, or an expired one) heard nothing: no snooze, and a call wakes.
    for (stamp in listOf(0uL, 5_000uL)) {
        val quiet = PolicyRig()
        quiet.front()
        quiet.policy.sessionStarted(SESSION)
        quiet.policy.sample(null, -1, true)
        quiet.advance(2_000)
        quiet.policy.sample(stamp, -1, true)
        check(quiet.policy.lastStamp == stamp && quiet.policy.lastAudio == null) { "stamp $stamp, age -1: no audio" }
        quiet.sleep()
        check(quiet.policy.armed && !quiet.policy.snoozed) { "stamp $stamp, age -1: no snooze" }
        quiet.audio()
        check(quiet.wakes == 1L) { "stamp $stamp, age -1: the next call wakes" }
    }
}

// ---- Histories ----

private fun splitScreenFocusedVersusUnfocused() {
    // Outside split screen, focus decides: a dialog over DSD-neo keeps it bright.
    val single = PolicyRig(mode = DIM)
    single.front()
    single.session()
    single.quiet()
    check(single.policy.outputs.dimmed)
    single.policy.focusChanged(false, true)
    check(!single.policy.outputs.dimmed)
    // In split screen the brightness override would dim the whole panel, so neither side dims.
    for (focused in listOf(true, false)) {
        val split = PolicyRig(mode = DIM)
        split.front()
        split.policy.multiWindowChanged(true)
        split.session()
        if (!focused) {
            split.policy.topResumedChanged(false, true)
            split.policy.focusChanged(false, true)
        }
        split.quiet()
        check(!split.policy.outputs.dimmed && split.policy.outputs.keepScreenOn) { "split screen, focused=$focused" }
    }
    // The focused side sleeping arms, and a call wakes.
    val focusedOff = PolicyRig()
    focusedOff.front()
    focusedOff.policy.multiWindowChanged(true)
    focusedOff.session()
    focusedOff.quiet()
    focusedOff.sleep()
    check(focusedOff.policy.armed)
    focusedOff.audio()
    check(focusedOff.wakes == 1L)
}

private fun unfocusedSplitScreenSleepDoesNotArm() {
    val rig = PolicyRig()
    rig.front()
    rig.policy.multiWindowChanged(true)
    rig.session()
    // The user taps the other app: DSD-neo stays visible, without top or focus, with the screen on.
    rig.policy.topResumedChanged(false, true)
    rig.policy.focusChanged(false, true)
    rig.quiet()
    check(rig.policy.visible && !rig.policy.armed)
    // Then the power button: later losses of that same sequence.
    rig.policy.paused(false)
    rig.policy.stopped(false)
    rig.policy.screenOff()
    check(!rig.policy.armed)
    rig.audio()
    check(rig.wakes == 0L)
}

private fun homeWithTheScreenOnThenOff() {
    val rig = PolicyRig()
    rig.front()
    rig.session()
    rig.quiet()
    rig.policy.topResumedChanged(false, true)
    rig.policy.focusChanged(false, true)
    rig.policy.paused(true)
    rig.policy.stopped(true)
    check(!rig.policy.armed)
    rig.advance(5_000)
    rig.policy.screenOff()
    check(!rig.policy.armed)
    rig.advance(60_000)
    rig.audio()
    check(rig.wakes == 0L) { "DSD-neo was not in front when the display went off" }
}

private fun userScreenOnDisarmsAtOnce() {
    val rig = PolicyRig().armedAsleep()
    rig.advance(10_000)
    rig.policy.screenOn(false, TIMEOUT_MS)
    check(!rig.policy.armed)
    rig.audio()
    check(rig.wakes == 0L && !rig.policy.outputs.holdLease)
    rig.advance(TIMEOUT_MS)
    rig.policy.screenOff()
    rig.advance(60_000)
    rig.audio()
    check(rig.wakes == 0L) { "a screen off does not re-arm" }
    // Only DSD-neo back in front, then asleep again, re-arms.
    rig.front()
    rig.quiet()
    rig.sleep()
    rig.audio()
    check(rig.wakes == 1L)
}

private fun unlockDisarmsAtOnceAndReleasesWhileStillInteractive() {
    val rig = PolicyRig().armedAsleep()
    rig.audio()
    check(rig.wakes == 1L)
    rig.ownWake()
    check(rig.policy.outputs.holdLease)
    rig.advance(3_000)
    rig.policy.userPresent()
    check(rig.policy.interactive && !rig.policy.armed && !rig.policy.outputs.holdLease)
    // The user is on the home screen now; when that screen goes off, calls no longer wake.
    rig.advance(TIMEOUT_MS)
    rig.policy.screenOff()
    rig.advance(60_000)
    rig.audio()
    check(rig.wakes == 1L)
}

private fun ownWakeOffOnTimeStaysArmed() {
    val rig = PolicyRig().armedAsleep()
    for (wake in 1L..3L) {
        rig.advance(60_000)
        rig.audio()
        check(rig.wakes == wake) { "wake $wake" }
        rig.ownWake()
        check(rig.policy.outputs.holdLease)
        rig.advance(DELAY_MS)
        rig.policy.tick()
        check(!rig.policy.outputs.holdLease)
        rig.advance(4_000) // The phone's own timeout, on time.
        rig.policy.screenOff()
        check(rig.policy.armed && !rig.policy.snoozed && rig.policy.wakeAt == null) { "still armed after wake $wake" }
    }
}

private fun lateOffAfterALockScreenTouchDisarms() {
    val rig = PolicyRig().armedAsleep()
    rig.audio()
    rig.ownWake()
    rig.advance(DELAY_MS)
    rig.policy.tick() // Lease released at wake + 30 s, when the phone's 30 s timer also ends.
    // A touch on the lock screen, or the camera, kept the screen on past that and the 5 s grace.
    rig.advance(5_001)
    rig.policy.screenOff()
    check(!rig.policy.armed)
    rig.advance(60_000)
    rig.audio()
    check(rig.wakes == 1L) { "someone used the phone: no more wakes" }
}

private fun unlockUseAndSleepInFrontAfterAnOwnWakeArmsAfresh() {
    // A 60 s phone timeout: in front, the screen goes off 60 s after the last touch, 30 s after DSD-neo's release and
    // long after the own wake's window. Both broadcast orders, since the loss callbacks usually come first.
    val timeoutMs = 60_000L
    val sleeps = listOf<Pair<String, (ScreenPolicy) -> Unit>>(
        "loss first" to {
            it.topResumedChanged(false, false)
            it.focusChanged(false, false)
            it.paused(false)
            it.stopped(false)
            it.screenOff()
        },
        "broadcast first" to {
            it.screenOff()
            it.topResumedChanged(false, false)
            it.focusChanged(false, false)
            it.paused(false)
            it.stopped(false)
        },
    )
    for ((name, sleep) in sleeps) {
        val rig = PolicyRig().armedAsleep()
        rig.audio()
        check(rig.wakes == 1L)
        rig.ownWake(timeoutMs)
        // The user unlocks 10 s after the wake, DSD-neo comes back in front, and they use it for two minutes.
        rig.advance(10_000)
        rig.policy.userPresent()
        rig.advance(500)
        rig.front()
        repeat(12) {
            rig.advance(10_000)
            rig.policy.userInteraction()
        }
        // Then they leave it in front: the delay runs out, then the phone's timeout from the last touch.
        rig.quiet()
        rig.advance(timeoutMs - DELAY_MS)
        sleep(rig.policy)
        check(rig.policy.armed && !rig.policy.snoozed) { "$name: DSD-neo was in front when the display went off" }
        check(rig.policy.wakeAt == null) { "$name: an arming set after the wake keeps no wake window" }
        rig.advance(60_000)
        rig.audio()
        check(rig.wakes == 2L) { "$name: the next call wakes" }
    }
}

private fun withoutALockScreenAWakeShowsDsdNeoAndTheNextSleepArms() {
    // With no lock screen a wake brings DSD-neo straight back to the front, and the screen-on broadcast can land on
    // either side of that. Either way nothing is armed in front, so the wake carries no arming and keeps no window.
    val timeoutMs = 60_000L
    val wakes = listOf<Pair<String, (PolicyRig) -> Unit>>(
        "broadcast after the regain" to {
            it.front()
            it.advance(100)
            it.ownWake(timeoutMs)
        },
        "broadcast before the regain" to {
            it.ownWake(timeoutMs)
            it.advance(100)
            it.front()
        },
    )
    for ((name, wake) in wakes) {
        val rig = PolicyRig().armedAsleep()
        rig.audio()
        check(rig.wakes == 1L)
        rig.advance(100)
        wake(rig)
        check(rig.policy.interactive && rig.policy.visible && !rig.policy.armed) { "$name: in front, nothing armed" }
        check(rig.policy.wakeAt == null) { "$name: a wake that carried no arming keeps no window" }
        // The user touches DSD-neo now and then for two minutes, then leaves it to time out in front.
        repeat(12) {
            rig.advance(10_000)
            rig.policy.userInteraction()
        }
        rig.quiet()
        rig.advance(timeoutMs - DELAY_MS)
        rig.sleep()
        check(rig.policy.armed && !rig.policy.snoozed) { "$name: DSD-neo was in front when the display went off" }
        rig.advance(60_000)
        rig.audio()
        check(rig.wakes == 2L) { "$name: the next call wakes" }
    }
}

private fun duplicateSessionStartDuringASnoozeIsANoOp() {
    val rig = PolicyRig().snoozedAsleep()
    val startedAt = rig.policy.lastStart
    rig.advance(2_000)
    rig.policy.sessionStarted(SESSION)
    check(rig.policy.snoozed && rig.policy.lastStart == startedAt)
    rig.audio()
    check(rig.wakes == 0L)
}

private fun powerDuringAudioSnoozesButNotDuringATouchOrAQuietStart() {
    val audio = PolicyRig()
    audio.front()
    audio.session()
    audio.quiet()
    audio.audio()
    audio.advance(DELAY_MS - 1_000)
    audio.sleep()
    check(audio.policy.armed && audio.policy.snoozed)
    audio.advance(500)
    audio.audio()
    check(audio.wakes == 0L)
    audio.advance(DELAY_MS - 1)
    audio.audio()
    check(audio.wakes == 0L) { "calls inside the delay keep the snooze going" }
    audio.advance(DELAY_MS)
    audio.audio()
    check(audio.wakes == 1L) { "after a full quiet delay the next call wakes" }
    // Only a touch within the delay.
    val touch = PolicyRig()
    touch.front()
    touch.session()
    touch.quiet()
    touch.policy.userInteraction()
    touch.advance(1_000)
    touch.sleep()
    check(touch.policy.armed && !touch.policy.snoozed)
    touch.audio()
    check(touch.wakes == 1L)
    // A quiet session start just before the off.
    val start = PolicyRig()
    start.front()
    start.quiet()
    start.session()
    start.advance(1_000)
    start.sleep()
    check(start.policy.armed && !start.policy.snoozed)
    start.audio()
    check(start.wakes == 1L)
}

private fun audioBetweenPollsSnoozesThroughTheSyncSample() {
    // The last poll saw nothing new; audio starts 400 ms later and the power press beats the next poll. The glue's
    // synchronous sample before the loss callbacks finds it.
    val synced = PolicyRig()
    synced.front()
    synced.session()
    synced.quiet()
    synced.advance(700)
    synced.audio(ageMs = 300)
    synced.sleep()
    check(synced.policy.armed && synced.policy.snoozed)
    // Without that sample the policy could not know.
    val unsynced = PolicyRig()
    unsynced.front()
    unsynced.session()
    unsynced.quiet()
    unsynced.advance(700)
    unsynced.sleep()
    check(unsynced.policy.armed && !unsynced.policy.snoozed)
    // Audio that starts after the loss callbacks but before the screen-off broadcast: the sample the glue takes before
    // screenOff() finds it. Until that broadcast the policy still counts the screen as on, so that sample cannot wake
    // the screen the user has just turned off. The arming loss saw the screen off and judged that off, so the call
    // began after it: the late broadcast does not snooze it, and, the activity being gone already, wakes for it.
    val late = PolicyRig()
    late.front()
    late.session()
    late.quiet()
    late.policy.topResumedChanged(false, false)
    late.policy.focusChanged(false, false)
    late.policy.paused(false)
    late.policy.stopped(false)
    check(late.policy.armed && !late.policy.snoozed && late.policy.interactive)
    late.advance(200)
    late.audio(ageMs = 100)
    check(late.wakes == 0L)
    var asked = 0
    late.policy.screenOff { asked++; true }
    check(!late.policy.snoozed && late.policy.armed && late.wakes == 1L && asked == 1)
    late.ownWake()
    late.advance(1_000)
    late.audio()
    check(late.wakes == 1L && !late.policy.snoozed)
    // Audio before the loss callbacks is still that off's: the arming loss snoozes, and the late broadcast keeps it.
    val loud = PolicyRig()
    loud.front()
    loud.session()
    loud.quiet()
    loud.advance(700)
    loud.audio(ageMs = 300)
    loud.policy.topResumedChanged(false, false)
    loud.policy.focusChanged(false, false)
    loud.policy.paused(false)
    loud.policy.stopped(false)
    check(loud.policy.armed && loud.policy.snoozed)
    loud.advance(200)
    loud.audio(ageMs = 100)
    asked = 0
    loud.policy.screenOff { asked++; true }
    check(loud.policy.snoozed && loud.policy.armed && loud.wakes == 0L && asked == 0)
}

private fun aCallHeardWhileTheScreenGoesOffInFrontWakesAtTheStop() {
    // The display goes off with DSD-neo in front: a first loss callback arms and the screen-off broadcast lands, but
    // Android has not stopped the activity yet. A short call heard in that window cannot wake a screen that is still
    // visible, and later polls only see its stamp again, so the stop judges it as any wake.
    val windows = listOf<Pair<String, (ScreenPolicy) -> Unit>>(
        "focus, then the broadcast" to {
            it.focusChanged(false, false)
            it.screenOff()
        },
        "the broadcast, then top-resumed" to {
            it.screenOff()
            it.topResumedChanged(false, false)
        },
        "top-resumed and paused, then the broadcast" to {
            it.topResumedChanged(false, false)
            it.paused(false)
            it.screenOff()
        },
    )
    /** Armed in [window], still visible, with a short call heard 300 ms before the poll that read it. */
    fun heardInTheWindow(window: (ScreenPolicy) -> Unit, mode: Int = OFF): Pair<PolicyRig, ULong> {
        val rig = PolicyRig(mode = mode)
        rig.front()
        rig.session()
        rig.quiet()
        window(rig.policy)
        check(rig.policy.armed && rig.policy.visible && !rig.policy.interactive)
        rig.advance(200)
        val stamp = rig.audio(ageMs = 300)
        check(rig.wakes == 0L) { "no wake while still visible" }
        return rig to stamp
    }
    for ((name, window) in windows) {
        val (rig, stamp) = heardInTheWindow(window)
        rig.advance(1_000)
        rig.policy.sample(stamp, 1_300, true) // The next poll: the same call, already over.
        var asked = 0
        rig.policy.stopped(false) { asked++; true }
        check(rig.wakes == 1L && asked == 1) { "$name: the stop wakes for the call, asking Android once" }
        rig.advance(1_000)
        rig.policy.sample(stamp, 2_300, true)
        check(rig.wakes == 1L) { "$name: and only once" }
    }
    val window = windows.first().second
    // At most 2 s old by the stop, like any wake.
    val (onTime, _) = heardInTheWindow(window)
    onTime.advance(1_700)
    onTime.policy.stopped(false)
    check(onTime.wakes == 1L) { "heard exactly 2 s before the stop" }
    val (late, _) = heardInTheWindow(window)
    late.advance(1_701)
    var asked = 0
    late.policy.stopped(false) { asked++; true }
    check(late.wakes == 0L && asked == 0) { "heard more than 2 s before the stop" }
    // Android's verdict, asked only now.
    val (refused, _) = heardInTheWindow(window)
    asked = 0
    refused.policy.stopped(false) { asked++; false }
    check(refused.wakes == 0L && asked == 1 && refused.policy.lastAudio != null) { "refused" }
    // Anything that disarms in the window: another screen-on, then off again, or an unlock.
    val (screenOn, _) = heardInTheWindow(window)
    screenOn.policy.screenOn(false, TIMEOUT_MS)
    screenOn.policy.screenOff()
    screenOn.policy.stopped(false)
    check(screenOn.wakes == 0L && !screenOn.policy.armed) { "a screen-on DSD-neo did not cause disarms" }
    val (unlocked, _) = heardInTheWindow(window)
    unlocked.policy.userPresent()
    unlocked.policy.stopped(false)
    check(unlocked.wakes == 0L) { "an unlock disarms" }
    // Only Off between calls wakes.
    val (dim, _) = heardInTheWindow(window, mode = DIM)
    dim.policy.stopped(false)
    check(dim.wakes == 0L) { "Dim" }
    // A call heard before the screen-off broadcast but after the arming loss saw the screen off began after that off:
    // the late broadcast does not snooze it, and the stop wakes for it.
    val early = PolicyRig()
    early.front()
    early.session()
    early.quiet()
    early.policy.focusChanged(false, false)
    early.audio(ageMs = 100)
    early.policy.screenOff()
    asked = 0
    early.policy.stopped(false) { asked++; true }
    check(!early.policy.snoozed && early.wakes == 1L && asked == 1) { "audio after the arming loss wakes" }
    // A call heard before the arming loss is that off's: the loss snoozes, and the stop wakes nothing.
    val before = PolicyRig()
    before.front()
    before.session()
    before.quiet()
    before.audio(ageMs = 100)
    before.policy.focusChanged(false, false)
    before.policy.screenOff()
    asked = 0
    before.policy.stopped(false) { asked++; true }
    check(before.policy.snoozed && before.wakes == 0L && asked == 0) { "audio before the arming loss snoozes" }
    // With no call in the window the stop asks nothing.
    val quiet = PolicyRig()
    quiet.front()
    quiet.session()
    quiet.quiet()
    window(quiet.policy)
    asked = 0
    quiet.policy.stopped(false) { asked++; true }
    check(quiet.wakes == 0L && asked == 0) { "nothing heard" }
    // A new session drops a call heard in the old one.
    val (restarted, _) = heardInTheWindow(window)
    restarted.policy.sessionStarted(SESSION + 1)
    restarted.policy.stopped(false)
    check(restarted.wakes == 0L) { "a new session" }
}

private fun aCallBeginningAfterTheScreenOffBroadcastIsNeverItsSnooze() {
    // The screen-off broadcast can land before any foreground-loss callback. With nothing heard for a while, that off
    // snoozes nothing. A call that begins after it, which the glue's synchronous sample before the first loss callback
    // reports, began after the display went off: the arming that loss decides must not snooze it, and the stop judges
    // it for a wake as it would a call heard after an arming loss.
    val firstLosses = listOf<Pair<String, (ScreenPolicy) -> Unit>>(
        "focus" to { it.focusChanged(false, false) },
        "top-resumed" to { it.topResumedChanged(false, false) },
        "paused" to { it.paused(false) },
    )
    /** Asleep in front of a quiet session after the broadcast, with a call heard 100 ms after it: [call]'s stamp. */
    fun offThenCall(mode: Int = OFF): Pair<PolicyRig, ULong> {
        val rig = PolicyRig(mode = mode)
        rig.front()
        rig.session()
        rig.quiet()
        rig.policy.screenOff()
        check(!rig.policy.snoozed && !rig.policy.armed && !rig.policy.interactive && rig.policy.visible)
        rig.advance(200)
        val call = rig.audio(ageMs = 100)
        check(rig.wakes == 0L) { "no wake while still in front" }
        return rig to call
    }
    for ((name, firstLoss) in firstLosses) {
        val (rig, call) = offThenCall()
        firstLoss(rig.policy)
        check(rig.policy.armed && !rig.policy.snoozed) { "$name: a call that began after the off is not snoozed" }
        rig.advance(500)
        rig.policy.sample(call, 600, true) // The stop's sample: the same call, already over.
        var asked = 0
        rig.policy.stopped(false) { asked++; true }
        check(rig.wakes == 1L && asked == 1) { "$name: the stop wakes for the call, asking Android once" }
        rig.advance(1_000)
        rig.policy.sample(call, 1_600, true)
        check(rig.wakes == 1L) { "$name: and only once" }
    }
    // The stop as the first loss callback arms and wakes in one step.
    val (stopFirst, _) = offThenCall()
    var asked = 0
    stopFirst.policy.stopped(false) { asked++; true }
    check(stopFirst.policy.armed && !stopFirst.policy.snoozed) { "stop first: armed, not snoozed" }
    check(stopFirst.wakes == 1L && asked == 1) { "stop first: the stop wakes for the call" }
    // A call that goes on past a stop more than 2 s after it was heard: the stop leaves it, and its next block wakes
    // the screen, as the off never snoozed it.
    val (goesOn, _) = offThenCall()
    goesOn.policy.focusChanged(false, false)
    goesOn.advance(1_901)
    asked = 0
    goesOn.policy.stopped(false) { asked++; true }
    check(goesOn.wakes == 0L && asked == 0) { "heard more than 2 s before the stop" }
    goesOn.advance(1_000)
    goesOn.audio()
    check(goesOn.wakes == 1L && !goesOn.policy.snoozed) { "the call's next block wakes" }
    // Only Off between calls holds the call for the stop.
    val (dim, _) = offThenCall(mode = DIM)
    dim.policy.focusChanged(false, false)
    dim.policy.stopped(false)
    check(dim.policy.armed && dim.wakes == 0L) { "Dim" }
    // A first loss that does not arm (the screen back on when it lands) drops the call.
    val (screenBack, _) = offThenCall()
    screenBack.policy.focusChanged(false, true)
    asked = 0
    screenBack.policy.stopped(false) { asked++; true }
    check(!screenBack.policy.armed && screenBack.wakes == 0L && asked == 0) { "a first loss that does not arm" }
    // Audio heard before the broadcast is that off's: it snoozes, the first loss keeps the snooze, and a call after
    // the off continues it, so the stop wakes nothing and asks nothing.
    val before = PolicyRig()
    before.front()
    before.session()
    before.quiet()
    before.advance(700)
    before.audio(ageMs = 300)
    before.policy.screenOff()
    check(before.policy.snoozed && !before.policy.armed) { "audio before the off snoozes" }
    before.advance(200)
    before.audio(ageMs = 100)
    before.policy.focusChanged(false, false)
    check(before.policy.armed && before.policy.snoozed) { "the arming loss keeps the snooze the off judged" }
    asked = 0
    before.policy.stopped(false) { asked++; true }
    check(before.wakes == 0L && asked == 0) { "snoozed: the stop wakes nothing" }
}

private fun aCallBeginningAfterAnArmingLossIsNeverTheLateBroadcastsSnooze() {
    // Power in front: the arming loss callback can land before the screen-off broadcast, and it already sees the
    // screen off. That loss judges the off's snooze; a call that begins after it began after the display went off, so
    // the late broadcast for the same off neither re-judges the snooze nor adds one, and the call wakes the screen once
    // the activity is gone and the screen is off by the broadcast, whichever of the two comes last.
    /** Armed in front by a focus loss that saw the screen off, a quiet session, then a call heard 100 ms after it. */
    fun lossThenCall(): Pair<PolicyRig, ULong> {
        val rig = PolicyRig()
        rig.front()
        rig.session()
        rig.quiet()
        rig.policy.focusChanged(false, false)
        check(rig.policy.armed && !rig.policy.snoozed && rig.policy.interactive && rig.policy.visible)
        rig.advance(200)
        val call = rig.audio(ageMs = 100)
        check(rig.wakes == 0L) { "no wake before the broadcast" }
        return rig to call
    }
    // The broadcast, then the stop.
    val (rig, call) = lossThenCall()
    rig.policy.screenOff()
    check(rig.policy.armed && !rig.policy.snoozed) { "the late broadcast does not snooze a call after the arming loss" }
    check(rig.wakes == 0L) { "still visible: no wake at the broadcast" }
    rig.advance(500)
    rig.policy.sample(call, 600, true) // The stop's sample: the same call, already over.
    var asked = 0
    rig.policy.stopped(false) { asked++; true }
    check(rig.wakes == 1L && asked == 1) { "the stop wakes for the call, asking Android once" }
    rig.advance(1_000)
    rig.policy.sample(call, 1_600, true)
    check(rig.wakes == 1L) { "and only once" }
    // The stop before the broadcast: the broadcast is the last of the two, so it wakes for the call.
    val (stopFirst, _) = lossThenCall()
    asked = 0
    stopFirst.policy.stopped(false) { asked++; true }
    check(stopFirst.wakes == 0L && asked == 0) { "the screen still counts as on at the stop: no wake yet" }
    stopFirst.policy.screenOff { asked++; true }
    check(!stopFirst.policy.snoozed && stopFirst.wakes == 1L && asked == 1) { "the late broadcast wakes for the call" }
    // A call that goes on is not snoozed either: its next block wakes the screen.
    val (goesOn, _) = lossThenCall()
    goesOn.policy.paused(false)
    goesOn.policy.screenOff()
    goesOn.advance(1_901)
    goesOn.policy.stopped(false)
    check(goesOn.wakes == 0L) { "heard more than 2 s before the stop" }
    goesOn.advance(1_000)
    goesOn.audio()
    check(goesOn.wakes == 1L && !goesOn.policy.snoozed) { "the call's next block wakes" }
    // Power during audio still snoozes in either order: the first of the loss and the broadcast judges it, and the
    // second keeps that snooze.
    val orders = listOf<Pair<String, (ScreenPolicy) -> Unit>>(
        "loss first" to {
            it.focusChanged(false, false)
            it.screenOff()
        },
        "broadcast first" to {
            it.screenOff()
            it.focusChanged(false, false)
        },
    )
    for ((name, order) in orders) {
        val loud = PolicyRig()
        loud.front()
        loud.session()
        loud.quiet()
        loud.audio()
        loud.advance(1_000)
        order(loud.policy)
        check(loud.policy.armed && loud.policy.snoozed) { "$name: Power during audio snoozes" }
        loud.advance(200)
        loud.audio(ageMs = 100)
        asked = 0
        loud.policy.stopped(false) { asked++; true }
        check(loud.wakes == 0L && asked == 0) { "$name: the snooze holds through the call" }
    }
}

private fun bothCallbackOrdersArmWhenSleepingEligible() {
    val orders = listOf<Pair<String, (ScreenPolicy) -> Unit>>(
        "focus first" to {
            it.focusChanged(false, false)
            it.topResumedChanged(false, false)
            it.paused(false)
            it.stopped(false)
            it.screenOff()
        },
        "lifecycle first" to {
            it.topResumedChanged(false, false)
            it.paused(false)
            it.focusChanged(false, false)
            it.stopped(false)
            it.screenOff()
        },
        "paused first" to {
            it.paused(false)
            it.focusChanged(false, false)
            it.stopped(false)
            it.screenOff()
        },
        "broadcast first" to {
            it.screenOff()
            it.focusChanged(false, false)
            it.topResumedChanged(false, false)
            it.paused(false)
            it.stopped(false)
        },
    )
    for ((name, order) in orders) {
        val rig = PolicyRig()
        rig.front()
        rig.session()
        rig.quiet()
        order(rig.policy)
        check(rig.policy.armed && !rig.policy.visible && !rig.policy.focused && !rig.policy.top) { "$name: armed" }
        rig.audio()
        check(rig.wakes == 1L) { "$name: a call wakes" }
    }
}

private fun quickLockScreenTouchDuringTheLease() {
    val rig = PolicyRig().armedAsleep()
    rig.audio()
    rig.ownWake()
    // A tap on the lock screen 5 s in reaches only the keyguard; the call runs on for 40 s and holds the lease.
    repeat(40) {
        rig.advance(1_000)
        rig.audio()
    }
    check(rig.wakes == 1L && rig.policy.outputs.holdLease)
    val releaseAt = rig.now + DELAY_MS
    rig.advance(DELAY_MS)
    rig.policy.tick()
    check(!rig.policy.outputs.holdLease && rig.policy.leaseReleasedAt == releaseAt)
    // The keyguard's timer from that tap ran out long before, so the screen goes off at the release: on time.
    rig.advance(400)
    rig.policy.screenOff()
    check(rig.policy.armed)
    rig.advance(60_000)
    rig.audio()
    check(rig.wakes == 2L)
}

private fun powerDuringALongWokenCallKeepsTheArming() {
    val rig = PolicyRig().armedAsleep()
    rig.audio()
    rig.ownWake()
    // A 90 s call, far past wake + timeout + 5 s, with the lease held throughout.
    repeat(90) {
        rig.advance(1_000)
        rig.audio()
    }
    check(rig.policy.outputs.holdLease)
    rig.policy.screenOff() // The power button, mid-call: the lease was held up to this off.
    check(rig.policy.armed && rig.policy.snoozed && !rig.policy.outputs.holdLease)
    check(rig.policy.leaseReleasedAt == rig.now)
    rig.advance(DELAY_MS)
    rig.audio()
    check(rig.wakes == 2L) { "after a full quiet delay the next call wakes" }
}

private fun aClosedShadeLeavesTheNextSleepEligible() {
    val rig = PolicyRig()
    rig.front()
    rig.session()
    rig.quiet()
    // The notification shade takes the focus; DSD-neo stays top-resumed.
    rig.policy.focusChanged(false, true)
    check(!rig.policy.armed && rig.policy.lossSequenceOpen)
    rig.policy.focusChanged(true, true)
    check(!rig.policy.lossSequenceOpen) { "focus back while top: DSD-neo is in front again" }
    rig.quiet()
    rig.sleep()
    check(rig.policy.armed)
    rig.audio()
    check(rig.wakes == 1L)
    // Power while the shade is still open is a later loss of the shade's sequence.
    val open = PolicyRig()
    open.front()
    open.session()
    open.quiet()
    open.policy.focusChanged(false, true)
    open.sleep()
    check(!open.policy.armed)
}

private fun sameStampDelayedDeliveryIsNotActivity() {
    val rig = PolicyRig()
    rig.front()
    rig.session()
    rig.quiet()
    val stamp = rig.audio(ageMs = 200)
    val deadline = rig.policy.deadline
    check(deadline == rig.now - 200 + DELAY_MS)
    rig.advance(3_000)
    rig.policy.sample(stamp, 3_200, true)
    check(rig.policy.deadline == deadline && rig.policy.lastAudio == rig.now - 3_200)
    val asleep = PolicyRig().armedAsleep()
    val heard = asleep.audio(ageMs = 100)
    asleep.policy.wakeRefused()
    asleep.advance(DELAY_MS + 1_000)
    asleep.policy.sample(heard, DELAY_MS + 1_100, true)
    check(asleep.wakes == 1L && !asleep.policy.snoozed) { "the same stamp again is not a new call" }
}

private fun shortGenuineFinalFrameExtendsTheDeadline() {
    val rig = PolicyRig()
    rig.front()
    rig.session()
    rig.quiet()
    rig.audio()
    val first = rig.now + DELAY_MS
    check(rig.policy.deadline == first)
    rig.advance(1_000)
    rig.audio(ageMs = 400) // A short final frame heard 400 ms before this poll.
    check(rig.policy.deadline == first + 600)
    rig.advance(1_000)
    rig.audio(ageMs = 5_000) // A new stamp reporting older audio never pulls it back.
    check(rig.policy.deadline == first + 600)
}

private fun staleAudioNeverWakes() {
    val rig = PolicyRig().armedAsleep()
    rig.audio(ageMs = 2_001)
    check(rig.wakes == 0L && rig.policy.lastAudio == rig.now - 2_001)
    rig.advance(1_000)
    rig.audio(ageMs = 59_000)
    check(rig.wakes == 0L)
}

private fun ageMinusOneManufacturesNothing() {
    val rig = PolicyRig().armedAsleep()
    for (stamp in listOf(5uL, 0uL, 6uL)) { // Absent, expired, reset to 0.
        rig.advance(1_000)
        rig.policy.sample(stamp, -1, true)
        check(rig.policy.lastStamp == stamp && rig.policy.lastAudio == null && rig.wakes == 0L)
        check(rig.policy.deadline == null)
    }
    val dim = PolicyRig(mode = DIM)
    dim.front()
    dim.session()
    dim.quiet()
    dim.policy.sample(77uL, -1, true)
    check(dim.policy.outputs.dimmed && dim.policy.lastStamp == 77uL)
}

private fun dimNeverWakes() {
    val rig = PolicyRig(mode = DIM).armedAsleep()
    repeat(5) {
        rig.advance(40_000)
        rig.audio()
    }
    check(rig.wakes == 0L && !rig.policy.outputs.holdLease)
}

private fun withoutASessionNothingDimsOrHolds() {
    for (mode in listOf(DIM, OFF)) {
        val rig = PolicyRig(mode = mode)
        rig.front()
        rig.quiet()
        check(rig.policy.outputs == NOTHING && rig.policy.deadline == null) { "mode $mode, no session" }
        rig.session()
        rig.audio()
        check(rig.policy.deadline != null)
        rig.policy.sessionEnded()
        check(rig.policy.outputs == NOTHING && rig.policy.deadline == null) { "mode $mode, ended" }
        rig.quiet()
        rig.policy.userInteraction()
        check(rig.policy.outputs == NOTHING && rig.policy.deadline == null)
    }
}

private fun systemAndAlwaysOnMatchToday() {
    val system = PolicyRig(mode = SYSTEM)
    system.front()
    check(system.policy.outputs == NOTHING)
    system.session()
    system.audio()
    check(system.policy.outputs == NOTHING && system.policy.deadline == null)
    system.quiet()
    check(system.policy.outputs == NOTHING)
    system.sleep()
    system.audio()
    check(system.wakes == 0L && system.policy.outputs == NOTHING)
    // Always on: the keep-on flag while visible, with or without a session, and nothing else.
    val always = PolicyRig(mode = ALWAYS_ON)
    val keepOn = Outputs(keepScreenOn = true, dimmed = false, holdLease = false)
    check(always.policy.outputs == NOTHING) { "nothing to keep on before the activity is visible" }
    always.front()
    check(always.policy.outputs == keepOn && always.policy.deadline == null)
    always.session()
    always.quiet()
    check(always.policy.outputs == keepOn && always.policy.deadline == null)
    always.policy.stopped(true)
    check(always.policy.outputs == NOTHING)
    always.policy.started()
    check(always.policy.outputs == keepOn)
    always.sleep()
    always.audio()
    check(always.wakes == 0L)
}

private fun wakeVerdictNeededExactlyWhereAWakeCanFire() {
    // Each state is built fresh three times: to read the property, and to deliver a fresh call with the verdict allowed
    // and refused. The glue reads Android's verdict only where the property holds, so it must hold wherever a wake can
    // fire and nowhere else.
    val states: List<Pair<String, () -> PolicyRig>> = listOf(
        "System, armed asleep" to { PolicyRig(mode = SYSTEM).armedAsleep() },
        "Always on, armed asleep" to { PolicyRig(mode = ALWAYS_ON).armedAsleep() },
        "Dim, armed asleep" to { PolicyRig(mode = DIM).armedAsleep() },
        "Off, in front with the screen on" to { PolicyRig().apply { front(); session(); quiet() } },
        "Off, armed with the screen off but still visible" to {
            PolicyRig().apply {
                front()
                session()
                quiet()
                policy.topResumedChanged(false, false)
                policy.screenOff()
                check(policy.armed && policy.visible && !policy.interactive)
            }
        },
        "Off, armed after its own wake (screen on)" to { PolicyRig().armedAsleep().apply { ownWake() } },
        "Off, unarmed with the screen off" to {
            PolicyRig().apply {
                front()
                session()
                quiet()
                policy.topResumedChanged(false, true)
                policy.focusChanged(false, true)
                policy.paused(true)
                policy.stopped(true)
                policy.screenOff()
                check(!policy.armed && !policy.visible && !policy.interactive)
            }
        },
        "Off, armed asleep without a session" to { PolicyRig().armedAsleep().apply { policy.sessionEnded() } },
        "Off, armed asleep" to { PolicyRig().armedAsleep() },
    )
    for ((name, build) in states) {
        val needed = name == "Off, armed asleep"
        check(build().policy.wakeVerdictNeeded == needed) { "$name: verdict needed should be $needed" }
        val allowed = build()
        allowed.audio(wakeAllowed = true)
        check((allowed.wakes > 0L) == needed) { "$name: a fresh call allowed to wake wakes exactly where it is needed" }
        val refused = build()
        refused.audio(wakeAllowed = false)
        check(refused.wakes == 0L) { "$name: a refused verdict never wakes" }
    }
}

private fun aSnoozeStillNeedsTheVerdict() {
    // A sample can end a snooze before it judges its wake, so a snoozed policy that is otherwise ready still asks.
    val rig = PolicyRig().snoozedAsleep()
    check(rig.policy.wakeVerdictNeeded)
    rig.advance(1_000)
    rig.audio()
    check(rig.wakes == 0L && rig.policy.snoozed) { "audio inside the delay continues the snooze" }
    rig.advance(DELAY_MS)
    check(rig.policy.snoozed && rig.policy.wakeVerdictNeeded)
    rig.audio()
    check(rig.wakes == 1L && !rig.policy.snoozed) { "audio a full delay later ends the snooze and wakes in one sample" }
}

private fun theVerdictIsAskedOnlyForASampleThatWouldWake() {
    // Android's verdict costs the glue two binder calls on the main thread, so the policy asks for it last: only once
    // everything else says this sample wakes.
    var asked = 0
    val allowed = { asked++; true }
    val rig = PolicyRig().armedAsleep()
    check(rig.policy.wakeVerdictNeeded)
    rig.advance(1_000)
    rig.policy.sample(null, -1, allowed) // No readable record.
    check(asked == 0) { "a sample with no record asks nothing" }
    rig.policy.sample(rig.policy.lastStamp, 0, allowed)
    check(asked == 0) { "an unchanged stamp asks nothing" }
    rig.policy.sample(9_000uL, -1, allowed)
    check(asked == 0) { "a changed stamp without an age asks nothing" }
    rig.policy.sample(9_001uL, ScreenPolicy.WAKE_MAX_AGE_MS + 1, allowed)
    check(asked == 0 && rig.wakes == 0L) { "audio too old to wake asks nothing" }
    rig.policy.sample(9_002uL, ScreenPolicy.WAKE_MAX_AGE_MS, allowed)
    check(asked == 1 && rig.wakes == 1L) { "a waking sample asks exactly once" }
    // Snoozed by audio inside the delay: the sample cannot end the snooze, so it asks nothing.
    val snoozed = PolicyRig().snoozedAsleep()
    snoozed.advance(1_000)
    asked = 0
    snoozed.policy.sample(9_003uL, 0, allowed)
    check(asked == 0 && snoozed.policy.snoozed && snoozed.wakes == 0L) { "a snoozed sample asks nothing" }
    // A session's first readable sample only primes.
    val primed = PolicyRig().armedAsleep()
    primed.policy.sessionStarted(SESSION + 1)
    asked = 0
    primed.policy.sample(9_004uL, 0, allowed)
    check(asked == 0 && primed.wakes == 0L) { "priming asks nothing" }
    // Nowhere a wake cannot fire: in front, armed but still visible, Dim, and after an unlock.
    val notNeeded: List<Pair<String, PolicyRig>> = listOf(
        "in front" to PolicyRig().apply { front(); session(); quiet() },
        "armed, still visible" to PolicyRig().apply {
            front()
            session()
            quiet()
            policy.focusChanged(false, false)
            policy.screenOff()
        },
        "the screen off before any loss callback" to PolicyRig().apply {
            front()
            session()
            quiet()
            policy.screenOff()
        },
        "Dim" to PolicyRig(mode = DIM).armedAsleep(),
        "unlocked" to PolicyRig().armedAsleep().apply { policy.userPresent() },
    )
    for ((name, state) in notNeeded) {
        asked = 0
        state.advance(1_000)
        state.policy.sample(9_005uL, 0, allowed)
        check(asked == 0 && state.wakes == 0L) { "$name: asks nothing" }
    }
    // Refused: asked once, no wake, and the audio still counts.
    val refused = PolicyRig().armedAsleep()
    asked = 0
    refused.policy.sample(9_006uL, 0) { asked++; false }
    check(asked == 1 && refused.wakes == 0L && refused.policy.lastAudio == refused.now)
}

private fun wakeAllowedOnlyInNormalAudioModeWithoutDoNotDisturb() {
    // AudioManager modes and NotificationManager interruption filters, as Android numbers them.
    check(ScreenPolicy.AUDIO_MODE_NORMAL == 0)
    check(ScreenPolicy.AUDIO_MODE_RINGTONE == 1)
    check(ScreenPolicy.AUDIO_MODE_IN_CALL == 2)
    check(ScreenPolicy.AUDIO_MODE_IN_COMMUNICATION == 3)
    check(ScreenPolicy.AUDIO_MODE_CALL_SCREENING == 4)
    check(ScreenPolicy.AUDIO_MODE_CALL_REDIRECT == 5)
    check(ScreenPolicy.AUDIO_MODE_COMMUNICATION_REDIRECT == 6)
    check(ScreenPolicy.INTERRUPTION_FILTER_UNKNOWN == 0)
    check(ScreenPolicy.INTERRUPTION_FILTER_ALL == 1)
    check(ScreenPolicy.INTERRUPTION_FILTER_PRIORITY == 2)
    check(ScreenPolicy.INTERRUPTION_FILTER_NONE == 3)
    check(ScreenPolicy.INTERRUPTION_FILTER_ALARMS == 4)
    // Every mode from MODE_INVALID (-2) and MODE_CURRENT (-1) past the newest, against every filter and beyond.
    for (mode in -2..7) {
        for (filter in -1..5) {
            val expected = mode == 0 && (filter == 0 || filter == 1)
            check(ScreenPolicy.wakeAllowed(mode, filter) == expected) { "audio mode $mode, filter $filter" }
        }
    }
    check(!ScreenPolicy.wakeAllowed(Int.MIN_VALUE, 1) && !ScreenPolicy.wakeAllowed(0, Int.MAX_VALUE))
}

private fun delaySanitising() {
    check(ScreenPolicy.DEFAULT_DELAY_SECONDS == 30)
    for (seconds in listOf(10, 30, 60, 120, 300)) {
        check(ScreenPolicy.sanitizeDelaySeconds(seconds) == seconds) { "$seconds s is a choice" }
    }
    val off = listOf(Int.MIN_VALUE, -30, -1, 0, 1, 9, 11, 29, 31, 59, 61, 90, 119, 121, 299, 301, 600, Int.MAX_VALUE)
    for (seconds in off) {
        check(ScreenPolicy.sanitizeDelaySeconds(seconds) == 30) { "$seconds s reads as 30 s" }
    }
    val rig = PolicyRig(mode = OFF, delaySeconds = 45)
    check(rig.policy.delaySeconds == 30 && rig.policy.mode == Mode.OFF_BETWEEN_CALLS)
    rig.policy.configure(OFF, 300)
    check(rig.policy.delaySeconds == 300)
}

private fun modeCodes() {
    check(Mode.SYSTEM.code == 0)
    check(Mode.ALWAYS_ON.code == 1)
    check(Mode.DIM_BETWEEN_CALLS.code == 2)
    check(Mode.OFF_BETWEEN_CALLS.code == 3)
    check(Mode.entries.size == 4)
    for (mode in Mode.entries) {
        check(Mode.fromCode(mode.code) == mode)
    }
    for (code in listOf(Int.MIN_VALUE, -1, 4, 5, 99, Int.MAX_VALUE)) {
        check(Mode.fromCode(code) == Mode.SYSTEM) { "unknown code $code reads as System" }
    }
}

fun main() {
    row01ConfigureUnchangedIsNoChange()
    row02ConfigureChangedSetsAndEngages()
    row03DuplicateSessionStartIsNoChange()
    row04NewSessionStartsAndPrimesOnItsFirstSample()
    row05SessionEndRestoresEverythingButArming()
    row06NewActivityRecordsAudioAndWakes()
    row06aChangedStampWithoutAgeOnlyMovesTheStamp()
    row07SameStampIsNoChange()
    row08RegainingTheForegroundDisarmsAndUnsnoozes()
    row09FirstLossWhileEligibleWithTheScreenOffArms()
    row10FirstLossOtherwiseDisarms()
    row11LaterLossesOnlyKeepBooks()
    row12OwnScreenOnRecordsTheWake()
    row13UserScreenOnDisarmsAtOnce()
    row14UnlockDisarmsAndReleasesAtOnce()
    row15ScreenOffJudgesTheOwnWakeWindow()
    row16FocusGainCountsAsATouch()
    row17MultiWindowNeverDims()
    row18InteractionCountsOnlyWhileVisible()
    rows19To21TouchLatchesTheGesture()
    row22RefusedWakeSnoozes()
    row23TickAtTheDeadlineDimsAndReleases()
    row24DestroyedForgetsTheWindow()
    housekeepingClearsTheSnoozeAfterAFullQuietDelay()
    aSnoozeEndsOnlyWhenTheNewAudioFollowsAFullQuietDelay()
    aSampleWithoutARecordChangesNothing()
    aPowerPressBeforeTheFirstReadableSampleSnoozesTheCall()
    splitScreenFocusedVersusUnfocused()
    unfocusedSplitScreenSleepDoesNotArm()
    homeWithTheScreenOnThenOff()
    userScreenOnDisarmsAtOnce()
    unlockDisarmsAtOnceAndReleasesWhileStillInteractive()
    ownWakeOffOnTimeStaysArmed()
    lateOffAfterALockScreenTouchDisarms()
    unlockUseAndSleepInFrontAfterAnOwnWakeArmsAfresh()
    withoutALockScreenAWakeShowsDsdNeoAndTheNextSleepArms()
    duplicateSessionStartDuringASnoozeIsANoOp()
    powerDuringAudioSnoozesButNotDuringATouchOrAQuietStart()
    audioBetweenPollsSnoozesThroughTheSyncSample()
    aCallHeardWhileTheScreenGoesOffInFrontWakesAtTheStop()
    aCallBeginningAfterTheScreenOffBroadcastIsNeverItsSnooze()
    aCallBeginningAfterAnArmingLossIsNeverTheLateBroadcastsSnooze()
    bothCallbackOrdersArmWhenSleepingEligible()
    quickLockScreenTouchDuringTheLease()
    powerDuringALongWokenCallKeepsTheArming()
    aClosedShadeLeavesTheNextSleepEligible()
    sameStampDelayedDeliveryIsNotActivity()
    shortGenuineFinalFrameExtendsTheDeadline()
    staleAudioNeverWakes()
    ageMinusOneManufacturesNothing()
    dimNeverWakes()
    withoutASessionNothingDimsOrHolds()
    systemAndAlwaysOnMatchToday()
    wakeVerdictNeededExactlyWhereAWakeCanFire()
    aSnoozeStillNeedsTheVerdict()
    theVerdictIsAskedOnlyForASampleThatWouldWake()
    wakeAllowedOnlyInNormalAudioModeWithoutDoNotDisturb()
    delaySanitising()
    modeCodes()
    println("PASS: screen policy transition table, arming, snooze, own-wake window, stamps and ages, modes and D5")
}
