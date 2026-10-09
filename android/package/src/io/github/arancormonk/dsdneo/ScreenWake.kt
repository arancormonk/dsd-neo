// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * What one wake pulse did ([ScreenLocks.pulseWake], [ScreenController.Effects.pulseWake]). Plain Kotlin, so the
 * controller that acts on it runs on the JVM.
 */
enum class ScreenWake {
    /** The screen was off, and the pulse turned it on: the screen-on broadcast that follows is DSD-neo's own wake. */
    WOKE,

    /**
     * The screen was already on, so nothing was pulsed and no wake is claimed. Someone else (the user, another app or a
     * notification) turned it on, and its screen-on broadcast, still to reach DSD-neo, disarms; or an own pulse did,
     * and its screen-on is judged by that pulse's claim.
     */
    ALREADY_ON,

    /** The screen was off and stayed off: Android refused the wake. */
    REFUSED,
}
