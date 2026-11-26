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

// #909: a headset tap or a Bluetooth Next with DSD-neo in front must reach an open playlist's media session, not Qt:
// it is handed to that session itself, never left to Android's routing, which could pick another app's.
private fun mediaKeysGoToAnOpenPlaylistsSession() {
    for ((name, code) in sessionKeys) {
        check(MediaKeys.isSessionKey(code)) { "$name ($code) is a media session key" }
        check(MediaKeys.sessionFor(code, "playlist") == "playlist") {
            "$name ($code) must be handed to the playlist's own media session, not Qt"
        }
    }
}

// With no playlist open the media keys stay with Qt, as before #909: a headset's Play must not resume another app over
// the live audio or take the focus from a sheet's play.
private fun mediaKeysStayWithQtWithNoPlaylist() {
    for ((name, code) in sessionKeys) {
        check(MediaKeys.sessionFor<String>(code, null) == null) {
            "$name ($code) must stay with Qt while no playlist is open"
        }
    }
}

// Typing, navigation, Back and the volume keys stay with Qt either way (the volume keys' own handling is Qt's and
// Android's).
private fun otherKeysStayWithQt() {
    for (open in listOf(true, false)) {
        for ((name, code) in qtKeys) {
            check(MediaKeys.sessionFor(code, if (open) "playlist" else null) == null) {
                "$name ($code) must stay with Qt (playlist open: $open)"
            }
        }
        for (code in 0..300) {
            if (code !in sessionKeys.values) {
                val handed = MediaKeys.sessionFor(code, if (open) "playlist" else null)
                check(!MediaKeys.isSessionKey(code) && handed == null) {
                    "key $code is not a media session key"
                }
            }
        }
    }
}

// #909: the playlist owns the service before its session is active; a key handed past Qt then would reach no session
// of this app. The handoff waits for the owner's session rendered active, goes to that session (its token), and stops
// at its end and its replacement.
private fun keysWaitForTheOwnersActiveSession() {
    val session = ActiveSession<String>()
    val play = MediaKeys.MEDIA_PLAY
    check(MediaKeys.sessionFor(play, session.sessionFor(1)) == null) { "owner 1 promoted, not rendered: Qt's key" }
    session.rendered(1, active = false, "token A")
    check(session.sessionFor(1) == null) { "rendered with the session inactive: still Qt's" }
    session.rendered(1, active = true, "token A")
    check(MediaKeys.sessionFor(play, session.sessionFor(1)) == "token A") { "the owner's session active: handed to it" }
    check(session.sessionFor(0) == null) { "no owner, no handoff" }
    check(session.sessionFor(2) == null) { "playlist 2 takes the service: not before its own render" }
    session.rendered(2, active = true, "token B")
    check(session.sessionFor(2) == "token B" && session.sessionFor(1) == null) { "rendered for 2: never 1's again" }
    session.ended(1)
    check(session.sessionFor(2) == "token B") { "an earlier playlist's end leaves 2's" }
    session.ended(2)
    check(session.sessionFor(2) == null) { "its own end clears it" }
    session.rendered(3, active = true, "token B")
    session.rendered(3, active = false, "token B")
    check(session.sessionFor(3) == null) { "a render with the session inactive clears it" }
    session.rendered(4, active = true, "token C")
    session.rendered(3, active = false, "token B")
    check(session.sessionFor(4) == "token C") { "another playlist's inactive render leaves it" }
    session.released()
    check(session.sessionFor(4) == null) { "the session released with its service" }
}

fun main() {
    mediaKeysGoToAnOpenPlaylistsSession()
    mediaKeysStayWithQtWithNoPlaylist()
    otherKeysStayWithQt()
    keysWaitForTheOwnersActiveSession()
    println("MediaKeysTest: OK")
}
