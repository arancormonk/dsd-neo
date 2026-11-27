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
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.SESSION_BUFFERING
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.SESSION_NONE
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.SESSION_PAUSED
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.SESSION_PLAYING
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.SESSION_STOPPED
import io.github.arancormonk.dsdneo.RecordingPlayer.Companion.STARTING
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
private class FakePlayer(
    val token: Long,
    private val length: Long,
    private val damaged: Set<String>,
    private val log: MutableList<String> = mutableListOf(),
    private val threads: FakeThreads = FakeThreads(),
    private val unmakeable: () -> Boolean = { false },
    /** What its own thread does with start request [Long] (the rig makes and acknowledges it, holds or fails it). */
    private val onStart: (FakePlayer, Long) -> Unit = { player, serial -> player.makeStart(serial) },
    /** What its own thread does with seek [Long]: the rig lands it at once, or holds its completion (R56). */
    private val onSeek: (FakePlayer, Long) -> Unit = { _, _ -> },
    /** What its own thread does with a silence request: the rig reports it at once, or holds it (a delayed owner). */
    private val onSilence: (FakePlayer) -> Unit = {},
    /** What its own thread does with the release: the rig ends the thread at once, or holds it in native code. */
    private val onRelease: (FakePlayer, () -> Unit) -> Unit = { _, ended -> ended() },
) : RecordingPlayer.Player {
    var path = ""

    /** The start the machine wants made, 0 for none, as MediaItem's: taken back by a pause or the release at once. */
    var wantedStart = 0L

    /** Every start asked for, by serial, made or not; [made] the ones its own thread made. */
    val startRequests = mutableListOf<Long>()

    /** The calls passed to its own thread, in order: "ask <serial>", "pause", "seek <ms>" (R45's order). */
    val ops = mutableListOf<String>()
    val made = mutableListOf<Long>()

    /** Its platform player was made, on its own thread. */
    var created = false
    var asked = false
    var prepared = false
    var started = false
    var released = false

    /** It reported an error: in MediaPlayer's Error state, where only a release is allowed. */
    var errored = false
    val seeks = mutableListOf<Long>()
    val calls = mutableListOf<String>()

    private fun valid(call: String, ok: Boolean) {
        calls += call
        check(!released) { "$call on a released player" }
        check(!errored) { "$call on a player in the Error state" }
        check(ok) { "$call in a state the player does not take it in: ${calls.joinToString()}" }
    }

    /** Handed to its own thread once, before anything else ([calls] lists the platform player's calls only). */
    override fun setup(work: () -> RecordingPlayer.SetupResult, deliver: (RecordingPlayer.SetupResult) -> Unit) {
        check(!released && !created && !asked && calls.isEmpty()) { "a setup on a player already used" }
        threads.setup(work, deliver)
    }

    override fun create() {
        check(!released && !created) { "create twice, or on a released player" }
        if (unmakeable()) {
            throw RuntimeException("the media server is away")
        }
        created = true
    }

    override fun prepare(source: RecordingPlayer.Source) {
        valid("prepare", created && !asked)
        asked = true
        val file = source as FakeSource
        check(!file.closed) { "prepared from a closed source" }
        if (file.path in damaged) {
            throw IllegalStateException("bad header")
        }
        path = file.path
    }

    override fun start(serial: Long) {
        check(!released) { "start asked of a released player" }
        check(wantedStart == 0L) { "a start asked for while start $wantedStart waits" }
        wantedStart = serial
        startRequests += serial
        ops += "ask $serial"
        onStart(this, serial)
    }

    /** Its own thread makes start [serial] if it is still the one wanted (as MediaItem checks): true when made. */
    fun makeStart(serial: Long): Boolean {
        if (released || wantedStart != serial) {
            return false
        }
        valid("start", prepared && !started)
        started = true
        made += serial
        return true
    }

    override fun pause() {
        wantedStart = 0L
        ops += "pause"
        if (started && !errored) {
            valid("pause", true)
            started = false
        }
        // In the Error state the platform's pause does not throw: it returns and reports error -38, which changes
        // nothing here.
    }

    override fun seekTo(positionMs: Long, serial: Long) {
        ops += "seek $positionMs"
        valid("seek", prepared)
        seeks += positionMs
        onSeek(this, serial)
    }

    override fun silence() {
        check(!released) { "silence asked of a released player" }
        ops += "silence"
        onSilence(this)
    }

    override val durationMs: Long
        get() {
            check(prepared) { "duration before prepared" }
            return length
        }

    override fun release(ended: () -> Unit) {
        check(!released) { "released twice" }
        wantedStart = 0L
        released = true
        log += "release $token"
        onRelease(this, ended)
    }
}

/** A setup handed to its player's thread and held there: the test runs its work and delivers its result when told. */
private class HeldSetup(
    val work: () -> RecordingPlayer.SetupResult,
    val deliver: (RecordingPlayer.SetupResult) -> Unit,
) {
    var result: RecordingPlayer.SetupResult? = null

    /** The player's thread does the work (it opens the file and prepares), its result not back yet. */
    fun run(): HeldSetup = also { result = work() }

    /**
     * The result reaches the machine's thread (the work first, if it has not run). Work that throws an Error posts
     * nothing, as an executor's thread that it ends would not.
     */
    fun land() {
        val landed = result ?: try {
            work()
        } catch (_: Error) {
            return
        }
        deliver(landed)
    }
}

/**
 * The players' own threads: each player's setup runs on a thread of its own, numbered in the order the setups are
 * handed over, from 1 ([made] so far, [threadOf] each setup's). A setup runs at once on the caller's thread unless its
 * thread is [stuck], or every setup waits ([holding]), when it waits in [setups]. While [refusing], a thread takes no
 * work, as one that has quit: the setup is delivered failed ([RecordingPlayer.REASON_PLAYER]) with no work run.
 */
private class FakeThreads {
    val stuck = mutableSetOf<Int>()
    var holding = false
    var refusing = false
    var made = 0
    val setups = mutableListOf<HeldSetup>()
    val threadOf = mutableListOf<Int>()

    fun setup(work: () -> RecordingPlayer.SetupResult, deliver: (RecordingPlayer.SetupResult) -> Unit) {
        val thread = ++made
        threadOf += thread
        when {
            refusing -> deliver(RecordingPlayer.SetupResult(RecordingPlayer.REASON_PLAYER))
            holding || thread in stuck -> setups += HeldSetup(work, deliver)
            else -> deliver(work())
        }
    }
}

/** The opener, on a player's own thread: the files refused, throwing or opened. */
private class FakeOpener : RecordingPlayer.Opener {
    val refusals = HashMap<String, String>()
    val throwing = HashSet<String>()

    /** Paths whose open throws an Error (out of memory), not an Exception. */
    val erroring = HashSet<String>()
    val opened = mutableListOf<String>()

    /** Whether each open checked the file's identity, in order. */
    val checks = mutableListOf<Boolean>()
    val sources = mutableListOf<FakeSource>()

