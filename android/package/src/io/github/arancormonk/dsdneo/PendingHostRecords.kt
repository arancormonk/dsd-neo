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
 * count of any after them, which is reported when they go out. From then on each record goes to [deliver] after
 * whatever was waiting. Thread-safe: the main thread and the watchdog's thread both write.
 *
 * [deliver] is never called under the lock, and [isReady] and [markReady] take no lock at all. In the app, [deliver]
 * is a call into DsdNative, which can wait for DsdNative's class initialization on another thread, and that
 * initialization marks this ready: a lock held across the call, or taken by the mark, could deadlock the two threads.
 * One thread delivers at a time, taking the waiting records in batches, so records keep one order; a record submitted
 * while another thread delivers is left for that thread and the call returns at once.
 */
class PendingHostRecords(private val capacity: Int, private val deliver: (String) -> Unit) {
    private val lock = Any()
    private val queue = ArrayDeque<String>()
    private var dropped = 0
    private var delivering = false

    @Volatile
    private var ready = false

    /** Whether [markReady] has been called. Lock-free. */
    val isReady: Boolean get() = ready

    /** How many records wait, for tests. */
    val heldCount: Int get() = synchronized(lock) { queue.size }

    fun submit(line: String) {
        synchronized(lock) {
            if (queue.size < capacity) {
                queue.addLast(line)
            } else {
                dropped += 1
            }
        }
        // Read after the record is queued, and markReady writes before its drain looks: one of the two sends it.
        drain()
    }

    /** The native side can take records now; whatever waits goes out at the next [drain] or [submit]. Lock-free. */
    fun markReady() {
        ready = true
    }

    /** Sends whatever waits, if ready, unless another thread is sending already. */
    fun drain() {
        if (!ready) {
            return
        }
        synchronized(lock) {
            if (delivering) {
                return
            }
            delivering = true
        }
        var finished = false
        try {
            while (true) {
                val batch = synchronized(lock) {
                    if (queue.isEmpty() && dropped == 0) {
                        delivering = false
                        finished = true
                        null
                    } else {
                        val lines = queue.toMutableList()
                        queue.clear()
                        if (dropped > 0) {
                            lines += "diagnostics: $dropped host records dropped while waiting for native diagnostics"
                            dropped = 0
                        }
                        lines
                    }
                } ?: return
                batch.forEach(deliver)
            }
        } finally {
            if (!finished) {
                synchronized(lock) { delivering = false }
            }
        }
    }
}
