// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * The decoder service's status poll, as the screen policy sees it. Main thread only.
 *
 * Every running tick feeds a sample, even when only the audio's age moved or the record did not change at all,
 * because the policy's lease is renewed per sample. A session id the feed has not seen starts a session. Every path
 * that stops the poll ends the session through [onTerminated], which acts once however many of them run.
 */
class StatusFeed(private val screen: Screen) {
    /** Where the feed delivers; [ScreenController] in the app. */
    interface Screen {
        fun sessionStarted(id: Long)

        fun sample(stamp: ULong, ageMs: Long, wakeAllowed: Boolean)

        fun sessionEnded()
    }

    private var session: Long? = null
    private var stamp: ULong? = null

    /**
     * One poll tick. While [running], starts [sessionId] if it is new and samples [status]. A null status (absent or
     * unreadable) samples as no change: the last stamp fed with no age, or before any readable record the wire's own
     * "nothing audible" (stamp 0, age -1). A tick that is not running ends the session.
     */
    fun onTick(running: Boolean, sessionId: Long, status: DecoderStatus?, wakeAllowed: Boolean) {
        if (!running) {
            onTerminated()
            return
        }
        if (sessionId != session) {
            session = sessionId
            stamp = null
            screen.sessionStarted(sessionId)
        }
        val sampled = status?.audibleStamp ?: stamp ?: 0uL
        stamp = sampled
        screen.sample(sampled, status?.audibleAgeMs ?: -1L, wakeAllowed)
    }

    /** The poll stopped. Ends the session once; later calls do nothing until a running tick starts one again. */
    fun onTerminated() {
        if (session == null) {
            return
        }
        session = null
        stamp = null
        screen.sessionEnded()
    }
}
