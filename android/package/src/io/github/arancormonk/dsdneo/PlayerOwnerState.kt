// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * What one call recording player's owner thread decides about its seeks and its silence (#909), apart from the
 * platform player, so the JVM tests drive it with a fake [Platform]. RecordingPlayback's MediaItem keeps one per
 * player and calls it on that player's owner thread only; not thread-safe.
 *
 * Seeks (R59): `OnSeekCompleteListener` carries no request, so at most one platform seek is in flight. A seek asked
 * for while one is in flight is held as the single pending one, the newest replacing an older one. Once the seek in
 * flight completes, its own serial is reported ([landed]), then the held one, if any, goes to the platform. So every
 * completion names the request that landed, and the machine treats one for a superseded serial as not landed. A seek
 * in flight is bounded at [SEEK_GIVE_UP_MS] (M1), the sheet's own fallback: with no completion by then it is given up,
 * never reported as landed, and the held one goes to the platform, matched to the next completion. The limit: a
 * completion lost past its bound that arrives late can land the following seek early. A start asked for while a seek
 * is held waits until that seek has gone to the platform (M2), so the platform sees seek, then start (R45).
 *
 * Silence (R62): a start the platform made ([started]) is undone only by a pause that returned. Asked for its silence,
 * the owner reports it ([silent]) at once when nothing sounds; when a start stands (its pause threw, or none ran), the
 * release, once it has returned, reports it instead. A silence that never comes is bounded by the incoming start's
 * watchdog.
 */
class PlayerOwnerState(
    private val platform: Platform,
    private val landed: (serial: Long) -> Unit,
    private val silent: () -> Unit,
    private val later: Later,
) {
    /** The platform player the owner drives. */
    interface Platform {
        fun seekTo(positionMs: Long)

        fun pause()
    }

    /** Runs a task on the owner after [delayMs]; the result cancels it. */
    fun interface Later {
        fun post(delayMs: Long, task: () -> Unit): () -> Unit
    }

    /** A platform start was made, and no pause that returned has undone it. */
    var started = false
        private set

    private var inFlight = 0L
    private var giveUp: (() -> Unit)? = null
    private var heldSerial = 0L
    private var heldMs = 0L
    private var heldStart: (() -> Unit)? = null
    private var silenceAtRelease = false

    /**
     * Make a start ([make], which checks it is still wanted): now, or once the held seek has gone to the platform
     * (M2). Only the newest waits; a pause or the release drops it.
     */
    fun start(make: () -> Unit) {
        if (heldSerial != 0L) {
            heldStart = make
        } else {
            make()
        }
    }

    /** The platform start was made. */
    fun onStarted() {
        started = true
    }

    /** Pause the platform player if a start stands; [started] clears only once the pause has returned. */
    fun pause() {
        heldStart = null
        if (started) {
            platform.pause()
            started = false
        }
    }

    /** Seek request [serial] to [positionMs]: to the platform now, or held while another is in flight. */
    fun seek(positionMs: Long, serial: Long) {
        if (inFlight != 0L) {
            heldSerial = serial
            heldMs = positionMs
            return
        }
        // In flight before the platform call, so a completion it delivered at once would match; a seek that throws
        // leaves none in flight, so a later one is not held forever.
        inFlight = serial
        try {
            platform.seekTo(positionMs)
        } catch (e: Throwable) {
            inFlight = 0L
            throw e
        }
        giveUp = later.post(SEEK_GIVE_UP_MS) {
            if (inFlight == serial) {
                inFlight = 0L // given up: never reported as landed (M1)
                giveUp = null
                next()
            }
        }
    }

    /** The platform's seek completed: the one in flight landed; the held one, if any, goes next. */
    fun seekCompleted() {
        val done = inFlight
        if (done == 0L) {
            return // not one of ours
        }
        inFlight = 0L
        giveUp?.invoke()
        giveUp = null
        landed(done)
        next()
    }

    /** The held seek, if any, goes to the platform, then a start that waited for it. */
    private fun next() {
        val serial = heldSerial
        if (serial != 0L) {
            heldSerial = 0L
            seek(heldMs, serial)
        }
        val waiting = heldStart
        heldStart = null
        waiting?.invoke()
    }

    /** Silence asked for, behind the move's pause: now if nothing sounds, else once the release has returned. */
    fun silence() {
        if (started) {
            silenceAtRelease = true
        } else {
            silent()
        }
    }

    /** The platform player was released; [returned] false when its release threw. */
    fun released(returned: Boolean) {
        started = false
        inFlight = 0L
        giveUp?.invoke()
        giveUp = null
        heldSerial = 0L
        heldStart = null
        if (silenceAtRelease && returned) {
            silenceAtRelease = false
            silent()
        }
    }

    companion object {
        /** How long a platform seek may stay in flight with no completion before it is given up (M1). */
        const val SEEK_GIVE_UP_MS = 2_000L
    }
}
