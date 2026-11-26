// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * The call recording playlist's service order (#909): which play the playback service serves, when a start is issued,
 * what each delivery does and when the service retires. RecordingPlayback and RecordingPlaybackService carry out what
 * it decides; it holds no Android state, so the JVM tests drive it directly.
 *
 * Everything is under one lock: admission (closed for good by [release]), the service's [Phase], the reserved request
 * (the playlist a start will promote) and the owner (the playlist the promoted service serves). At most one start is
 * ever outstanding. A playlist accepted while the service is promoted is handed to it in the process; one accepted
 * while a start is issued becomes that start's request; only one accepted with no service issues a start
 * ([Start.ISSUE]). Issuing, handing over, retiring and closing admission all take the lock, so a play, a retire and
 * the teardown never interleave.
 *
 * A start carries this process's token and the generation it was issued for. Its delivery ([promote]) promotes the
 * current request only when it is that start; anything else (a start from an earlier process, redelivered after it
 * died) leaves the instance in the foreground with nothing to serve. A transport delivery (a notification button)
 * names the owner it was posted for and is applied only while that playlist still owns the service. After every
 * delivery and every asynchronous end the instance asks [retire]: with no owner, no request and no start outstanding
 * it stops against the newest start id it has seen, so a start that arrived after the decision keeps it alive.
 */
class PlaybackServicePhases(private val processToken: Long) {
    enum class Phase {
        /** No service, and no start outstanding. */
        NONE,

        /** A start was issued and not delivered yet. */
        ISSUED,

        /** An instance is in the foreground. */
        PROMOTED,

        /** The instance stopped itself (past stopSelfResult) and is on its way out. */
        RETIRING,
    }

    /** What an accepted play does about the service. */
    enum class Start {
        /** Not a playlist: no service. */
        NONE,

        /** No service runs: issue a start for the play. */
        ISSUE,

        /** A start is outstanding: the play becomes its request, and no other start is issued. */
        JOIN,

        /** The service is in the foreground: it serves the play at once, in the process. */
        HAND_OVER,
    }

    /** What a start's delivery did: the playlist it promoted, or the one whose start was refused (0: none). */
    class Promotion(val promoted: Int, val refused: Int)

    private val lock = Any()
    private var open = true
    private var generations = 0
    private var phase = Phase.NONE

    /** The generation the outstanding start was issued for: its delivery names it. */
    private var issuedFor = 0
    private var reserved = 0
    private var owner = 0
    private var instances = 0

    /** The service instance alive now (one at a time); 0 when none. */
    private var alive = 0

    val currentPhase: Phase get() = synchronized(lock) { phase }

    /** The playlist the promoted service serves, or 0. */
    val currentOwner: Int get() = synchronized(lock) { owner }

    /** The playlist waiting for the outstanding start, or 0. */
    val currentRequest: Int get() = synchronized(lock) { reserved }

    /**
     * Accept a play, a [playlist] or not, replacing what played: its generation, or 0 once admission is closed. [then]
     * runs under the lock with the generation and what to do about the service, so what it posts is ordered against a
     * teardown. A play that is not a playlist ends the service's request and ownership at once.
     */
    fun accept(playlist: Boolean, then: (generation: Int, start: Start) -> Unit): Int = synchronized(lock) {
        if (!open) {
            return 0
        }
        val generation = ++generations
        val start = when {
            !playlist -> {
                owner = 0
                reserved = 0
                Start.NONE
            }
            phase == Phase.PROMOTED -> {
                owner = generation
                reserved = 0
                Start.HAND_OVER
            }
            phase == Phase.ISSUED -> {
                reserved = generation
                Start.JOIN
            }
            else -> {
                owner = 0
                reserved = generation
                issuedFor = generation
                phase = Phase.ISSUED
                Start.ISSUE
            }
        }
        then(generation, start)
        generation
    }

