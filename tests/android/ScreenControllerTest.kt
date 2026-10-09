// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

private const val ALWAYS_ON = 1
private const val DIM = 2
private const val OFF = 3
private const val SYSTEM = 0
private const val DELAY_MS = 30_000L
private const val SESSION = 7L

/** A sample with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenController.sample(stamp: ULong?, ageMs: Long, wakeAllowed: Boolean) =
    sample(stamp, ageMs) { wakeAllowed }

/** A stop with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenController.stopped(interactive: Boolean) = stopped(interactive) { true }

/** A screen-off broadcast with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenController.screenOff() = screenOff { true }

/**
 * Records every effect in order; pulseWake() answers as Android would: the screen found already on, woken by the
 * wake lock, or left off.
 */
private class RecordingEffects : ScreenController.Effects {
    private val calls = mutableListOf<String>()
    var interactiveAfterPulse = true

    /** The screen is on before the pulse: someone else turned it on, or an own pulse did, its broadcast to come. */
    var screenAlreadyOn = false

    override fun setKeepScreenOn(on: Boolean) {
        calls += "keepOn=$on"
    }

    override fun setDimmed(dimmed: Boolean) {
        calls += "dimmed=$dimmed"
    }

    override fun renewLease() {
        calls += "renew"
    }

    override fun releaseLease() {
        calls += "release"
    }

    override fun pulseWake(): ScreenWake {
        calls += "pulse"
        return when {
            screenAlreadyOn -> ScreenWake.ALREADY_ON
            interactiveAfterPulse -> ScreenWake.WOKE
            else -> ScreenWake.REFUSED
        }
    }

    override fun schedule(atMs: Long?) {
        calls += "schedule=$atMs"
    }

    /** The calls since the last take(). */
    fun take(): List<String> = calls.toList().also { calls.clear() }
}

private class ControllerRig(mode: Int = OFF) {
    var now = 2_000_000L
    val effects = RecordingEffects()
    val controller = ScreenController({ now }, effects)
    private var nextStamp = 1_000uL

    init {
        controller.configure(mode, 30)
    }

    val policy: ScreenPolicyView get() = controller.state

    fun front() {
        controller.started()
        controller.topResumedChanged(true, true)
        controller.focusChanged(true, true)
    }

    fun session() {
        controller.sessionStarted(SESSION)
        controller.sample(0uL, -1, true)
    }

    fun audio(ageMs: Long = 0): ULong {
        nextStamp++
        controller.sample(nextStamp, ageMs, true)
        return nextStamp
    }

    fun quiet() {
        now += DELAY_MS
        controller.tick()
    }

    fun sleep() {
        controller.topResumedChanged(false, false)
        controller.focusChanged(false, false)
        controller.paused(false)
        controller.stopped(false)
        controller.screenOff()
    }

    fun armedAsleep(): ControllerRig {
        front()
        session()
        quiet()
        sleep()
        check(policy.armed && !policy.interactive)
        effects.take()
        return this
    }
}

private fun effectsApplyOnlyOnChange() {
    val rig = ControllerRig(mode = DIM)
    check(rig.effects.take().isEmpty()) { "nothing to apply before a window or a session" }
    rig.front()
    check(rig.effects.take().isEmpty()) { "Dim without a session behaves like System" }
    rig.controller.sessionStarted(SESSION)
    check(rig.effects.take() == listOf("keepOn=true", "schedule=${rig.now + DELAY_MS}"))
    // Events that change no output and no deadline apply nothing.
    rig.controller.sessionStarted(SESSION)
    rig.controller.multiWindowChanged(false)
    rig.controller.configure(DIM, 30)
    rig.controller.sample(0uL, -1, true)
    rig.controller.sample(0uL, -1, true)
    check(rig.effects.take().isEmpty())
    rig.now += DELAY_MS
    rig.controller.tick()
    check(rig.effects.take() == listOf("dimmed=true")) { "the tick that dims has nothing left to schedule" }
    rig.controller.tick()
    rig.controller.sample(0uL, -1, true)
    check(rig.effects.take().isEmpty())
    rig.controller.userInteraction()
    check(rig.effects.take() == listOf("dimmed=false", "schedule=${rig.now + DELAY_MS}"))
    rig.controller.userInteraction()
    check(rig.effects.take().isEmpty()) { "a second touch at the same instant changes nothing" }
}

