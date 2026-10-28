// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import java.io.File
import java.io.FileNotFoundException

/**
 * The call recording player's state machine (#708, D6/D7/D10): one recording at a time, beside the live audio.
 *
 * It keeps what the user asked for (play or pause) apart from what the player does, and starts the player only when
 * it is prepared, play is wanted, the app is in the foreground and the audio focus is granted. A pause, the app leaving
 * the foreground or a focus change while the player prepares only change what is wanted, so the player is never asked
 * for a call its state does not take. Leaving the foreground pauses (D10); the user resumes. A focus request that is
 * denied (a phone call) fails the play as "unavailable"; a transient loss pauses until the focus comes back, a lasting
 * one pauses for good. Each play has a generation, and callbacks of a replaced player are dropped.
 *
 * Plain Kotlin, so the JVM tests drive it with fakes; RecordingPlayback runs it on one looper thread with a
 * MediaPlayer and the AudioManager. Not thread-safe: every call comes from that one thread. [status] may be read on
 * any thread.
 */
class RecordingPlayer(
    private val players: (generation: Int) -> Player,
    private val focus: Focus,
    private val clockMs: () -> Long,
    private var foreground: Boolean = true,
) {
    /** One recording's player; it reports back through [onPrepared], [onCompleted] and [onFailed]. */
    interface Player {
        /** Open [path] and prepare it, asynchronously; FileNotFoundException when it cannot be opened. */
        fun prepare(path: String)

        fun start()

        fun pause()

        fun seekTo(positionMs: Long)

        /** The length, once prepared. */
        val durationMs: Long

        fun release()
    }

    /** The app's transient audio focus. */
    interface Focus {
        /** Ask for it; false when it is denied. */
        fun request(): Boolean

        fun abandon()
    }

    enum class FocusChange { GAINED, LOST_TRANSIENT, LOST }

    /**
     * What the player does, for DecoderHostAndroid's poll: the position runs on from [positionMs] while [playingSinceMs]
     * is set (not -1).
     */
    class Status internal constructor(
        val generation: Int,
        val state: Int,
        private val positionMs: Long,
        private val playingSinceMs: Long,
        val durationMs: Long,
        val error: String,
    ) {
        fun positionMs(nowMs: Long): Long {
            val position = if (playingSinceMs < 0) positionMs else positionMs + (nowMs - playingSinceMs).coerceAtLeast(0)
            return if (durationMs > 0) position.coerceAtMost(durationMs) else position
        }

        /** "generation, state, position, duration, error", tab-separated: what DecoderHostAndroid parses. */
        fun record(nowMs: Long): String = "$generation\t$state\t${positionMs(nowMs)}\t$durationMs\t$error"
    }

    companion object {
        /** States; must match DecoderHost::PlaybackState. */
        const val STOPPED = 0
        const val PREPARING = 1
        const val PLAYING = 2
        const val PAUSED = 3
        const val FAILED = 4

        /** Why a play failed, for the UI to word (DecoderHost::playbackError). */
        const val ERROR_MISSING = "missing"
        const val ERROR_UNAVAILABLE = "unavailable"
        const val ERROR_FAILED = "failed"
    }

    @Volatile
    var status = Status(0, STOPPED, 0, -1, 0, "")
        private set

    private var generation = 0
    private var player: Player? = null
    private var prepared = false
    private var playing = false
    private var wanted = false
    private var focusHeld = false
    private var focusSuspended = false
    private var positionMs = 0L
    private var playingSinceMs = -1L
    private var durationMs = 0L
    private var pendingSeekMs = -1L
    private var error = ""

    /** Play [path] from its start as play [generation], replacing what plays. */
    fun play(generation: Int, path: String) {
        close()
        this.generation = generation
        wanted = true
        val next = players(generation)
        player = next
        publish()
        try {
            next.prepare(path)
        } catch (_: FileNotFoundException) {
            // Also thrown for a file there that cannot be opened (no access, a folder): only one gone is missing.
            fail(if (File(path).exists()) ERROR_FAILED else ERROR_MISSING)
        } catch (_: Exception) {
            fail(ERROR_FAILED)
        }
    }

    fun pause() {
        wanted = false
        halt()
        dropFocus()
        publish()
    }

    fun resume() {
        if (player == null) {
            return
        }
        wanted = true
        advance()
    }

    fun seek(positionMs: Long) {
        val current = player ?: return
        if (!prepared) {
            pendingSeekMs = positionMs.coerceAtLeast(0)
            return
        }
        val target = positionMs.coerceIn(0, durationMs.coerceAtLeast(0))
        current.seekTo(target)
        this.positionMs = target
        if (playing) {
            playingSinceMs = clockMs()
        }
        publish()
    }

    /** Close the recording; the next status is Stopped with no generation. */
    fun stop() {
        close()
        generation = 0
        publish()
    }

    /** The app came to the foreground or left it; leaving it pauses (D10). */
    fun setForeground(foreground: Boolean) {
        this.foreground = foreground
        if (foreground) {
            advance()
        } else {
            pause()
        }
    }

    fun onPrepared(generation: Int) {
        val current = player ?: return
        if (generation != this.generation || prepared) {
            return
        }
        prepared = true
        durationMs = current.durationMs.coerceAtLeast(0)
        if (pendingSeekMs > 0) {
            positionMs = pendingSeekMs.coerceAtMost(durationMs)
            current.seekTo(positionMs)
        }
        pendingSeekMs = -1
        advance()
    }

    /** It played to its end: Stopped, with the position at its length. */
    fun onCompleted(generation: Int) {
        if (generation != this.generation || player == null) {
            return
        }
        val length = durationMs
        close()
        durationMs = length
        positionMs = length
        publish()
    }

    fun onFailed(generation: Int, error: String) {
        if (generation != this.generation || player == null) {
            return
        }
        fail(error)
    }

    fun onFocusChange(change: FocusChange) {
        if (!focusHeld) {
            return
        }
        when (change) {
            FocusChange.GAINED -> {
                focusSuspended = false
                advance()
            }
            FocusChange.LOST_TRANSIENT -> {
                focusSuspended = true
                halt()
                publish()
            }
            FocusChange.LOST -> pause()
        }
    }

    /** Start the player when everything allows it: prepared, wanted, in the foreground, with the focus. */
    private fun advance() {
        val current = player
        if (current == null || !prepared || playing || !wanted || !foreground) {
            publish()
            return
        }
        if (!focusHeld) {
            if (!focus.request()) {
                fail(ERROR_UNAVAILABLE)
                return
            }
            focusHeld = true
            focusSuspended = false
        }
        if (!focusSuspended) {
            current.start()
            playing = true
            playingSinceMs = clockMs()
        }
        publish()
    }

    /** Pause the player if it plays, keeping its position. */
    private fun halt() {
        val current = player ?: return
        if (!playing) {
            return
        }
        current.pause()
        positionMs = Status(generation, PLAYING, positionMs, playingSinceMs, durationMs, "").positionMs(clockMs())
        playing = false
        playingSinceMs = -1
    }

    private fun dropFocus() {
        if (focusHeld) {
            focus.abandon()
        }
        focusHeld = false
        focusSuspended = false
    }

    private fun fail(error: String) {
        close()
        this.error = error
        publish()
    }

    /** Release the player and the focus; nothing plays, nothing is wanted. */
    private fun close() {
        player?.release()
        player = null
        prepared = false
        playing = false
        wanted = false
        positionMs = 0
        playingSinceMs = -1
        durationMs = 0
        pendingSeekMs = -1
        error = ""
        dropFocus()
    }

    private fun publish() {
        val state = when {
            error.isNotEmpty() -> FAILED
            player == null -> STOPPED
            !prepared -> PREPARING
            playing -> PLAYING
            else -> PAUSED
        }
        status = Status(generation, state, positionMs, if (playing) playingSinceMs else -1, durationMs, error)
    }
}
