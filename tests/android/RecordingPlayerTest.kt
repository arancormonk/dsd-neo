// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.ACTION_NEXT
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.ACTION_PAUSE
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.ACTION_PLAY
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.ACTION_PREVIOUS
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.ACTION_SEEK
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.ACTION_STOP
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.FAILED
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.MODE_PARTS
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.MODE_PLAYLIST
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.PAUSED
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.PLAYING
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.PREPARING
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.SESSION_NONE
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.SESSION_PAUSED
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.SESSION_PLAYING
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.SESSION_STOPPED
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.STOPPED
import io.github.arancormonk.dsdneo.RecordingPlayer.FocusChange
import io.github.arancormonk.dsdneo.RecordingPlayer.FocusMode
import io.github.arancormonk.dsdneo.RecordingPlayer.OpenResult

private class FakeSource(val path: String) : RecordingPlayer.Source {
    var closed = false

    override fun close() {
        check(!closed) { "a source closed twice" }
        closed = true
    }
}

/** A MediaPlayer stand-in that refuses every call its state would not take, as the platform's throws. */
private class FakePlayer(val token: Long, private val length: Long, private val damaged: Set<String>) :
    RecordingPlayer.Player {
    var path = ""
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

    override fun prepare(source: RecordingPlayer.Source) {
        valid("prepare", !asked)
        asked = true
        val file = source as FakeSource
        check(!file.closed) { "prepared from a closed source" }
        if (file.path in damaged) {
            throw IllegalStateException("bad header")
        }
        path = file.path
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

private class FakeOpener : RecordingPlayer.Opener {
    val refusals = HashMap<String, String>()
    val throwing = HashSet<String>()
    val opened = mutableListOf<String>()
    val sources = mutableListOf<FakeSource>()

    override fun open(item: RecordingPlayer.Item): OpenResult {
        opened += item.path
        if (item.path in throwing) {
            throw SecurityException("no access")
        }
        refusals[item.path]?.let { return OpenResult.Refused(it) }
        return OpenResult.Opened(FakeSource(item.path).also { sources += it })
    }
}

private class FakeFocus : RecordingPlayer.Focus {
    var grants = true
    var held = false
    val requests = mutableListOf<FocusMode>()
    var epoch = 0
    var abandons = 0

    override fun request(mode: FocusMode, epoch: Int): Boolean {
        requests += mode
        this.epoch = epoch
        held = grants
        return grants
    }

    override fun abandon() {
        abandons++
        held = false
    }
}

/** Tasks run when the test says, in due order; a cancelled one stays listed so a test can run it anyway. */
private class FakeScheduler(private val now: () -> Long) : RecordingPlayer.Scheduler {
    inner class Entry(val dueMs: Long, val delayMs: Long, val task: () -> Unit) {
        var cancelled = false
        var ran = false

        /** Run it whatever happened to it since, as a late or stale Handler message would. */
        fun force() {
            ran = true
            task()
        }
    }

    val entries = mutableListOf<Entry>()

    override fun post(delayMs: Long, task: () -> Unit): RecordingPlayer.Cancellable {
        val entry = Entry(now() + delayMs, delayMs, task)
        entries += entry
        return RecordingPlayer.Cancellable { entry.cancelled = true }
    }

    fun live(): List<Entry> = entries.filter { !it.cancelled && !it.ran }

    fun runDue() {
        while (true) {
            val entry = live().filter { it.dueMs <= now() }.minByOrNull { it.dueMs } ?: return
            entry.force()
        }
    }
}

private class FakeService : RecordingPlayer.ServiceLink {
    val changes = mutableListOf<RecordingPlayer.Status>()
    val ends = mutableListOf<Pair<Int, String>>()

    override fun changed(status: RecordingPlayer.Status) {
        changes += status
    }

    override fun ended(generation: Int, reason: String) {
        ends += generation to reason
    }
}

private class Rig(foreground: Boolean = true, var length: Long = 60_000) {
    var nowMs = 1_000L
    val players = mutableListOf<FakePlayer>()
    val damaged = HashSet<String>()
    var factoryFails = false
    val opener = FakeOpener()
    val focus = FakeFocus()
    val scheduler = FakeScheduler { nowMs }
    val service = FakeService()
    val recheck = RecordingRecheck()
    val machine = RecordingPlayer(
        { token ->
            if (factoryFails) {
                throw RuntimeException("the media server is away")
            }
            FakePlayer(token, length, damaged).also { players += it }
        },
        opener,
        focus,
        { nowMs },
        scheduler,
        service,
        recheck,
        foreground,
    )
    var generation = 0

    val status: RecordingPlayer.Status get() = machine.status
    val state: Int get() = machine.status.state
    val position: Long get() = machine.status.positionMs(nowMs)
    val last: FakePlayer get() = players.last()

    /** The player the status names. */
    val current: FakePlayer get() = players.last { it.token == status.playToken }

    fun play(path: String = "/r/Explore/a.wav"): Int = (++generation).also { machine.play(it, path) }

    /** Play calls of [parts] parts each (paths from [path]), in [mode], promoted at once unless [promote] is false. */
    fun queue(vararg parts: Int, mode: Int = MODE_PLAYLIST, start: Int = 0, promote: Boolean = true): Int {
        val items = parts.withIndex().flatMap { (call, count) ->
            (1..count).map { part ->
                RecordingPlayer.Item(path(call, part), 100, 1_700_000_000, 5, call, part, count, "k$call", "Call $call", "")
            }
        }
        val generation = ++generation
        machine.play(generation, mode, items, start)
        if (promote && mode == MODE_PLAYLIST) {
            machine.onPromoted(generation)
        }
        return generation
    }

    /** The newest player finished preparing, as MediaPlayer's callback says on the looper. */
    fun prepared(player: FakePlayer = last) {
        player.prepared = true
        machine.onPrepared(player.token)
    }

    /** The current player played to its end. */
    fun complete(player: FakePlayer = current) {
        player.started = false
        machine.onCompleted(player.token)
    }

    /** Prepare and complete the current part. */
    fun playOut() {
        prepared()
        complete()
    }

    fun turn() = scheduler.runDue()

    fun elapse(ms: Long) {
        nowMs += ms
        scheduler.runDue()
    }

    fun fail(path: String, reason: String) {
        opener.refusals[path] = reason
    }
}

private fun path(call: Int, part: Int) = "/r/c${call}p$part.wav"

// ---- SINGLE (#708's player; its readings unchanged)

private fun aPlayStartsOncePreparedAndHoldsTheFocus() {
    val rig = Rig()
    val generation = rig.play()
    check(rig.state == PREPARING && rig.opener.opened == listOf("/r/Explore/a.wav")) { "a play opens its file" }
    check(rig.last.asked && rig.opener.sources.single().closed) { "the player takes the file and the source is closed" }
    check(rig.focus.requests.isEmpty() && !rig.last.started) { "nothing starts, and no focus is asked, before it is prepared" }
    rig.prepared()
    check(rig.state == PLAYING && rig.last.started && rig.focus.held) { "prepared, it asks for the focus and plays" }
    check(rig.focus.requests == listOf(FocusMode.GAIN_TRANSIENT)) { "a single recording takes the transient focus" }
    check(rig.status.generation == generation && rig.status.durationMs == 60_000L) { "the status names the play and its length" }
    rig.nowMs += 1_500
    check(rig.position == 1_500L) { "the position runs while it plays: ${rig.position}" }
    val token = rig.status.playToken
    val record = "$generation\t$PLAYING\t1500\t60000\t\t0\t$token\t0\t1\t1\t1\t0\t0\t1\t\t\t0\t0"
    check(rig.status.record(rig.nowMs) == record) { "the record is what the host parses: ${rig.status.record(rig.nowMs)}" }
    check(rig.status.record(rig.nowMs).split('\t').size == 18) { "the record has its 18 fields" }

    rig.machine.pause()
    check(rig.state == PAUSED && !rig.last.started && !rig.focus.held) { "a pause pauses and gives the focus back" }
    check(rig.status.pausedReason == RecordingPlayer.PAUSED_USER) { "the pause says it was the user's" }
    rig.nowMs += 5_000
    check(rig.position == 1_500L) { "the position holds while paused" }
    rig.machine.resume()
    check(rig.state == PLAYING && rig.focus.requests.size == 2) { "a resume asks for the focus again and plays" }
    rig.nowMs += 70_000
    check(rig.position == 60_000L) { "the position never runs past the length" }
    check(rig.service.changes.isEmpty() && rig.scheduler.entries.isEmpty()) { "a single play uses no service and no task" }
}

private fun aPauseOrTheBackgroundWhilePreparingOnlyChangesWhatIsWanted() {
    val paused = Rig()
    paused.play()
    paused.machine.pause()
    check(paused.state == PREPARING && paused.last.calls == listOf("prepare")) { "a pause while preparing calls nothing" }
    paused.prepared()
    check(paused.state == PAUSED && !paused.last.started && paused.focus.requests.isEmpty()) {
        "prepared after a pause, it waits paused, without the focus"
    }

    val away = Rig()
    away.play()
    away.machine.setForeground(false)
    check(away.state == PREPARING && away.last.calls == listOf("prepare")) { "leaving while preparing calls nothing" }
    away.prepared()
    check(away.state == PAUSED && !away.last.started && away.focus.requests.isEmpty()) { "prepared in the background, it waits" }
    check(away.status.pausedReason == RecordingPlayer.PAUSED_BACKGROUND) { "the pause says the app left" }
    away.machine.setForeground(true)
    check(away.state == PAUSED && !away.last.started) { "coming back does not start what the background paused" }
    away.machine.resume()
    check(away.state == PLAYING) { "the user resumes it" }

    // A focus change before any focus was asked for is not this player's.
    val early = Rig()
    early.play()
    early.machine.onFocusChange(0, FocusChange.LOST)
    early.machine.onFocusChange(0, FocusChange.GAINED)
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
    check(rig.state == PAUSED && !rig.last.started && rig.focus.requests.size == 1) { "nothing plays in the background" }
    rig.machine.setForeground(true)
    check(rig.state == PLAYING) { "the resume plays once the app is back" }

    val background = Rig(foreground = false)
    background.play()
    background.prepared()
    check(background.state == PAUSED && background.focus.requests.isEmpty()) { "a player made in the background waits" }
}

private fun aDeniedFocusFailsThePlay() {
    val rig = Rig()
    rig.focus.grants = false
    rig.play()
    rig.prepared()
    check(rig.state == FAILED && rig.status.error == RecordingPlayer.ERROR_UNAVAILABLE) {
        "a denied focus (a phone call) fails as unavailable"
    }
    check(rig.status.endedReason == RecordingPlayer.ENDED_FAILED) { "a failed play reads ended, failed" }
    check(!rig.last.started && rig.last.released) { "the player never starts and is released" }
    rig.machine.resume()
    check(rig.state == FAILED && rig.players.size == 1) { "a resume does not revive a failed play" }
    rig.focus.grants = true
    rig.play()
    rig.prepared()
    check(rig.state == PLAYING && rig.status.error.isEmpty() && rig.status.endedReason.isEmpty()) {
        "the next play clears the failure"
    }
}

private fun aFocusLossPausesAndATransientOneComesBack() {
    val rig = Rig()
    rig.play()
    rig.prepared()
    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.LOST_TRANSIENT)
    check(rig.state == PAUSED && !rig.last.started && rig.focus.abandons == 0) { "a transient loss pauses, keeping the request" }
    check(rig.status.interrupted && rig.status.pausedReason.isEmpty()) { "it reads interrupted, still wanted (#909)" }
    rig.machine.resume()
    check(!rig.last.started && rig.state == PAUSED) { "a resume while the focus is away waits for it" }
    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.GAINED)
    check(rig.state == PLAYING && rig.last.started && rig.focus.requests.size == 1) { "the focus back resumes it" }

    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.LOST)
    check(rig.state == PAUSED && !rig.last.started && rig.focus.abandons == 1) { "a lasting loss pauses and abandons" }
    check(rig.status.pausedReason == RecordingPlayer.PAUSED_FOCUS) { "the pause says another app took the focus" }
    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.GAINED)
    check(rig.state == PAUSED && !rig.last.started) { "a later gain does not restart it" }
}