private fun keepOnIsSetBeforeTheLeaseIsReleased() {
    for (mode in listOf(DIM, ALWAYS_ON)) {
        val rig = ControllerRig(mode = OFF)
        rig.front()
        rig.session()
        rig.audio()
        check(rig.policy.outputs.holdLease)
        rig.effects.take()
        rig.controller.configure(mode, 30)
        val calls = rig.effects.take()
        val keepOn = calls.indexOf("keepOn=true")
        val release = calls.indexOf("release")
        check(keepOn >= 0 && release > keepOn) { "mode $mode: make before break, got $calls" }
    }
}

private fun theLeaseIsTakenBeforeKeepOnIsCleared() {
    for (mode in listOf(DIM, ALWAYS_ON)) {
        val rig = ControllerRig(mode = mode)
        rig.front()
        rig.session()
        check(rig.policy.outputs.keepScreenOn)
        rig.effects.take()
        rig.controller.configure(OFF, 30)
        val calls = rig.effects.take()
        val renew = calls.indexOf("renew")
        val keepOff = calls.indexOf("keepOn=false")
        check(renew >= 0 && keepOff > renew) { "mode $mode to Off: make before break, got $calls" }
    }
}

private fun theStateIsReadOnly() {
    val rig = ControllerRig(mode = OFF)
    check(rig.controller.state !is ScreenPolicy) { "the glue must not reach the policy past the effects" }
    rig.front()
    rig.session()
    check(rig.controller.state.outputs.holdLease && rig.controller.state.session == SESSION)
}

private fun aHeldLeaseIsRenewedOnEverySample() {
    val rig = ControllerRig(mode = OFF)
    rig.front()
    check(rig.effects.take().isEmpty())
    rig.controller.sessionStarted(SESSION)
    check(rig.effects.take() == listOf("renew", "schedule=${rig.now + DELAY_MS}")) { "taking the lease is a renewal" }
    rig.controller.sample(0uL, -1, true)
    check(rig.effects.take() == listOf("renew"))
    // Age-only polls change nothing in the policy but still renew the 10 s lease.
    repeat(5) {
        rig.now += 1_000
        rig.controller.sample(0uL, -1, true)
    }
    check(rig.effects.take() == List(5) { "renew" })
    // Other events never renew a lease already held.
    rig.controller.multiWindowChanged(true)
    rig.controller.multiWindowChanged(false)
    check(rig.effects.take().isEmpty())
    // Without a lease, samples renew nothing.
    val system = ControllerRig(mode = SYSTEM)
    system.front()
    system.session()
    repeat(3) { system.audio() }
    check(system.effects.take().isEmpty())
}

private fun theLeaseIsReleasedAtOnce() {
    // At the deadline: dim first, then release.
    val deadline = ControllerRig(mode = OFF)
    deadline.front()
    deadline.session()
    deadline.quiet()
    deadline.audio()
    deadline.effects.take()
    deadline.now += DELAY_MS
    deadline.controller.tick()
    check(deadline.effects.take() == listOf("dimmed=true", "release"))
    // On a mode change.
    val mode = ControllerRig(mode = OFF)
    mode.front()
    mode.session()
    mode.audio()
    mode.effects.take()
    mode.controller.configure(SYSTEM, 30)
    check(mode.effects.take() == listOf("release", "schedule=null"))
    // On the session's end.
    val ended = ControllerRig(mode = OFF)
    ended.front()
    ended.session()
    ended.audio()
    ended.effects.take()
    ended.controller.sessionEnded()
    check(ended.effects.take() == listOf("release", "schedule=null"))
    // On a disarm: an unlock after an own wake, and the activity destroyed under one.
    for (disarm in listOf<(ScreenController) -> Unit>({ it.userPresent() }, { it.destroyed() })) {
        val woken = ControllerRig(mode = OFF).armedAsleep()
        woken.audio()
        woken.controller.screenOn(30_000)
        check(woken.effects.take() == listOf("pulse", "renew", "schedule=${woken.now + DELAY_MS}"))
        woken.now += 1_000
        disarm(woken.controller)
        check(woken.effects.take() == listOf("release", "schedule=null"))
    }
}

private fun aRefusedWakeIsFedBackAndNotRetried() {
    val rig = ControllerRig(mode = OFF).armedAsleep()
    rig.effects.interactiveAfterPulse = false
    rig.audio()
    check(rig.effects.take() == listOf("pulse")) { "one pulse, no lease over a screen that stayed off" }
    check(rig.policy.snoozed && !rig.policy.interactive && rig.policy.armed)
    rig.now += 5_000
    rig.audio()
    check(rig.effects.take().isEmpty()) { "refused: no retry within the same run of calls" }
    rig.now += DELAY_MS
    rig.audio()
    check(rig.effects.take() == listOf("pulse")) { "after a full quiet delay a new call tries again" }
}