    override fun open(item: RecordingPlayer.Item, checked: Boolean): OpenResult {
        opened += item.path
        checks += checked
        if (item.path in throwing) {
            throw SecurityException("no access")
        }
        if (item.path in erroring) {
            throw OutOfMemoryError("no room for the data source")
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

    /** "changed" and "ended" in the order the service heard them. */
    val heard = mutableListOf<String>()

    override fun changed(status: RecordingPlayer.Status) {
        changes += status
        heard += "changed"
    }

    override fun ended(generation: Int, reason: String) {
        ends += generation to reason
        heard += "ended"
    }
}

private class Rig(foreground: Boolean = true, var length: Long = 60_000) {
    var nowMs = 1_000L
    val players = mutableListOf<FakePlayer>()

    /** What the players did, in order: built, released (by token). */
    val log = mutableListOf<String>()
    val damaged = HashSet<String>()
    var factoryFails = false

    /** The player's own thread cannot start (an Error). */
    var threadFails = false

    /** The platform player cannot be made on its own thread. */
    var createFails = false
    val opener = FakeOpener()
    val threads = FakeThreads()

    /**
     * Starts: made and acknowledged at once unless [holdStarts] (they wait in [heldStarts] for [ackStart]) or
     * [failStarts] (the player's thread throws, which MediaItem reports as the player's failure).
     */
    var holdStarts = false
    var failStarts = false
    val heldStarts = mutableListOf<Pair<FakePlayer, Long>>()

    /**
     * Silences: reported at once unless [holdSilences] (the owner delayed: they wait in [heldSilences] for
     * [ackSilence]). A player whose silence is held still sounds ([audible]); a start made while another sounds fails.
     */
    var holdSilences = false

    /** Seek completions: reported at once unless [holdSeeks] (held in [heldSeeks], by player and serial). */
    var holdSeeks = false
    val heldSeeks = mutableListOf<Pair<FakePlayer, Long>>()

    /** Held seek [at] (the oldest by default) lands. */
    fun landSeek(at: Int = 0) {
        val (player, serial) = heldSeeks.removeAt(at)
        machine.onSeekCompleted(player.token, serial)
    }
    val heldSilences = mutableListOf<FakePlayer>()

    fun audible(player: FakePlayer) = (player.started && !player.released && !player.errored) || player in heldSilences

    /** The first player of the play now: the overlap check is within a play (Stop and a replacement do not wait). */
    var playFirst = 0

    /** The oldest held silence reaches the machine; its player's release, queued behind it on its owner, then runs. */
    fun ackSilence() {
        val player = heldSilences.removeAt(0)
        machine.onSilenced(player.token)
        for (blocked in silenceBlocked.filter { it.first === player }) {
            silenceBlocked.remove(blocked)
            blocked.second()
        }
    }

    /** Releases queued on their owner behind a held silence (the owner is one thread: first in, first out). */
    val silenceBlocked = mutableListOf<Pair<FakePlayer, () -> Unit>>()

    /** Silences the owner never answers (its pause threw: R62); only the release's end is left to say it. */
    var refuseSilences = false

    /** Releases: their threads end at once unless [holdReleases] (held in native code, in [heldReleases]). */
    var holdReleases = false
    val heldReleases = mutableListOf<Pair<FakePlayer, () -> Unit>>()
    val focus = FakeFocus()
    val scheduler = FakeScheduler { nowMs }
    val service = FakeService()
    val recheck = RecordingRecheck()
    val abandoned = AbandonedSetups()
    val machine: RecordingPlayer = RecordingPlayer(
        { token ->
            if (factoryFails) {
                throw RuntimeException("the media server is away")
            }
            if (threadFails) {
                throw OutOfMemoryError("unable to create a native thread")
            }
            log += "build $token"
            FakePlayer(
                token, length, damaged, log, threads, { createFails },
                { player, serial ->
                    check(players.drop(playFirst).none { it !== player && audible(it) }) {
                        "a start made while another player of the play sounds"
                    }
                    when {
                        failStarts -> machine.onFailed(player.token, RecordingPlayer.REASON_PLAYER)
                        holdStarts -> heldStarts += player to serial
                        player.makeStart(serial) -> machine.onStarted(player.token, serial)
                    }
                },
                onSeek = { player, serial ->
                    if (holdSeeks) heldSeeks += player to serial else machine.onSeekCompleted(player.token, serial)
                },
                onSilence = { player ->
                    when {
                        refuseSilences -> Unit
                        holdSilences -> heldSilences += player
                        else -> machine.onSilenced(player.token)
                    }
                },
            ) { player, ended ->
                when {
                    holdReleases -> heldReleases += player to ended
                    player in heldSilences -> silenceBlocked += player to ended
                    else -> ended()
                }
            }.also { players += it }
        },
        opener,
        focus,
        { nowMs },
        scheduler,
        service,
        recheck,
        foreground,
        abandoned,
    )
    var generation = 0

    val status: RecordingPlayer.Status get() = machine.status
    val state: Int get() = machine.status.state
    val position: Long get() = machine.status.positionMs(nowMs)
    val last: FakePlayer get() = players.last()

    /**
     * The players handed a file. A player is built before its file is opened (on its own thread), so one whose file
     * was gone or changed was built and released without one.
     */
    val given: List<FakePlayer> get() = players.filter { it.asked }

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
        playFirst = players.size
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

    /** [player] reported an error, as MediaPlayer's error listener says on the looper; it is in the Error state. */
    fun error(player: FakePlayer, reason: String) {
        player.errored = true
        machine.onFailed(player.token, reason)
    }

    /** The current player played to its end. */
    fun complete(player: FakePlayer = current) {
        player.started = false
        machine.onCompleted(player.token)
    }

    /**
     * The oldest held start reaches its player's thread, which makes it if it is still wanted, and its acknowledgement
     * reaches the machine: true when it was made.
     */
    fun ackStart(): Boolean {
        val (player, serial) = heldStarts.removeAt(0)
        if (!player.makeStart(serial)) {
            return false
        }
        machine.onStarted(player.token, serial)
        return true
    }

    /** The oldest held release returns from native code: its thread ends, as the looper hears. */
    fun finishRelease() {
        heldReleases.removeAt(0).second()
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
    val record = "$generation\t$PLAYING\t1500\t60000\t\t0\t$token\t0\t1\t1\t1\t0\t0\t1\t\t\t0\t0\t0\t-1\t0\t0"
    check(rig.status.record(rig.nowMs) == record) { "the record is what the host parses: ${rig.status.record(rig.nowMs)}" }
    check(rig.status.record(rig.nowMs).split('\t').size == 22) { "the record has its 22 fields" }

    rig.machine.pause()
    check(rig.state == PAUSED && !rig.last.started && !rig.focus.held) { "a pause pauses and gives the focus back" }
    check(rig.status.pausedReason == RecordingPlayer.PAUSED_USER) { "the pause says it was the user's" }
    rig.nowMs += 5_000
    check(rig.position == 1_500L) { "the position holds while paused" }
    rig.machine.resume()
    check(rig.state == PLAYING && rig.focus.requests.size == 2) { "a resume asks for the focus again and plays" }
    rig.nowMs += 70_000
    check(rig.position == 60_000L) { "the position never runs past the length" }
    val prepareTimeoutsOnly = rig.scheduler.entries.all { it.delayMs == RecordingPlayer.PREPARE_TIMEOUT_MS }
    check(rig.service.changes.isEmpty() && prepareTimeoutsOnly) {
        "a single play uses no service and no task but its prepare timeout"
    }
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
    check(rig.players.single().released && !rig.players.single().asked) {
        "the player built before the open is released, never given a file"
    }
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
    check(single.opener.opened.isEmpty() && !single.focus.held) {
        "built before the open: nothing opened, nothing held"
    }

    val playlist = Rig()
    playlist.factoryFails = true
    playlist.queue(1, 1, 1)
    check(playlist.state == FAILED && playlist.status.endedReason == RecordingPlayer.ENDED_FAILED) {
        "a playlist whose player cannot be built ends failed"
    }
    playlist.turn()
    check(playlist.opener.opened.isEmpty() && playlist.status.skipped == 0) {
        "with no skip: it is not the file's fault"
    }
    check(playlist.service.ends.single() == playlist.generation to RecordingPlayer.ENDED_FAILED) { "the service is told once" }

    // Its own thread cannot start: an Error, which is the player's failure too, never a crashed looper.
    val noThread = Rig()
    noThread.threadFails = true
    noThread.queue(1, 1)
    check(noThread.state == FAILED && noThread.status.endedReason == RecordingPlayer.ENDED_FAILED) {
        "a player whose thread cannot start ends the playlist failed"
    }
    check(noThread.status.skipped == 0 && noThread.opener.opened.isEmpty()) { "with no skip and nothing opened" }

    // The platform player is made on the part's own thread, after its file opened: one that cannot be made there is the
    // player's failure, delivered as a failed setup, so a playlist ends with no skip; what was opened is closed.
    val unmade = Rig()
    unmade.createFails = true
    unmade.queue(1, 1)
    check(unmade.state == FAILED && unmade.status.endedReason == RecordingPlayer.ENDED_FAILED) {
        "a player that cannot be made on its thread ends the playlist failed (${unmade.status.endedReason})"
    }
    check(unmade.status.skipped == 0 && unmade.opener.opened == listOf(path(0, 1))) { "no skip, nothing tried after" }
    check(unmade.opener.sources.single().closed && unmade.last.released && !unmade.last.asked) {
        "the file opened is closed and the player released, never handed the file"
    }

    // A thread that refuses the setup (it has quit) delivers a failed setup, the player's: the play ends.
    val refused = Rig()
    refused.threads.refusing = true
    refused.play()
    check(refused.state == FAILED && refused.status.error == RecordingPlayer.ERROR_FAILED) {
        "a refused hand-off fails"
    }
    check(refused.opener.opened.isEmpty() && refused.last.released) { "nothing opened, its player released" }

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
    check(rig.scheduler.entries.none { it.delayMs == RecordingPlayer.GAP_MS }) { "no gap between parts" }

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

/**
 * A restart that fails keeps searching backward (R55): with A, B and C, B deleted while it plays and Previous after
 * 3 s, B's reopen fails and A plays, not C; with B the last call, A plays and the playlist does not finish.
 */
private fun aFailedRestartKeepsSearchingBackward() {
    for (last in listOf(false, true)) {
        val rig = Rig()
        if (last) rig.queue(1, 1) else rig.queue(1, 1, 1)
        rig.prepared()
        rig.machine.next()
        rig.prepared()
        check(rig.status.call == 1 && rig.state == PLAYING) { "B plays" }
        rig.elapse(3_500)
        rig.fail(path(1, 1), RecordingPlayer.REASON_GONE)
        rig.machine.previous()
        rig.turn()
        check(rig.status.call == 0 && rig.state != STOPPED) { "last $last: A opens, on call ${rig.status.call}" }
        rig.prepared()
        check(rig.status.call == 0 && rig.state == PLAYING && rig.service.ends.isEmpty()) {
            "last $last: A plays, and the playlist goes on (${rig.state}, ${rig.service.ends})"
        }
    }
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
    check(rig.scheduler.live().none { it.delayMs in 1..RecordingPlayer.GAP_MS }) { "no gap task is left" }
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

/**
 * Every open starts at position 0 and an unknown length (#909): a skip that opens the next part after one failed while
 * it played never carries the failed part's readings. B1 plays 4 s of 60 and fails; while B2 opens, the record shows
 * 0 and unknown, and Previous applies the 3,000 ms rule to B2's own position, so it goes to call A. The gap after a
 * call still names the part that played in it.
 */
private fun aSkipOpensThePartWithItsOwnReadings() {
    val rig = Rig()
    rig.queue(1, 2)
    rig.playOut()
    rig.elapse(RecordingPlayer.GAP_MS)
    rig.prepared()
    val b1 = rig.current
    rig.elapse(4_000)
    check(rig.status.call == 1 && rig.position == 4_000L && rig.status.durationMs == 60_000L) { "B1 plays at 4 s" }
    rig.error(b1, RecordingPlayer.REASON_DAMAGED)
    rig.turn()
    check(rig.status.partOrdinal == 2 && rig.state == PREPARING && rig.status.skipped == 1) { "B1 skipped, B2 opens" }
    check(rig.position == 0L && rig.status.durationMs == 0L && !rig.status.canSeek) {
        "B2 opening reads 0 and unknown, not B1's 4 s of 60 (${rig.position}, ${rig.status.durationMs})"
    }
    val record = rig.status.record(rig.nowMs).split('\t')
    check(record[2] == "0" && record[3] == "0") { "and so does its record: ${record.take(4)}" }
    check(rig.service.changes.last().positionMs(rig.nowMs) == 0L && rig.service.changes.last().durationMs == 0L) {
        "and the session's reading"
    }
    rig.machine.previous()
    check(rig.status.call == 0 && rig.last.path == path(0, 1)) { "Previous before B2 prepares goes to call A" }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.call == 0) { "which plays" }

    // A call's last part fails while it plays, and the call's remaining part is gone: the gap names the part that
    // played, at the position it reached, not the part tried after it.
    val gap = Rig()
    gap.queue(2, 1)
    gap.prepared()
    val first = gap.current
    gap.elapse(4_000)
    gap.fail(path(0, 2), RecordingPlayer.REASON_GONE)
    gap.error(first, RecordingPlayer.REASON_DAMAGED)
    gap.turn()
    gap.turn()
    check(gap.status.gap && gap.status.partOrdinal == 1 && gap.status.playToken == first.token) {
        "the gap names part 1, which played (${gap.status.partOrdinal})"
    }
    check(gap.position == 4_000L && gap.status.durationMs == 60_000L) { "at its own readings" }
}

/**
 * Each move makes only the candidates it tries (#909), never a copy of the queue's tail: playing N healthy calls, or N
 * parts of one call, makes N candidates, not N²/2; Previous from far along makes one; a skip makes each one it tries.
 */
private fun eachMoveMakesOnlyTheCandidatesItTries() {
    val count = 300
    val calls = Rig()
    calls.queue(*IntArray(count) { 1 })
    repeat(count) { i ->
        calls.playOut()
        if (i < count - 1) {
            calls.elapse(RecordingPlayer.GAP_MS)
        }
    }
    check(calls.status.endedReason == RecordingPlayer.ENDED_FINISHED && calls.players.size == count) { "all played" }
    check(calls.machine.candidatesMade == count.toLong()) { "$count calls: ${calls.machine.candidatesMade} candidates" }

    val parts = Rig()
    parts.queue(count, mode = MODE_PARTS)
    repeat(count) { parts.playOut() }
    check(parts.status.endedReason == RecordingPlayer.ENDED_FINISHED && parts.players.size == count) { "all parts" }
    check(parts.machine.candidatesMade == count.toLong()) { "$count parts: ${parts.machine.candidatesMade} candidates" }

    val back = Rig()
    back.queue(*IntArray(count) { 1 }, start = count - 1)
    val made = back.machine.candidatesMade
    back.machine.previous()
    check(back.status.call == count - 2 && back.machine.candidatesMade == made + 1) {
        "Previous from the last call makes one candidate (${back.machine.candidatesMade - made})"
    }

    val skip = Rig()
    for (call in 1..3) skip.fail(path(call, 1), RecordingPlayer.REASON_GONE)
    skip.queue(*IntArray(count) { 1 })
    skip.prepared()
    val before = skip.machine.candidatesMade
    skip.machine.next()
    repeat(3) { skip.turn() }
    check(skip.status.call == 4 && skip.machine.candidatesMade == before + 4) {
        "Next past three gone calls makes the four it tries (${skip.machine.candidatesMade - before})"
    }
}

/**
 * A start is the player's thread's to make and acknowledge (#909, R39): the machine plays from the acknowledgement,
 * never from the request. Until then the reading stays as it was and the position does not run; a start taken back
 * (a pause, a focus loss, Stop, a move, a sheet play) is never made, and its late acknowledgement changes nothing.
 */
private fun aStartPlaysFromItsAcknowledgement() {
    // Resume with the acknowledgement held: paused, the position still; the acknowledgement plays it.
    val held = Rig()
    held.queue(1, 1)
    held.prepared()
    held.elapse(2_000)
    held.machine.pause()
    held.holdStarts = true
    held.machine.resume()
    check(held.heldStarts.size == 1 && held.current.made.size == 1) { "the start is asked for, not made yet" }
    held.elapse(5_000)
    check(held.state == RecordingPlayer.STARTING && held.position == 2_000L && held.status.speed == 0f) {
        "starting, not playing, before the acknowledgement (${held.state}, ${held.position})"
    }
    // R49: the controls follow the wish, the clock the audio.
    check(held.status.sessionState == RecordingPlayer.SESSION_BUFFERING && held.status.allows(ACTION_PAUSE)) {
        "the session buffers, with Pause (${held.status.sessionState})"
    }
    check(held.status.allows(ACTION_SEEK) && !held.status.allows(ACTION_PLAY)) { "seek allowed, no Play" }
    check(held.status.record(held.nowMs).split('\t')[1] == "5") { "the record carries the starting state" }
    held.machine.resume()
    check(held.heldStarts.size == 1) { "never asked for twice" }
    check(held.ackStart() && held.state == PLAYING) { "the acknowledgement plays it" }
    held.elapse(1_000)
    check(held.position == 3_000L) { "the position runs from the acknowledgement: ${held.position}" }

    // A first start held reads starting too, its seek allowed (R45).
    val first = Rig()
    first.holdStarts = true
    first.queue(1, 1)
    first.prepared()
    check(first.state == RecordingPlayer.STARTING && first.focus.held) { "prepared, its start waits (${first.state})" }
    check(first.status.canSeek && first.status.allows(ACTION_PAUSE)) { "with seek and Pause" }
    first.ackStart()
    check(first.state == PLAYING && first.status.canSeek) { "then playing, with its seek" }

    // Pause while a start is pending takes it back: paused, no start made.
    val undo = Rig()
    undo.queue(1, 1)
    undo.holdStarts = true
    undo.prepared()
    undo.machine.pause()
    check(undo.state == PAUSED && undo.status.sessionState == RecordingPlayer.SESSION_PAUSED) { "paused (${undo.state})" }
    check(!undo.ackStart() && undo.current.made.isEmpty()) { "and the start is never made" }

    // A part opened paused (Next while paused), then Resume with its start held: starting, with seek; the
    // acknowledgement plays it.
    val opened = Rig()
    opened.queue(1, 1)
    opened.prepared()
    opened.machine.pause()
    opened.machine.next()
    opened.prepared()
    check(opened.state == PAUSED && opened.status.call == 1) { "call 1 opened paused (${opened.state})" }
    opened.holdStarts = true
    opened.machine.resume()
    check(opened.heldStarts.size == 1) { "its first start is asked for" }
    check(opened.state == RecordingPlayer.STARTING && opened.status.canSeek && opened.status.allows(ACTION_SEEK)) {
        "until its acknowledgement it reads starting, seek allowed (${opened.state}, ${opened.status.canSeek})"
    }
    check(opened.ackStart() && opened.state == PLAYING) { "the acknowledgement plays it" }

    // Resume 1 s before the deadline, then a transient focus loss before the acknowledgement: the old stretch counts.
    val late = Rig()
    late.queue(1, 1)
    late.prepared()
    late.machine.pause()
    late.elapse(RecordingPlayer.PAUSED_LIMIT_MS - 1_000)
    late.holdStarts = true
    late.machine.resume()
    late.machine.onFocusChange(late.focus.epoch, FocusChange.LOST_TRANSIENT)
    check(late.current.made.size == 1 && late.state != STOPPED) { "taken back before it was made" }
    check(!late.ackStart()) { "the held start is never made" }
    late.elapse(999)
    check(late.state != STOPPED) { "not yet at 9:59.999" }
    late.elapse(1)
    check(late.state == STOPPED && late.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) {
        "the deadline fires at its original time (${late.status.endedReason})"
    }

    // Stop, Next and a sheet play before the acknowledgement: that start is never made, its late one changes nothing.
    for (leave in listOf("stop", "next", "sheet")) {
        val rig = Rig()
        rig.holdStarts = true
        val g = rig.queue(1, 1)
        rig.prepared()
        val asked = rig.current
        val (_, serial) = rig.heldStarts.single()
        when (leave) {
            "stop" -> rig.machine.stop(g)
            "next" -> rig.machine.next()
            else -> rig.play()
        }
        val reading = rig.status.record(rig.nowMs)
        check(!rig.ackStart() && asked.made.isEmpty() && !asked.started) { "$leave: the start is never made" }
        rig.machine.onStarted(asked.token, serial)
        check(rig.status.record(rig.nowMs) == reading && rig.state != PLAYING) {
            "$leave: a late acknowledgement changes nothing"
        }
    }

    // A start the player's thread fails is the player's failure.
    val single = Rig()
    single.failStarts = true
    single.play()
    single.prepared()
    check(single.state == FAILED && single.status.error == RecordingPlayer.ERROR_FAILED && single.last.released) {
        "a single play whose start fails is Failed"
    }
    val list = Rig()
    list.failStarts = true
    list.queue(1, 1)
    list.prepared()
    check(list.state == FAILED && list.status.endedReason == RecordingPlayer.ENDED_FAILED && list.status.skipped == 0) {
        "a playlist whose start fails ends failed, as any player failure (${list.status.endedReason})"
    }

    // Previous's 3,000 ms rule counts from the acknowledgement, not from the request.
    val back = Rig()
    back.queue(1, 1)
    back.playOut()
    back.elapse(RecordingPlayer.GAP_MS)
    back.holdStarts = true
    back.prepared()
    back.elapse(2_500)
    back.ackStart()
    back.elapse(1_000)
    back.machine.previous()
    check(back.status.call == 0) { "1 s played after a 2.5 s wait: Previous goes to the call before (${back.status.call})" }
}

/**
 * The queue is let go when the play ends (R43): finished, stopped, timed out or unplayable, the machine holds none of
 * its recordings, and the terminal record (the item it ended on, its call count, the counters) stays as it was, also
 * when a later setup result publishes again.
 */
private fun theQueueIsLetGoAtTheEnd() {
    fun withoutSerial(record: String) = record.split('\t').filterIndexed { i, _ -> i != 17 }

    // Finished, with a superseded setup landing after: its gone file is rechecked and published, the reading kept.
    val done = Rig()
    done.threads.stuck += 1
    done.queue(1, 2)
    done.machine.next()
    done.prepared()
    done.playOut()
    check(done.machine.queueHeld > 0 && done.status.partOrdinal == 2) { "the play holds its queue while it plays" }
    done.playOut()
    check(done.status.endedReason == RecordingPlayer.ENDED_FINISHED && done.machine.queueHeld == 0) {
        "finished: the queue is let go (${done.machine.queueHeld})"
    }
    val record = done.status.record(done.nowMs)
    check(done.status.item?.path == path(1, 2) && done.status.calls == 2 && done.status.partCount == 2) {
        "the terminal reading still names the part it ended on and the calls"
    }
    val serial = done.status.recheckSerial
    done.fail(path(0, 1), RecordingPlayer.REASON_GONE)
    done.threads.setups.single().land()
    check(done.status.recheckSerial == serial + 1) { "a later result publishes again" }
    check(withoutSerial(done.status.record(done.nowMs)) == withoutSerial(record)) {
        "with the terminal record unchanged: ${done.status.record(done.nowMs)}"
    }

    // Stopped, timed out and unplayable let it go too.
    val stopped = Rig()
    val g = stopped.queue(1, 1)
    stopped.prepared()
    stopped.machine.stop(g)
    check(stopped.status.endedReason == RecordingPlayer.ENDED_STOPPED && stopped.machine.queueHeld == 0) { "stopped" }
    check(stopped.status.itemIndex == 0 && stopped.status.calls == 2) { "naming its item and calls" }
    val paused = Rig()
    paused.queue(1, 1)
    paused.prepared()
    paused.machine.pause()
    paused.elapse(RecordingPlayer.PAUSED_LIMIT_MS)
    check(paused.status.endedReason == RecordingPlayer.ENDED_TIMEOUT && paused.machine.queueHeld == 0) { "timed out" }
    val none = Rig()
    none.fail(path(0, 1), RecordingPlayer.REASON_GONE)
    none.fail(path(1, 1), RecordingPlayer.REASON_GONE)
    none.queue(1, 1)
    none.turn()
    check(none.status.endedReason == RecordingPlayer.ENDED_UNPLAYABLE && none.machine.queueHeld == 0) { "unplayable" }
    check(none.status.skipped == 2 && none.status.itemIndex == 1) { "with its counters and the item it ended on" }

    // The host's own stop (no generation) lets it go too, the failed marks and call starts included.
    val host = Rig()
    host.queue(2, 1, 3)
    host.prepared()
    check(host.machine.queueHeld > 0) { "a play holds its queue" }
    host.machine.stop()
    check(host.state == STOPPED && host.status.generation == 0 && host.machine.queueHeld == 0) {
        "the host's stop leaves nothing held (${host.machine.queueHeld})"
    }
}

/**
 * The emulator's Previous burst (D1, evidence emu-r10): five Previous from Call 6 of 7 (two parts, the first 1.44 s
 * long). While its first part plays, each press moves one call and the burst ends on Call 1. In the failing runs that
 * part had completed just as the burst arrived (its player released unpaused), so the first press found the second
 * part current and restarted the call (D7: a playable part of the call before the current one), and the burst ended on
 * Call 2. No press is lost in either order: the opening part or its start pending changes nothing.
 */
private fun aPreviousBurstAtAPartBoundaryRestartsTheCallFirst() {
    for (boundary in listOf(false, true)) {
        for (heldStart in listOf(false, true)) {
            val rig = Rig()
            rig.queue(12, 1, 1, 1, 1, 2, 1)
            repeat(5) { rig.machine.next() }
            rig.prepared()
            rig.elapse(1_440)
            check(rig.status.call == 5 && rig.status.partOrdinal == 1) { "Call 6's first part plays" }
            if (boundary) {
                rig.complete()
                check(rig.status.call == 5 && rig.status.partOrdinal == 2) { "its second part opens" }
            }
            rig.holdStarts = heldStart
            rig.machine.previous()
            if (boundary) {
                check(rig.status.call == 5 && rig.status.partOrdinal == 1) { "from part 2, Previous restarts Call 6" }
            } else {
                check(rig.status.call == 4) { "from the first part, Previous moves to Call 5" }
            }
            // The first press's candidate plays, as in the failing run 4, before the other four.
            rig.prepared()
            if (heldStart) {
                rig.ackStart()
            }
            check(rig.state == PLAYING) { "the first press's candidate plays" }
            repeat(4) { rig.machine.previous() }
            rig.prepared()
            if (heldStart) {
                rig.ackStart()
            }
            val ends = if (boundary) 1 else 0
            check(rig.status.call == ends && rig.state == PLAYING) {
                "boundary $boundary, start held $heldStart: on call ${rig.status.call}, not $ends"
            }
        }
    }
}

/**
 * An opening that is wanted but interrupted (the focus, held across the parts, lost for a while before the next part
 * prepared) reads PAUSED on the session, not BUFFERING: the interruption ranks above the wish (m5).
 */
private fun anInterruptedOpeningReadsPausedOnTheSession() {
    val rig = Rig()
    rig.queue(2, 1)
    rig.prepared()
    rig.complete()
    check(rig.status.partOrdinal == 2 && rig.status.state == PREPARING) { "part 2 opens" }
    check(rig.status.sessionState == SESSION_BUFFERING) { "a wanted opening buffers" }
    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.LOST_TRANSIENT)
    check(rig.status.interrupted && rig.status.state == PREPARING) {
        "interrupted while it opens (${rig.status.interrupted}, ${rig.status.state})"
    }
    check(rig.status.sessionState == SESSION_PAUSED) { "interrupted, it reads paused: ${rig.status.sessionState}" }
    rig.machine.onFocusChange(rig.focus.epoch, FocusChange.GAINED)
    check(rig.status.sessionState == SESSION_BUFFERING) { "the focus back: buffering again" }
}

/**
 * A seek is published as landed only once the player confirms it (R56): until the latest seek's completion the
 * readings keep the confirmed position and its clock, Previous's 3,000 ms rule reads it, and a superseded seek's
 * completion counts for nothing; the landing counts in the record.
 */
private fun aSeekLandsOnlyWhenThePlayerConfirmsIt() {
    val rig = Rig()
    rig.queue(1, 1)
    rig.prepared()
    rig.elapse(1_000)
    rig.holdSeeks = true
    val published = rig.service.changes.size
    rig.machine.seek(20_000)
    check(rig.service.changes.size == published) { "a plain seek publishes nothing until it lands (M4)" }
    check(rig.current.seeks == listOf(20_000L) && rig.position == 1_000L) {
        "the seek is posted; the position stays the confirmed one: ${rig.position}"
    }
    rig.elapse(500)
    check(rig.position == 1_500L && rig.status.seeksLanded == 0) { "its clock runs on from there: ${rig.position}" }
    rig.landSeek()
    check(rig.position == 20_000L && rig.status.seeksLanded == 1) { "landed: the target, counted" }
    check(rig.service.changes.size == published + 1 && rig.service.changes.last().positionMs(rig.nowMs) == 20_000L) {
        "the landing publishes once, at the target"
    }
    rig.elapse(1_000)
    check(rig.position == 21_000L) { "and the clock counts from it" }

    // Two seeks, the first completing late: only the second lands.
    rig.machine.seek(5_000)
    rig.machine.seek(40_000)
    rig.landSeek() // the first's completion, late
    check(rig.position == 21_000L && rig.status.seeksLanded == 1) { "a superseded seek lands nothing: ${rig.position}" }
    rig.landSeek()
    check(rig.position == 40_000L && rig.status.seeksLanded == 2) { "the latest lands" }

    // Previous reads the confirmed position: a seek back to 0 not landed yet still restarts the call.
    val back = Rig()
    back.queue(1, 1)
    back.prepared()
    back.machine.next()
    back.prepared()
    back.elapse(5_000)
    back.holdSeeks = true
    back.machine.seek(0)
    back.machine.previous()
    back.prepared()
    check(back.status.call == 1) { "past 3 s confirmed: Previous restarts the call, on call ${back.status.call}" }
}

/**
 * The silence wait covers a replacement too (R52, the M3 ruling): a sheet play replacing a playlist whose player A
 * sounds waits for A's silence before its first start, under its own watchdog; A's silence counts only for that wait.
 */
private fun aReplacementStartsOnceTheOldPlayIsSilent() {
    for (silent in listOf(true, false)) {
        val rig = Rig()
        rig.queue(1, 1)
        rig.prepared()
        val a = rig.current
        check(rig.state == PLAYING) { "the playlist plays A" }
        rig.holdSilences = true
        rig.machine.play(++rig.generation, "/r/x.wav") // a sheet's play of X
        check(rig.heldSilences == listOf(a) && a.ops.takeLast(2) == listOf("pause", "silence")) {
            "the replacement pauses A, then asks for its silence: ${a.ops}"
        }
        rig.prepared()
        val x = rig.current
        check(x !== a && x.startRequests.isEmpty() && rig.state == STARTING) { "X prepared: its start waits" }
        if (silent) {
            rig.ackSilence()
            check(x.startRequests.size == 1 && rig.state == PLAYING) { "A silent: X starts" }
        } else {
            rig.elapse(RecordingPlayer.START_TIMEOUT_MS)
            check(rig.state == FAILED && x.startRequests.isEmpty()) { "A's silence never comes: X's watchdog ends it" }
        }
    }

    // Stop during the wait: the play ends at once, nothing starts, and A's late silence changes nothing.
    val stop = Rig()
    stop.queue(1, 1)
    stop.prepared()
    stop.holdSilences = true
    stop.machine.play(++stop.generation, "/r/x.wav")
    stop.prepared()
    val x = stop.current
    check(stop.state == STARTING && stop.heldSilences.size == 1) { "X waits for A's silence" }
    stop.machine.stop(stop.generation)
    check(stop.state == STOPPED && x.startRequests.isEmpty() && x.released) { "Stop ends X at once, unstarted" }
    stop.elapse(RecordingPlayer.START_TIMEOUT_MS)
    check(stop.state == STOPPED && x.startRequests.isEmpty()) { "the watchdog's time changes nothing" }
    // The wait outlives the play (R61): the next play waits for A too, and A's silence starts it, not X.
    stop.holdSilences = false
    stop.queue(1, 1)
    stop.prepared()
    val y = stop.current
    check(stop.state == STARTING && y.startRequests.isEmpty()) { "a play after the Stop waits for A's silence" }
    stop.ackSilence()
    check(stop.state == PLAYING && y.startRequests.size == 1 && x.startRequests.isEmpty()) {
        "A's silence starts the next play, once, and nothing of X"
    }
}

/**
 * Every end lets a player that may sound go silent first (R61, I1, I2): with A playing and its owner slow, Stop, the
 * host's own stop, a lasting focus loss and a failure of A's player each pause A and ask for its silence, the end
 * published at once; the next play prepares and its start waits for A's silence, which starts it once.
 */
private fun everyEndLetsASoundingPlayerFallSilentFirst() {
    val ends = listOf<Pair<String, (Rig) -> Unit>>(
        "Stop" to { rig -> rig.machine.stop(rig.generation) },
        "the host's stop" to { rig -> rig.machine.stop() },
        "a lasting focus loss" to { rig -> rig.machine.onFocusChange(rig.focus.epoch, FocusChange.LOST) },
        "a failure of A's player" to { rig -> rig.machine.onFailed(rig.current.token, RecordingPlayer.REASON_PLAYER) },
    )
    for ((name, endIt) in ends) {
        val rig = Rig()
        rig.queue(1, 1)
        rig.prepared()
        val a = rig.current
        check(rig.state == PLAYING) { "$name: A plays" }
        rig.holdSilences = true
        endIt(rig)
        check(rig.state != PLAYING && rig.state != STARTING) { "$name: the end is published at once (${rig.state})" }
        check(rig.heldSilences == listOf(a) && a.ops.takeLast(2) == listOf("pause", "silence")) {
            "$name: A is paused, then asked for its silence: ${a.ops}"
        }
        rig.holdSilences = false
        rig.machine.play(++rig.generation, "/r/next.wav")
        rig.prepared()
        val next = rig.current
        check(next !== a && rig.state == STARTING && next.startRequests.isEmpty()) { "$name: the next start waits" }
        rig.ackSilence()
        check(rig.state == PLAYING && next.startRequests.size == 1) { "$name: A silent: it starts, once" }
    }
}

/**
 * An ended owner thread is silent (M3): A's owner never answers its silence (its pause threw, so it waits for the
 * release, which threw too), and A's thread ends. The next play's start goes on as the thread ends, with no 10 s wait.
 */
private fun anEndedOwnerThreadIsSilent() {
    val rig = Rig()
    rig.queue(1, 1)
    rig.prepared()
    rig.refuseSilences = true
    rig.holdReleases = true
    rig.machine.play(++rig.generation, "/r/next.wav")
    rig.prepared()
    val next = rig.current
    check(rig.state == STARTING && next.startRequests.isEmpty()) { "no silence answered: the next start waits" }
    rig.elapse(1_000)
    rig.finishRelease() // A's release returns (or throws) and its thread ends
    check(rig.state == PLAYING && next.startRequests.size == 1) { "A's thread ended: silent, the next start goes on" }
}

/**
 * A completion while paused is held (the emulator's defect, ruled): the platform plays out what it had buffered at the
 * Pause and reports completion. The part stays current, paused at its end, with no seek; Resume moves on as if it had
 * just completed: the next part, or the gap after a call's last part. Previous reads the end position (the 3,000 ms
 * rule), and the paused deadline counts on.
 */
private fun aCompletionWhilePausedIsHeld() {
    val rig = Rig()
    rig.queue(2, 1)
    rig.prepared()
    rig.elapse(9_850)
    rig.machine.pause()
    val first = rig.current
    rig.complete()
    check(rig.status.call == 0 && rig.status.partOrdinal == 1 && rig.state == PAUSED) {
        "still part 1, paused (${rig.status.partOrdinal}, ${rig.state})"
    }
    check(rig.position == rig.status.durationMs && !rig.status.canSeek) { "at its end, no seek: ${rig.position}" }
    check(rig.players.last() === first) { "nothing opened yet" }
    rig.machine.resume()
    check(rig.status.partOrdinal == 2 && rig.players.last() !== first) { "Resume: on to part 2" }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.partOrdinal == 2) { "and part 2 plays" }

    // At a call's last part: Resume enters the gap before the next call.
    rig.elapse(1_000)
    rig.machine.pause()
    rig.complete()
    check(rig.status.call == 0 && rig.status.partOrdinal == 2 && rig.state == PAUSED && !rig.status.gap) {
        "the call's last part, held paused at its end"
    }
    rig.machine.resume()
    check(rig.status.gap && rig.state == PLAYING) { "Resume: the gap before the next call" }

    // Previous from a held completion reads the end position: past 3,000 ms it restarts the call.
    val back = Rig()
    back.queue(1, 1)
    back.prepared()
    back.machine.next()
    back.prepared()
    back.elapse(9_000)
    back.machine.pause()
    back.complete()
    back.machine.previous()
    check(back.status.call == 1) { "Previous at the end position restarts the call, on call ${back.status.call}" }

    // The paused deadline counts on through a held completion.
    val late = Rig()
    late.queue(1, 1)
    late.prepared()
    late.machine.pause()
    late.complete()
    late.elapse(RecordingPlayer.PAUSED_LIMIT_MS)
    check(late.state == STOPPED && late.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) { "the deadline ends it" }
}

/**
 * A silence outstanding outlives a normal end (R61): A sounds, its pause held, and Next finds every later recording
 * gone, so the play ends finished at once; a Play then prepares C and its start waits for A's silence, which starts it.
 */
private fun aSilenceOutlivesANormalEnd() {
    val rig = Rig()
    rig.fail(path(1, 1), RecordingPlayer.REASON_GONE)
    rig.fail(path(2, 1), RecordingPlayer.REASON_GONE)
    rig.queue(1, 1, 1)
    rig.prepared()
    val a = rig.current
    rig.holdSilences = true
    rig.machine.next()
    rig.turn()
    rig.turn()
    check(rig.state == STOPPED && rig.status.endedReason == RecordingPlayer.ENDED_FINISHED) {
        "every later recording gone: finished, published at once (${rig.state}, ${rig.status.endedReason})"
    }
    check(rig.heldSilences == listOf(a)) { "A's silence is still outstanding" }
    rig.holdSilences = false
    rig.machine.play(++rig.generation, "/r/c.wav")
    rig.prepared()
    val c = rig.current
    check(c !== a && rig.state == STARTING && c.startRequests.isEmpty()) { "C prepares; its start waits for A" }
    rig.ackSilence()
    check(rig.state == PLAYING && c.startRequests.size == 1) { "A silent: C starts" }
}

/**
 * The next part starts only once the outgoing one is silent (R52): a move lets go of a player that may sound and asks
 * its owner for its silence; the next part opens and prepares meanwhile, and its start, asked for under its watchdog,
 * is posted only once every such player is silent. A player that never sounded needs none; Stop and a replacement
 * need no wait, and a silence from an earlier play is ignored.
 */
private fun theNextPartStartsOnceTheOutgoingOneIsSilent() {
    val rig = Rig()
    rig.queue(1, 1, 1, 1, 1, 1, 1)
    rig.prepared()
    val a = rig.current
    check(rig.state == PLAYING) { "A plays" }
    rig.holdSilences = true
    rig.machine.next()
    check(rig.heldSilences == listOf(a) && a.ops.takeLast(2) == listOf("pause", "silence")) {
        "Next pauses A, then asks its owner for its silence: ${a.ops}"
    }
    rig.prepared()
    val b = rig.current
    check(b.startRequests.isEmpty() && rig.state == STARTING) { "B prepared: its start waits, the reading starting" }
    rig.ackSilence()
    check(b.startRequests.size == 1 && rig.state == PLAYING) { "A silent: B's start is posted, and B plays" }

    // A silence that never comes: B's watchdog ends the play as the player's.
    val stuck = Rig()
    stuck.queue(1, 1)
    stuck.prepared()
    stuck.holdSilences = true
    stuck.machine.next()
    stuck.prepared()
    stuck.elapse(RecordingPlayer.START_TIMEOUT_MS - 1)
    check(stuck.state == STARTING && stuck.current.startRequests.isEmpty()) { "waiting, under the watchdog" }
    stuck.elapse(1)
    check(stuck.state == FAILED && stuck.service.ends.size == 1) {
        "the watchdog ends the play at 10 s (${stuck.state}, ${stuck.service.ends})"
    }
    // The documented limit: a wait that failed at its watchdog is dropped, so the next play does not wait again.
    stuck.queue(1, 1)
    stuck.prepared()
    check(stuck.state == PLAYING && stuck.current.startRequests.size == 1) { "the next play starts at once" }

    // Five Next with the silences late: only the last candidate starts, once.
    val burst = Rig()
    burst.queue(1, 1, 1, 1, 1, 1, 1)
    burst.prepared()
    burst.holdSilences = true
    repeat(5) {
        burst.machine.next()
        burst.prepared()
    }
    check(burst.heldSilences.size == 1 && burst.status.call == 5) { "only A sounded: one silence asked for" }
    check(burst.players.drop(1).none { it.startRequests.isNotEmpty() }) { "no start posted yet" }
    burst.ackSilence()
    val started = burst.players.drop(1).filter { it.startRequests.isNotEmpty() }
    check(started == listOf(burst.current) && burst.current.startRequests.size == 1 && burst.state == PLAYING) {
        "the last candidate starts, once"
    }

    // A player that never sounded (paused before it started) needs no silence: B starts at once.
    val quiet = Rig()
    quiet.queue(1, 1)
    quiet.machine.pause()
    quiet.prepared()
    quiet.holdSilences = true
    quiet.machine.resume()
    quiet.machine.pause()
    quiet.machine.next()
    check(quiet.heldSilences.size == 1) { "a player that had a start posted is asked, even paused since" }
    val never = Rig()
    never.queue(1, 1)
    never.holdSilences = true
    never.machine.next()
    never.prepared()
    check(never.heldSilences.isEmpty() && never.current.startRequests.size == 1 && never.state == PLAYING) {
        "the outgoing player never started: B starts at once"
    }

    // A silence outstanding outlives Stop (R61): the end is published at once, and the next play waits for it.
    val stop = Rig()
    stop.queue(1, 1)
    stop.prepared()
    stop.holdSilences = true
    stop.machine.next()
    stop.machine.stop(stop.generation)
    check(stop.state == STOPPED && stop.service.ends.size == 1) { "Stop is published at once" }
    stop.holdSilences = false
    stop.queue(1, 1)
    stop.prepared()
    check(stop.state == STARTING && stop.current.startRequests.isEmpty()) { "a new play after Stop waits for A" }
    stop.ackSilence()
    check(stop.state == PLAYING && stop.current.startRequests.size == 1) { "A's silence starts it, once" }
}

/**
 * A move keeps its direction until its candidate plays (R46): a candidate that prepared but failed before its first
 * start was acknowledged is a candidate that failed, so the search goes on the way the move was going, with no gap.
 */
private fun aMoveKeepsItsWayUntilItsCandidatePlays() {
    // Previous from C: B prepares, then fails before its start is acknowledged: A plays, not C again.
    val back = Rig()
    back.queue(1, 1, 1)
    back.playOut()
    back.elapse(RecordingPlayer.GAP_MS)
    back.playOut()
    back.elapse(RecordingPlayer.GAP_MS)
    back.prepared()
    check(back.status.call == 2 && back.state == PLAYING) { "C plays" }
    back.machine.previous()
    check(back.status.call == 1) { "Previous at its start goes to B" }
    back.holdStarts = true
    back.prepared()
    back.error(back.current, RecordingPlayer.REASON_DAMAGED)
    back.turn()
    check(back.status.call == 0 && !back.status.gap && back.status.skipped == 1) {
        "B failed before it played: the backward search goes on to A (call ${back.status.call}, gap ${back.status.gap})"
    }
    back.holdStarts = false
    back.prepared()
    check(back.state == PLAYING && back.status.call == 0) { "and A plays" }

    // Next from A: B prepares and fails the same way: C opens at once, with no gap after B.
    val ahead = Rig()
    ahead.queue(1, 1, 1)
    ahead.prepared()
    ahead.machine.next()
    ahead.holdStarts = true
    ahead.prepared()
    ahead.error(ahead.current, RecordingPlayer.REASON_DAMAGED)
    ahead.turn()
    check(ahead.status.call == 2 && !ahead.status.gap && ahead.state == PREPARING) {
        "B never played: C opens at once (call ${ahead.status.call}, gap ${ahead.status.gap})"
    }

    // A part that failed after it played is followed as before: the gap after its call, then the next call.
    val played = Rig()
    played.queue(1, 1)
    played.prepared()
    played.elapse(2_000)
    played.error(played.current, RecordingPlayer.REASON_DAMAGED)
    played.turn()
    check(played.status.gap && played.status.call == 0) { "played, then failed: the gap after its call" }
}

/**
 * A pending start has a watchdog (R44): a start not acknowledged within START_TIMEOUT_MS fails as the player's, ending
 * the play with the focus given back and its player let go; an acknowledgement in time plays, a late one changes
 * nothing, and a Pause before the expiry takes it back with its watchdog.
 */
private fun aPendingStartHasAWatchdog() {
    val limit = RecordingPlayer.START_TIMEOUT_MS
    val stuck = Rig()
    stuck.holdStarts = true
    val g = stuck.queue(1, 1)
    stuck.prepared()
    val asked = stuck.current
    val (_, serial) = stuck.heldStarts.single()
    stuck.elapse(limit - 100)
    check(stuck.state == RecordingPlayer.STARTING && stuck.focus.held) { "still waiting at 9.9 s" }
    stuck.elapse(100)
    check(stuck.state == FAILED && stuck.status.endedReason == RecordingPlayer.ENDED_FAILED) {
        "at 10 s the start has failed: the play ends (${stuck.status.endedReason})"
    }
    check(!stuck.focus.held && asked.released && stuck.service.ends == listOf(g to RecordingPlayer.ENDED_FAILED)) {
        "the focus is given back, the player let go, the service told"
    }
    val reading = stuck.status.record(stuck.nowMs)
    stuck.machine.onStarted(asked.token, serial)
    check(stuck.status.record(stuck.nowMs) == reading) { "a late acknowledgement changes nothing" }

    val inTime = Rig()
    inTime.holdStarts = true
    inTime.queue(1, 1)
    inTime.prepared()
    inTime.elapse(limit - 100)
    check(inTime.ackStart() && inTime.state == PLAYING) { "acknowledged at 9.9 s: it plays" }
    inTime.elapse(1_000)
    check(inTime.state == PLAYING) { "and the watchdog is gone" }

    val paused = Rig()
    paused.holdStarts = true
    paused.queue(1, 1)
    paused.prepared()
    paused.elapse(5_000)
    paused.machine.pause()
    paused.elapse(limit)
    check(paused.state == PAUSED && paused.status.endedReason.isEmpty() && paused.current.made.isEmpty()) {
        "a Pause before the expiry takes the start back, its watchdog with it (${paused.state})"
    }
}

/**
 * A seek while a start is pending takes that start back before the seek, then asks again behind it (R45): the old
 * request's acknowledgement is stale, and the new one plays from the seek's target.
 */
private fun aSeekTakesAPendingStartBack() {
    val rig = Rig()
    rig.queue(1, 1)
    rig.prepared()
    rig.elapse(2_000)
    rig.machine.pause()
    rig.holdStarts = true
    rig.machine.resume()
    val player = rig.current
    val (_, old) = rig.heldStarts.single()
    rig.machine.seekFor(rig.status.generation, rig.status.playToken, 20_000)
    check(player.wantedStart != old && player.startRequests.size == 3) {
        "the old start is taken back, a new one asked for behind the seek (${player.startRequests})"
    }
    check(player.seeks == listOf(20_000L)) { "the seek is posted" }
    // On the owner, in order: the old start taken back, then the seek, then the new start (m3).
    check(player.ops.takeLast(3) == listOf("pause", "seek 20000", "ask ${player.startRequests.last()}")) {
        "taken back, then the seek, then the new start: ${player.ops}"
    }
    rig.machine.onStarted(player.token, old)
    check(rig.state != PLAYING) { "the old start's acknowledgement is stale" }
    rig.heldStarts.removeAt(0)
    check(rig.ackStart() && rig.state == PLAYING) { "the new start's plays" }
    rig.elapse(1_000)
    check(rig.position == 21_000L) { "from the seek's target: ${rig.position}" }

    // The new start failing at once ends the play, and nothing is published after the end (m4).
    val fails = Rig()
    fails.queue(1, 1)
    fails.prepared()
    fails.machine.pause()
    fails.holdStarts = true
    fails.machine.resume()
    fails.holdStarts = false
    fails.failStarts = true
    val before = fails.service.changes.size
    fails.machine.seekFor(fails.status.generation, fails.status.playToken, 5_000)
    check(fails.state == FAILED && fails.service.ends.size == 1) { "the re-asked start failed: the play ended" }
    check(fails.service.changes.size > before && fails.service.heard.last() == "ended") {
        "the service hears no change after the end (${fails.service.heard.takeLast(3)})"
    }
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
    check(open.released) { "the part left lets its player go at once" }
    at.prepared()
    check(at.state == PLAYING) { "the previous call plays" }

    // At 3,001 ms: a restart, which opens the part again on a new player, under a new token.
    val past = Rig()
    past.queue(1, 1)
    past.playOut()
    past.elapse(1_000)
    past.prepared()
    past.nowMs += 3_001
    val playing = past.current
    val token = past.status.playToken
    past.machine.previous()
    check(past.players.size == 3 && playing.released && playing.seeks.isEmpty()) { "past 3,000 ms it reopens the part" }
    check(past.last.path == path(1, 1) && past.status.playToken != token && past.position == 0L) { "under a new token" }
    past.prepared()
    check(past.state == PLAYING && past.current === past.last) { "on the new player, playing" }

    // A later part: restart the call on its first part.
    val part = Rig()
    part.queue(1, 2)
    part.playOut()
    part.elapse(1_000)
    part.playOut()
    part.prepared()
    val second = part.current
    part.machine.previous()
    check(part.last.path == path(1, 1) && second.released) { "Previous on part 2 restarts the call at its first part" }

    // The first call restarts.
    val first = Rig()
    first.queue(1, 1)
    first.prepared()
    first.nowMs += 1_000
    first.machine.previous()
    check(first.players.size == 2 && first.players[0].released && first.last.path == path(0, 1)) {
        "Previous at the first call restarts it, opening it again"
    }
    first.prepared()
    check(first.state == PLAYING && first.players[0].seeks.isEmpty()) { "on a new player" }

    // In the gap: restart the call just heard, opened again (the gap holds no player).
    val gap = Rig()
    gap.queue(1, 1)
    gap.prepared()
    val heard = gap.current
    gap.complete()
    check(gap.status.gap && heard.released) { "the part that completed let its player go" }
    gap.machine.previous()
    check(!gap.status.gap && gap.players.size == 2 && gap.last.path == path(0, 1) && heard.seeks.isEmpty()) {
        "Previous in the gap opens the call just heard again"
    }
    gap.prepared()
    gap.elapse(5_000)
    check(gap.players.size == 2 && gap.status.call == 0) { "the gap is over" }

    // In a paused gap: the call opened again stays paused, its new player never started and no focus asked.
    val pausedGap = Rig()
    pausedGap.queue(1, 1)
    pausedGap.prepared()
    pausedGap.complete()
    pausedGap.machine.pause()
    check(pausedGap.status.gap && pausedGap.state == PAUSED) { "a paused gap" }
    val asked = pausedGap.focus.requests.size
    pausedGap.machine.previous()
    check(!pausedGap.status.gap && pausedGap.players.size == 2 && pausedGap.last.path == path(0, 1)) {
        "Previous in the paused gap opens the call again"
    }
    pausedGap.prepared()
    check(pausedGap.state == PAUSED && !pausedGap.last.started && pausedGap.focus.requests.size == asked) {
        "and stays paused: never started, no focus asked"
    }

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

    // Pressed 0.2 s before the deadline, the part preparing past it: the resume waits for its focus answer.
    for (grants in listOf(true, false)) {
        val late = Rig()
        val g = late.queue(1, 1)
        late.prepared()
        late.machine.pause()
        late.elapse(599_800)
        late.machine.next()
        late.machine.resume()
        late.elapse(300)
        check(late.state == PREPARING && late.status.endedReason.isEmpty()) { "grants=$grants: not ended while it prepares" }
        late.focus.grants = grants
        late.prepared()
        if (grants) {
            check(late.state == PLAYING && late.scheduler.live().isEmpty()) { "granted at 600.1 s: it plays" }
        } else {
            check(late.state == STOPPED && late.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) {
                "refused at 600.1 s: it ends at the refusal, the deadline from the pause"
            }
            check(late.service.ends == listOf(g to RecordingPlayer.ENDED_TIMEOUT)) { "once" }
        }
    }
    // A pause while the resume waits puts the deadline back, from the first pause.
    val again = Rig()
    again.queue(1, 1)
    again.prepared()
    again.machine.pause()
    again.elapse(500_000)
    again.machine.next()
    again.machine.resume()
    again.machine.pause()
    again.prepared()
    again.elapse(99_999)
    check(again.state == PAUSED && again.status.endedReason.isEmpty()) { "not yet ten minutes from the pause" }
    again.elapse(1)
    check(again.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) { "ten minutes from the first pause" }
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
    val aTimeout = ab.scheduler.live().single { it.delayMs == RecordingPlayer.PROMOTION_MS }
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
    check(all.status.skipped == 3 && all.focus.requests.isEmpty() && all.given.isEmpty()) { "nothing played" }
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
    check(rest.status.gap && rest.status.partOrdinal == 1 && rest.given.size == 1) {
        "back in the gap after the last part heard"
    }
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
    check(rig.last.path == path(0, 1) && c.released) { "Previous at C's start reaches A" }
    check(rig.opener.opened.count { it.startsWith("/r/c1") } == 2) { "B's failed parts are not tried again" }

    // The current call gone too: A fails now, and C opened again is gone: each skipped and counted, then the end.
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
    check(gone.opener.opened.last() == path(0, 1) && open.released) { "A is tried, C let go" }
    gone.turn()
    check(gone.opener.opened.last() == path(2, 1) && open.seeks.isEmpty()) { "C is opened again, never reused" }
    gone.turn()
    check(gone.state == FAILED && gone.status.endedReason == RecordingPlayer.ENDED_UNPLAYABLE) {
        "every candidate failed: B before, A and C gone since: unplayable"
    }
    check(gone.status.skipped == 3) { "A and C skipped and counted, as any gone recording" }
    check(gone.opener.opened == listOf(path(0, 1), path(1, 1), path(2, 1), path(0, 1), path(2, 1))) {
        "bounded: one try each"
    }

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
        check(a.released && rig.last !== a && rig.last.path == path(0, 1) && rig.status.call == 0 && rig.position == 0L) {
            "before the turn = $beforeTurn: A is opened again at 0, on a new player"
        }
        rig.turn()
        rig.prepared()
        check(rig.current === rig.last && rig.last.started && rig.state == PLAYING) { "and plays; the skip search is over" }
        check(rig.opener.opened.count { it == path(1, 1) } == 1) { "B1 is not tried again" }
    }
}

/**
 * A part done with (completed, or left by a move) has released its player: an error it reports after that never
 * reaches the gap, the part that plays next or the call opened again, and no skip is counted.
 */
private fun anErrorFromAPartDoneWithNeverReachesALaterPlay() {
    val rig = Rig()
    rig.queue(1, 1)
    rig.prepared()
    val finished = rig.current
    rig.complete()
    check(rig.status.gap && finished.released) { "the gap holds no player" }
    val gapTask = rig.scheduler.live().single()
    val token = rig.status.playToken
    rig.nowMs += 400
    rig.error(finished, RecordingPlayer.REASON_DAMAGED)
    rig.turn()
    check(rig.status.gap && rig.state == PLAYING && rig.status.skipped == 0) { "still the gap, no skip counted" }
    check(rig.scheduler.live().single() === gapTask) { "the gap is not restarted" }
    check(rig.status.playToken == token && rig.status.call == 0 && rig.status.durationMs == 60_000L) {
        "the gap keeps the completed part's readings"
    }
    val calls = finished.calls.size
    rig.machine.previous()
    check(rig.players.size == 2 && rig.last.path == path(0, 1) && finished.calls.size == calls) {
        "Previous in the gap opens the call again on a new player, never the old one"
    }
    rig.prepared()
    rig.error(finished, RecordingPlayer.REASON_PLAYER)
    check(rig.state == PLAYING && rig.status.call == 0 && rig.status.skipped == 0) { "the old error reaches nothing" }

    // Left by a move: Next while it plays, then its error.
    val left = Rig()
    left.queue(1, 1)
    left.prepared()
    val first = left.current
    left.machine.next()
    check(first.released) { "the part left lets its player go" }
    left.error(first, RecordingPlayer.REASON_DAMAGED)
    left.prepared()
    check(left.state == PLAYING && left.status.call == 1 && left.status.skipped == 0) { "the next call plays, unskipped" }
    left.machine.previous()
    left.turn()
    left.prepared()
    check(left.status.call == 0 && left.state == PLAYING && left.status.skipped == 0) { "the first call opens and plays" }

    // A call of two parts: part 1 completed, part 2 gone: the gap names part 1, with no player kept for it.
    val held = Rig()
    held.fail(path(0, 2), RecordingPlayer.REASON_GONE)
    held.queue(2, 1)
    held.prepared()
    val played = held.current
    held.complete()
    held.turn()
    check(held.status.gap && held.status.skipped == 1 && held.status.partOrdinal == 1 && played.released) {
        "the gap, named for the part that played, its player gone"
    }
    held.error(played, RecordingPlayer.REASON_PLAYER)
    held.turn()
    check(held.status.gap && held.state == PLAYING && held.status.skipped == 1) { "its error reaches nothing" }
    held.machine.previous()
    held.prepared()
    check(held.state == PLAYING && held.status.partOrdinal == 1 && held.last !== played) {
        "Previous opens part 1 again on a new player, which plays"
    }
}

/** A Previous on a recording deleted while it played opens it again, as any move does: a failed open, skipped. */
private fun previousOnADeletedRecordingSkipsIt() {
    val rig = Rig()
    rig.queue(1, 1, 1)
    rig.prepared()
    rig.machine.next()
    rig.prepared()
    val playing = rig.current
    rig.fail(path(1, 1), RecordingPlayer.REASON_GONE) // deleted under it
    rig.nowMs += 5_000
    check(rig.state == PLAYING && !playing.released) { "D9: the part playing plays on from its descriptor" }
    rig.machine.previous()
    check(playing.released && rig.status.skipped == 1 && rig.recheck.take().toList() == listOf(path(1, 1))) {
        "past 3 s Previous opens it again: gone, skipped, counted and rechecked"
    }
    rig.turn()
    check(rig.last.path == path(0, 1)) { "with nothing playable in its call, it keeps searching backward (R55)" }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.call == 0) { "the call before plays" }
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
    check(opening.status.refusals == 1 && opening.status.record(opening.nowMs).split('\t')[18] == "1") {
        "the refusal is counted in the record"
    }
    opening.machine.resume()
    check(opening.status.refusals == 2 && opening.status.pausedReason == RecordingPlayer.PAUSED_UNAVAILABLE) {
        "asked again and refused again, the reading is the same but for the count, which moved"
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

/**
 * The skip notice names the call that played after the skips and their count, while that call is current: A skipped,
 * B plays (the notice on B), C opens (none).
 */
private fun theSkipNoticeIsOnTheCallThatPlayedAfterIt() {
    val rig = Rig()
    rig.fail(path(0, 1), RecordingPlayer.REASON_GONE)
    rig.queue(1, 1, 1)
    check(rig.status.skipped == 1 && rig.status.skipNoticeCall == -1) { "A skipped: nothing has played after it yet" }
    rig.turn()
    check(rig.status.call == 1 && rig.status.skipNoticeCall == -1) { "B opening: still none" }
    rig.prepared()
    check(rig.status.skipNoticeCall == 1 && rig.status.skipNoticeCount == 1) { "B plays: the notice is B's" }
    val record = rig.status.record(rig.nowMs).split('\t')
    check(record[19] == "1" && record[20] == "1") { "in the record: $record" }
    rig.complete()
    check(rig.status.gap && rig.status.skipNoticeCall == 1) { "through B's gap" }
    rig.elapse(1_000)
    check(rig.status.call == 2 && rig.status.skipNoticeCall == -1 && rig.status.skipNoticeCount == 0) {
        "C opens: the notice is over, whoever looked when"
    }
    rig.prepared()
    check(rig.status.skipNoticeCall == -1 && rig.status.skipped == 1) { "C plays with none; the total stays" }

    // A part of a call skipped: the notice is on that call, once a later part of it plays, and adds up there.
    val part = Rig()
    part.fail(path(0, 2), RecordingPlayer.REASON_GONE)
    part.fail(path(0, 4), RecordingPlayer.REASON_CHANGED)
    part.queue(5, 1)
    part.prepared()
    part.complete()
    part.turn()
    check(part.last.path == path(0, 3) && part.status.skipNoticeCall == -1) { "part 3 opening after part 2's skip" }
    part.prepared()
    check(part.status.skipNoticeCall == 0 && part.status.skipNoticeCount == 1) { "part 3 plays: call 0, one" }
    part.complete()
    part.turn()
    part.prepared()
    check(part.status.skipNoticeCall == 0 && part.status.skipNoticeCount == 2) { "part 5 plays after part 4's: two" }

    // The last part skipped while it played: never said in the gap after its call, but on the next call.
    val last = Rig()
    last.queue(1, 1)
    last.prepared()
    last.machine.onFailed(last.current.token, RecordingPlayer.REASON_DAMAGED)
    last.turn()
    check(last.status.gap && last.status.skipNoticeCall == -1) { "the gap after the call it happened on: none" }
    last.elapse(1_000)
    last.prepared()
    check(last.status.skipNoticeCall == 1 && last.status.skipNoticeCount == 1) { "the next call plays: its notice" }
    last.queue(1)
    check(last.status.skipNoticeCall == -1 && last.status.refusals == 0 && last.status.skipped == 0) {
        "the next play starts with none"
    }
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
    expect(rig, "opening (wanted)", PREPARING, SESSION_BUFFERING, moves or ACTION_PAUSE)
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
    expect(rig, "opening the next call", PREPARING, SESSION_BUFFERING, ACTION_STOP or ACTION_PREVIOUS or ACTION_PAUSE)
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
    check(single.state == PAUSED && single.scheduler.entries.none { it.delayMs == RecordingPlayer.PAUSED_LIMIT_MS }) {
        "a single play has no paused deadline"
    }
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

/**
 * An invalid request is refused before anything is replaced (R63): the play running plays on untouched, its player,
 * focus and service as they were, and nothing is opened for the refused one. A path alone is part 1 of 1.
 */
private fun invalidItemsAreRefusedLeavingThePlayRunning() {
    fun item(call: Int) = RecordingPlayer.Item("/r/c$call.wav", 1, call = call)
    val rig = Rig()
    rig.queue(1, 1)
    rig.prepared()
    val running = rig.current
    val generation = rig.status.generation
    val opened = rig.opener.opened.size
    val refused = listOf(
        "no items" to (MODE_PLAYLIST to emptyList<RecordingPlayer.Item>()),
        "calls that do not start at 0" to (MODE_PLAYLIST to listOf(RecordingPlayer.Item("/r/a.wav", 1, call = 1))),
        "a part outside its count (0 of 0)" to
            (RecordingPlayer.MODE_SINGLE to listOf(RecordingPlayer.Item("/r/a.wav", partOrdinal = 0, partCount = 0))),
        "a queued item with no identity" to (MODE_PARTS to listOf(RecordingPlayer.Item("/r/a.wav"))),
        // R70: what the host refuses, Kotlin refuses too.
        "an empty path" to (MODE_PLAYLIST to listOf(RecordingPlayer.Item("", 1))),
        "an empty single path" to (RecordingPlayer.MODE_SINGLE to listOf(RecordingPlayer.Item(""))),
        "a single play of two items" to
            (RecordingPlayer.MODE_SINGLE to listOf(RecordingPlayer.Item("/r/a.wav"), RecordingPlayer.Item("/r/b.wav"))),
        "parts of two calls" to
            (MODE_PARTS to listOf(RecordingPlayer.Item("/r/a.wav", 1), RecordingPlayer.Item("/r/b.wav", 1, call = 1))),
        "calls with a hole" to (MODE_PLAYLIST to listOf(item(0), item(2))),
        "calls going back" to (MODE_PLAYLIST to listOf(item(0), item(1), item(0))),
    )
    for ((name, request) in refused) {
        check(!RecordingPlayer.validRequest(request.first, request.second, 0)) { "$name: not a request" }
        check(!rig.machine.play(generation + 1, request.first, request.second)) { "$name: refused" }
        check(rig.status.generation == generation && rig.state == PLAYING && !running.released) {
            "$name: the play running plays on untouched"
        }
    }
    check(!rig.machine.play(generation + 1, MODE_PARTS, listOf(RecordingPlayer.Item("/r/a.wav", 1)), start = 1)) {
        "a start outside the items"
    }
    check(rig.opener.opened.size == opened && rig.service.ends.isEmpty() && rig.focus.held) {
        "nothing opened, ended or given back for the refused ones"
    }
    check(RecordingPlayer.validRequest(RecordingPlayer.MODE_SINGLE, listOf(RecordingPlayer.Item("/r/a.wav")), 0)) {
        "a path alone is part 1 of 1: a request"
    }
    check(!rig.machine.play(generation + 1, "") && rig.status.generation == generation && !running.released) {
        "the path-only play of an empty path is refused, the play running untouched"
    }
}

/**
 * Queued plays carry each file's identity (#909): the store lists one for every recording, so a PARTS or PLAYLIST
 * item with a negative size is refused before the play is accepted, and only SINGLE opens a file unchecked, chosen by
 * its mode, never by the size's sign.
 */
private fun queuedItemsCarryTheirIdentity() {
    for (mode in listOf(MODE_PARTS, MODE_PLAYLIST)) {
        check(!RecordingPlayer.identified(mode, longArrayOf(100, -1))) { "mode $mode: an item with no identity is refused" }
        check(RecordingPlayer.identified(mode, longArrayOf(100, 0))) { "mode $mode: sizes from 0 are taken" }
    }
    check(RecordingPlayer.identified(RecordingPlayer.MODE_SINGLE, longArrayOf(-1))) { "a single play needs none" }

    // The machine refuses one too, as a second guard: nothing is opened unchecked.
    val bare = Rig()
    val unsized = listOf(RecordingPlayer.Item("/r/a.wav"), RecordingPlayer.Item("/r/b.wav", 10))
    check(!bare.machine.play(1, MODE_PARTS, unsized)) { "parts with an item without its size are refused" }
    check(bare.status.generation == 0 && bare.opener.opened.isEmpty()) { "none opened" }

    val single = Rig()
    single.play()
    single.machine.play(2, RecordingPlayer.MODE_SINGLE, listOf(RecordingPlayer.Item("/r/b.wav", 100, 1, 2)))
    check(single.opener.checks == listOf(false, false)) { "a single play opens unchecked, whatever its size says" }

    val queued = Rig()
    queued.queue(1, 1)
    queued.playOut()
    queued.elapse(RecordingPlayer.GAP_MS)
    queued.machine.play(9, MODE_PARTS, listOf(RecordingPlayer.Item("/r/z.wav", 0, 1, 0)))
    check(queued.opener.checks == listOf(true, true, true)) { "every queued open is checked: ${queued.opener.checks}" }
}

// ---- the focus requests the platform holds

private fun onlyAGrantedFocusRequestIsKept() {
    val given = mutableListOf<String>()
    val requests = FocusRequests<String> { given += it }
    check(!requests.request("first") { false } && given == listOf("first")) { "a refused one is given back at once" }
    check(!requests.request("second") { false } && given == listOf("first", "second")) { "each refusal, every time" }
    requests.abandon()
    check(given.size == 2) { "nothing refused is kept to give back later" }
    check(requests.request("third") { true } && given.size == 2) { "a granted one is kept" }
    requests.abandon()
    check(given == listOf("first", "second", "third")) { "and given back once it is let go" }
    requests.abandon()
    check(given.size == 3) { "once" }
    check(requests.request("fourth") { true } && requests.request("fifth") { true }) { "granted twice" }
    check(given == listOf("first", "second", "third", "fourth")) { "a request superseded is given back" }
}

/** A prepare that never answers fails its part after PREPARE_TIMEOUT_MS, as a player error would. */
private fun aPrepareThatNeverAnswersFailsItsPart() {
    val limit = RecordingPlayer.PREPARE_TIMEOUT_MS
    // PLAYLIST: the part is skipped at 10 s, its player released, and the next call opens.
    val list = Rig()
    list.queue(1, 1)
    val hung = list.last
    list.elapse(limit - 1)
    check(list.state == PREPARING && list.status.skipped == 0) { "not before the timeout" }
    list.elapse(1)
    check(hung.released && list.status.skipped == 1) { "at the timeout: released and skipped" }
    list.turn()
    check(list.last.path == path(1, 1) && list.last !== hung) { "the next call opens" }
    list.machine.onPrepared(hung.token)
    hung.prepared = true
    check(list.state == PREPARING && list.status.call == 1) { "the hung player's late prepare is ignored" }
    list.prepared()
    check(list.state == PLAYING && list.scheduler.live().none { it.delayMs == limit }) { "the next one plays, no timeout" }

    // SINGLE: Failed.
    val single = Rig()
    single.play()
    single.elapse(limit)
    check(single.state == FAILED && single.status.error == RecordingPlayer.ERROR_FAILED && single.last.released) {
        "a single play reads Failed"
    }

    // PARTS: the hung part is skipped (an error on a part still ends it), part 2 plays, and part 1's late callbacks
    // are ignored.
    val parts = Rig()
    parts.queue(2, mode = MODE_PARTS)
    val hungPart = parts.last
    parts.elapse(limit)
    check(hungPart.released && parts.status.skipped == 1 && parts.state == PREPARING) {
        "Play all parts skips the hung part (${parts.state}, ${parts.status.endedReason})"
    }
    parts.turn()
    check(parts.last.path == path(0, 2) && parts.status.partOrdinal == 2 && parts.last !== hungPart) { "part 2 opens" }
    parts.machine.onPrepared(hungPart.token)
    parts.machine.onFailed(hungPart.token, RecordingPlayer.REASON_DAMAGED)
    check(parts.state == PREPARING && parts.status.partOrdinal == 2 && !hungPart.started) {
        "part 1's late prepare and error are ignored"
    }
    parts.prepared()
    check(parts.state == PLAYING && parts.last.started && parts.status.partOrdinal == 2) { "part 2 plays" }
    check(parts.status.skipNoticeCall == 0 && parts.status.skipNoticeCount == 1) {
        "the skip notice names the part played after it, for the sheet to say"
    }
    parts.complete()
    check(parts.state == STOPPED && parts.status.endedReason == RecordingPlayer.ENDED_FINISHED) { "and finishes" }
    val allHung = Rig()
    allHung.queue(2, mode = MODE_PARTS)
    allHung.elapse(limit)
    allHung.turn()
    allHung.elapse(limit)
    allHung.turn()
    check(allHung.state == FAILED && allHung.status.endedReason == RecordingPlayer.ENDED_UNPLAYABLE) {
        "every part hung: unplayable"
    }

    // Prepared at 9.9 s: no timeout, and none later.
    val inTime = Rig()
    inTime.queue(1, 1)
    inTime.elapse(9_900)
    inTime.prepared()
    inTime.elapse(60_000)
    check(inTime.state == PLAYING && inTime.status.skipped == 0) { "prepared in time, it plays on" }

    // A move cancels it: Next during a hung prepare, the next part prepared, no failure later.
    val moved = Rig()
    moved.queue(1, 1, 1)
    moved.elapse(5_000)
    moved.machine.next()
    moved.elapse(9_000)
    moved.prepared()
    moved.elapse(limit)
    check(moved.state == PLAYING && moved.status.skipped == 0 && moved.status.call == 1) { "the old part's timeout is gone" }

    // A: paused 599.8 s, Next, Resume; the part never prepares. At 10 s it is skipped, the resume still waiting with
    // the deadline held off, and the next call that prepares plays.
    val waits = Rig()
    waits.queue(1, 1, 1)
    waits.prepared()
    waits.machine.pause()
    waits.elapse(599_800)
    waits.machine.next()
    waits.machine.resume()
    waits.elapse(limit - 1)
    check(waits.state == PREPARING && waits.status.endedReason.isEmpty()) { "A: the resume waits for the prepare" }
    waits.elapse(1)
    check(waits.status.skipped == 1 && waits.status.endedReason.isEmpty()) { "A: the hung part is skipped, not ended" }
    waits.turn()
    check(waits.status.call == 2 && waits.state == PREPARING) { "A: the next call opens, the resume still wanted" }
    waits.prepared()
    check(waits.state == PLAYING && waits.scheduler.live().none { it.delayMs == RecordingPlayer.PAUSED_LIMIT_MS }) {
        "A: it plays, and the stretch is over"
    }

    // B: paused 599.0 s, Next, Resume; call 2's file errors at once, and call 3 prepares after 600 s: it plays.
    val fast = Rig()
    fast.queue(1, 1, 1)
    fast.prepared()
    fast.machine.pause()
    fast.elapse(599_000)
    fast.machine.next()
    fast.machine.resume()
    fast.nowMs += 50
    fast.machine.onFailed(fast.current.token, RecordingPlayer.REASON_DAMAGED)
    fast.turn()
    check(fast.status.call == 2 && fast.state == PREPARING) { "B: call 2 skipped, call 3 opens" }
    fast.elapse(1_050)
    check(fast.status.endedReason.isEmpty()) { "B: 600.1 s from the pause, still waiting" }
    fast.prepared()
    check(fast.state == PLAYING) { "B: call 3 prepared after the deadline plays" }

    // Every remaining part fails: the play ends as the search ends (finished), not "timeout".
    val none = Rig()
    val ng = none.queue(1, 1, 1)
    none.prepared()
    none.machine.pause()
    none.elapse(599_800)
    none.machine.next()
    none.machine.resume()
    none.elapse(limit)
    none.turn()
    none.elapse(limit)
    none.turn()
    check(none.state == STOPPED && none.status.endedReason == RecordingPlayer.ENDED_FINISHED) {
        "the search ended: finished, though the deadline has passed (${none.status.endedReason})"
    }
    check(none.service.ends == listOf(ng to RecordingPlayer.ENDED_FINISHED)) { "once" }

    // A refusal after a skip puts the deadline back, and past it the play ends then.
    val refused = Rig()
    refused.queue(1, 1, 1)
    refused.prepared()
    refused.machine.pause()
    refused.elapse(599_800)
    refused.machine.next()
    refused.machine.resume()
    refused.elapse(limit)
    refused.turn()
    refused.focus.grants = false
    refused.prepared()
    check(refused.state == STOPPED && refused.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) {
        "refused after the skip: timeout from the pause"
    }

    // Before the deadline the same skip goes on to a call that plays.
    val early = Rig()
    early.queue(1, 1, 1)
    early.prepared()
    early.machine.pause()
    early.elapse(100_000)
    early.machine.next()
    early.machine.resume()
    early.elapse(limit)
    early.turn()
    check(early.status.call == 2 && early.state == PREPARING) { "skipped to the next call, still wanted" }
    early.prepared()
    check(early.state == PLAYING) { "which plays" }
}

/**
 * A part's setup (the file, the player made, the data source, the prepare's start) runs on its player's own thread:
 * the machine's thread never waits for it, the prepare timeout covers it, and a result that comes back for a part no
 * longer current is let go.
 */
private fun aPartsSetupRunsOffTheMachinesThread() {
    val limit = RecordingPlayer.PREPARE_TIMEOUT_MS

    // Blocked in a read: the timeout fails the part at 10 s, Stop works meanwhile, and the late result is released.
    val blocked = Rig()
    blocked.threads.holding = true
    val g = blocked.queue(1, 1)
    val stuck = blocked.last
    check(blocked.state == PREPARING && blocked.threads.setups.size == 1) { "the setup is handed over" }
    check(blocked.opener.opened.isEmpty() && !stuck.asked && blocked.status.playToken == stuck.token) {
        "nothing is opened on the machine's thread; the record names the opening part"
    }
    blocked.machine.pauseFor(g, stuck.token)
    check(blocked.status.pausedReason == RecordingPlayer.PAUSED_USER) { "the opening part takes its item commands" }
    blocked.machine.resumeFor(g, stuck.token)
    blocked.elapse(limit - 1)
    check(blocked.state == PREPARING && blocked.status.skipped == 0 && !stuck.released) { "still opening at 9.999 s" }
    blocked.threads.stuck += 2 // the next call's read stalls too
    blocked.elapse(1)
    check(blocked.status.skipped == 1 && !stuck.released) { "at 10 s the part fails; its player is still the setup's" }
    blocked.turn()
    check(blocked.threads.setups.size == 2 && blocked.status.call == 1) { "the next call's setup is handed over" }
    check(blocked.threads.threadOf == listOf(1, 2)) { "on a thread of its own" }
    val queued = blocked.last
    blocked.machine.stop(g)
    check(blocked.state == STOPPED && blocked.status.endedReason == RecordingPlayer.ENDED_STOPPED) {
        "Stop works while the setup is stuck"
    }
    check(blocked.service.ends == listOf(g to RecordingPlayer.ENDED_STOPPED)) { "the service is told once" }
    blocked.threads.setups.forEach { it.land() }
    check(stuck.released && queued.released && !stuck.started && !queued.started) { "the late results are released" }
    check(blocked.state == STOPPED && blocked.focus.requests.isEmpty()) { "and nothing is adopted" }

    // Every setup stuck, each on its own thread: each part times out in turn and the playlist ends by itself.
    val stalled = Rig()
    stalled.threads.stuck += setOf(1, 2)
    val s = stalled.queue(1, 1)
    stalled.elapse(limit)
    stalled.turn()
    stalled.elapse(limit)
    stalled.turn()
    check(stalled.threads.made == 2) { "each part on a thread of its own" }
    check(stalled.state == FAILED && stalled.status.endedReason == RecordingPlayer.ENDED_UNPLAYABLE) {
        "every part timed out: unplayable"
    }
    check(stalled.service.ends == listOf(s to RecordingPlayer.ENDED_UNPLAYABLE) && stalled.scheduler.live().isEmpty()) {
        "settled, nothing left"
    }
    stalled.threads.setups.forEach { it.land() }
    check(stalled.players.all { it.released } && stalled.state == FAILED) { "late results let go" }

    // A result that comes back after a move: released, and the part moved to plays.
    val moved = Rig()
    moved.threads.holding = true
    moved.queue(1, 1, 1)
    val first = moved.last
    moved.threads.stuck += 2 // the next call's read is slow too
    moved.machine.next()
    val second = moved.last
    check(moved.threads.setups.size == 2 && moved.status.playToken == second.token) { "Next opens the next call" }
    check(moved.threads.threadOf == listOf(1, 2)) { "its setup on a thread of its own, not behind the first's read" }
    moved.threads.setups[0].land()
    check(first.released && moved.status.call == 1 && moved.state == PREPARING) { "the first call's result is let go" }
    moved.threads.setups[1].land()
    moved.prepared(second)
    check(moved.state == PLAYING && second.started && !second.released) {
        "the next call's result is adopted and plays"
    }

    // The prepare answers before the setup's result is back: prepared as soon as it is adopted.
    val early = Rig()
    early.threads.holding = true
    early.queue(1, 1)
    val quick = early.last
    early.threads.setups[0].run()
    early.prepared(quick)
    check(early.state == PREPARING && !quick.started) { "not touched before its result" }
    early.threads.setups[0].land()
    check(early.state == PLAYING && quick.started) { "adopted, prepared: it plays" }
    check(early.scheduler.live().none { it.delayMs == limit }) { "the timeout is cancelled" }

    // An error before the result: the part fails at once; its result releases the player.
    val broken = Rig()
    broken.threads.holding = true
    broken.queue(1, 1)
    val bad = broken.last
    broken.threads.setups[0].run()
    broken.error(bad, RecordingPlayer.REASON_DAMAGED)
    check(broken.status.skipped == 1 && !bad.released) { "skipped; the player still the setup's" }
    broken.threads.setups[0].land()
    check(bad.released && broken.status.skipped == 1) { "released when its result is back" }

    // The setup's own failures, and the normal path, for every mode: a sheet's single and parts plays too.
    val gone = Rig()
    gone.threads.holding = true
    gone.fail(path(0, 1), RecordingPlayer.REASON_GONE)
    gone.queue(1, mode = MODE_PARTS)
    gone.threads.setups.single().land()
    check(gone.state == FAILED && gone.status.error == RecordingPlayer.ERROR_MISSING && gone.last.released) {
        "a part gone fails Play all parts, its player released"
    }
    val single = Rig()
    single.threads.holding = true
    single.play()
    check(single.state == PREPARING && single.opener.opened.isEmpty()) { "a single play's open is the setup's too" }
    single.threads.setups.single().land()
    check(single.opener.sources.single().closed && single.last.asked) { "the file given, its source closed" }
    single.prepared()
    check(single.state == PLAYING && single.last.started) { "and it plays" }
    val parts = Rig()
    parts.threads.holding = true
    parts.queue(2, mode = MODE_PARTS)
    parts.threads.setups.single().land()
    parts.playOut()
    parts.threads.setups.last().land()
    parts.prepared()
    check(parts.state == PLAYING && parts.status.partOrdinal == 2) { "Play all parts goes through its parts" }
}

/**
 * A setup stuck in a read past the prepare timeout holds only its own player's thread: the next item's setup runs on
 * its own and plays, and so does a later sheet play, which never waits behind the stuck read.
 */
private fun aStuckSetupHoldsOnlyItsOwnThread() {
    val rig = Rig()
    rig.threads.stuck += 1
    rig.queue(1, 1)
    val stuck = rig.last
    rig.elapse(RecordingPlayer.PREPARE_TIMEOUT_MS)
    check(rig.status.skipped == 1 && rig.abandoned.outstanding == 1) { "item 1 timed out; its read is still out" }
    rig.turn()
    check(rig.threads.threadOf == listOf(1, 2) && rig.last.asked && rig.status.call == 1) {
        "item 2's setup runs on its own thread"
    }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.call == 1) { "and plays" }
    rig.machine.stop(rig.generation)
    rig.play()
    check(rig.threads.threadOf.last() == 3 && rig.last.asked) { "a later sheet play is set up at once" }
    rig.prepared()
    check(rig.state == PLAYING) { "and plays" }
    rig.threads.setups.single().land()
    check(stuck.released && rig.state == PLAYING) { "the stuck read's late result is released, nothing else changes" }

    // Left before its timeout: Stop, or Next, at 5 s with the read still stuck. A sheet play right after is set up on
    // its own thread and plays, never failing behind the stuck read.
    for (leave in listOf("stop", "next")) {
        val early = Rig()
        early.threads.stuck += 1
        val g = early.queue(1, 1)
        val hung = early.last
        early.elapse(5_000)
        if (leave == "stop") early.machine.stop(g) else early.machine.next()
        check(early.abandoned.outstanding == 1 && !hung.released) { "$leave at 5 s: the stuck setup is given up" }
        if (leave == "next") {
            check(early.threads.threadOf == listOf(1, 2) && early.last.asked) { "next: the next call set up at once" }
            early.prepared()
            check(early.state == PLAYING && early.status.call == 1 && early.status.skipped == 0) {
                "next: it plays, nothing skipped"
            }
        }
        early.play()
        check(early.last.asked && early.threads.threadOf.last() == early.threads.made) {
            "$leave: the sheet play set up at once on its own thread"
        }
        early.prepared()
        early.elapse(RecordingPlayer.PREPARE_TIMEOUT_MS)
        check(early.state == PLAYING && early.status.error.isEmpty()) { "$leave: it plays, and never fails as damaged" }
        early.threads.setups.single().land()
        check(hung.released) { "$leave: the stuck read's late result is released" }
    }

    // A setup that came back in time (the prepare is what hangs) is not counted as stuck.
    val slow = Rig()
    slow.queue(1, 1)
    slow.elapse(RecordingPlayer.PREPARE_TIMEOUT_MS)
    check(slow.status.skipped == 1 && slow.abandoned.outstanding == 0 && slow.players[0].released) {
        "a hung prepare is not a stuck setup: its player released, nothing counted"
    }
}

/**
 * Stuck setups are bounded for the process: with two given up and still running, an open makes no player and starts
 * no setup. It waits for one to return, under its own prepare timeout, so stalled storage costs one ten-second timeout
 * per recording, one at a time, never a burst of failures; once one returns, the next open works.
 */
private fun stuckSetupsAreBounded() {
    val limit = RecordingPlayer.PREPARE_TIMEOUT_MS
    val rig = Rig()
    rig.threads.stuck += setOf(1, 2)
    val g = rig.queue(1, 1)
    rig.elapse(limit)
    rig.turn()
    check(rig.threads.setups.size == 2 && rig.status.call == 1) { "call 1's setup stuck too" }
    rig.machine.stop(g)
    check(rig.threads.made == 2 && rig.abandoned.outstanding == 2) { "two stuck setups given up" }
    val built = rig.players.size
    val handed = rig.threads.threadOf.size
    fun nothingMade(what: String) = check(rig.players.size == built && rig.threads.threadOf.size == handed) {
        "$what: no player made, no setup started"
    }

    rig.play()
    check(rig.state == PREPARING && rig.status.error.isEmpty()) { "a single play waits (${rig.state})" }
    nothingMade("waiting")
    rig.elapse(limit)
    check(rig.state == FAILED && rig.status.error == RecordingPlayer.ERROR_FAILED) {
        "at its timeout it fails as unplayable (${rig.state}, ${rig.status.error})"
    }
    rig.queue(2, mode = MODE_PARTS)
    rig.elapse(limit - 1)
    check(rig.state == PREPARING && rig.status.skipped == 0) { "Play all parts waits" }
    rig.elapse(1)
    check(rig.status.skipped == 1 && rig.status.partOrdinal == 2 && rig.state == PREPARING) {
        "part 1 skipped at its timeout; part 2 waits in turn"
    }
    rig.elapse(limit)
    check(rig.state == FAILED && rig.status.endedReason == RecordingPlayer.ENDED_UNPLAYABLE) { "then unplayable" }
    val list = rig.queue(1, 1)
    rig.elapse(limit - 1)
    check(rig.state == PREPARING && rig.status.skipped == 0) { "a playlist waits" }
    rig.elapse(1)
    check(rig.status.skipped == 1 && rig.status.call == 1 && rig.state == PREPARING) {
        "one skip at the timeout, not a burst: the next call waits"
    }
    rig.elapse(limit)
    check(rig.state == FAILED && rig.status.endedReason == RecordingPlayer.ENDED_UNPLAYABLE) { "and ends unplayable" }
    check(rig.service.ends.last() == list to RecordingPlayer.ENDED_UNPLAYABLE) { "told once" }
    nothingMade("all of them")
    check(rig.focus.requests.isEmpty()) { "and no focus asked" }

    rig.threads.setups[0].land()
    check(rig.players[0].released && rig.abandoned.outstanding == 1) { "a stuck setup returns: its player released" }
    rig.play()
    check(rig.players.size == built + 1 && rig.last.asked && rig.threads.threadOf.last() == 3) {
        "the next open makes its player and sets up on its own thread"
    }
    rig.prepared()
    check(rig.state == PLAYING) { "and plays" }
}

/**
 * An open at the limit waits for a stuck setup to return: one returning lets it go on under its own token, and it plays
 * with nothing skipped; Stop while it waits ends the play with no player made.
 */
private fun anOpenAtTheLimitWaitsForASetup() {
    val rig = Rig()
    rig.threads.stuck += setOf(1, 2)
    rig.queue(1, 1, 1, 1)
    rig.machine.next()
    rig.machine.next()
    check(rig.abandoned.outstanding == 2 && rig.status.call == 2 && rig.state == PREPARING) { "two running; call 2 waits" }
    check(rig.players.size == 2 && rig.threads.setups.size == 2) { "with no player and no setup of its own" }
    val token = rig.status.playToken
    rig.threads.setups[0].land()
    check(rig.players.size == 3 && rig.last.asked && rig.status.playToken == token) {
        "a setup returns: the waiting open goes on under its own token"
    }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.call == 2 && rig.status.skipped == 0) { "it plays, nothing skipped" }
    rig.threads.setups[1].land()
    check(rig.state == PLAYING && rig.players[1].released) { "the other late result changes nothing" }

    val stop = Rig()
    stop.threads.stuck += setOf(1, 2)
    val g = stop.queue(1, 1, 1)
    stop.machine.next()
    stop.machine.next()
    stop.machine.stop(g)
    check(stop.state == STOPPED && stop.status.endedReason == RecordingPlayer.ENDED_STOPPED) { "Stop while it waits" }
    stop.threads.setups.toList().forEach { it.land() }
    check(stop.players.size == 2 && stop.players.all { it.released } && stop.state == STOPPED) {
        "the wait is dropped: no player made when the setups return"
    }
    stop.elapse(RecordingPlayer.PREPARE_TIMEOUT_MS)
    check(stop.status.skipped == 0 && stop.state == STOPPED) { "and nothing times out after" }
}

/**
 * A setup given up whose work throws an Error still posts a result: its player is released, the count drops, and the
 * open waiting for a stuck setup goes on at once, never waiting out its timeout.
 */
private fun aSetupThatThrowsAnErrorStillWakesTheWait() {
    val rig = Rig()
    rig.threads.stuck += setOf(1, 2)
    rig.opener.erroring += path(0, 1)
    rig.queue(1, 1, 1, 1)
    rig.machine.next()
    rig.machine.next()
    check(rig.status.call == 2 && rig.players.size == 2) { "two running; call 2 waits" }
    rig.threads.setups[0].land()
    check(rig.players[0].released && rig.abandoned.outstanding == 1) { "the Error is a failed setup: released" }
    check(rig.players.size == 3 && rig.last.asked && rig.status.call == 2) { "the waiting open goes on at once" }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.skipped == 0) { "and plays, nothing skipped" }

