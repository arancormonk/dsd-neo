// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.FAILED
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.PAUSED
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.PLAYING
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.PREPARING
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.STOPPED
import io.github.arancormonk.dsdneo.RecordingPlayer.FocusChange
import java.io.File
import java.io.FileNotFoundException

/** A MediaPlayer stand-in that refuses every call its state would not take, as the platform's throws. */
private class FakePlayer(val generation: Int, private val length: Long) : RecordingPlayer.Player {
    var path = ""
    var prepareFails: Exception? = null
    var asked = false
    var prepared = false
    var started = false
    var released = false
    val seeks = mutableListOf<Long>()
    val calls = mutableListOf<String>()

    private fun valid(call: String, ok: Boolean) {
        calls += call
        check(!released) { "$call on a released player" }
        check(ok) { "$call in a state the player does not take it in: ${calls.joinToString()}" }
    }

    override fun prepare(path: String) {
        valid("prepare", !asked)
        asked = true
        prepareFails?.let { throw it }
        this.path = path
    }

    override fun start() {
        valid("start", prepared && !started)
        started = true
    }

    override fun pause() {
        valid("pause", started)
        started = false
    }

    override fun seekTo(positionMs: Long) {
        valid("seek", prepared)
        seeks += positionMs
    }

    override val durationMs: Long
        get() {
            check(prepared) { "duration before prepared" }
            return length
        }

    override fun release() {
        check(!released) { "released twice" }
        released = true
    }
}

private class FakeFocus : RecordingPlayer.Focus {
    var grants = true
    var held = false
    var requests = 0
    var abandons = 0

    override fun request(): Boolean {
        requests++
        held = grants
        return grants
    }

    override fun abandon() {
        abandons++
        held = false
    }
}

private class Rig(foreground: Boolean = true) {
    var nowMs = 1_000L
    val players = mutableListOf<FakePlayer>()
    var nextPrepareFails: Exception? = null
    val focus = FakeFocus()
    val machine = RecordingPlayer(
        { generation -> FakePlayer(generation, 60_000).also { it.prepareFails = nextPrepareFails; players += it } },
        focus,
        { nowMs },
        foreground,
    )
    private var generation = 0

    val last: FakePlayer get() = players.last()
    val state: Int get() = machine.status.state
    val position: Long get() = machine.status.positionMs(nowMs)

    fun play(path: String = "/r/Explore/a.wav"): Int = (++generation).also { machine.play(it, path) }

    /** The player finished preparing, as MediaPlayer's callback says on the looper. */
    fun prepared(player: FakePlayer = last) {
        player.prepared = true
        machine.onPrepared(player.generation)
    }
}

private fun aPlayStartsOncePreparedAndHoldsTheFocus() {
    val rig = Rig()
    val generation = rig.play()
    check(rig.state == PREPARING && rig.last.path == "/r/Explore/a.wav") { "a play prepares its file" }
    check(rig.focus.requests == 0 && !rig.last.started) { "nothing starts, and no focus is asked, before it is prepared" }
    rig.prepared()
    check(rig.state == PLAYING && rig.last.started && rig.focus.held) { "prepared, it asks for the focus and plays" }
    check(rig.machine.status.generation == generation && rig.machine.status.durationMs == 60_000L) {
        "the status names the play and its length"
    }
    rig.nowMs += 1_500
    check(rig.position == 1_500L) { "the position runs while it plays: ${rig.position}" }
    check(rig.machine.status.record(rig.nowMs) == "$generation\t$PLAYING\t1500\t60000\t") {
        "the record is what the host parses: ${rig.machine.status.record(rig.nowMs)}"
    }

    rig.machine.pause()
    check(rig.state == PAUSED && !rig.last.started && !rig.focus.held) { "a pause pauses and gives the focus back" }
    rig.nowMs += 5_000
    check(rig.position == 1_500L) { "the position holds while paused" }
    rig.machine.resume()
    check(rig.state == PLAYING && rig.focus.requests == 2) { "a resume asks for the focus again and plays" }
    rig.nowMs += 70_000
    check(rig.position == 60_000L) { "the position never runs past the length" }
}

