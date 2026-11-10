// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Whether the activity's launch intent is a new delivery or one Android hands back again (#831 B1).
 *
 * A relaunch from Recents re-delivers the intent the task was first started with: after the process died, with the
 * saved state it left; for a task whose activity had finished, with [LAUNCHED_FROM_HISTORY] set. Neither is a new USB
 * attachment or a new document, so `onCreate` reports the launch intent only when it is fresh. `onNewIntent` always
 * carries a new delivery. A cold launch (no saved state, no history flag) is fresh, so the first attachment that starts
 * the app still reports.
 *
 * Plain Kotlin with no Android imports, so the JVM tests run it as is.
 */
object LaunchDelivery {
    /** `Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY`, a public constant since API 1. */
    const val LAUNCHED_FROM_HISTORY = 0x00100000

    /** [saved] is whether `onCreate` got a saved instance state; [flags] are the launch intent's. */
    fun fresh(saved: Boolean, flags: Int): Boolean = !saved && (flags and LAUNCHED_FROM_HISTORY) == 0
}