    val current = Rig()
    current.opener.erroring += path(0, 1)
    current.queue(1, 1)
    check(current.state == FAILED && current.status.error == RecordingPlayer.ERROR_FAILED && current.last.released) {
        "an Error in the current part's setup is the player's failure: the play ends, its player released"
    }
}

/**
 * The emulator's sequence on healthy storage, every setup still in flight: five Next at once, then five Previous at
 * once. Nothing fails and the play goes on, landing on the first call; opens that meet the limit wait, never fail, and
 * go on as the threads let go end.
 */
private fun rapidNextThenPreviousNeverFailsAnItem() {
    val rig = Rig()
    rig.threads.stuck += (1..20).toSet()
    rig.queue(1, 1, 1, 1, 1, 1, 1)
    repeat(5) { rig.machine.next() }
    check(rig.status.call == 5 && rig.status.skipped == 0) { "five Next: call 5, nothing skipped" }
    repeat(5) { rig.machine.previous() }
    check(rig.status.call == 0 && rig.status.skipped == 0 && rig.state == PREPARING) {
        "five Previous: back on call 0 (${rig.status.call}), nothing skipped (${rig.status.skipped})"
    }
    while (rig.threads.setups.any { it.result == null }) {
        rig.threads.setups.first { it.result == null }.also { it.run() }.land()
    }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.call == 0 && rig.status.skipped == 0) { "call 0 plays" }
    check(rig.service.ends.isEmpty() && rig.abandoned.outstanding == 0) { "the play never ended; nothing left counted" }

    // Dropped after its work returned, its result not delivered yet: its thread has not ended, so it counts until its
    // result lands and its release ends (R40).
    val done = Rig()
    done.threads.holding = true
    done.queue(1, 1)
    done.threads.setups[0].run()
    done.machine.next()
    check(done.abandoned.outstanding == 1) { "dropped, its thread not ended: it counts" }
    done.threads.setups[0].land()
    check(done.abandoned.outstanding == 0) { "its result lands and its release ends: none" }
}