private fun aPauseOrTheBackgroundWhilePreparingOnlyChangesWhatIsWanted() {
    val paused = Rig()
    paused.play()
    paused.machine.pause()
    check(paused.state == PREPARING && paused.last.calls == listOf("prepare")) { "a pause while preparing calls nothing" }
    paused.prepared()
    check(paused.state == PAUSED && !paused.last.started && paused.focus.requests == 0) {
        "prepared after a pause, it waits paused, without the focus"
    }

    val away = Rig()
    away.play()
    away.machine.setForeground(false)
    check(away.state == PREPARING && away.last.calls == listOf("prepare")) { "leaving while preparing calls nothing" }
    away.prepared()
    check(away.state == PAUSED && !away.last.started && away.focus.requests == 0) { "prepared in the background, it waits" }
    away.machine.setForeground(true)
    check(away.state == PAUSED && !away.last.started) { "coming back does not start what the background paused" }
    away.machine.resume()
    check(away.state == PLAYING) { "the user resumes it" }

    // A focus change before any focus was asked for is not this player's.
    val early = Rig()
    early.play()
    early.machine.onFocusChange(FocusChange.LOST)
    early.machine.onFocusChange(FocusChange.GAINED)
    check(early.state == PREPARING && early.last.calls == listOf("prepare")) { "a focus change while preparing calls nothing" }
    early.prepared()
    check(early.state == PLAYING) { "and the play goes ahead once prepared" }
}

private fun leavingTheForegroundPausesForGood() {
    val rig = Rig()
    rig.play()
    rig.prepared()
    rig.nowMs += 2_000
    rig.machine.setForeground(false)
    check(rig.state == PAUSED && !rig.last.started && !rig.focus.held) { "leaving the foreground pauses (D10)" }
    rig.nowMs += 10_000
    check(rig.position == 2_000L) { "at the position it reached" }
    rig.machine.setForeground(true)
    check(rig.state == PAUSED) { "coming back leaves it paused" }

    // A resume that arrives after the app left (sent just before) waits for the foreground.
    rig.machine.setForeground(false)
    rig.machine.resume()
    check(rig.state == PAUSED && !rig.last.started && rig.focus.requests == 1) { "nothing plays in the background" }
    rig.machine.setForeground(true)
    check(rig.state == PLAYING) { "the resume plays once the app is back" }

    val background = Rig(foreground = false)
    background.play()
    background.prepared()
    check(background.state == PAUSED && background.focus.requests == 0) { "a player made in the background waits" }
}

private fun aDeniedFocusFailsThePlay() {
    val rig = Rig()
    rig.focus.grants = false
    rig.play()
    rig.prepared()
    check(rig.state == FAILED && rig.machine.status.error == RecordingPlayer.ERROR_UNAVAILABLE) {
        "a denied focus (a phone call) fails as unavailable"
    }
    check(!rig.last.started && rig.last.released) { "the player never starts and is released" }
    rig.machine.resume()
    check(rig.state == FAILED && rig.players.size == 1) { "a resume does not revive a failed play" }
    rig.focus.grants = true
    rig.play()
    rig.prepared()
    check(rig.state == PLAYING && rig.machine.status.error.isEmpty()) { "the next play clears the failure" }
}

private fun aFocusLossPausesAndATransientOneComesBack() {
    val rig = Rig()
    rig.play()
    rig.prepared()
    rig.machine.onFocusChange(FocusChange.LOST_TRANSIENT)
    check(rig.state == PAUSED && !rig.last.started && rig.focus.abandons == 0) { "a transient loss pauses, keeping the request" }
    rig.machine.resume()
    check(!rig.last.started && rig.state == PAUSED) { "a resume while the focus is away waits for it" }
    rig.machine.onFocusChange(FocusChange.GAINED)
    check(rig.state == PLAYING && rig.last.started && rig.focus.requests == 1) { "the focus back resumes it" }

    rig.machine.onFocusChange(FocusChange.LOST)
    check(rig.state == PAUSED && !rig.last.started && rig.focus.abandons == 1) { "a lasting loss pauses and abandons" }
    rig.machine.onFocusChange(FocusChange.GAINED)
    check(rig.state == PAUSED && !rig.last.started) { "a later gain does not restart it" }
}

private fun completionStopsAtTheEndAndReleases() {
    val rig = Rig()
    val generation = rig.play()
    rig.prepared()
    rig.machine.onCompleted(generation)
    val status = rig.machine.status
    check(status.state == STOPPED && status.generation == generation) { "a play that ends is Stopped, still this play's" }
    check(status.positionMs(rig.nowMs) == 60_000L && status.durationMs == 60_000L) { "at its end" }
    check(rig.last.released && !rig.focus.held) { "the player and the focus are let go" }
    rig.machine.onPrepared(generation)
    rig.machine.onFailed(generation, RecordingPlayer.ERROR_FAILED)
    check(rig.state == STOPPED) { "callbacks after the end change nothing" }
}