private fun completionStopsAtTheEndAndReleases() {
    val rig = Rig()
    val generation = rig.play()
    rig.prepared()
    val player = rig.last
    rig.complete()
    val status = rig.status
    check(status.state == STOPPED && status.generation == generation) { "a play that ends is Stopped, still this play's" }
    check(status.positionMs(rig.nowMs) == 60_000L && status.durationMs == 60_000L) { "at its end" }
    check(status.endedReason == RecordingPlayer.ENDED_FINISHED && status.error.isEmpty()) { "it reads finished" }
    check(player.released && !rig.focus.held) { "the player and the focus are let go" }
    rig.machine.onPrepared(player.token)
    rig.machine.onFailed(player.token, RecordingPlayer.REASON_DAMAGED)
    check(rig.state == STOPPED && rig.status.endedReason == RecordingPlayer.ENDED_FINISHED) { "callbacks after the end change nothing" }
}

private fun aReplacedPlayersCallbacksAreDropped() {
    val rig = Rig()
    val first = rig.play("/r/a.wav")
    val old = rig.last
    val second = rig.play("/r/b.wav")
    check(first != second && old.released && rig.last.path == "/r/b.wav") { "a new play releases the old player" }
    check(old.token != rig.last.token) { "each prepare has its own token" }
    old.prepared = true
    rig.machine.onPrepared(old.token)
    rig.machine.onCompleted(old.token)
    rig.machine.onFailed(old.token, RecordingPlayer.REASON_DAMAGED)
    check(rig.state == PREPARING && rig.status.generation == second) { "the old player's callbacks are dropped" }
    rig.prepared()
    check(rig.state == PLAYING) { "the new one plays" }

    // Replaced while it played: the focus is given back and asked again for the next.
    rig.play("/r/c.wav")
    check(!rig.focus.held && rig.focus.abandons == 1) { "the replaced play gives the focus back" }
}

private fun failuresSayWhy() {
    val rig = Rig()
    rig.fail("/r/gone.wav", RecordingPlayer.REASON_GONE)
    rig.play("/r/gone.wav")
    check(rig.state == FAILED && rig.status.error == RecordingPlayer.ERROR_MISSING) { "a file gone is missing" }
    check(rig.players.isEmpty()) { "no player is built for a file that cannot be opened" }
    // A file there that cannot be opened (no access, a folder) is a failure, not a missing file.
    rig.fail("/r/locked.wav", RecordingPlayer.REASON_CHANGED)
    rig.play("/r/locked.wav")
    check(rig.state == FAILED && rig.status.error == RecordingPlayer.ERROR_FAILED) { "a file there that cannot be opened fails" }
    rig.opener.throwing += "/r/odd.wav"
    rig.play("/r/odd.wav")
    check(rig.state == FAILED && rig.status.error == RecordingPlayer.ERROR_FAILED) { "an open that throws fails" }
    rig.damaged += "/r/bad.wav"
    rig.play("/r/bad.wav")
    check(rig.state == FAILED && rig.status.error == RecordingPlayer.ERROR_FAILED) { "an unreadable file fails" }
    check(rig.last.released && rig.opener.sources.last().closed) { "its player is released and its source closed" }
    check(rig.recheck.take().isEmpty()) { "a single play's failures keep the existing missing path, not the recheck list" }
    rig.play()
    rig.prepared()
    rig.machine.onFailed(rig.last.token, RecordingPlayer.ERROR_FAILED)
    check(rig.state == FAILED && rig.last.released && !rig.focus.held) { "a player error fails and releases" }
}

private fun aPlayerThatCannotBeBuiltFailsThePlay() {
    // The native constructor can throw; inside the guard it is a failed play, not a crashed looper (#909).
    val single = Rig()
    single.factoryFails = true
    single.play()
    check(single.state == FAILED && single.status.error == RecordingPlayer.ERROR_FAILED) { "a player that cannot be built fails" }
    check(single.opener.sources.single().closed && !single.focus.held) { "its source is closed, nothing held" }

    val playlist = Rig()
    playlist.factoryFails = true
    playlist.queue(1, 1, 1)
    check(playlist.state == FAILED && playlist.status.endedReason == RecordingPlayer.ENDED_FAILED) {
        "a playlist whose player cannot be built ends failed"
    }
    playlist.turn()
    check(playlist.opener.opened == listOf(path(0, 1)) && playlist.status.skipped == 0) { "with no skip: it is not the file's fault" }
    check(playlist.service.ends.single() == playlist.generation to RecordingPlayer.ENDED_FAILED) { "the service is told once" }

    // The media server dying under a playing part is the player's fault too.
    val died = Rig()
    died.queue(2, 1)
    died.prepared()
    died.machine.onFailed(died.current.token, RecordingPlayer.REASON_PLAYER)
    died.turn()
    check(died.state == FAILED && died.status.error == RecordingPlayer.ERROR_FAILED && died.players.size == 1) {
        "a player-wide error ends the playlist without trying the next part"
    }
    check(died.focus.abandons == 1 && died.players.all { it.released }) { "everything let go once" }
}

private fun seeksFollowTheTableAndKeepTheClock() {
    val rig = Rig()
    rig.play()
    rig.machine.seek(12_000)
    check(!rig.status.canSeek && rig.last.seeks.isEmpty()) { "a seek while opening is dropped (the table: opening, no seek)" }
    rig.prepared()
    check(rig.status.canSeek && rig.position == 0L) { "prepared, it plays from the start and can seek" }
    rig.machine.seek(12_000)
    check(rig.last.seeks == listOf(12_000L) && rig.position == 12_000L) { "a seek while playing moves the position" }
    rig.nowMs += 500
    check(rig.position == 12_500L) { "which runs on from there" }
    rig.machine.seek(90_000)
    rig.machine.seek(-5)
    check(rig.last.seeks.takeLast(2) == listOf(60_000L, 0L)) { "seeks stay inside the recording" }
    rig.machine.pause()
    rig.machine.seek(30_000)
    check(rig.position == 30_000L && rig.state == PAUSED) { "a paused recording seeks too" }
}

private fun aStopReleasesAndForgetsThePlay() {
    val rig = Rig()
    rig.play()
    rig.prepared()
    val player = rig.last
    rig.machine.stop()
    check(rig.state == STOPPED && rig.status.generation == 0) { "a stop is Stopped, no play's" }
    check(rig.status.itemIndex == -1 && rig.status.calls == 0 && rig.status.endedReason.isEmpty()) { "with no item" }
    check(player.released && !rig.focus.held && rig.focus.abandons == 1) { "a stop releases the player and the focus" }
    rig.machine.onCompleted(player.token)
    rig.machine.resume()
    rig.machine.seek(5)
    check(rig.state == STOPPED && rig.players.size == 1) { "after a stop nothing revives it" }
    rig.machine.stop()
    check(rig.focus.abandons == 1) { "a second stop abandons nothing more" }
}

