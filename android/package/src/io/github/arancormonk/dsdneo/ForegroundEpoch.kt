// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import java.util.concurrent.atomic.AtomicLong

/**
 * How many times the app has left the foreground (#786): 0 at launch, one more on every Activity `onStop`
 * ([LocationStream.onActivityStop]). Plain Kotlin with no Android imports, so the decoder service reads it as well as
 * the location feed: a GPS roam's start carries the epoch its switch was decided in, and never runs once the app has
 * left the foreground since.
 */
internal object ForegroundEpoch {
    private val epoch = AtomicLong(0)

    /** Runs right after each [advance], on its thread: the JVM tests' window for a reader that sees the new epoch. */
    @Volatile internal var afterAdvance: (() -> Unit)? = null

    /** The epoch now, from any thread. */
    fun current(): Long = epoch.get()

    /** The app left the foreground: the next epoch. */
    fun advance(): Long = epoch.incrementAndGet().also { afterAdvance?.invoke() }
}
