// SPDX-License-Identifier: GPL-3.0-or-later
package android.os

// The wake-lock surface ScreenLocks uses. It records what it is asked and models the platform's reference counting
// (a counted lock stays held until every acquire is released; a release too many throws), so a test can tell a
// counted lock from a non-counted one. The constants carry the platform's values; the two the platform deprecates
// are deprecated here too, so an unsuppressed use outside ScreenLocks' factory shows as a warning.
class PowerManager {
    companion object {
        @Deprecated("Deprecated on the platform since API 17")
        const val SCREEN_BRIGHT_WAKE_LOCK = 0x0000000a

        @Deprecated("Deprecated on the platform since API 33")
        const val ACQUIRE_CAUSES_WAKEUP = 0x10000000

        const val ON_AFTER_RELEASE = 0x20000000

        private const val WAKEUP_FLAG = 0x10000000
    }

    /** Whether the screen is on; a pulse with the wakeup flag turns it on unless [wakeupRefused]. */
    var isInteractive = false

    /** Android declining to turn the screen on for ACQUIRE_CAUSES_WAKEUP. */
    var wakeupRefused = false

    val created = mutableListOf<WakeLock>()

    fun newWakeLock(levelAndFlags: Int, tag: String): WakeLock =
        WakeLock(this, levelAndFlags, tag).also { created += it }

    class WakeLock internal constructor(private val owner: PowerManager, val levelAndFlags: Int, val tag: String) {
        var referenceCounted = true
            private set
        var count = 0
            private set
        val acquireTimeouts = mutableListOf<Long>()
        var releases = 0
            private set

        val isHeld: Boolean get() = count > 0

        fun setReferenceCounted(value: Boolean) {
            referenceCounted = value
        }

        fun acquire(timeout: Long) {
            acquireTimeouts += timeout
            count = if (referenceCounted) count + 1 else 1
            if (levelAndFlags and WAKEUP_FLAG != 0 && !owner.wakeupRefused) {
                owner.isInteractive = true
            }
        }

        fun release() {
            releases++
            if (!referenceCounted) {
                count = 0
                return
            }
            if (count == 0) {
                throw RuntimeException("WakeLock under-locked $tag")
            }
            count--
        }
    }
}