/**
 * A paused playlist whose deadline passed while the device slept: the prepare timeout of a part opened paused fires
 * first (the Handler counts uptime), and the play ends "timeout" at once, with no skip counted and no further open.
 */
private fun aPrepareTimeoutPastTheDeadlineEndsTimeout() {
    val rig = Rig()
    rig.queue(1, 1, 1)
    rig.prepared()
    rig.machine.pause()
    rig.nowMs += 599_000
    rig.machine.next()
    val built = rig.players.size
    rig.nowMs += RecordingPlayer.PREPARE_TIMEOUT_MS
    rig.scheduler.live().first { it.delayMs == RecordingPlayer.PREPARE_TIMEOUT_MS }.force()
    check(rig.state == STOPPED && rig.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) {
        "it ends timeout (${rig.status.endedReason})"
    }
    check(rig.status.skipped == 0 && rig.players.size == built) { "no skip counted, no further open" }
}

/**
 * Quick moves on healthy storage never meet the bound (R40): with every setup landing and every release ending at
 * once, five Next then five Previous fail nothing, every open is made at once, and nothing is left counted.
 */
private fun quickMovesNeverMeetTheLimit() {
    val rig = Rig()
    rig.queue(1, 1, 1, 1, 1, 1, 1)
    repeat(5) { rig.machine.next() }
    repeat(5) { rig.machine.previous() }
    check(rig.status.skipped == 0 && rig.status.call == 0 && rig.state == PREPARING) {
        "ten moves: nothing skipped (${rig.status.skipped}), back on call ${rig.status.call}"
    }
    check(rig.players.size == 11 && rig.abandoned.outstanding == 0) { "each open made at once, nothing counted" }
    check(rig.players.dropLast(1).all { it.released }) { "every player left is released" }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.call == 0 && rig.service.ends.isEmpty()) { "and call 0 plays" }
}

