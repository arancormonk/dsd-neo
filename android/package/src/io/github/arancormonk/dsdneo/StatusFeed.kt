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
 *
 * The feed keeps no stamp of its own: the policy is the only keeper of the last one. So a synchronous sample the glue
 * sends straight to the controller (before a screen-off or an arming loss) can never be undone by a later tick.
 */
class StatusFeed(private val screen: Screen) {
    /** Where the feed delivers; [ScreenController] in the app. */
    interface Screen {
        fun sessionStarted(id: Long)

        fun sample(stamp: ULong?, ageMs: Long, wakeAllowed: () -> Boolean)

        fun sessionEnded()
    }

    private var session: Long? = null

    /**
     * One poll tick. While [running], starts [sessionId] if it is new and samples [status]. A null status (absent or
     * unreadable) samples with no stamp, which the policy takes as no change. A tick that is not running ends the
     * session. [wakeAllowed] goes to the sample as it is, never asked here: the policy asks it only for a sample that
     * would wake.
     */
    fun onTick(running: Boolean, sessionId: Long, status: DecoderStatus?, wakeAllowed: () -> Boolean) {
        if (!running) {
            onTerminated()
            return
        }
        if (sessionId != session) {
            session = sessionId
            screen.sessionStarted(sessionId)
        }
        screen.sample(status?.audibleStamp, status?.audibleAgeMs ?: -1L, wakeAllowed)
    }

    /** The poll stopped. Ends the session once; later calls do nothing until a running tick starts one again. */
    fun onTerminated() {
        if (session == null) {
            return
        }
        session = null
        screen.sessionEnded()
    }
}
