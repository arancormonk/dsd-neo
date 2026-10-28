// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Where the activity stands, for the main-looper watchdog and the screen trace (#782). Plain Kotlin, so it runs on the
 * JVM.
 *
 * The activity generation counts creates and the window generation counts window attaches, so records taken across a
 * recreation, or before and after a window came back, say which instance and which window they describe. As with
 * [ActivitySlot], Android can create the new activity before the old one's onDestroy runs, so only the instance created
 * last moves the phase or the window generation; the old one's late callbacks are ignored.
 *
 * Written on the main thread only. [snapshot] and [touches] are published for the watchdog's thread, which reads them
 * while the main thread may be blocked.
 */
class ActivityTrace {
    /** One consistent reading: the generations, the latest phase and whether the window is attached. */
    data class Snapshot(val activity: Int, val window: Int, val phase: String, val windowAttached: Boolean) {
        override fun toString(): String =
            "activity $activity $phase, window $window " + if (windowAttached) "attached" else "detached"
    }

    private var current: Any? = null

    @Volatile
    var snapshot = Snapshot(0, 0, "not created", false)
        private set

    /** Touch events delivered to the activity so far; one writer, the main thread. */
    @Volatile
    var touches = 0L
        private set

    fun created(instance: Any) {
        current = instance
        val last = snapshot
        snapshot = Snapshot(last.activity + 1, last.window, "created", false)
    }

    /** A lifecycle callback of [instance]; ignored unless it is the activity created last. */
    fun phase(instance: Any, phase: String) {
        if (instance === current) {
            snapshot = snapshot.copy(phase = phase)
        }
    }

    fun windowAttached(instance: Any) {
        if (instance === current) {
            val last = snapshot
            snapshot = last.copy(window = last.window + 1, windowAttached = true)
        }
    }

    fun windowDetached(instance: Any) {
        if (instance === current) {
            snapshot = snapshot.copy(windowAttached = false)
        }
    }

    /** The activity is gone; the generations stay, so the next create counts on from them. */
    fun destroyed(instance: Any) {
        if (instance === current) {
            snapshot = snapshot.copy(phase = "destroyed")
            current = null
        }
    }

    fun touched() {
        touches += 1
    }
}
