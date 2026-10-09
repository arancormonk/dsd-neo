// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Runs a [ScreenPolicy] and carries its outputs out through [Effects], so the Android glue stays one-line delegations.
 * Main thread only, like the policy.
 *
 * After every event it applies what changed and nothing else, make-before-break in both directions: a keep-on flag
 * being set goes first, then a lease being taken, then a keep-on flag being cleared, the dimming, and last a lease
 * being released, so a switch between the flag and the lease never leaves the screen with neither. A held lease is
 * renewed on every [sample], since each renewal only lasts a few seconds. When the policy asks for a wake the
 * controller pulses once and, if the screen stayed off, tells the policy so it stops trying until the calls go quiet.
 * It decides which screen-on is DSD-neo's own wake: the first within [OWN_WAKE_SETTLE_MS] of a pulse Android accepted.
 * It keeps one [Effects.schedule] request in step with the policy's deadline.
 */
class ScreenController(private val clock: () -> Long, private val effects: Effects) : StatusFeed.Screen {
    /** What the controller asks of the platform. Each is called only when its value changes, except [renewLease]. */
    interface Effects {
        fun setKeepScreenOn(on: Boolean)

        /** Minimum brightness while [dimmed]; the system's own brightness otherwise. */
        fun setDimmed(dimmed: Boolean)

        /** Takes or extends DSD-neo's screen lease. */
        fun renewLease()

        fun releaseLease()

        /** Turns the screen on; returns whether it is interactive right after. */
        fun pulseWake(): Boolean

        /** Calls [tick] at [atMs] on the policy's clock, replacing any earlier request; null cancels. */
        fun schedule(atMs: Long?)
    }

    private val policy = ScreenPolicy(clock)

    /** The policy's state, read-only; every change goes through this controller, so the effects never drift. */
    val state: ScreenPolicyView = object : ScreenPolicyView by policy {}

    private var applied = policy.outputs
    private var scheduled: Long? = null
    private var pulsedSerial = policy.wakeSerial

    /** When the last pulse Android accepted went out, until the screen-on it causes; null with none. */
    private var pulsedAt: Long? = null

    fun configure(modeCode: Int, delaySeconds: Int) = after { policy.configure(modeCode, delaySeconds) }

    override fun sessionStarted(id: Long) = after { policy.sessionStarted(id) }

    override fun sessionEnded() = after { policy.sessionEnded() }

    override fun sample(stamp: ULong?, ageMs: Long, wakeAllowed: () -> Boolean) {
        policy.sample(stamp, ageMs, wakeAllowed)
        apply(renew = true)
    }

    fun focusChanged(focused: Boolean, interactive: Boolean) = after { policy.focusChanged(focused, interactive) }

    fun topResumedChanged(top: Boolean, interactive: Boolean) = after { policy.topResumedChanged(top, interactive) }

    fun paused(interactive: Boolean) = after { policy.paused(interactive) }

    fun stopped(interactive: Boolean, wakeAllowed: () -> Boolean) = after { policy.stopped(interactive, wakeAllowed) }

    fun started() = after { policy.started() }

    fun destroyed() = after { policy.destroyed() }

    fun multiWindowChanged(multiWindow: Boolean) = after { policy.multiWindowChanged(multiWindow) }

    fun userInteraction() = after { policy.userInteraction() }

    /** Whether the activity should swallow this touch event: `if (controller.touch(...)) return true`. */
    fun touch(actionMasked: Int, exploring: Boolean): Boolean {
        val swallow = policy.touch(actionMasked, exploring)
        apply()
        return swallow
    }

    /**
     * The screen came on, with the phone's [screenOffTimeoutMs]. It is DSD-neo's own wake only within
     * [OWN_WAKE_SETTLE_MS] of a pulse Android accepted, and only the first screen-on after it; any other screen-on,
     * one right after a refused pulse included, is someone else's and disarms.
     */
    fun screenOn(screenOffTimeoutMs: Long) = after {
        val pulsed = pulsedAt
        pulsedAt = null
        val ours = pulsed != null && clock() - pulsed < OWN_WAKE_SETTLE_MS
        policy.screenOn(ours, screenOffTimeoutMs)
    }

    /** The screen went off. The policy may wake here, for a call held until this broadcast, so it takes the verdict. */
    fun screenOff(wakeAllowed: () -> Boolean) = after { policy.screenOff(wakeAllowed) }

    fun userPresent() = after { policy.userPresent() }

    /** The scheduled moment came. */
    fun tick() {
        // The request that brought us here is spent, so whatever deadline remains is asked for again.
        scheduled = null
        policy.tick()
        apply()
    }

    private inline fun after(event: () -> Unit) {
        event()
        apply()
    }

    private fun apply(renew: Boolean = false) {
        val next = policy.outputs
        // Whatever keeps the screen on is put in place before whatever else kept it on goes away.
        if (next.keepScreenOn && !applied.keepScreenOn) {
            effects.setKeepScreenOn(true)
        }
        if (next.holdLease && (renew || !applied.holdLease)) {
            effects.renewLease()
        }
        if (!next.keepScreenOn && applied.keepScreenOn) {
            effects.setKeepScreenOn(false)
        }
        if (next.dimmed != applied.dimmed) {
            effects.setDimmed(next.dimmed)
        }
        if (!next.holdLease && applied.holdLease) {
            effects.releaseLease()
        }
        applied = next
        if (policy.wakeSerial != pulsedSerial) {
            pulsedSerial = policy.wakeSerial
            pulsedAt = clock()
            if (!effects.pulseWake()) {
                // The screen stayed off, so no screen-on to come is this pulse's: the next one disarms (D7). The
                // refusal changes the policy's state, so its outputs and deadline are applied from there.
                pulsedAt = null
                policy.wakeRefused()
                apply()
                return
            }
        }
        val deadline = policy.deadline
        if (deadline != scheduled) {
            scheduled = deadline
            effects.schedule(deadline)
        }
    }

    companion object {
        /** How long after a pulse a screen-on still counts as DSD-neo's own wake. */
        const val OWN_WAKE_SETTLE_MS = 2_000L
    }
}
