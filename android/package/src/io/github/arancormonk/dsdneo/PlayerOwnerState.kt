// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * What one call recording player's owner thread decides about its seeks (#909), apart from the
 * platform player, so the JVM tests drive it with a fake [Platform]. RecordingPlayback's MediaItem keeps one per
 * player and calls it on that player's owner thread only; not thread-safe.
 *
 * Seeks (R59): `OnSeekCompleteListener` carries no request, so at most one platform seek is in flight. A seek asked
 * for while one is in flight is held as the single pending one, the newest replacing an older one. Once the seek in
 * flight completes, its own serial is reported ([landed]), then the held one, if any, goes to the platform. So every
 * completion names the request that landed, and the machine treats one for a superseded serial as not landed.
 */
class PlayerOwnerState(
    private val platform: Platform,
    private val landed: (serial: Long) -> Unit,
) {
    /** The platform player the owner drives. */
    interface Platform {
        fun seekTo(positionMs: Long)
    }

    private var inFlight = 0L
    private var heldSerial = 0L
    private var heldMs = 0L

    /** Seek request [serial] to [positionMs]: to the platform now, or held while another is in flight. */
    fun seek(positionMs: Long, serial: Long) {
        if (inFlight != 0L) {
            heldSerial = serial
            heldMs = positionMs
            return
        }
        platform.seekTo(positionMs)
        // Only once the platform took it: a seek that throws leaves none in flight, so a later one is not held forever.
        inFlight = serial
    }

    /** The platform's seek completed: the one in flight landed; the held one, if any, goes next. */
    fun seekCompleted() {
        val done = inFlight
        if (done == 0L) {
            return // not one of ours
        }
        inFlight = 0L
        landed(done)
        val next = heldSerial
        if (next != 0L) {
            heldSerial = 0L
            seek(heldMs, next)
        }
    }

    /** The platform player was released: no seek is in flight or held any more. */
    fun released() {
        inFlight = 0L
        heldSerial = 0L
    }
}
