// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

// KeyEvent's codes, as android.view.KeyEvent names them.
private val sessionKeys = mapOf(
    "KEYCODE_HEADSETHOOK" to 79,
    "KEYCODE_MEDIA_PLAY_PAUSE" to 85,
    "KEYCODE_MEDIA_STOP" to 86,
    "KEYCODE_MEDIA_NEXT" to 87,
    "KEYCODE_MEDIA_PREVIOUS" to 88,
    "KEYCODE_MEDIA_REWIND" to 89,
    "KEYCODE_MEDIA_FAST_FORWARD" to 90,
    "KEYCODE_MEDIA_PLAY" to 126,
    "KEYCODE_MEDIA_PAUSE" to 127,
    "KEYCODE_MEDIA_RECORD" to 130,
)

private val qtKeys = mapOf(
    "KEYCODE_BACK" to 4,
    "KEYCODE_DPAD_CENTER" to 23,
    "KEYCODE_VOLUME_UP" to 24,
    "KEYCODE_VOLUME_DOWN" to 25,
    "KEYCODE_A" to 29,
    "KEYCODE_TAB" to 61,
    "KEYCODE_ENTER" to 66,
    "KEYCODE_DEL" to 67,
    "KEYCODE_MUTE" to 91,
    "KEYCODE_ESCAPE" to 111,
    "KEYCODE_VOLUME_MUTE" to 164,
)

// #909: a headset tap or a Bluetooth Next with DSD-neo in front must reach the playlist's media session, never Qt.
private fun mediaKeysGoToTheSession() {
    for ((name, code) in sessionKeys) {
        check(MediaKeys.routedToSession(code)) { "$name ($code) must go to the media session, not Qt" }
    }
}

// Typing, navigation, Back and the volume keys stay with Qt (the volume keys' own handling is Qt's and Android's).
private fun otherKeysStayWithQt() {
    for ((name, code) in qtKeys) {
        check(!MediaKeys.routedToSession(code)) { "$name ($code) must stay with Qt" }
    }
    for (code in 0..300) {
        if (code !in sessionKeys.values) {
            check(!MediaKeys.routedToSession(code)) { "key $code is not a media session key" }
        }
    }
}

fun main() {
    mediaKeysGoToTheSession()
    otherKeysStayWithQt()
    println("MediaKeysTest: OK")
}