    /**
     * The start issued for [generation] was refused as it was asked for: nothing is outstanding, and whatever request
     * waited for that start is settled with it. The plays to tell they could not start: [generation] and the request
     * that waited, if another (each once, none 0).
     */
    fun startFailed(generation: Int): IntArray = synchronized(lock) {
        if (phase != Phase.ISSUED || issuedFor != generation) {
            return intArrayOf(generation)
        }
        phase = Phase.NONE
        issuedFor = 0
        val waited = reserved
        reserved = 0
        if (waited == 0 || waited == generation) intArrayOf(generation) else intArrayOf(generation, waited)
    }

    /**
     * A service instance was created: its serial, which its later calls name. The instance before it is gone, so a
     * retiring phase ends here, even when that instance's destroy is reported late.
     */
    fun created(): Int = synchronized(lock) {
        alive = ++instances
        if (phase == Phase.RETIRING) {
            phase = Phase.NONE
        }
        alive
    }

    /** The playlist whose notification a start's delivery shows as it enters the foreground: the owner, or 0. */
    fun foregroundOwner(): Int = synchronized(lock) { owner }

    /**
     * A start delivered to [instance] with the [token] and [generation] it carries, after the instance tried to enter
     * the foreground ([foreground]: whether it did). Only the outstanding start of this process promotes, and it
     * promotes the request standing now, which may be a later play than the one it was issued for, or none (settled).
     */
    fun promote(instance: Int, token: Long, generation: Int, foreground: Boolean): Promotion = synchronized(lock) {
        if (token != processToken || phase != Phase.ISSUED || generation != issuedFor || instance != alive) {
            return Promotion(0, 0)
        }
        issuedFor = 0
        val request = reserved
        reserved = 0
        if (!foreground) {
            phase = Phase.NONE
            return Promotion(0, request)
        }
        phase = Phase.PROMOTED
        owner = request
        Promotion(request, 0)
    }

    /** A transport delivery for [generation] from [token]: the playlist to apply it to, or 0 when it is stale. */
    fun transport(token: Long, generation: Int): Int = synchronized(lock) {
        if (token == processToken && generation != 0 && generation == owner) generation else 0
    }

    /** Whether [generation] owns the service now: a command is applied only then, as it runs. */
    fun isOwner(generation: Int): Boolean = synchronized(lock) { generation != 0 && generation == owner }

    /** Playlist [generation] ended (finished, stopped, timed out, refused, replaced): it no longer owns or waits. */
    fun ended(generation: Int) = synchronized(lock) {
        if (generation == owner) {
            owner = 0
        }
        if (generation == reserved) {
            reserved = 0
        }
    }

    /** The playlist the service serves or waits for, for an end the instance asks for (task removed); 0 when none. */
    fun current(): Int = synchronized(lock) { if (owner != 0) owner else reserved }

    /**
     * The retire check, after every delivery and every asynchronous end: with no owner, no request and no start
     * outstanding, [stop] (stopSelfResult with the newest start id) runs under the lock; true when it stopped.
     */
    fun retire(instance: Int, stop: () -> Boolean): Boolean = synchronized(lock) {
        if (instance != alive || owner != 0 || reserved != 0 || phase == Phase.ISSUED || phase == Phase.RETIRING) {
            return false
        }
        if (!stop()) {
            return false
        }
        phase = Phase.RETIRING
        true
    }

    /**
     * [instance] is destroyed: the playlist it served, for the caller to settle (0: none). A start issued since it
     * retired is the next instance's and stays outstanding.
     */
    fun destroyed(instance: Int): Int = synchronized(lock) {
        if (instance != alive) {
            return 0
        }
        alive = 0
        val served = owner
        owner = 0
        if (phase == Phase.PROMOTED || phase == Phase.RETIRING) {
            phase = Phase.NONE
        }
        served
    }

    /**
     * The app is ending: admission closes, the request and the owner are settled, and [then] runs under the lock (the
     * player's stop). True when a promoted service must be stopped (stopService). A start issued and not promoted is
     * left to its delivery, which promotes and retires: stopping it before its startForeground() would crash.
     */
    fun release(then: () -> Unit): Boolean = synchronized(lock) {
        open = false
        owner = 0
        reserved = 0
        val promoted = phase == Phase.PROMOTED
        if (promoted) {
            phase = Phase.RETIRING
        }
        then()
        promoted
    }
}
