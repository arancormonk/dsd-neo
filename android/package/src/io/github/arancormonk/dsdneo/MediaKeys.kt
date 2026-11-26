// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * The media keys DsdNeoActivity hands past Qt while a History playlist's media session is open (#909): a headset's
 * button, a Bluetooth headset's or a car's transport controls, a keyboard's media keys. Qt's activity consumes every
 * key it is given, so with DSD-neo in front these never reached the playlist's session; left unhandled, Android
 * delivers them to the media button session, which is the playlist's while it is open. With no playlist open they stay
 * with Qt, as before, so a headset's Play never resumes another app over the live audio or a sheet's play.
 *
 * The codes are KeyEvent's, the same on every API level the app runs on (KeyEvent.isMediaSessionKey() is API 31+).
 * KEYCODE_MUTE, which that method also counts, mutes the microphone and stays with Qt. Plain Kotlin with no Android
 * imports, so the JVM tests build it without stubs.
 */
object MediaKeys {
    const val HEADSETHOOK = 79
    const val MEDIA_PLAY_PAUSE = 85
    const val MEDIA_STOP = 86
    const val MEDIA_NEXT = 87
    const val MEDIA_PREVIOUS = 88
    const val MEDIA_REWIND = 89
    const val MEDIA_FAST_FORWARD = 90
    const val MEDIA_PLAY = 126
    const val MEDIA_PAUSE = 127
    const val MEDIA_RECORD = 130

    private val SESSION_KEYS = setOf(
        HEADSETHOOK, MEDIA_PLAY_PAUSE, MEDIA_STOP, MEDIA_NEXT, MEDIA_PREVIOUS, MEDIA_REWIND, MEDIA_FAST_FORWARD,
        MEDIA_PLAY, MEDIA_PAUSE, MEDIA_RECORD,
    )

    /** Whether [keyCode] is one of the media session keys. */
    fun isSessionKey(keyCode: Int): Boolean = keyCode in SESSION_KEYS

    /**
     * Whether the key with [keyCode] goes to Android's media session routing instead of Qt: a media session key while
     * a History playlist's session is open ([playlistSessionOpen], RecordingPlayback.playlistSessionOpen()).
     */
    fun routedToSession(keyCode: Int, playlistSessionOpen: Boolean): Boolean =
        playlistSessionOpen && isSessionKey(keyCode)
}

/**
 * Which playlist the media session is active for, as the playback service last rendered it (#909). The service owns a
 * playlist (it is promoted) before the looper has started it and before the service has rendered its session active;
 * a key handed past Qt in between would reach no session of this app, and Android could give Play to the media app
 * whose session was active before. So the handoff waits for [rendered] with the session active for the owner, and
 * stops at the playlist's end, at a render with the session inactive, and when the session is released; another
 * playlist taking the service is not the one rendered until its own render. Any thread.
 */
class ActiveSession {
    private val lock = Any()
    private var generation = 0

    /** The service rendered playlist [generation], its session active or not ([active]). */
    fun rendered(generation: Int, active: Boolean) = synchronized(lock) {
        if (active) {
            this.generation = generation
        } else if (this.generation == generation) {
            this.generation = 0
        }
    }

    /** Playlist [generation] ended: its session no longer takes keys. */
    fun ended(generation: Int) = synchronized(lock) {
        if (this.generation == generation) {
            this.generation = 0
        }
    }

    /** The session was released with its service. */
    fun released() = synchronized(lock) {
        generation = 0
    }

    /** Whether the session is active for [owner], the playlist that owns the service now (0: none). */
    fun activeFor(owner: Int): Boolean = synchronized(lock) { owner != 0 && generation == owner }
}