// ---- PARTS (Play all parts)

private fun partsPlayThroughHoldingOneTransientFocus() {
    // One GAIN_TRANSIENT request across the parts, never abandoned and asked again between them (#909).
    val rig = Rig()
    rig.queue(3, mode = MODE_PARTS)
    check(rig.service.changes.isEmpty()) { "Play all parts has no service" }
    for (part in 1..3) {
        check(rig.last.path == path(0, part) && rig.status.partOrdinal == part && rig.status.partCount == 3) { "part $part" }
        rig.playOut()
    }
    check(rig.state == STOPPED && rig.status.endedReason == RecordingPlayer.ENDED_FINISHED) { "it finishes after the last part" }
    check(rig.focus.requests == listOf(FocusMode.GAIN_TRANSIENT) && rig.focus.abandons == 1) { "one request, one abandon" }
    check(rig.scheduler.entries.isEmpty()) { "no gap between parts" }

    // A part MediaPlayer reports no length for still advances on completion (#909).
    val zero = Rig(length = 0)
    zero.queue(2, mode = MODE_PARTS)
    zero.prepared()
    check(!zero.status.canSeek && zero.status.actions and ACTION_SEEK == 0) { "no seek for a part of no length" }
    zero.complete()
    check(zero.last.path == path(0, 2) && zero.state == PREPARING) { "completion advances whatever the duration" }

    // D10 holds for Play all parts.
    val away = Rig()
    away.queue(2, mode = MODE_PARTS)
    away.prepared()
    away.machine.setForeground(false)
    check(away.state == PAUSED && away.status.pausedReason == RecordingPlayer.PAUSED_BACKGROUND) { "leaving pauses it" }
}

private fun aFailedPartEndsPlayAllParts() {
    val gone = Rig()
    gone.fail(path(0, 2), RecordingPlayer.REASON_GONE)
    gone.queue(3, mode = MODE_PARTS)
    gone.playOut()
    check(gone.state == FAILED && gone.status.error == RecordingPlayer.ERROR_MISSING) { "a part gone ends it as missing" }
    check(gone.status.partOrdinal == 2) { "on the part that failed" }
    gone.turn()
    check(gone.opener.opened.size == 2) { "nothing after it is tried" }
    check(gone.recheck.take().toList() == listOf(path(0, 2))) { "a part gone goes on the recheck list" }

    val changed = Rig()
    changed.fail(path(0, 1), RecordingPlayer.REASON_CHANGED)
    changed.queue(2, mode = MODE_PARTS)
    check(changed.state == FAILED && changed.status.error == RecordingPlayer.ERROR_FAILED) { "a part changed ends it as failed" }
    check(changed.recheck.take().toList() == listOf(path(0, 1))) { "and is rechecked" }

    val damaged = Rig()
    damaged.queue(2, mode = MODE_PARTS)
    damaged.prepared()
    damaged.machine.onFailed(damaged.current.token, RecordingPlayer.REASON_DAMAGED)
    check(damaged.state == FAILED && damaged.recheck.take().isEmpty()) { "a damaged part ends it; nothing to recheck" }
}

// ---- PLAYLIST: order, gap, moves

private fun aPlaylistPlaysCallsAndPartsInOrderWithAGapBetweenCalls() {
    val rig = Rig()
    val generation = rig.queue(2, 1, 3)
    val order = mutableListOf<String>()
    fun part(call: Int, part: Int) {
        check(rig.last.path == path(call, part)) { "call $call part $part opens, not ${rig.last.path}" }
        check(rig.status.call == call && rig.status.calls == 3 && rig.status.partOrdinal == part) { "the status names it" }
        order += rig.last.path
        rig.prepared()
        check(rig.state == PLAYING) { "it plays" }
        rig.nowMs += 2_000
        rig.complete()
    }
    part(0, 1)
    check(!rig.status.gap && rig.last.path == path(0, 2)) { "the next part opens at once, no gap" }
    part(0, 2)
    check(rig.status.gap && rig.state == PLAYING && rig.players.size == 2) { "a call's last part: the gap, nothing opened" }
    rig.elapse(999)
    check(rig.players.size == 2) { "the gap lasts a second" }
    rig.elapse(1)
    part(1, 1)
    rig.elapse(1_000)
    part(2, 1)
    part(2, 2)
    part(2, 3)
    check(order == listOf(path(0, 1), path(0, 2), path(1, 1), path(2, 1), path(2, 2), path(2, 3))) { "oldest first" }
    check(rig.state == STOPPED && rig.status.endedReason == RecordingPlayer.ENDED_FINISHED) { "it finishes" }
    check(rig.status.generation == generation && rig.status.call == 2) { "on its last call" }
    check(rig.scheduler.live().isEmpty()) { "no gap after the last call" }
    check(rig.focus.requests == listOf(FocusMode.GAIN) && rig.focus.abandons == 1) { "GAIN held across it, abandoned once" }
    check(rig.players.all { it.released }) { "every player let go" }
    check(rig.service.ends == listOf(generation to RecordingPlayer.ENDED_FINISHED)) { "the service is told once" }
}

private fun theGapReadsFrozenAndPausesWithIt() {
    val rig = Rig()
    rig.queue(1, 1)
    rig.prepared()
    val finished = rig.current
    rig.complete()
    val status = rig.status
    check(status.gap && status.state == PLAYING && status.sessionState == SESSION_PLAYING && status.speed == 0f) {
        "the gap reads playing, speed 0"
    }
    rig.nowMs += 600
    check(rig.position == 60_000L && !status.canSeek && status.actions and ACTION_SEEK == 0) {
        "its position frozen at the finished part's end, no seek"
    }
    rig.machine.seek(1_000)
    check(finished.seeks.isEmpty()) { "a seek in the gap is dropped" }
    rig.machine.pause()
    check(rig.status.gap && rig.state == PAUSED && rig.status.sessionState == SESSION_PAUSED) { "paused in the gap" }
    check(rig.status.actions and ACTION_PLAY != 0 && !rig.focus.held) { "it offers Play; the focus goes back" }
    rig.elapse(60_000)
    check(rig.players.size == 1) { "a paused gap does not run out" }
    rig.machine.resume()
    check(!finished.started && finished.seeks.isEmpty()) { "Play does not replay the finished part" }
    check(rig.last.path == path(1, 1) && !rig.status.gap && rig.focus.requests.size == 2) {
        "Play in a paused gap opens the next call at once, the gap not resumed"
    }
    check(rig.scheduler.live().none { it.delayMs > 0 }) { "no gap task is left" }
    rig.prepared()
    check(rig.state == PLAYING) { "and plays it" }
}

private fun nextOpensTheNextCallAtOnceAlsoFromTheGap() {
    val rig = Rig()
    rig.queue(2, 1, 1)
    rig.prepared()
    rig.machine.next()
    check(rig.last.path == path(1, 1)) { "Next skips the call's other parts" }
    rig.prepared()
    rig.complete()
    val gapTask = rig.scheduler.live().single()
    rig.machine.next()
    check(rig.last.path == path(2, 1) && !rig.status.gap) { "Next in the gap opens the next call at once" }
    gapTask.force()
    check(rig.players.size == 3 && rig.last.path == path(2, 1)) { "the gap's task from before the skip is dropped" }
    rig.prepared()
    check(rig.status.actions and ACTION_NEXT == 0) { "no Next at the last call" }
    rig.machine.next()
    check(rig.state == PLAYING && rig.players.size == 3) { "a Next at the last call is dropped" }
}

private fun nextPastOnlyFailedCallsFinishes() {
    // A, B good; C gone and D damaged while it plays: Previous from D, before its skip turn, lands on B.
    val rig = Rig()
    rig.fail(path(2, 1), RecordingPlayer.REASON_GONE)
    val generation = rig.queue(1, 1, 1, 1)
    rig.prepared()
    rig.machine.next()
    rig.prepared()
    rig.machine.next()
    rig.turn()
    rig.prepared()
    check(rig.status.call == 3 && rig.status.skipped == 1) { "C skipped, D plays" }
    rig.machine.onFailed(rig.current.token, RecordingPlayer.REASON_DAMAGED)
    rig.machine.previous()
    rig.prepared()
    check(rig.status.call == 1 && rig.state == PLAYING && rig.status.skipped == 2) { "on B, C and D failed" }
    check(rig.status.actions and ACTION_NEXT == 0) { "the session offers no Next" }
    val b = rig.current
    rig.machine.next()
    check(rig.state == STOPPED && rig.status.endedReason == RecordingPlayer.ENDED_FINISHED) {
        "Next with only failed calls ahead finishes, as the advance at the end does"
    }
    check(rig.status.call == 1 && b.released && rig.opener.opened.count { it == path(2, 1) } == 1) {
        "on the call that played, nothing failed tried again"
    }
    check(rig.service.ends == listOf(generation to RecordingPlayer.ENDED_FINISHED)) { "the service is told once" }
}