private fun aScreenOnIsOursOnlyJustAfterAnAcceptedPulse() {
    val settle = ScreenController.OWN_WAKE_SETTLE_MS
    // Refused: the screen stayed off, so a screen-on in the next moments is the user's, another app's or a
    // notification's, and disarms at once.
    val refused = ControllerRig(mode = OFF).armedAsleep()
    refused.effects.interactiveAfterPulse = false
    refused.audio()
    check(refused.effects.take() == listOf("pulse") && refused.policy.armed)
    refused.now += 500
    refused.controller.screenOn(30_000)
    check(!refused.policy.armed && refused.policy.wakeAt == null) { "a screen-on after a refused pulse disarms" }
    check(!refused.policy.outputs.holdLease && "renew" !in refused.effects.take()) { "and takes no lease" }
    // Accepted: a screen-on within the settle window is the pulse's own and keeps the arming, but only the first.
    val accepted = ControllerRig(mode = OFF).armedAsleep()
    accepted.audio()
    check(accepted.effects.take() == listOf("pulse"))
    accepted.now += settle - 1
    accepted.controller.screenOn(30_000)
    check(accepted.policy.armed && accepted.policy.wakeAt == accepted.now && accepted.policy.outputs.holdLease)
    accepted.controller.screenOn(30_000)
    check(!accepted.policy.armed) { "only the first screen-on after a pulse is its own" }
    // Past the window, or with no pulse at all, a screen-on is someone else's.
    val late = ControllerRig(mode = OFF).armedAsleep()
    late.audio()
    late.now += settle
    late.controller.screenOn(30_000)
    check(!late.policy.armed) { "a screen-on 2 s after the pulse" }
    val none = ControllerRig(mode = OFF).armedAsleep()
    none.controller.screenOn(30_000)
    check(!none.policy.armed) { "a screen-on with no pulse" }
}

private fun aScreenFoundOnBeforeItsBroadcastIsNotOurWake() {
    // The user, another app or a notification turns the screen on while DSD-neo is armed, and a status tick with a new
    // call runs before that screen-on broadcast does: the policy still counts the screen off and asks for a wake, but
    // the screen is already on, so nothing is pulsed and nothing is claimed. The broadcast then disarms (D7).
    val settle = ScreenController.OWN_WAKE_SETTLE_MS
    val rig = ControllerRig(mode = OFF).armedAsleep()
    rig.effects.screenAlreadyOn = true
    rig.audio()
    check(rig.effects.take() == listOf("pulse")) { "the wake is asked for and nothing else follows" }
    check(rig.policy.armed && !rig.policy.snoozed && !rig.policy.interactive) { "a screen found on is no refusal" }
    rig.now += 200
    rig.controller.screenOn(30_000)
    check(!rig.policy.armed && rig.policy.wakeAt == null) { "the screen-on that follows is someone else's and disarms" }
    check(!rig.policy.outputs.holdLease && "renew" !in rig.effects.take()) { "and takes no lease" }
    rig.now += 1_000
    rig.audio()
    check("pulse" !in rig.effects.take()) { "and no later call asks for a wake" }
    // An own pulse whose screen-on is still to come keeps its claim when the next call finds that screen on, and the
    // claim still dates from that pulse: the second ask adds none.
    val own = ControllerRig(mode = OFF).armedAsleep()
    own.audio()
    check(own.effects.take() == listOf("pulse"))
    val pulsedAt = own.now
    own.effects.screenAlreadyOn = true
    own.now += 1_000
    own.audio()
    check(own.effects.take() == listOf("pulse") && own.policy.armed)
    own.now = pulsedAt + settle - 1
    own.controller.screenOn(30_000)
    check(own.policy.armed && own.policy.wakeAt == own.now && own.policy.outputs.holdLease) {
        "the pulse's own screen-on keeps the arming"
    }
    val stale = ControllerRig(mode = OFF).armedAsleep()
    stale.audio()
    val staleAt = stale.now
    stale.effects.screenAlreadyOn = true
    stale.now += 1_000
    stale.audio()
    stale.now = staleAt + settle
    stale.controller.screenOn(30_000)
    check(!stale.policy.armed) { "a screen found on renews no claim: 2 s after the pulse the screen-on is foreign" }
}

private fun anAcceptedWakeHoldsTheLeaseOnceTheScreenIsOn() {
    val rig = ControllerRig(mode = OFF).armedAsleep()
    rig.audio()
    check(rig.effects.take() == listOf("pulse") && !rig.policy.snoozed)
    rig.now += 200
    rig.controller.screenOn(30_000)
    check(rig.effects.take() == listOf("renew", "schedule=${rig.now - 200 + DELAY_MS}"))
    rig.now += 800
    rig.audio()
    check(rig.effects.take() == listOf("renew", "schedule=${rig.now + DELAY_MS}")) { "no second pulse" }
}