/**
 * The D1 burst as on a device (R40): five Next, then five Previous, with every release held until after the burst, as
 * each thread's end reaches the looper only after the moves already posted. The opens past the limit wait with no
 * player and nothing fails; the play does not end; the release returning first lets the waiting open go on, and once
 * all have returned nothing is counted and call 0 plays.
 */
private fun aBurstWhoseReleasesEndLateFailsNothing() {
    val rig = Rig()
    rig.holdReleases = true
    rig.queue(1, 1, 1, 1, 1, 1, 1)
    repeat(5) { rig.machine.next() }
    repeat(5) { rig.machine.previous() }
    check(rig.status.call == 0 && rig.status.skipped == 0 && rig.state == PREPARING) {
        "back on call 0 (${rig.status.call}), nothing skipped (${rig.status.skipped})"
    }
    check(rig.abandoned.outstanding == 2 && rig.players.size == 2) {
        "two releases held: call 0 waits with no player (${rig.abandoned.outstanding}, ${rig.players.size})"
    }
    check(rig.service.ends.isEmpty()) { "the play goes on" }
    rig.elapse(RecordingPlayer.PREPARE_TIMEOUT_MS - 1)
    rig.finishRelease()
    check(rig.abandoned.outstanding == 1 && rig.players.size == 3 && rig.last.path == path(0, 1)) {
        "the first release returns: call 0 sets up at once"
    }
    while (rig.heldReleases.isNotEmpty()) {
        rig.finishRelease()
    }
    check(rig.abandoned.outstanding == 0) { "every release returned: nothing counted" }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.call == 0 && rig.status.skipped == 0 && rig.service.ends.isEmpty()) {
        "call 0 plays, nothing failed"
    }
}