private fun previousFollowsTheThreeSecondRule() {
    // At 3,000 ms on a call's first part: the previous call.
    val at = Rig()
    at.queue(1, 1)
    at.playOut()
    at.elapse(1_000)
    at.prepared()
    at.nowMs += 3_000
    val open = at.current
    at.machine.previous()
    check(at.last.path == path(0, 1) && at.status.call == 0) { "at 3,000 ms Previous goes to the previous call" }
    check(!open.released && !open.started) { "the open player is kept until the new one opens" }
    at.prepared()
    check(open.released && at.state == PLAYING) { "and let go once it has" }

    // At 3,001 ms: a restart, a seek to 0 on the open player, never a reopen.
    val past = Rig()
    past.queue(1, 1)
    past.playOut()
    past.elapse(1_000)
    past.prepared()
    past.nowMs += 3_001
    val playing = past.current
    val token = past.status.playToken
    past.machine.previous()
    check(past.players.size == 2 && playing.seeks == listOf(0L) && past.position == 0L) { "past 3,000 ms it restarts" }
    check(past.state == PLAYING && playing.started && past.status.playToken == token) { "on the same player, playing" }

    // A later part: restart the call on its first part.
    val part = Rig()
    part.queue(1, 2)
    part.playOut()
    part.elapse(1_000)
    part.playOut()
    part.prepared()
    val second = part.current
    part.machine.previous()
    check(part.last.path == path(1, 1) && !second.released) { "Previous on part 2 restarts the call at its first part" }

    // The first call restarts.
    val first = Rig()
    first.queue(1, 1)
    first.prepared()
    first.nowMs += 1_000
    first.machine.previous()
    check(first.players.size == 1 && first.current.seeks == listOf(0L) && first.state == PLAYING) {
        "Previous at the first call restarts it"
    }

    // In the gap: restart the call just heard.
    val gap = Rig()
    gap.queue(1, 1)
    gap.prepared()
    val heard = gap.current
    gap.complete()
    gap.machine.previous()
    check(!gap.status.gap && gap.players.size == 1 && heard.seeks == listOf(0L) && heard.started) {
        "Previous in the gap restarts the call just heard, on its open player"
    }
    gap.elapse(5_000)
    check(gap.players.size == 1) { "the gap is over" }

    val parts = Rig()
    parts.queue(2, 1)
    parts.playOut()
    parts.playOut()
    parts.machine.previous()
    check(parts.last.path == path(0, 1) && parts.players.size == 3) { "a call of parts restarts on its first part" }
}

private fun movesWhilePausedStayPausedAndKeepTheDeadline() {
    val rig = Rig()
    val generation = rig.queue(1, 1, 1)
    rig.prepared()
    rig.machine.pause()
    rig.elapse(240_000)
    rig.machine.next()
    rig.prepared()
    check(rig.state == PAUSED && !rig.last.started && rig.status.call == 1) { "Next while paused moves and stays paused" }
    rig.machine.previous()
    rig.prepared()
    check(rig.state == PAUSED && rig.status.call == 0 && rig.focus.requests.size == 1) { "Previous too, without the focus" }
    rig.elapse(359_999)
    check(rig.state == PAUSED) { "not yet ten minutes from the pause" }
    rig.elapse(1)
    check(rig.state == STOPPED && rig.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) {
        "ten minutes from the pause it ends: navigation did not extend it"
    }
    check(rig.service.ends == listOf(generation to RecordingPlayer.ENDED_TIMEOUT)) { "the service is told" }
}

/** A resume pressed while the next part opens carries on only once the focus is granted: a refusal keeps the clock. */
private fun aResumeWaitingForTheFocusKeepsTheDeadline() {
    val refused = Rig()
    val generation = refused.queue(1, 1)
    refused.prepared()
    refused.machine.pause()
    refused.elapse(599_000)
    refused.machine.next()
    refused.machine.resume()
    check(refused.state == PREPARING && refused.status.call == 1) { "Resume while the next call opens" }
    refused.focus.grants = false
    refused.prepared()
    check(refused.state == PAUSED && refused.status.pausedReason == RecordingPlayer.PAUSED_UNAVAILABLE) {
        "the focus refused once it prepared: paused"
    }
    refused.elapse(999)
    check(refused.state == PAUSED) { "not yet ten minutes from the pause" }
    refused.elapse(1)
    check(refused.state == STOPPED && refused.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) {
        "ten minutes from the pause it ends: the refused resume started no fresh ten minutes"
    }
    check(refused.service.ends == listOf(generation to RecordingPlayer.ENDED_TIMEOUT)) { "the service is told" }

    // Granted, the stretch ends there.
    val granted = Rig()
    granted.queue(1, 1)
    granted.prepared()
    granted.machine.pause()
    granted.elapse(599_000)
    granted.machine.next()
    granted.machine.resume()
    granted.prepared()
    check(granted.state == PLAYING) { "the focus granted, it plays" }
    granted.elapse(600_000)
    check(granted.state == PLAYING && granted.scheduler.live().isEmpty()) { "playing again ended the paused stretch" }
}

// ---- identities

private fun lateCallbacksTasksAndFocusAreDropped() {
    // A late completion after Next.
    val rig = Rig()
    rig.queue(1, 1, 1)
    rig.prepared()
    val a = rig.current
    rig.machine.next()
    rig.machine.onCompleted(a.token)
    rig.machine.onFailed(a.token, RecordingPlayer.REASON_DAMAGED)
    check(rig.last.path == path(1, 1) && rig.state == PREPARING && rig.status.skipped == 0) { "A's callbacks after Next are dropped" }
    rig.prepared()
    val b = rig.current
    rig.nowMs += 500
    rig.machine.previous()
    rig.machine.onPrepared(b.token)
    rig.machine.onCompleted(b.token)
    check(rig.status.call == 0 && rig.state == PREPARING) { "B's after Previous too" }
    rig.prepared()
    val stale = rig.current
    rig.machine.stop(rig.generation)
    rig.machine.onCompleted(stale.token)
    check(rig.status.endedReason == RecordingPlayer.ENDED_STOPPED) { "after Stop too" }
    rig.queue(1)
    rig.machine.onPrepared(stale.token)
    check(rig.state == PREPARING) { "and after a replacement" }

    // A deadline from an earlier pause.
    val pauses = Rig()
    pauses.queue(1, 1)
    pauses.prepared()
    pauses.machine.pause()
    val firstDeadline = pauses.scheduler.live().single()
    pauses.elapse(300_000)
    pauses.machine.resume()
    pauses.machine.pause()
    pauses.nowMs += 300_000
    val secondDeadline = pauses.scheduler.live().single()
    firstDeadline.force()
    check(pauses.state == PAUSED && pauses.scheduler.live() == listOf(secondDeadline)) {
        "an earlier pause's deadline task is dropped: it neither ends the play nor waits again"
    }
    pauses.machine.resume()
    firstDeadline.force()
    secondDeadline.force()
    check(pauses.state == PLAYING && pauses.scheduler.live().isEmpty()) { "after a resume neither deadline acts" }
    pauses.machine.pause()
    pauses.elapse(600_000)
    check(pauses.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) { "the third pause's runs out" }

    // A focus change for an earlier request.
    val epochs = Rig()
    epochs.queue(1, 1)
    epochs.prepared()
    val firstEpoch = epochs.focus.epoch
    epochs.machine.pause()
    epochs.machine.resume()
    check(epochs.focus.epoch != firstEpoch) { "each request has its epoch" }
    epochs.machine.onFocusChange(firstEpoch, FocusChange.LOST)
    epochs.machine.onFocusChange(firstEpoch, FocusChange.LOST_TRANSIENT)
    check(epochs.state == PLAYING && !epochs.status.interrupted) { "an old request's changes are dropped" }
}

private fun theServiceIsPromotedOnceInEitherOrder() {
    // Promoted after the play: it prepares, but asks for no focus and starts nothing until then.
    val after = Rig()
    val g = after.queue(1, 1, promote = false)
    after.prepared()
    check(after.state == PREPARING && after.status.sessionState == SESSION_NONE && after.focus.requests.isEmpty()) {
        "before the service is promoted it reads opening, with no session state"
    }
    check(after.status.actions and ACTION_PAUSE != 0 && !after.status.canSeek) { "Pause, no seek" }
    after.machine.onPromoted(g)
    check(after.state == PLAYING && after.focus.requests == listOf(FocusMode.GAIN)) { "promoted, it plays" }
    after.machine.onPromoted(g)
    after.elapse(10_000)
    check(after.focus.requests.size == 1 && after.state == PLAYING) { "exactly once; the timeout was cancelled" }

    // Promoted before the play.
    val before = Rig()
    before.machine.onPromoted(1)
    before.queue(1, promote = false)
    before.prepared()
    check(before.state == PLAYING && before.scheduler.live().isEmpty()) { "a promotion that came first counts" }

    // Refused, before and after.
    val refused = Rig()
    val r = refused.queue(1, promote = false)
    refused.machine.onPromotionRefused(r)
    check(refused.state == FAILED && refused.status.endedReason == RecordingPlayer.ENDED_START) { "refused: couldn't start" }
    check(refused.players.single().released && refused.service.ends == listOf(r to RecordingPlayer.ENDED_START)) { "settled" }
    val early = Rig()
    early.machine.onPromotionRefused(1)
    early.queue(1, promote = false)
    check(early.state == FAILED && early.status.endedReason == RecordingPlayer.ENDED_START && early.players.isEmpty()) {
        "a refusal that came first ends it at once"
    }

    // Timed out, then a late promotion.
    val late = Rig()
    val l = late.queue(1, promote = false)
    late.prepared()
    late.elapse(4_999)
    check(late.state == PREPARING) { "five seconds to promote" }
    late.elapse(1)
    check(late.state == FAILED && late.status.endedReason == RecordingPlayer.ENDED_START) { "not promoted in time: couldn't start" }
    late.machine.onPromoted(l)
    check(late.state == FAILED && late.focus.requests.isEmpty()) { "a late promotion for a settled play changes nothing" }

    // Stopped before the promotion arrived.
    val stopped = Rig()
    val s = stopped.queue(1, promote = false)
    stopped.machine.stop(s)
    stopped.machine.onPromoted(s)
    check(stopped.state == STOPPED && stopped.status.endedReason == RecordingPlayer.ENDED_STOPPED) { "a Stop before delivery" }
    check(stopped.focus.requests.isEmpty() && stopped.service.ends == listOf(s to RecordingPlayer.ENDED_STOPPED)) {
        "the promotion after it starts nothing"
    }

    // A times out while B starts.
    val ab = Rig()
    val a = ab.queue(1, promote = false)
    val aTimeout = ab.scheduler.live().single()
    ab.elapse(3_000)
    val b = ab.queue(1, promote = false)
    check(ab.service.ends == listOf(a to RecordingPlayer.ENDED_REPLACED)) { "A is replaced" }
    ab.elapse(2_500)
    aTimeout.force()
    check(ab.status.generation == b && ab.state == PREPARING && ab.service.ends.size == 1) { "A's timeout does not end B" }
    ab.machine.onPromoted(a)
    ab.prepared()
    check(ab.state == PREPARING && ab.focus.requests.isEmpty()) { "nor does A's promotion start B" }
    ab.machine.onPromoted(b)
    check(ab.state == PLAYING) { "B's does" }
}

