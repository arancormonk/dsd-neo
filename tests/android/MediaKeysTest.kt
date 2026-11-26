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

// #909: a headset tap or a Bluetooth Next with DSD-neo in front must reach an open playlist's media session, not Qt.
private fun mediaKeysGoToAnOpenPlaylistsSession() {
    for ((name, code) in sessionKeys) {
        check(MediaKeys.isSessionKey(code)) { "$name ($code) is a media session key" }
        check(MediaKeys.routedToSession(code, playlistSessionOpen = true)) {
            "$name ($code) must go to the playlist's media session, not Qt"
        }
    }
}

// With no playlist open the media keys stay with Qt, as before #909: a headset's Play must not resume another app over
// the live audio or take the focus from a sheet's play.
private fun mediaKeysStayWithQtWithNoPlaylist() {
    for ((name, code) in sessionKeys) {
        check(!MediaKeys.routedToSession(code, playlistSessionOpen = false)) {
            "$name ($code) must stay with Qt while no playlist is open"
        }
    }
}

// Typing, navigation, Back and the volume keys stay with Qt either way (the volume keys' own handling is Qt's and
// Android's).
private fun otherKeysStayWithQt() {
    for (open in listOf(true, false)) {
        for ((name, code) in qtKeys) {
            check(!MediaKeys.routedToSession(code, open)) { "$name ($code) must stay with Qt (playlist open: $open)" }
        }
        for (code in 0..300) {
            if (code !in sessionKeys.values) {
                check(!MediaKeys.isSessionKey(code) && !MediaKeys.routedToSession(code, open)) {
                    "key $code is not a media session key"
                }
            }
        }
    }
}

// #909: the playlist owns the service before its session is active; a key handed past Qt then would reach no session
// of this app. The handoff waits for the owner's session rendered active, and stops at its end and its replacement.
private fun keysWaitForTheOwnersActiveSession() {
    val session = ActiveSession()
    val play = MediaKeys.MEDIA_PLAY
    check(!MediaKeys.routedToSession(play, session.activeFor(1))) { "owner 1 promoted, not rendered: Qt keeps the key" }
    session.rendered(1, active = false)
    check(!session.activeFor(1)) { "rendered with the session inactive: still Qt's" }
    session.rendered(1, active = true)
    check(MediaKeys.routedToSession(play, session.activeFor(1))) { "the owner's session active: the session's" }
    check(!session.activeFor(0)) { "no owner, no handoff" }
    check(!session.activeFor(2)) { "playlist 2 takes the service: not before its own render" }
    session.rendered(2, active = true)
    check(session.activeFor(2) && !session.activeFor(1)) { "rendered for 2: 2's, never 1's again" }
    session.ended(1)
    check(session.activeFor(2)) { "an earlier playlist's end leaves 2's" }
    session.ended(2)
    check(!session.activeFor(2)) { "its own end clears it" }
    session.rendered(3, active = true)
    session.rendered(3, active = false)
    check(!session.activeFor(3)) { "a render with the session inactive clears it" }
    session.rendered(4, active = true)
    session.rendered(3, active = false)
    check(session.activeFor(4)) { "another playlist's inactive render leaves it" }
    session.released()
    check(!session.activeFor(4)) { "the session released with its service" }
}

fun main() {
    mediaKeysGoToAnOpenPlaylistsSession()
    mediaKeysStayWithQtWithNoPlaylist()
    otherKeysStayWithQt()
    keysWaitForTheOwnersActiveSession()
    println("MediaKeysTest: OK")
}
