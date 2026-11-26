// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * The media keys DsdNeoActivity hands past Qt (#909): a headset's button, a Bluetooth headset's or a car's transport
 * controls, a keyboard's media keys. Qt's activity consumes every key it is given, so with DSD-neo in front these
 * never reached a media session; left unhandled, Android delivers them to the media button session, which is the
 * History playlist's while one plays (and another app's otherwise, as for any app that does not use them).
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

    /** Whether the key with [keyCode] goes to Android's media session routing instead of Qt. */
    fun routedToSession(keyCode: Int): Boolean = keyCode in SESSION_KEYS
}