private fun itemCommandsCarryTheirToken() {
    val rig = Rig()
    val g = rig.queue(1, 1)
    rig.prepared()
    val firstA = rig.status.playToken
    rig.machine.next()
    rig.prepared()
    val b = rig.status.playToken
    rig.machine.seekFor(g, firstA, 30_000)
    rig.machine.pauseFor(g, firstA)
    check(rig.state == PLAYING && rig.current.seeks.isEmpty()) { "A's seek and pause are dropped on B (a drag begun on A)" }
    rig.machine.seekFor(g, b, 30_000)
    check(rig.current.seeks == listOf(30_000L)) { "B's own seek applies" }
    rig.machine.seekFor(g + 1, b, 10_000)
    check(rig.current.seeks.size == 1) { "another generation's is dropped" }
    rig.machine.previous()
    rig.machine.previous()
    rig.prepared()
    val secondA = rig.status.playToken
    check(rig.status.call == 0 && secondA != firstA && secondA != b) { "back on A, a new token" }
    rig.machine.seekFor(g, firstA, 5_000)
    rig.machine.pauseFor(g, b)
    check(rig.current.seeks.isEmpty() && rig.state == PLAYING) { "A's first token and B's are dropped" }
    rig.machine.pauseFor(g, secondA)
    check(rig.state == PAUSED) { "the current token pauses" }
    rig.machine.resumeFor(g, secondA)
    check(rig.state == PLAYING) { "and resumes" }
}

/** A call's last part failed while it played: the gap after it holds no player, and its token still steers the play. */
private fun theGapWithNoPlayerTakesItsItemsPauseAndResume() {
    val rig = Rig()
    val g = rig.queue(1, 1)
    rig.prepared()
    val failed = rig.current
    rig.machine.onFailed(failed.token, RecordingPlayer.REASON_DAMAGED)
    rig.turn()
    check(rig.status.gap && failed.released && rig.players.size == 1) { "the gap, with no player held" }
    val token = rig.status.playToken
    check(token == failed.token) { "the status still names the part that failed" }
    rig.machine.pauseFor(g, token)
    check(rig.state == PAUSED && rig.status.gap && rig.status.pausedReason == RecordingPlayer.PAUSED_USER) {
        "its Pause freezes the gap"
    }
    rig.machine.seekFor(g, token, 1_000)
    check(rig.state == PAUSED && rig.players.size == 1) { "a seek needs a live player: dropped" }
    rig.machine.resumeFor(g + 1, token)
    rig.machine.resumeFor(g, token + 1)
    check(rig.state == PAUSED) { "another generation's or token's Resume is dropped" }
    rig.machine.resumeFor(g, token)
    check(rig.last.path == path(1, 1) && !rig.status.gap) { "its Resume opens the next call at once" }
}

// ---- PLAYLIST: skips and failures

private fun goneChangedAndDamagedRecordingsAreSkipped() {
    for (reason in listOf(RecordingPlayer.REASON_GONE, RecordingPlayer.REASON_CHANGED, RecordingPlayer.REASON_DAMAGED)) {
        for (bad in 0..2) {
            val rig = Rig()
            val name = path(bad, 1)
            if (reason == RecordingPlayer.REASON_DAMAGED) rig.damaged += name else rig.fail(name, reason)
            rig.queue(1, 1, 1)
            for (call in 0..2) {
                if (call == bad) {
                    check(rig.status.skipped == 1 && rig.players.none { it.path == name && !it.released }) {
                        "$reason call $call is skipped"
                    }
                    check(rig.opener.opened.count { it == name } == 1) { "it was tried once" }
                    rig.turn()
                    continue
                }
                check(rig.last.path == path(call, 1)) { "$reason at $bad: call $call plays" }
                rig.playOut()
                rig.elapse(1_000)
            }
            check(rig.state == STOPPED && rig.status.endedReason == RecordingPlayer.ENDED_FINISHED) { "$reason at $bad: finished" }
            check(rig.focus.requests.size == 1) { "one focus request" }
            val rechecked = rig.recheck.take().toList()
            check(rechecked == if (reason == RecordingPlayer.REASON_DAMAGED) emptyList() else listOf(name)) {
                "$reason: rechecked $rechecked"
            }
        }
    }

    // A damaged error from a call's last part while it plays is followed as a completion: the gap, then the next call.
    val playing = Rig()
    playing.queue(1, 1)
    playing.prepared()
    playing.nowMs += 2_000
    playing.machine.onFailed(playing.current.token, RecordingPlayer.REASON_DAMAGED)
    check(playing.status.skipped == 1 && playing.players.size == 1) { "skipped on the next turn, not at once" }
    playing.turn()
    check(playing.status.gap && playing.players.size == 1 && playing.state == PLAYING) { "the gap after its call" }
    check(playing.position == 2_000L && !playing.status.canSeek) { "frozen where it failed, no seek" }
    playing.elapse(999)
    check(playing.players.size == 1) { "the gap lasts a second" }
    playing.elapse(1)
    check(playing.last.path == path(1, 1)) { "then the next call opens" }

    // One from a part before the call's last: the rest of the call at once, no gap.
    val part = Rig()
    part.queue(2, 1)
    part.prepared()
    part.machine.onFailed(part.current.token, RecordingPlayer.REASON_DAMAGED)
    part.turn()
    check(part.last.path == path(0, 2) && !part.status.gap) { "the call's next part at once" }

    // The last call's last part: it ends, finished, with the skip counted.
    val end = Rig()
    end.queue(1, 1)
    end.playOut()
    end.elapse(1_000)
    end.prepared()
    end.machine.onFailed(end.current.token, RecordingPlayer.REASON_DAMAGED)
    end.turn()
    check(end.state == STOPPED && end.status.endedReason == RecordingPlayer.ENDED_FINISHED && end.status.skipped == 1) {
        "finished: not every recording failed"
    }
}

private fun allFailedEndsAndGoodThenFailuresFinishes() {
    val all = Rig()
    for (call in 0..2) all.fail(path(call, 1), RecordingPlayer.REASON_GONE)
    val g = all.queue(1, 1, 1)
    all.turn()
    check(all.state == FAILED && all.status.endedReason == RecordingPlayer.ENDED_UNPLAYABLE) { "couldn't play these recordings" }
    check(all.status.skipped == 3 && all.focus.requests.isEmpty() && all.players.isEmpty()) { "nothing played" }
    check(all.service.ends == listOf(g to RecordingPlayer.ENDED_UNPLAYABLE)) { "settled once" }
    check(all.recheck.take().size == 3) { "each gone one rechecked" }

    val good = Rig()
    good.fail(path(1, 1), RecordingPlayer.REASON_GONE)
    good.fail(path(2, 1), RecordingPlayer.REASON_CHANGED)
    good.queue(1, 1, 1)
    good.playOut()
    good.elapse(1_000)
    good.turn()
    check(good.state == STOPPED && good.status.endedReason == RecordingPlayer.ENDED_FINISHED) { "good then failures finishes" }
    check(good.status.call == 0 && good.focus.abandons == 1 && good.status.skipped == 2) {
        "on the call that played, with the skipped count"
    }

    // Every recording failed, even one that played in part first: couldn't play these recordings.
    val partly = Rig()
    partly.fail(path(1, 1), RecordingPlayer.REASON_GONE)
    val p = partly.queue(1, 1)
    partly.prepared()
    partly.nowMs += 5_000
    partly.machine.onFailed(partly.current.token, RecordingPlayer.REASON_DAMAGED)
    partly.turn()
    partly.elapse(1_000)
    partly.turn()
    check(partly.state == FAILED && partly.status.endedReason == RecordingPlayer.ENDED_UNPLAYABLE) {
        "every item failed: unplayable, whatever played in part"
    }
    check(partly.service.ends == listOf(p to RecordingPlayer.ENDED_UNPLAYABLE)) { "settled once" }

    // Many failures in a row: one per turn, and a Stop between turns ends the search.
    val many = Rig()
    val count = 50
    for (call in 0 until count - 1) many.fail(path(call, 1), RecordingPlayer.REASON_GONE)
    val m = many.queue(*IntArray(count) { 1 })
    check(many.opener.opened.size == 1) { "the first failure waits for the next turn" }
    repeat(10) { many.scheduler.live().single().force() }
    check(many.opener.opened.size == 11 && many.status.skipped == 11) { "one open per turn" }
    many.machine.stop(m)
    many.turn()
    check(many.opener.opened.size == 11 && many.status.endedReason == RecordingPlayer.ENDED_STOPPED) { "stoppable mid-run" }
    val finish = Rig()
    for (call in 0 until count - 1) finish.fail(path(call, 1), RecordingPlayer.REASON_GONE)
    finish.queue(*IntArray(count) { 1 })
    finish.turn()
    check(finish.last.path == path(count - 1, 1) && finish.opener.opened.size == count) { "each tried once, the last plays" }
}