private fun aTickAlwaysReschedulesWhatIsLeft() {
    val rig = ControllerRig(mode = OFF)
    rig.front()
    rig.session()
    rig.audio()
    val deadline = rig.now + DELAY_MS
    rig.effects.take()
    // A tick that runs early (or twice) consumed the pending one: the same deadline is asked for again.
    rig.now += 1_000
    rig.controller.tick()
    check(rig.effects.take() == listOf("schedule=$deadline"))
    rig.now = deadline
    rig.controller.tick()
    check(rig.effects.take() == listOf("dimmed=true", "release"))
}

private fun theVerdictReachesThePolicyUnasked() {
    // The controller hands the glue's verdict to the policy as it is, so Android is asked only for a waking sample.
    var asked = 0
    val allowed = { asked++; true }
    val rig = ControllerRig(mode = OFF).armedAsleep()
    rig.now += 1_000
    rig.controller.sample(rig.policy.lastStamp, 0, allowed)
    rig.controller.sample(null, -1, allowed)
    check(asked == 0 && rig.effects.take().isEmpty())
    rig.controller.sample(9_000uL, 0, allowed)
    check(asked == 1 && rig.effects.take() == listOf("pulse"))
}

private fun theStopPulsesForACallHeardWhileStillVisible() {
    val rig = ControllerRig(mode = OFF)
    rig.front()
    rig.session()
    rig.quiet()
    rig.controller.focusChanged(false, false)
    rig.controller.screenOff()
    rig.effects.take()
    rig.now += 200
    rig.audio(ageMs = 300)
    check("pulse" !in rig.effects.take()) { "no pulse while still visible" }
    rig.now += 500
    rig.controller.paused(false)
    rig.controller.stopped(false)
    check(rig.effects.take().contains("pulse")) { "the stop pulses for it" }
}

private fun theLateBroadcastPulsesForACallHeardAfterTheArmingLoss() {
    // Every loss callback, the stop included, lands before the screen-off broadcast; a call heard in between began
    // after the arming loss saw the screen off, so the broadcast pulses for it, asking Android then.
    val rig = ControllerRig(mode = OFF)
    rig.front()
    rig.session()
    rig.quiet()
    rig.controller.topResumedChanged(false, false)
    rig.controller.focusChanged(false, false)
    rig.controller.paused(false)
    rig.controller.stopped(false)
    rig.effects.take()
    rig.now += 200
    rig.audio(ageMs = 100)
    check("pulse" !in rig.effects.take()) { "no pulse before the broadcast" }
    var asked = 0
    rig.controller.screenOff { asked++; true }
    check(rig.effects.take().contains("pulse") && asked == 1) { "the late broadcast pulses for it" }
    check(!rig.controller.state.snoozed) { "and does not snooze it" }
}

private fun touchDelegatesInOneCall() {
    val rig = ControllerRig(mode = DIM)
    rig.front()
    rig.session()
    rig.quiet()
    rig.effects.take()
    check(rig.controller.touch(ScreenPolicy.ACTION_DOWN, false))
    check(rig.effects.take() == listOf("dimmed=false", "schedule=${rig.now + DELAY_MS}"))
    check(rig.controller.touch(ScreenPolicy.ACTION_UP, false))
    check(rig.effects.take().isEmpty())
}

fun main() {
    effectsApplyOnlyOnChange()
    keepOnIsSetBeforeTheLeaseIsReleased()
    theLeaseIsTakenBeforeKeepOnIsCleared()
    theStateIsReadOnly()
    aHeldLeaseIsRenewedOnEverySample()
    theLeaseIsReleasedAtOnce()
    aRefusedWakeIsFedBackAndNotRetried()
    aScreenOnIsOursOnlyJustAfterAnAcceptedPulse()
    aScreenFoundOnBeforeItsBroadcastIsNotOurWake()
    anAcceptedWakeHoldsTheLeaseOnceTheScreenIsOn()
    aTickAlwaysReschedulesWhatIsLeft()
    theVerdictReachesThePolicyUnasked()
    theStopPulsesForACallHeardWhileStillVisible()
    theLateBroadcastPulsesForACallHeardAfterTheArmingLoss()
    touchDelegatesInOneCall()
    println("PASS: screen controller idempotence, make before break, renewal, immediate release and wake refusal")
}
