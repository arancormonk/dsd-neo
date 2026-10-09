// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Runs a [ScreenPolicy] and carries its outputs out through [Effects], so the Android glue stays one-line delegations.
 * Main thread only, like the policy.
 *
 * After every event it applies what changed and nothing else, in a make-before-break order: the keep-on flag and the
 * dimming first, then the lease taken or released, so the screen is never left with neither. A held lease is renewed
 * on every [sample], since each renewal only lasts a few seconds. When the policy asks for a wake the controller
 * pulses once and, if the screen stayed off, tells the policy so it stops trying until the calls go quiet. It keeps
 * one [Effects.schedule] request in step with the policy's deadline.
 */
class ScreenController(clock: () -> Long, private val effects: Effects) : StatusFeed.Screen {
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

    val policy = ScreenPolicy(clock)

    private var applied = policy.outputs
    private var scheduled: Long? = null
    private var pulsedSerial = policy.wakeSerial

    fun configure(modeCode: Int, delaySeconds: Int) = after { policy.configure(modeCode, delaySeconds) }

    override fun sessionStarted(id: Long) = after { policy.sessionStarted(id) }

    override fun sessionEnded() = after { policy.sessionEnded() }

    override fun sample(stamp: ULong, ageMs: Long, wakeAllowed: Boolean) {
        policy.sample(stamp, ageMs, wakeAllowed)
        apply(renew = true)
    }

    fun focusChanged(focused: Boolean, interactive: Boolean) = after { policy.focusChanged(focused, interactive) }

    fun topResumedChanged(top: Boolean, interactive: Boolean) = after { policy.topResumedChanged(top, interactive) }

    fun paused(interactive: Boolean) = after { policy.paused(interactive) }

    fun stopped(interactive: Boolean) = after { policy.stopped(interactive) }

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

    fun screenOn(ours: Boolean, screenOffTimeoutMs: Long) = after { policy.screenOn(ours, screenOffTimeoutMs) }

    fun screenOff() = after { policy.screenOff() }

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
        if (next.keepScreenOn != applied.keepScreenOn) {
            effects.setKeepScreenOn(next.keepScreenOn)
        }
        if (next.dimmed != applied.dimmed) {
            effects.setDimmed(next.dimmed)
        }
        if (next.holdLease && (renew || !applied.holdLease)) {
            effects.renewLease()
        }
        if (!next.holdLease && applied.holdLease) {
            effects.releaseLease()
        }
        applied = next
        if (policy.wakeSerial != pulsedSerial) {
            pulsedSerial = policy.wakeSerial
            if (!effects.pulseWake()) {
                // The refusal changes the policy's state, so its outputs and deadline are applied from there.
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
}