private fun failuresArePerRecordingAndKeepTheirDirection() {
    // A failed first part with a good sibling plays the sibling.
    val first = Rig()
    first.fail(path(1, 1), RecordingPlayer.REASON_GONE)
    first.queue(1, 2, 1)
    first.playOut()
    first.elapse(1_000)
    first.turn()
    check(first.last.path == path(1, 2) && first.status.partOrdinal == 2 && first.status.partCount == 2) {
        "the call's later part plays, numbered as listed"
    }
    first.prepared()
    first.nowMs += 500
    first.machine.previous()
    check(first.last.path == path(0, 1)) { "Previous at the call's first playable part goes to the previous call" }
    first.prepared()
    first.machine.next()
    check(first.last.path == path(1, 2) && first.opener.opened.count { it == path(1, 1) } == 1) {
        "Next again starts the call at its first not-failed part; the failed file is never reopened"
    }

    // A failed middle part: no gap, the part after it at once.
    val middle = Rig()
    middle.fail(path(0, 2), RecordingPlayer.REASON_CHANGED)
    middle.queue(3, 1)
    middle.playOut()
    middle.turn()
    check(middle.last.path == path(0, 3) && !middle.status.gap) { "the part after the failed one" }
    middle.prepared()
    middle.machine.previous()
    check(middle.last.path == path(0, 1)) { "Previous on part 3 restarts the call at part 1" }

    // The call's remaining parts all fail: the gap still comes before the next call.
    val rest = Rig()
    rest.fail(path(0, 2), RecordingPlayer.REASON_GONE)
    rest.queue(2, 1)
    rest.playOut()
    rest.turn()
    check(rest.status.gap && rest.status.partOrdinal == 1 && rest.players.size == 1) { "back in the gap after the last part heard" }
    rest.elapse(1_000)
    check(rest.last.path == path(1, 1)) { "then the next call" }
}

private fun previousPassesOverACallWhoseRecordingsAllFailed() {
    // A good, B failed, C good: Previous at C's start reaches A.
    val rig = Rig()
    rig.fail(path(1, 1), RecordingPlayer.REASON_GONE)
    rig.fail(path(1, 2), RecordingPlayer.REASON_GONE)
    rig.queue(1, 2, 1)
    rig.playOut()
    rig.elapse(1_000)
    rig.turn()
    rig.turn()
    check(rig.last.path == path(2, 1)) { "C plays after B's parts fail" }
    rig.prepared()
    val c = rig.current
    rig.machine.previous()
    check(rig.last.path == path(0, 1) && !c.released) { "Previous at C's start reaches A" }
    check(rig.opener.opened.count { it.startsWith("/r/c1") } == 2) { "B's failed parts are not tried again" }

    // The current call gone too: A fails now, so it stays on C's open player at 0, reopening nothing failed.
    val gone = Rig()
    gone.fail(path(1, 1), RecordingPlayer.REASON_GONE)
    gone.queue(1, 1, 1)
    gone.playOut()
    gone.elapse(1_000)
    gone.turn()
    gone.prepared()
    val open = gone.current
    gone.nowMs += 2_000
    gone.fail(path(0, 1), RecordingPlayer.REASON_GONE)
    gone.fail(path(2, 1), RecordingPlayer.REASON_GONE)
    gone.machine.previous()
    check(gone.opener.opened.last() == path(0, 1) && !open.released) { "A is tried, C kept open" }
    gone.turn()
    check(gone.current === open && open.seeks == listOf(0L) && open.started && gone.state == PLAYING) {
        "every candidate failed: it stays on C at 0"
    }
    check(gone.opener.opened == listOf(path(0, 1), path(1, 1), path(2, 1), path(0, 1))) { "bounded: one try each" }
    gone.machine.previous()
    check(gone.opener.opened.size == 4 && open.seeks == listOf(0L, 0L)) { "Previous again: a restart, no failed file reopened" }

    // With no open player and nothing playable behind, it goes on forward, then ends.
    val ahead = Rig()
    ahead.fail(path(0, 1), RecordingPlayer.REASON_GONE)
    ahead.queue(1, 1)
    ahead.machine.previous()
    ahead.turn()
    check(ahead.last.path == path(1, 1)) { "Previous during the first skip still reaches what plays" }
    check(ahead.opener.opened == listOf(path(0, 1), path(1, 1))) { "without reopening the failed one" }

    val nothing = Rig()
    for (call in 0..2) nothing.fail(path(call, 1), RecordingPlayer.REASON_GONE)
    nothing.queue(1, 1, 1, start = 1)
    nothing.machine.previous()
    nothing.turn()
    check(nothing.state == FAILED && nothing.status.endedReason == RecordingPlayer.ENDED_UNPLAYABLE) {
        "backward, then forward with no player: every item failed, unplayable"
    }
    check(nothing.opener.opened.sorted() == (0..2).map { path(it, 1) }) { "each tried once" }

    val rest = Rig()
    rest.fail(path(1, 1), RecordingPlayer.REASON_GONE)
    rest.fail(path(2, 1), RecordingPlayer.REASON_GONE)
    rest.queue(1, 1, 1, start = 1)
    rest.machine.previous()
    check(rest.last.path == path(0, 1)) { "backward it reaches the call before the start" }
}

private fun previousDuringASkipDoesNotWaitForTheTurn() {
    // A good, B's first part gone, B's second part good: Previous issued before or after B1's skip turn reaches A.
    for (beforeTurn in listOf(true, false)) {
        val rig = Rig()
        rig.fail(path(1, 1), RecordingPlayer.REASON_GONE)
        rig.queue(1, 2, 1)
        rig.prepared()
        val a = rig.current
        rig.complete()
        rig.nowMs += 1_000
        rig.scheduler.live().single().force() // the gap's task: B1 is tried and fails
        check(rig.opener.opened.last() == path(1, 1) && rig.status.skipped == 1) { "B1 failed" }
        if (!beforeTurn) {
            rig.turn()
            check(rig.last.path == path(1, 2) && rig.state == PREPARING) { "B2 is opening" }
        }
        rig.machine.previous()
        check(rig.current === a && a.seeks == listOf(0L) && rig.status.call == 0 && rig.position == 0L) {
            "before the turn = $beforeTurn: A restarts at 0 on its open player"
        }
        rig.turn()
        check(rig.current === a && a.started && rig.state == PLAYING) { "and plays; the skip search is over" }
        check(rig.opener.opened.count { it == path(1, 1) } == 1) { "B1 is not tried again" }
    }
}

private fun anErrorFromThePartThatPlayedOutIsIgnoredInTheGap() {
    // The gap after a call's last part, the part that completed still current: its late error changes nothing.
    val rig = Rig()
    rig.queue(1, 1)
    rig.prepared()
    val finished = rig.current
    rig.complete()
    val gapTask = rig.scheduler.live().single()
    rig.nowMs += 400
    rig.machine.onFailed(finished.token, RecordingPlayer.REASON_DAMAGED)
    rig.turn()
    check(rig.status.gap && rig.state == PLAYING && rig.status.skipped == 0) { "still the gap, no skip counted" }
    check(rig.scheduler.live().single() === gapTask && !finished.released) { "the gap is not restarted" }
    rig.elapse(600)
    check(rig.last.path == path(1, 1) && !rig.status.gap) { "it runs out on time and the next call opens" }
    rig.prepared()
    rig.machine.previous()
    rig.prepared()
    check(rig.status.call == 0 && rig.opener.opened.count { it == path(0, 1) } == 2) { "the part is not marked failed" }

    // The gap entered after the call's other part failed: the part that played is held into it, and its error too.
    val held = Rig()
    held.fail(path(0, 2), RecordingPlayer.REASON_GONE)
    held.queue(2, 1)
    held.prepared()
    val played = held.current
    held.complete()
    held.turn()
    check(held.status.gap && held.status.skipped == 1 && held.current === played) { "the gap, on the part that played" }
    val heldGap = held.scheduler.live().single()
    held.machine.onFailed(played.token, RecordingPlayer.REASON_PLAYER)
    held.turn()
    check(held.status.gap && held.state == PLAYING && held.status.skipped == 1) { "ignored: not failed, not ended" }
    check(held.scheduler.live().single() === heldGap) { "and the gap runs on" }
}

