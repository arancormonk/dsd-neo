// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.os.PowerManager

// The platform's values, spelled out so the test does not lean on the stub's copies of them.
private const val SCREEN_BRIGHT_WAKE_LOCK = 0x0000000a
private const val ACQUIRE_CAUSES_WAKEUP = 0x10000000
private const val ON_AFTER_RELEASE = 0x20000000

private fun bothLocksAreCreatedOnceAndNotReferenceCounted() {
    val power = PowerManager()
    val locks = ScreenLocks(power)
    check(power.created.size == 2)
    val (lease, wake) = power.created
    check(lease.tag == "dsd-neo:screen" && wake.tag == "dsd-neo:screen-wake")
    check(!lease.referenceCounted && !wake.referenceCounted)
    check(lease.levelAndFlags == SCREEN_BRIGHT_WAKE_LOCK) { "the lease is a plain screen lock" }
    check(lease.levelAndFlags and ON_AFTER_RELEASE == 0) { "releasing the lease must not poke user activity" }
    check(wake.levelAndFlags == SCREEN_BRIGHT_WAKE_LOCK or ACQUIRE_CAUSES_WAKEUP)
    repeat(3) {
        locks.renewLease()
        locks.pulseWake()
        locks.releaseLease()
    }
    check(power.created.size == 2) { "no lock is made after the first two" }
}

private fun renewalsThenOneReleaseFreeTheLease() {
    val power = PowerManager()
    val locks = ScreenLocks(power)
    val lease = power.created[0]
    repeat(5) { locks.renewLease() }
    check(lease.isHeld && lease.acquireTimeouts == List(5) { 10_000L }) { "each renewal is a 10 s acquire" }
    locks.releaseLease()
    check(!lease.isHeld && lease.releases == 1) { "one release undoes any number of renewals" }
    locks.releaseLease()
    check(lease.releases == 1) { "releasing a lease not held calls nothing" }
    check(!power.isInteractive) { "the lease never turns the screen on" }
    // The same renewals on a counted lock would leave it held: what setReferenceCounted(false) prevents.
    val counted = power.newWakeLock(SCREEN_BRIGHT_WAKE_LOCK, "counted")
    repeat(5) { counted.acquire(10_000L) }
    counted.release()
    check(counted.isHeld)
}

private fun aPulseWakesWithAOneSecondLockAndReportsTheScreen() {
    val power = PowerManager()
    val locks = ScreenLocks(power)
    val wake = power.created[1]
    check(locks.pulseWake() == ScreenWake.WOKE) { "the screen came on" }
    check(wake.acquireTimeouts == listOf(1_000L) && power.created[0].acquireTimeouts.isEmpty())
    val refused = PowerManager().apply { wakeupRefused = true }
    check(ScreenLocks(refused).pulseWake() == ScreenWake.REFUSED) { "a refused wake reports the screen still off" }
    // Pulses repeat without piling up holds, each over a screen gone off again.
    repeat(3) {
        power.isInteractive = false
        check(locks.pulseWake() == ScreenWake.WOKE)
    }
    check(wake.count == 1 && wake.acquireTimeouts.size == 4)
}

private fun aScreenAlreadyOnIsNotPulsed() {
    // Someone else turned the screen on and its broadcast has not reached DSD-neo yet: no pulse goes out.
    val power = PowerManager().apply { isInteractive = true }
    val locks = ScreenLocks(power)
    val wake = power.created[1]
    check(locks.pulseWake() == ScreenWake.ALREADY_ON) { "a screen already on is reported as such" }
    check(wake.acquireTimeouts.isEmpty() && !wake.isHeld) { "no pulse over a screen already on" }
}

fun main() {
    bothLocksAreCreatedOnceAndNotReferenceCounted()
    renewalsThenOneReleaseFreeTheLease()
    aPulseWakesWithAOneSecondLockAndReportsTheScreen()
    aScreenAlreadyOnIsNotPulsed()
    println("PASS: screen locks are non-counted, the lease has no ON_AFTER_RELEASE, the pulse wakes for 1 s when off")
}
