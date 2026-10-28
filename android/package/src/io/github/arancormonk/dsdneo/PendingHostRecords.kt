// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Host diagnostics written from Kotlin before the native library can take them (#782). Plain Kotlin, so it runs on the
 * JVM; [HostDiagnostics] is the app's one instance.
 *
 * The main-looper watchdog starts in the first onCreate, before Qt has loaded the native library, and a stall at
 * startup is exactly what it must not lose. Until [markReady], records wait here: the first [capacity] in order, and a
 * count of any after them, which is reported when they go out. From then on each record goes straight to [deliver],
 * after whatever was waiting. Thread-safe: the main thread and the watchdog's thread both write.
 */
class PendingHostRecords(private val capacity: Int, private val deliver: (String) -> Unit) {
    private val lock = Any()
    private val held = ArrayDeque<String>()
    private var dropped = 0
    private var ready = false

    /** Whether [markReady] has been called. */
    val isReady: Boolean get() = synchronized(lock) { ready }

    /** How many records wait, for tests. */
    val heldCount: Int get() = synchronized(lock) { held.size }

    fun submit(line: String) {
        synchronized(lock) {
            if (!ready) {
                if (held.size < capacity) {
                    held.addLast(line)
                } else {
                    dropped += 1
                }
                return
            }
            // Delivered under the lock, so records from both threads keep one order; native submission never waits.
            drainLocked()
            deliver(line)
        }
    }

    /** The native side can take records now; whatever waits goes out at the next [drain] or [submit]. */
    fun markReady() {
        synchronized(lock) { ready = true }
    }

    /** Sends whatever waits, if ready. */
    fun drain() {
        synchronized(lock) {
            if (ready) {
                drainLocked()
            }
        }
    }

    private fun drainLocked() {
        while (held.isNotEmpty()) {
            deliver(held.removeFirst())
        }
        if (dropped > 0) {
            deliver("diagnostics: $dropped host records dropped before native diagnostics were ready")
            dropped = 0
        }
    }
}