private fun aRefusedResumeStaysPausedInEveryMode() {
    // A playlist paused while its first part opens, resumed during a phone call: still paused, not failed.
    val opening = Rig()
    opening.queue(1, 1)
    opening.machine.pause()
    opening.prepared()
    opening.focus.grants = false
    opening.machine.resume()
    check(opening.state == PAUSED && opening.status.pausedReason == RecordingPlayer.PAUSED_UNAVAILABLE) {
        "a resume refused the focus stays paused, though no request had been granted yet"
    }
    check(opening.status.endedReason.isEmpty() && !opening.current.released) { "it has not ended" }
    opening.focus.grants = true
    opening.machine.resume()
    check(opening.state == PLAYING) { "a later resume plays" }

    // The same in a paused gap: Play is refused and nothing opens.
    val gap = Rig()
    gap.queue(1, 1)
    gap.prepared()
    gap.complete()
    gap.machine.pause()
    gap.focus.grants = false
    gap.machine.resume()
    check(gap.state == PAUSED && gap.status.gap && gap.players.size == 1) { "a refused Play leaves the gap paused" }

    // SINGLE and PARTS too; only the play's own start refused fails it.
    for (mode in listOf(-1, MODE_PARTS)) {
        val rig = Rig()
        if (mode < 0) rig.play() else rig.queue(2, mode = mode)
        rig.prepared()
        rig.machine.pause()
        rig.focus.grants = false
        rig.machine.resume()
        check(rig.state == PAUSED && rig.status.pausedReason == RecordingPlayer.PAUSED_UNAVAILABLE) {
            "mode $mode: a refused resume stays paused"
        }
    }
    val parts = Rig()
    parts.focus.grants = false
    parts.queue(2, mode = MODE_PARTS)
    parts.prepared()
    check(parts.state == FAILED && parts.status.error == RecordingPlayer.ERROR_UNAVAILABLE) { "refused at its start it fails" }
}

private fun aPauseHoldsWhileItSearches() {
    val rig = Rig()
    rig.fail(path(0, 1), RecordingPlayer.REASON_GONE)
    rig.queue(1, 1)
    rig.machine.pause()
    rig.turn()
    check(rig.last.path == path(1, 1) && rig.state == PREPARING) { "the search goes on paused" }
    rig.prepared()
    check(rig.state == PAUSED && rig.position == 0L && !rig.last.started && rig.focus.requests.isEmpty()) {
        "what it found waits paused at 0"
    }
}

private fun theRecheckListOutlivesThePlayAndHandsEachPathOnce() {
    val rig = Rig()
    rig.fail(path(0, 1), RecordingPlayer.REASON_GONE)
    val serial = rig.status.recheckSerial
    rig.queue(1, 1)
    check(rig.status.recheckSerial == serial + 1) { "the status record's serial moved" }
    rig.machine.stop()
    rig.fail(path(1, 1), RecordingPlayer.REASON_CHANGED)
    rig.queue(1, 1)
    rig.turn()
    rig.queue(1)
    check(rig.recheck.take().toList() == listOf(path(0, 1), path(1, 1))) { "kept through a Stop and a replacement" }
    check(rig.recheck.take().isEmpty()) { "each path handed over once" }
}

// ---- the table

private fun theTableDrivesStateSpeedAndActions() {
    fun expect(rig: Rig, name: String, state: Int, session: Int, actions: Int) {
        val s = rig.status
        check(s.state == state && s.sessionState == session && s.actions == actions) {
            "$name: state ${s.state}/$state session ${s.sessionState}/$session actions ${s.actions}/$actions"
        }
        check(s.canSeek == (actions and ACTION_SEEK != 0)) { "$name: canSeek follows the seek action" }
        check(s.allows(ACTION_STOP) == (s.sessionState != SESSION_STOPPED)) { "$name: Stop offered until it ends" }
    }
    val moves = ACTION_STOP or ACTION_NEXT or ACTION_PREVIOUS
    val rig = Rig()
    rig.queue(1, 1)
    expect(rig, "opening (wanted)", PREPARING, SESSION_PLAYING, moves or ACTION_PAUSE)
    check(rig.status.speed == 0f) { "opening plays at speed 0" }
    rig.machine.pause()
    expect(rig, "opening (paused)", PREPARING, SESSION_PAUSED, moves or ACTION_PLAY)
    rig.prepared()
    expect(rig, "paused", PAUSED, SESSION_PAUSED, moves or ACTION_PLAY or ACTION_SEEK)
    rig.machine.resume()
    expect(rig, "playing", PLAYING, SESSION_PLAYING, moves or ACTION_PAUSE or ACTION_SEEK)
    check(rig.status.speed == 1f) { "playing at speed 1" }
    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.LOST_TRANSIENT)
    expect(rig, "interrupted", PAUSED, SESSION_PAUSED, moves or ACTION_PLAY)
    rig.machine.seek(5_000)
    check(rig.current.seeks.isEmpty()) { "a seek while interrupted is dropped" }
    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.GAINED)
    rig.complete()
    expect(rig, "gap", PLAYING, SESSION_PLAYING, moves or ACTION_PAUSE)
    rig.machine.pause()
    expect(rig, "paused gap", PAUSED, SESSION_PAUSED, moves or ACTION_PLAY)
    rig.machine.resume()
    rig.elapse(1_000)
    rig.prepared()
    expect(rig, "last call", PLAYING, SESSION_PLAYING, ACTION_STOP or ACTION_PREVIOUS or ACTION_PAUSE or ACTION_SEEK)
    rig.machine.stop(rig.generation)
    expect(rig, "ended", STOPPED, SESSION_STOPPED, 0)

    val before = Rig()
    before.queue(1, 1, promote = false)
    expect(before, "before promotion", PREPARING, SESSION_NONE, moves or ACTION_PAUSE)

    val zero = Rig(length = 0)
    zero.queue(1, 1)
    zero.prepared()
    expect(zero, "zero duration", PLAYING, SESSION_PLAYING, moves or ACTION_PAUSE)
    zero.machine.seek(0)
    check(zero.current.seeks.isEmpty()) { "a seek on a part of no length is dropped" }

    val single = Rig()
    single.play()
    single.prepared()
    check(single.status.actions == ACTION_STOP or ACTION_PAUSE or ACTION_SEEK) { "a single play has no moves" }
    single.machine.next()
    single.machine.previous()
    check(single.players.size == 1 && single.last.seeks.isEmpty()) { "Next and Previous are dropped for it" }
}

// ---- focus, noisy, deadline

private fun anInterruptionCarriesOnByItselfAndTheAppsPauseCancelsIt() {
    val rig = Rig()
    rig.queue(1, 1)
    rig.prepared()
    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.LOST_TRANSIENT)
    check(rig.status.interrupted && !rig.current.started && rig.focus.abandons == 0) { "interrupted: halted, the request kept" }
    rig.machine.resume()
    check(rig.status.interrupted && !rig.current.started && rig.focus.requests.size == 1) { "the system's Play is a no-op" }
    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.GAINED)
    check(rig.state == PLAYING && !rig.status.interrupted && rig.focus.requests.size == 1) { "the original GAIN carries on" }

    val cancel = Rig()
    cancel.queue(1, 1)
    cancel.prepared()
    val epoch = cancel.focus.epoch
    cancel.machine.onFocusChange(epoch, FocusChange.LOST_TRANSIENT)
    cancel.machine.pause()
    check(cancel.state == PAUSED && !cancel.status.interrupted && cancel.focus.abandons == 1) { "the app's Pause: paused, abandoned" }
    cancel.machine.onFocusChange(epoch, FocusChange.GAINED)
    check(cancel.state == PAUSED && !cancel.current.started) { "the GAIN that follows stays silent" }

    // Interrupted in the gap freezes it.
    val gap = Rig()
    gap.queue(1, 1)
    gap.prepared()
    gap.complete()
    gap.elapse(300)
    gap.machine.onFocusChange(gap.focus.epoch, FocusChange.LOST_TRANSIENT)
    gap.elapse(5_000)
    check(gap.players.size == 1 && gap.status.gap && gap.state == PAUSED) { "the gap is frozen while interrupted" }
    gap.machine.onFocusChange(gap.focus.epoch, FocusChange.GAINED)
    gap.elapse(699)
    check(gap.players.size == 1) { "it carries on with what was left" }
    gap.elapse(1)
    check(gap.last.path == path(1, 1)) { "then the next call" }

    // A resume refused the focus stays paused (PLAYLIST), it does not fail.
    val refused = Rig()
    refused.queue(1, 1)
    refused.prepared()
    refused.machine.pause()
    refused.focus.grants = false
    refused.machine.resume()
    check(refused.state == PAUSED && refused.status.pausedReason == RecordingPlayer.PAUSED_UNAVAILABLE) {
        "a refused resume: can't play right now, still paused"
    }
    val start = Rig()
    start.focus.grants = false
    start.queue(1)
    start.prepared()
    check(start.state == FAILED && start.status.error == RecordingPlayer.ERROR_UNAVAILABLE) { "refused at the start it ends" }
}

private fun aLastingLossEndsAPlaylistAndPausesTheOthers() {
    val playlist = Rig()
    val g = playlist.queue(1, 1)
    playlist.prepared()
    playlist.machine.onFocusChange(playlist.focus.epoch, FocusChange.LOST)
    check(playlist.state == STOPPED && playlist.status.endedReason == RecordingPlayer.ENDED_FOCUS) { "a playlist ends" }
    check(playlist.focus.abandons == 1 && playlist.service.ends == listOf(g to RecordingPlayer.ENDED_FOCUS)) { "and settles" }

    val parts = Rig()
    parts.queue(2, mode = MODE_PARTS)
    parts.prepared()
    parts.machine.onFocusChange(parts.focus.epoch, FocusChange.LOST)
    check(parts.state == PAUSED && parts.status.pausedReason == RecordingPlayer.PAUSED_FOCUS) { "Play all parts pauses" }
}

