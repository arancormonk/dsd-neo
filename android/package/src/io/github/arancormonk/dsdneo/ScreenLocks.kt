// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.os.PowerManager

/**
 * DSD-neo's own screen wake locks: the lease that holds the screen on through a call in Off between calls, and the
 * one-shot pulse that turns it back on for a new call.
 *
 * Android gives an app no way to turn the screen off, and clearing FLAG_KEEP_SCREEN_ON counts as user activity (the
 * window manager's lock carries ON_AFTER_RELEASE), so the lease is a lock of its own without that flag: releasing it
 * lets the screen go off on the phone's timer. Both locks are created once and are not reference-counted, so one
 * release undoes any number of renewals and a repeated pulse never piles up holds.
 */
class ScreenLocks(private val powerManager: PowerManager) {
    private val lease: PowerManager.WakeLock
    private val wake: PowerManager.WakeLock

    init {
        val (leaseLock, wakeLock) = createLocks(powerManager)
        lease = leaseLock
        wake = wakeLock
    }

    /** Holds the screen on for [LEASE_MS] from now; the status poll renews it each second while it is wanted. */
    fun renewLease() {
        lease.acquire(LEASE_MS)
    }

    fun releaseLease() {
        if (lease.isHeld) {
            lease.release()
        }
    }

    /** Turns the screen on; returns whether it is interactive right after, false when Android refused the wake. */
    fun pulseWake(): Boolean {
        wake.acquire(WAKE_PULSE_MS)
        return powerManager.isInteractive
    }

    companion object {
        const val LEASE_TAG = "dsd-neo:screen"
        const val WAKE_TAG = "dsd-neo:screen-wake"
        const val LEASE_MS = 10_000L
        const val WAKE_PULSE_MS = 1_000L

        // The one place the deprecated levels appear. SCREEN_BRIGHT_WAKE_LOCK is deprecated for FLAG_KEEP_SCREEN_ON,
        // which cannot let the screen go off on release (above); ACQUIRE_CAUSES_WAKEUP for setTurnScreenOn, which only
        // acts when an activity resumes.
        @Suppress("DEPRECATION")
        private fun createLocks(powerManager: PowerManager): Pair<PowerManager.WakeLock, PowerManager.WakeLock> {
            val lease = powerManager.newWakeLock(PowerManager.SCREEN_BRIGHT_WAKE_LOCK, LEASE_TAG)
            val wake = powerManager.newWakeLock(
                PowerManager.SCREEN_BRIGHT_WAKE_LOCK or PowerManager.ACQUIRE_CAUSES_WAKEUP,
                WAKE_TAG,
            )
            lease.setReferenceCounted(false)
            wake.setReferenceCounted(false)
            return lease to wake
        }
    }
}