private fun aReplacedPlayersCallbacksAreDropped() {
    val rig = Rig()
    val first = rig.play("/r/a.wav")
    val old = rig.last
    val second = rig.play("/r/b.wav")
    check(old.released && rig.last.path == "/r/b.wav") { "a new play releases the old player" }
    old.prepared = true
    rig.machine.onPrepared(first)
    rig.machine.onCompleted(first)
    rig.machine.onFailed(first, RecordingPlayer.ERROR_FAILED)
    check(rig.state == PREPARING && rig.machine.status.generation == second) { "the old player's callbacks are dropped" }
    rig.prepared()
    check(rig.state == PLAYING) { "the new one plays" }

    // Replaced while it played: the focus is given back and asked again for the next.
    rig.play("/r/c.wav")
    check(!rig.focus.held && rig.focus.abandons == 1) { "the replaced play gives the focus back" }
}

private fun failuresSayWhy() {
    val rig = Rig()
    rig.nextPrepareFails = FileNotFoundException("/r/gone.wav")
    rig.play("/r/gone.wav")
    check(rig.state == FAILED && rig.machine.status.error == RecordingPlayer.ERROR_MISSING) { "a file gone is missing" }
    // The same exception for a file that is there (no access, a folder) is a failure, not a missing file.
    val there = File.createTempFile("dsd-player", ".wav")
    try {
        rig.nextPrepareFails = FileNotFoundException("${there.path} (Permission denied)")
        rig.play(there.path)
        check(rig.state == FAILED && rig.machine.status.error == RecordingPlayer.ERROR_FAILED) {
            "a file there that cannot be opened fails, it is not missing"
        }
    } finally {
        there.delete()
    }
    rig.nextPrepareFails = IllegalStateException("bad header")
    rig.play()
    check(rig.state == FAILED && rig.machine.status.error == RecordingPlayer.ERROR_FAILED) { "an unreadable file fails" }
    rig.nextPrepareFails = null
    val generation = rig.play()
    rig.prepared()
    rig.machine.onFailed(generation, RecordingPlayer.ERROR_FAILED)
    check(rig.state == FAILED && rig.last.released && !rig.focus.held) { "a player error fails and releases" }
}

private fun seeksWaitForThePlayerAndKeepTheClock() {
    val rig = Rig()
    rig.play()
    rig.machine.seek(12_000)
    check(rig.last.seeks.isEmpty()) { "a seek while preparing is held" }
    rig.prepared()
    check(rig.last.seeks == listOf(12_000L) && rig.position == 12_000L) { "and applied once prepared" }
    rig.nowMs += 1_000
    rig.machine.seek(30_000)
    check(rig.position == 30_000L) { "a seek while playing moves the position" }
    rig.nowMs += 500
    check(rig.position == 30_500L) { "which runs on from there" }
    rig.machine.seek(90_000)
    rig.machine.seek(-5)
    check(rig.last.seeks.takeLast(2) == listOf(60_000L, 0L)) { "seeks stay inside the recording" }
}

private fun aStopReleasesAndForgetsThePlay() {
    val rig = Rig()
    val generation = rig.play()
    rig.prepared()
    rig.machine.stop()
    check(rig.state == STOPPED && rig.machine.status.generation == 0) { "a stop is Stopped, no play's" }
    check(rig.last.released && !rig.focus.held && rig.focus.abandons == 1) { "a stop releases the player and the focus" }
    rig.machine.onCompleted(generation)
    rig.machine.resume()
    rig.machine.seek(5)
    check(rig.state == STOPPED && rig.players.size == 1) { "after a stop nothing revives it" }
    rig.machine.stop()
    check(rig.focus.abandons == 1) { "a second stop abandons nothing more" }
}

fun main() {
    aPlayStartsOncePreparedAndHoldsTheFocus()
    aPauseOrTheBackgroundWhilePreparingOnlyChangesWhatIsWanted()
    leavingTheForegroundPausesForGood()
    aDeniedFocusFailsThePlay()
    aFocusLossPausesAndATransientOneComesBack()
    completionStopsAtTheEndAndReleases()
    aReplacedPlayersCallbacksAreDropped()
    failuresSayWhy()
    seeksWaitForThePlayerAndKeepTheClock()
    aStopReleasesAndForgetsThePlay()
    println("PASS: the call recording player starts, pauses, follows the focus and the foreground, and ends as specified")
}