private fun headphonesUnpluggedPauseWhateverWouldSound() {
    // In every state that would start audio (#909).
    fun unplug(rig: Rig, name: String) {
        rig.machine.onNoisy()
        check(rig.status.pausedReason == RecordingPlayer.PAUSED_NOISY && !rig.focus.held) { "$name: paused, noisy" }
        check(rig.players.none { it.started }) { "$name: silent" }
    }
    val playing = Rig()
    playing.queue(1, 1)
    playing.prepared()
    unplug(playing, "playing")
    check(playing.state == PAUSED) { "playing → paused" }

    val opening = Rig()
    opening.queue(1, 1)
    unplug(opening, "opening")
    opening.prepared()
    check(opening.state == PAUSED) { "opening stays paused once prepared" }

    val gap = Rig()
    gap.queue(1, 1)
    gap.prepared()
    gap.complete()
    unplug(gap, "gap")
    gap.elapse(5_000)
    check(gap.players.size == 1 && gap.status.gap) { "the gap is held" }

    val interrupted = Rig()
    interrupted.queue(1, 1)
    interrupted.prepared()
    val epoch = interrupted.focus.epoch
    interrupted.machine.onFocusChange(epoch, FocusChange.LOST_TRANSIENT)
    unplug(interrupted, "interrupted")
    interrupted.machine.onFocusChange(epoch, FocusChange.GAINED)
    check(interrupted.state == PAUSED) { "the carry-on is cancelled" }

    val single = Rig()
    single.play()
    single.prepared()
    unplug(single, "single")

    val paused = Rig()
    paused.queue(1, 1)
    paused.prepared()
    paused.machine.pause()
    paused.machine.onNoisy()
    check(paused.status.pausedReason == RecordingPlayer.PAUSED_USER) { "already paused: unchanged" }
}

private fun theTenMinuteDeadlineCountsFromThePause() {
    // A clock jump: the next event finds it past the deadline, even before its task runs.
    val jump = Rig()
    val g = jump.queue(1, 1)
    jump.prepared()
    jump.machine.pause()
    jump.nowMs += 600_000
    jump.machine.resume()
    check(jump.state == STOPPED && jump.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) { "the resume finds it ended" }
    check(!jump.current.started && jump.focus.requests.size == 1) { "and plays nothing" }
    check(jump.service.ends == listOf(g to RecordingPlayer.ENDED_TIMEOUT) && jump.scheduler.live().isEmpty()) { "settled" }

    // A late task (deep sleep): it ends at the first chance after ten minutes.
    val late = Rig()
    late.queue(1, 1)
    late.prepared()
    late.machine.pause()
    late.elapse(900_000)
    check(late.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) { "a late task ends it" }

    // Deep sleep: the task counts awake time and has not run; the screen turning on ends it at once.
    val asleep = Rig()
    val slept = asleep.queue(1, 1)
    asleep.prepared()
    asleep.machine.pause()
    asleep.nowMs += 3_600_000
    check(asleep.state == PAUSED) { "nothing has looked yet" }
    asleep.machine.onAwake()
    check(asleep.state == STOPPED && asleep.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) { "awake, it ends" }
    check(asleep.service.ends == listOf(slept to RecordingPlayer.ENDED_TIMEOUT) && asleep.scheduler.live().isEmpty()) {
        "settled"
    }
    asleep.machine.onAwake()
    check(asleep.service.ends.size == 1) { "once" }
    val early = Rig()
    early.queue(1, 1)
    early.prepared()
    early.machine.pause()
    early.nowMs += 599_999
    early.machine.onAwake()
    check(early.state == PAUSED) { "awake before ten minutes, it waits" }
    val alone = Rig()
    alone.play()
    alone.prepared()
    alone.machine.pause()
    alone.nowMs += 3_600_000
    alone.machine.onAwake()
    check(alone.state == PAUSED) { "a single play has no paused deadline to check" }

    // Interrupted for ten minutes ends too.
    val interrupted = Rig()
    interrupted.queue(1, 1)
    interrupted.prepared()
    interrupted.machine.onFocusChange(interrupted.focus.epoch, FocusChange.LOST_TRANSIENT)
    interrupted.elapse(600_000)
    check(interrupted.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) { "interrupted ten minutes ends" }

    // Playing again clears it; SINGLE has none.
    val resumed = Rig()
    resumed.queue(1, 1)
    resumed.prepared()
    resumed.machine.pause()
    resumed.elapse(500_000)
    resumed.machine.resume()
    resumed.elapse(500_000)
    check(resumed.state == PLAYING) { "a resume clears the deadline" }
    val single = Rig()
    single.play()
    single.prepared()
    single.machine.pause()
    single.elapse(3_600_000)
    check(single.state == PAUSED && single.scheduler.entries.isEmpty()) { "a single play has no paused deadline" }
}

private fun theTerminalRecordStaysUntilTheNextPlay() {
    val rig = Rig()
    val g = rig.queue(1, 2, 1)
    rig.playOut()
    rig.elapse(1_000)
    rig.prepared()
    rig.nowMs += 4_000
    rig.machine.stop(g)
    val record = rig.status.record(rig.nowMs).split('\t')
    check(record[0] == "$g" && record[1] == "$STOPPED" && record[15] == RecordingPlayer.ENDED_STOPPED) {
        "a service Stop keeps the play's terminal record: $record"
    }
    check(record[7] == "1" && record[8] == "3" && record[9] == "1" && record[10] == "2") { "on the call it ended on" }
    check(record[2] == "4000" && rig.players.all { it.released } && !rig.focus.held) { "at its position, all let go" }
    check(rig.service.ends == listOf(g to RecordingPlayer.ENDED_STOPPED)) { "the service is told once" }
    rig.machine.stop(g)
    rig.machine.next(g)
    rig.elapse(700_000)
    check(rig.status.record(rig.nowMs).split('\t') == record && rig.service.ends.size == 1) { "it stays as it was" }
    rig.machine.stop(g + 1)
    check(rig.status.generation == g) { "a Stop for another generation does nothing" }
    val next = rig.queue(1)
    check(rig.status.generation == next && rig.status.endedReason.isEmpty()) { "until the next play" }
    check(rig.service.ends.size == 1) { "an ended play is not replaced again" }

    // A host stop forgets the play, and tells the service.
    rig.machine.stop()
    check(rig.status.generation == 0 && rig.service.ends.last() == next to RecordingPlayer.ENDED_STOPPED) { "a host stop" }
}

private fun invalidItemsFailThePlay() {
    val rig = Rig()
    rig.machine.play(1, MODE_PLAYLIST, emptyList())
    check(rig.state == FAILED && rig.service.ends == listOf(1 to RecordingPlayer.ENDED_FAILED)) { "no items" }
    rig.machine.play(2, MODE_PLAYLIST, listOf(RecordingPlayer.Item("/r/a.wav", call = 1)))
    check(rig.state == FAILED && rig.opener.opened.isEmpty()) { "calls that do not start at 0" }
    rig.machine.play(3, MODE_PARTS, listOf(RecordingPlayer.Item("/r/a.wav")), start = 1)
    check(rig.state == FAILED && rig.status.itemIndex == -1) { "a start outside the items" }
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
    aPlayerThatCannotBeBuiltFailsThePlay()
    seeksFollowTheTableAndKeepTheClock()
    aStopReleasesAndForgetsThePlay()
    partsPlayThroughHoldingOneTransientFocus()
    aFailedPartEndsPlayAllParts()
    aPlaylistPlaysCallsAndPartsInOrderWithAGapBetweenCalls()
    theGapReadsFrozenAndPausesWithIt()
    nextOpensTheNextCallAtOnceAlsoFromTheGap()
    nextPastOnlyFailedCallsFinishes()
    previousFollowsTheThreeSecondRule()
    movesWhilePausedStayPausedAndKeepTheDeadline()
    aResumeWaitingForTheFocusKeepsTheDeadline()
    lateCallbacksTasksAndFocusAreDropped()
    theServiceIsPromotedOnceInEitherOrder()
    itemCommandsCarryTheirToken()
    theGapWithNoPlayerTakesItsItemsPauseAndResume()
    goneChangedAndDamagedRecordingsAreSkipped()
    allFailedEndsAndGoodThenFailuresFinishes()
    failuresArePerRecordingAndKeepTheirDirection()
    previousPassesOverACallWhoseRecordingsAllFailed()
    previousDuringASkipDoesNotWaitForTheTurn()
    anErrorFromThePartThatPlayedOutIsIgnoredInTheGap()
    aRefusedResumeStaysPausedInEveryMode()
    aPauseHoldsWhileItSearches()
    theRecheckListOutlivesThePlayAndHandsEachPathOnce()
    theTableDrivesStateSpeedAndActions()
    anInterruptionCarriesOnByItselfAndTheAppsPauseCancelsIt()
    aLastingLossEndsAPlaylistAndPausesTheOthers()
    headphonesUnpluggedPauseWhateverWouldSound()
    theTenMinuteDeadlineCountsFromThePause()
    theTerminalRecordStaysUntilTheNextPlay()
    invalidItemsFailThePlay()
    println("PASS: the call recording player queues calls and parts, skips, follows the focus, the service and the table")
}