/**
 * A part that completes counts its thread as let go before the next part's open checks the limit (R48): with part 1's
 * release still out, part 2 completing makes part 3 wait at the limit, and a Stop then leaves two threads counted,
 * never three.
 */
private fun aCompletedPartCountsBeforeTheNextOpens() {
    val rig = Rig()
    rig.holdReleases = true
    val g = rig.queue(3)
    rig.playOut()
    check(rig.status.partOrdinal == 2 && rig.abandoned.outstanding == 1) { "part 1's release is out" }
    rig.playOut()
    check(rig.abandoned.outstanding == 2 && rig.status.partOrdinal == 3 && rig.players.size == 2) {
        "part 2 counted before part 3 opens: it waits at the limit (${rig.abandoned.outstanding}, ${rig.players.size})"
    }
    rig.machine.stop(g)
    check(rig.abandoned.outstanding == 2) { "Stop leaves two counted, never three (${rig.abandoned.outstanding})" }
    while (rig.heldReleases.isNotEmpty()) {
        rig.finishRelease()
    }
    check(rig.abandoned.outstanding == 0) { "each counted once" }
}

/**
 * A player let go counts until its thread ends (R40), not only while its setup runs: a prepare that stalls after
 * its setup returned, dropped by its timeout, whose release then blocks in native code on the stalled read, keeps its
 * thread counted. Two such, and the next open waits, with no player made, until one release returns.
 */
