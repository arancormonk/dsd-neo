// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Why the engine is being asked to stop, carried from the caller that asked.
 *
 * Only a stop the user starts gives up the rdio uploads still pending (#718 D4): someone who taps
 * Stop listening, or switches away, is done and should not wait minutes for a slow or dead endpoint to work
 * through the backlog. Every other stop drains as before, so a session that ends on its own
 * still uploads its last calls. The abandoned calls' files stay on disk either way.
 *
 * Plain Kotlin with no Android imports, so the JVM tests build it without stubs.
 */
enum class StopCause(val wire: String) {
    /**
     * A stop or restart the user started from the app (every Qt host stop: Stop listening,
     * switching systems, retry, leaving Signal search, Back or close with background listening
     * off, dismissing a failure), the notification's Stop action, or removal of the app's task
     * from Recents (handled by the service even without Activity destruction).
     */
    USER("user"),

    /** The SDR the session reads from was unplugged. A normal end: drain. */
    DEVICE_DETACHED("detached"),

    /** The service or Activity is being torn down. Drain, as the CLI does. */
    LIFECYCLE("lifecycle");

    /** Whether this stop abandons the pending rdio uploads instead of waiting for them. */
    val abandonsUploads: Boolean
        get() = this == USER

    companion object {
        /**
         * The cause a stop intent carries. A missing or unknown value drains: abandoning work
         * is only ever done for a caller that said it was the user.
         */
        fun fromWire(value: String?): StopCause = entries.firstOrNull { it.wire == value } ?: LIFECYCLE
    }
}

/** The one way the app stops the engine, so every caller has to say why. */
object EngineStop {
    /**
     * Raises the engine's stop flag through [nativeStop], which receives whether the stop was
     * the user's and the cause's wire value. Returns what [nativeStop] returned.
     */
    fun request(cause: StopCause, nativeStop: (Boolean, String) -> Int): Int =
        nativeStop(cause.abandonsUploads, cause.wire)
}