private fun aStalledReleaseCountsUntilItsThreadEnds() {
    val limit = RecordingPlayer.PREPARE_TIMEOUT_MS
    val rig = Rig()
    rig.holdReleases = true
    rig.queue(1, 1, 1, 1)
    rig.elapse(limit)
    check(rig.status.skipped == 1 && rig.players[0].released && rig.abandoned.outstanding == 1) {
        "call 0's prepare timed out: released, its release held, still counted (${rig.abandoned.outstanding})"
    }
    rig.turn()
    check(rig.status.call == 1 && rig.players.size == 2) { "call 1 opens: one held release is under the limit" }
    rig.elapse(limit)
    check(rig.abandoned.outstanding == 2) { "two held releases count" }
    rig.turn()
    check(rig.status.call == 2 && rig.players.size == 2 && rig.state == PREPARING) {
        "at the limit call 2 waits, with no player made (${rig.players.size})"
    }
    rig.finishRelease()
    check(rig.abandoned.outstanding == 1 && rig.players.size == 3 && rig.last.asked) {
        "a release returns: the waiting open goes on at once"
    }
    rig.holdReleases = false
    rig.prepared()
    check(rig.state == PLAYING && rig.status.call == 2 && rig.status.skipped == 2) { "and plays" }
    rig.finishRelease()
    check(rig.abandoned.outstanding == 0) { "the last held release ends: nothing counted" }

    // One thread counts once: dropped during its setup, then released when its result lands.
    val once = Rig()
    once.threads.stuck += 1
    once.holdReleases = true
    once.queue(1, 1)
    once.machine.next()
    check(once.abandoned.outstanding == 1) { "dropped during its setup: counted" }
    once.threads.setups.single().land()
    check(once.abandoned.outstanding == 1 && once.players[0].released) { "released when it lands: still once" }
    once.finishRelease()
    check(once.abandoned.outstanding == 0) { "its thread ends: none" }
}

/** Previous while the part opens opens it again: a file deleted meanwhile is skipped; the old open plays nothing. */
private fun previousWhileOpeningOpensAgain() {
    val rig = Rig()
    rig.queue(1, 1)
    val first = rig.last
    check(first.asked && rig.state == PREPARING) { "the first call's file is opened, not prepared yet" }
    rig.fail(path(0, 1), RecordingPlayer.REASON_GONE)
    rig.machine.previous()
    check(first.released && rig.status.playToken != first.token) { "a fresh open under a new token" }
    check(rig.status.skipped == 1 && rig.recheck.take().toList() == listOf(path(0, 1))) { "gone: skipped, rechecked" }
    first.prepared = true
    rig.machine.onPrepared(first.token)
    check(!first.started && rig.state != PLAYING) { "the old open's prepare starts no audio" }
    rig.turn()
    check(rig.last.path == path(1, 1)) { "it goes on forward" }
    rig.prepared()
    check(rig.state == PLAYING && rig.status.call == 1 && rig.status.skipped == 1) { "the next call plays" }
}

/**
 * A setup that found its file gone, its result delivered after Stop or Next: the file still goes on the recheck list,
 * and the replacement's audio and skip count are untouched. A single play's file is never rechecked.
 */
private fun aSupersededSetupStillRechecksItsFile() {
    for (leave in listOf("stop", "next")) {
        val rig = Rig()
        rig.threads.stuck += 1
        rig.fail(path(0, 1), RecordingPlayer.REASON_GONE)
        val g = rig.queue(1, 1)
        rig.threads.setups[0].run()
        if (leave == "stop") rig.machine.stop(g) else rig.machine.next()
        val serial = rig.status.recheckSerial
        rig.threads.setups[0].land()
        check(rig.recheck.take().toList() == listOf(path(0, 1))) { "$leave: the gone file is rechecked" }
        check(rig.status.recheckSerial == serial + 1) { "$leave: the status names the new serial" }
        check(rig.status.skipped == 0) { "$leave: nothing counted skipped" }
        if (leave == "next") {
            rig.prepared()
            check(rig.state == PLAYING && rig.status.call == 1 && rig.last.started) { "next: the next call plays" }
        } else {
            check(rig.state == STOPPED && rig.status.endedReason == RecordingPlayer.ENDED_STOPPED) { "stop: stopped" }
        }
    }
    val single = Rig()
    single.threads.holding = true
    single.fail("/r/Explore/a.wav", RecordingPlayer.REASON_GONE)
    single.play()
    single.threads.setups[0].run()
    single.machine.stop()
    single.threads.setups[0].land()
    check(single.recheck.take().isEmpty()) { "a single play's file is not the store's to recheck" }
}

/** A part that completes hands off the next part first, and is released after: its release is not in the joint. */
private fun theNextPartIsHandedOffBeforeTheCompletedOneIsReleased() {
    val rig = Rig()
    rig.queue(2)
    rig.prepared()
    val first = rig.current
    rig.log.clear()
    rig.complete()
    check(rig.log == listOf("build ${rig.last.token}", "release ${first.token}")) {
        "the next part built and handed off, then the completed one released: ${rig.log}"
    }
    check(rig.last.asked && first.released && rig.status.partOrdinal == 2) { "part 2 opens; part 1 let go" }

    // Into the gap, or at the end, it is released all the same.
    val gap = Rig()
    gap.queue(1, 1)
    gap.prepared()
    val heard = gap.current
    gap.complete()
    check(gap.status.gap && heard.released) { "the gap: released" }
    val end = Rig()
    end.queue(1)
    end.prepared()
    val last = end.current
    end.complete()
    check(end.state == STOPPED && last.released) { "the end: released" }
}

/**
 * A completion of the pass before never reaches the pass Previous opened, in either order: after the Previous it carries
 * a token no open has any more; before it, it entered the gap, and Previous in the gap opens the call again.
 */
private fun aCompletionAroundPreviousNeverReachesTheNewPass() {
    // The completion after the Previous (at the part's end, in flight as the user presses), before and after the new
    // pass has prepared and started.
    for (late in listOf("opening", "playing")) {
        val rig = Rig()
        rig.queue(1, 1)
        rig.prepared()
        val old = rig.current
        rig.nowMs += 60_000
        rig.machine.previous()
        check(old.released && rig.last !== old && rig.status.playToken == rig.last.token) { "$late: a new pass" }
        if (late == "playing") {
            rig.prepared()
        }
        rig.machine.onCompleted(old.token)
        check(!rig.status.gap && rig.status.call == 0 && rig.position == 0L) { "$late: the old completion is dropped" }
        rig.prepared()
        check(rig.state == PLAYING && !rig.status.gap) { "$late: the new pass plays" }
        rig.nowMs += 60_000
        rig.complete()
        check(rig.status.gap) { "$late: the new pass's own completion reaches the gap" }
    }

    // The completion first: the gap, then Previous in it opens the call again; nothing of the old pass follows.
    val first = Rig()
    first.queue(1, 1)
    first.prepared()
    val old = first.current
    first.nowMs += 60_000
    first.complete()
    check(first.status.gap) { "completed: the gap" }
    first.machine.previous()
    check(!first.status.gap && first.last !== old && first.last.path == path(0, 1)) { "Previous reopens the call" }
    first.machine.onCompleted(old.token)
    first.prepared()
    check(first.state == PLAYING && !first.status.gap && first.status.call == 0) { "the new pass plays" }
}

/**
 * Only a part playing ends a paused stretch: a resume from the gap, or the focus back after an interruption, while the
 * next part opens keeps the stretch from its start, so a pause again before a part plays finds the deadline.
 */
private fun aResumeKeepsThePausedStretchUntilAPartPlays() {
    val gap = Rig()
    val g = gap.queue(1, 1)
    gap.prepared()
    gap.complete()
    gap.machine.pause()
    gap.elapse(599_950)
    check(gap.state == PAUSED && gap.status.gap) { "a paused gap" }
    gap.machine.resume()
    check(gap.state == PREPARING && gap.status.call == 1 && gap.focus.held) { "the next call opens, the focus granted" }
    gap.nowMs += 100
    gap.machine.pause()
    check(gap.state == STOPPED && gap.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) {
        "paused again 600.05 s from the first pause: it ends"
    }
    check(gap.service.ends == listOf(g to RecordingPlayer.ENDED_TIMEOUT)) { "the service is told once" }

    // The same resume, its part prepared after the deadline: it plays, and that ends the stretch.
    val plays = Rig()
    plays.queue(1, 1)
    plays.prepared()
    plays.complete()
    plays.machine.pause()
    plays.elapse(599_950)
    plays.machine.resume()
    plays.elapse(200)
    check(plays.state == PREPARING) { "held off while it opens" }
    plays.prepared()
    check(plays.state == PLAYING && plays.scheduler.live().none { it.delayMs == RecordingPlayer.PAUSED_LIMIT_MS }) {
        "it plays: the stretch is over"
    }
    plays.machine.pause()
    plays.elapse(599_999)
    check(plays.state == PAUSED) { "a new pause starts a new stretch" }

    // Interrupted, the next call opening: the focus back is no part playing.
    val back = Rig()
    back.queue(1, 1)
    back.prepared()
    back.machine.onFocusChange(back.focus.epoch, FocusChange.LOST_TRANSIENT)
    back.elapse(599_000)
    back.machine.next()
    back.machine.onFocusChange(back.focus.epoch, FocusChange.GAINED)
    check(back.state == PREPARING && !back.status.interrupted) { "the focus back while the part opens" }
    back.nowMs += 1_100
    back.machine.pause()
    check(back.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) { "paused 600.1 s from the interruption: it ends" }

    // Interrupted again before the part plays: the hold-off ends and the deadline counts from the first pause.
    val again = Rig()
    again.queue(1, 1)
    again.prepared()
    again.complete()
    again.machine.pause()
    again.elapse(599_000)
    again.machine.resume()
    again.machine.onFocusChange(again.focus.epoch, FocusChange.LOST_TRANSIENT)
    again.elapse(999)
    check(again.state == PREPARING && again.status.interrupted) { "not yet" }
    again.elapse(1)
    check(again.status.endedReason == RecordingPlayer.ENDED_TIMEOUT) { "ten minutes from the first pause: it ends" }
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
    aFailedRestartKeepsSearchingBackward()
    aPlaylistPlaysCallsAndPartsInOrderWithAGapBetweenCalls()
    theGapReadsFrozenAndPausesWithIt()
    nextOpensTheNextCallAtOnceAlsoFromTheGap()
    nextPastOnlyFailedCallsFinishes()
    previousFollowsTheThreeSecondRule()
    aPendingStartHasAWatchdog()
    aSeekTakesAPendingStartBack()
    aMoveKeepsItsWayUntilItsCandidatePlays()
    theNextPartStartsOnceTheOutgoingOneIsSilent()
    aReplacementStartsOnceTheOldPlayIsSilent()
    aSilenceOutlivesANormalEnd()
    everyEndLetsASoundingPlayerFallSilentFirst()
    anEndedOwnerThreadIsSilent()
    aCompletionWhilePausedIsHeld()
    aSeekLandsOnlyWhenThePlayerConfirmsIt()
    aPreviousBurstAtAPartBoundaryRestartsTheCallFirst()
    anInterruptedOpeningReadsPausedOnTheSession()
    theQueueIsLetGoAtTheEnd()
    aStartPlaysFromItsAcknowledgement()
    eachMoveMakesOnlyTheCandidatesItTries()
    aSkipOpensThePartWithItsOwnReadings()
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
    anErrorFromAPartDoneWithNeverReachesALaterPlay()
    previousOnADeletedRecordingSkipsIt()
    aRefusedResumeStaysPausedInEveryMode()
    theSkipNoticeIsOnTheCallThatPlayedAfterIt()
    aPauseHoldsWhileItSearches()
    theRecheckListOutlivesThePlayAndHandsEachPathOnce()
    theTableDrivesStateSpeedAndActions()
    anInterruptionCarriesOnByItselfAndTheAppsPauseCancelsIt()
    aLastingLossEndsAPlaylistAndPausesTheOthers()
    headphonesUnpluggedPauseWhateverWouldSound()
    theTenMinuteDeadlineCountsFromThePause()
    theTerminalRecordStaysUntilTheNextPlay()
    invalidItemsAreRefusedLeavingThePlayRunning()
    queuedItemsCarryTheirIdentity()
    onlyAGrantedFocusRequestIsKept()
    aPrepareThatNeverAnswersFailsItsPart()
    aPartsSetupRunsOffTheMachinesThread()
    aCompletionAroundPreviousNeverReachesTheNewPass()
    aStuckSetupHoldsOnlyItsOwnThread()
    theNextPartIsHandedOffBeforeTheCompletedOneIsReleased()
    aResumeKeepsThePausedStretchUntilAPartPlays()
    stuckSetupsAreBounded()
    quickMovesNeverMeetTheLimit()
    aStalledReleaseCountsUntilItsThreadEnds()
    aCompletedPartCountsBeforeTheNextOpens()
    aBurstWhoseReleasesEndLateFailsNothing()
    anOpenAtTheLimitWaitsForASetup()
    rapidNextThenPreviousNeverFailsAnItem()
    aSetupThatThrowsAnErrorStillWakesTheWait()
    aPrepareTimeoutPastTheDeadlineEndsTimeout()
    previousWhileOpeningOpensAgain()
    aSupersededSetupStillRechecksItsFile()
    println("PASS: the call recording player queues calls and parts, skips, follows the focus, the service and the table")
}
