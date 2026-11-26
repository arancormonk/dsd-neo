// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import java.util.concurrent.atomic.AtomicLong

/**
 * The recordings a play found gone or changed at their turn (#909), for the store to look at again. The playback side
 * owns one for the process, so a Stop or a replacement never drops a path. [add] runs on the playback looper, [take]
 * on any thread; a path waiting in the list is listed once, and each [take] hands over what was added since the last.
 */
class RecordingRecheck {
    private val paths = LinkedHashSet<String>()

    /** Moves each time a path is added, so a poll knows to [take]; it appears in the player's status record. */
    @Volatile
    var serial = 0
        private set

    fun add(path: String) {
        synchronized(this) {
            if (paths.add(path)) {
                serial++
            }
        }
    }

    /** The paths added since the last take, oldest first; the list is empty after it. */
    fun take(): Array<String> = synchronized(this) { paths.toTypedArray().also { paths.clear() } }
}

/**
 * The call recording player's state machine (#708 D6/D7/D10, #909): a queue of recordings beside the live audio.
 *
 * A play is one of three modes. SINGLE plays one recording by path, unchecked, and keeps #708's readings. PARTS plays a
 * sheet's parts of one call, each checked against the identity the store listed, and ends on the first part that
 * fails. PLAYLIST plays History's calls oldest first, each call's parts in order with no gap between them and a
 * [GAP_MS] gap after a call; a recording found gone, changed or damaged at its turn is marked failed for this play and
 * skipped, keeping the direction of the move that reached it. SINGLE and PARTS take the transient audio focus and
 * pause when the app leaves the foreground (D10); PLAYLIST takes the lasting focus, ignores the foreground (its service
 * keeps it playing) and starts audio only once the service has been promoted ([onPromoted], in either order with the
 * play, within [PROMOTION_MS]). The focus is asked for once and kept across parts, calls and gaps until a pause or the
 * end.
 *
 * It keeps what the user asked for (play or pause) apart from what the player does, and starts a player only when it
 * is prepared, play is wanted, the foreground (SINGLE, PARTS) and the service (PLAYLIST) allow it and the focus is
 * granted. Every prepare gets a fresh play token, unique in the process and never reused, which the player's callbacks
 * and the host's item commands carry; each scheduled task (gap, skip search, paused deadline, promotion timeout) has
 * its own id; focus callbacks carry the epoch of the request they answer. A callback, task or command whose identity is
 * not current is dropped.
 *
 * Moves: [next] opens the next call's first playable part (also from the gap). [previous] restarts the current call
 * when a playable part of the call comes before the current one, the position is past [RESTART_MS] or it is in the
 * gap; otherwise it opens the previous call's first playable part, and at the first call it restarts. Restarting the
 * part already open is a seek to 0 on that player, never a reopen; moving to another part keeps the open player until
 * the new one has prepared, and a backward move whose every candidate fails stays on it at 0. A move that finds nothing
 * forward ends the play: "unplayable" when every recording of the play failed, else finished. One backward with no
 * open player goes on forward. Each search visits each recording at most once; the next candidate after a failure
 * opens on the next looper turn. A recording that fails while it plays is followed as if it had completed: the rest
 * of its call at once, then the gap before the next call. Play in a paused gap opens the next call at once.
 *
 * A PLAYLIST paused or interrupted ends after [PAUSED_LIMIT_MS], counted from the start of the continuous stretch in
 * which nothing was wanted or the focus was away, checked by a task and at every event against the elapsed clock.
 * Neither navigating while paused, the app's Pause during an interruption nor a refused resume restarts or extends
 * it; only audio carrying on (or wanted again) ends the stretch. A focus request refused for the play's own start ends
 * it as unavailable; one refused for a resume leaves it paused (any mode). A transient focus loss interrupts (the request
 * stays outstanding, Play is a no-op, the app's Pause cancels the carry-on); a lasting one ends a PLAYLIST and pauses
 * SINGLE and PARTS. Headphones unplugged ([onNoisy]) pause every mode. An ended play keeps its terminal reading
 * (generation, reason, the item it ended on) until the next play, so the host reads a Stop it did not ask for.
 *
 * Plain Kotlin, so the JVM tests drive it with fakes; RecordingPlayback runs it on one looper thread with MediaPlayer,
 * the AudioManager and a Handler. Not thread-safe: every call comes from that one thread. [status] may be read on any
 * thread.
 */
class RecordingPlayer(
    private val players: (token: Long) -> Player,
    private val opener: Opener,
    private val focus: Focus,
    private val clock: Clock,
    private val scheduler: Scheduler,
    private val service: ServiceLink,
    private val recheck: RecordingRecheck,
    private var foreground: Boolean = true,
) {
    /**
     * One recording's player, made by the factory with its play token; it reports back through [onPrepared],
     * [onCompleted] and [onFailed] with that token.
     */
    interface Player {
        /**
         * Take [source]'s file and prepare it, asynchronously; throws when the file cannot be set (a damaged file).
         * The machine closes [source] once this returns: the player keeps its own copy of the descriptor.
         */
        fun prepare(source: Source)

        fun start()

        fun pause()

        fun seekTo(positionMs: Long)

        /** The length, once prepared. */
        val durationMs: Long

        fun release()
    }

    /** An opened recording, handed to [Player.prepare]. */
    interface Source {
        fun close()
    }

    /** What [Opener.open] found. */
    sealed class OpenResult {
        class Opened(val source: Source) : OpenResult()

        /** [reason] is [REASON_GONE] or [REASON_CHANGED]. */
        class Refused(val reason: String) : OpenResult()
    }

    /**
     * Opens an item's file and, for an item with a size (not SINGLE's -1), checks it is the regular file the store
     * listed (size and modification time). Gone: no file there. Changed: another file, a link, or any other open error.
     */
    interface Opener {
        fun open(item: Item): OpenResult
    }

    enum class FocusMode { GAIN, GAIN_TRANSIENT }

    /** The app's audio focus; the request's changes come back to [onFocusChange] with [epoch]. */
    interface Focus {
        /** Ask for it; false when it is denied. */
        fun request(mode: FocusMode, epoch: Int): Boolean

        fun abandon()
    }

    enum class FocusChange { GAINED, LOST_TRANSIENT, LOST }

    /** The elapsed clock (SystemClock.elapsedRealtime on Android): it runs through deep sleep. */
    fun interface Clock {
        fun elapsedMs(): Long
    }

    fun interface Cancellable {
        fun cancel()
    }

    /** Runs a task on the machine's thread after a delay. */
    fun interface Scheduler {
        fun post(delayMs: Long, task: () -> Unit): Cancellable
    }

    /** The playlist's service, for PLAYLIST plays only. */
    interface ServiceLink {
        /** The play's reading changed (state, item, readings): the session and the notification follow it. */
        fun changed(status: Status)

        /**
         * The play [generation] is over ([ENDED_FINISHED] ... or [ENDED_REPLACED]): the service may retire. Once per
         * PLAYLIST generation, after the machine let its audio and focus go.
         */
        fun ended(generation: Int, reason: String)
    }

    /**
     * One recording of a play. [size] is -1 for SINGLE (no identity check). [call] is the 0-based call it belongs to
     * (non-decreasing along the items, starting at 0, never skipping one); [partOrdinal] is its 1-based place among
     * all its call's parts and [partCount] their number, both as listed (failed or away parts included). [key],
     * [title] and [line] are carried for the service's notification.
     */
    class Item(
        val path: String,
        val size: Long = -1,
        val mtimeSec: Long = 0,
        val mtimeNanos: Int = 0,
        val call: Int = 0,
        val partOrdinal: Int = 1,
        val partCount: Int = 1,
        val key: String = "",
        val title: String = "",
        val line: String = "",
    )

    /**
     * What the player does, for DecoderHostAndroid's poll and the playlist's session: the position runs on from
     * [positionMs] while the player plays.
     */
    class Status internal constructor(
        val generation: Int,
        val state: Int,
        private val positionMs: Long,
        private val playingSinceMs: Long,
        val durationMs: Long,
        val error: String,
        val mode: Int,
        val itemIndex: Int,
        val item: Item?,
        val playToken: Long,
        val call: Int,
        val calls: Int,
        val partOrdinal: Int,
        val partCount: Int,
        val gap: Boolean,
        val interrupted: Boolean,
        val canSeek: Boolean,
        val pausedReason: String,
        val endedReason: String,
        val skipped: Int,
        val recheckSerial: Int,
        /** SESSION_NONE, SESSION_PLAYING, SESSION_PAUSED or SESSION_STOPPED (the playlist table). */
        val sessionState: Int,
        /** The ACTION_* bits the session offers. */
        val actions: Int,
    ) {
        /** 1 while the player plays, else 0 (opening, the gap, paused, interrupted). */
        val speed: Float get() = if (playingSinceMs < 0) 0f else 1f

        fun allows(action: Int): Boolean = actions and action != 0

        fun positionMs(nowMs: Long): Long {
            val position = if (playingSinceMs < 0) positionMs else positionMs + (nowMs - playingSinceMs).coerceAtLeast(0)
            return if (durationMs > 0) position.coerceAtMost(durationMs) else position
        }

        /**
         * The status record DecoderHostAndroid parses: 18 fields, tab-separated, in this order:
         * generation, state, position ms, duration ms, error, item index, play token, call, calls, part ordinal,
         * part count, gap, interrupted, can seek, paused reason, ended reason, skipped count, recheck serial.
         *
         * Numbers are decimal (the play token is a 64-bit count from 1; 0 = none). The flags are 0 or 1. With no item
         * (before the first play, after a host stop): item index -1, call -1, calls 0, part ordinal and count 0. Error
         * is empty or ERROR_*, set only while the state is FAILED. Paused reason is empty or PAUSED_*, set only while
         * nothing is wanted (or SINGLE/PARTS wait for the foreground). Ended reason is empty or ENDED_* (never
         * ENDED_REPLACED), set only once the play has ended; the state is then STOPPED or FAILED.
         */
        fun record(nowMs: Long): String = listOf(
            generation, state, positionMs(nowMs), durationMs, error, itemIndex, playToken, call, calls, partOrdinal,
            partCount, flag(gap), flag(interrupted), flag(canSeek), pausedReason, endedReason, skipped, recheckSerial,
        ).joinToString("\t")

        private fun flag(value: Boolean) = if (value) 1 else 0
    }

    companion object {
        /** States; must match DecoderHost::PlaybackState. */
        const val STOPPED = 0
        const val PREPARING = 1
        const val PLAYING = 2
        const val PAUSED = 3
        const val FAILED = 4

        /** Modes; must match DecoderHost::PlaybackMode. */
        const val MODE_SINGLE = 0
        const val MODE_PARTS = 1
        const val MODE_PLAYLIST = 2

        /** Why a play failed, for the UI to word (DecoderHost::playbackError). */
        const val ERROR_MISSING = "missing"
        const val ERROR_UNAVAILABLE = "unavailable"
        const val ERROR_FAILED = "failed"

        /** Why a recording could not be played ([OpenResult.Refused], [onFailed]). */
        const val REASON_GONE = "gone"
        const val REASON_CHANGED = "changed"
        const val REASON_DAMAGED = "damaged"

        /** The player itself failed (it cannot be built, or its media server died): not the file's fault. */
        const val REASON_PLAYER = "player"

        /** Why nothing is wanted: the user's Pause (app, bar, session, media key, headset). */
        const val PAUSED_USER = "user"

        /** Headphones unplugged. */
        const val PAUSED_NOISY = "noisy"

        /** SINGLE or PARTS: the app left the foreground (D10), or a play waits for it. */
        const val PAUSED_BACKGROUND = "background"

        /** SINGLE or PARTS: another app took the focus for good. */
        const val PAUSED_FOCUS = "focus"

        /** PLAYLIST: a resume was refused the focus (a phone call); it stays paused. */
        const val PAUSED_UNAVAILABLE = "unavailable"

        /** Played its last part to the end (every mode; SINGLE's reading is #708's). */
        const val ENDED_FINISHED = "finished"

        /** Stopped for its generation ([stop] with a generation: the bar, the notification, the session). */
        const val ENDED_STOPPED = "stopped"

        /** PLAYLIST: another app took the focus for good. */
        const val ENDED_FOCUS = "focus"

        /** PLAYLIST: paused or interrupted for [PAUSED_LIMIT_MS]. */
        const val ENDED_TIMEOUT = "timeout"

        /** PLAYLIST: every recording of the play failed (one that played in part included). FAILED, error "failed". */
        const val ENDED_UNPLAYABLE = "unplayable"

        /** PLAYLIST: the service was refused or not promoted in time. FAILED, error "failed". */
        const val ENDED_START = "start"

        /** It failed: the error says why (a SINGLE or PARTS file, the focus at the start, the player). FAILED. */
        const val ENDED_FAILED = "failed"

        /** Another play replaced it: given to [ServiceLink.ended] only, never in a status. */
        const val ENDED_REPLACED = "replaced"

        const val SESSION_NONE = 0
        const val SESSION_PLAYING = 1
        const val SESSION_PAUSED = 2
        const val SESSION_STOPPED = 3

        const val ACTION_PLAY = 1
        const val ACTION_PAUSE = 2
        const val ACTION_NEXT = 4
        const val ACTION_PREVIOUS = 8
        const val ACTION_SEEK = 16
        const val ACTION_STOP = 32

        /** The added delay after a call's last part before the next call opens (D5). */
        const val GAP_MS = 1_000L

        /** Previous restarts the call past this position (strictly past), else goes to the previous call (D7). */
        const val RESTART_MS = 3_000L

        /** A PLAYLIST paused or interrupted this long ends (D11). */
        const val PAUSED_LIMIT_MS = 600_000L

        /** A PLAYLIST whose service is not promoted in this time ends "start". */
        const val PROMOTION_MS = 5_000L

        /** Play tokens, unique in the process. */
        private val tokens = AtomicLong()
    }

    private enum class Slot { GAP, SEARCH, DEADLINE, PROMOTION }

    private class Pending(val id: Int, val handle: Cancellable)

    /** The player that was open when a move began: kept until the move's player prepares. */
    private class Held(val player: Player, val index: Int, val token: Long, val durationMs: Long, val positionMs: Long)

    /**
     * A search for the next playable recording: [candidates] in order, each visited once. Exhausted, a forward move
     * finishes; a backward one stays on the held player at 0, or goes on forward after [resumeAfter]; one with
     * [gapAfter] (the rest of a call after its part completed) puts the held player in the gap before the next call.
     */
    private class Move(
        val candidates: IntArray,
        val forward: Boolean,
        val resumeAfter: Int = -1,
        val gapAfter: Boolean = false,
    ) {
        var next = 0
    }

    private class Ended(val state: Int, val error: String, val reason: String)

    private var generation = 0
    private var latestGeneration = 0
    private var mode = MODE_SINGLE
    private var items: List<Item> = emptyList()
    private var callStarts = IntArray(1)
    private var failed = BooleanArray(0)
    private var skipped = 0
    private var index = -1
    private var token = 0L
    private var player: Player? = null
    private var prepared = false
    private var playing = false
    private var positionMs = 0L
    private var playingSinceMs = -1L
    private var durationMs = 0L
    private var held: Held? = null
    private var move: Move? = null
    private var wanted = false
    private var pausedReason = ""
    private var promoted = true
    private var earlyPromoted = 0
    private var earlyRefused = 0
    private var focusHeld = false
    /** The next focus request is the play's own start (not a resume): refused, it ends the play. */
    private var startRequest = true
    private var focusEpoch = 0
    private var interrupted = false
    private var gap = false
    private var gapRemainingMs = 0L
    private var gapSinceMs = -1L
    private var pausedSinceMs = -1L
    private var ended: Ended? = null
    private var endNotified = true
    private var taskIds = 0
    private val pending = HashMap<Slot, Pending>()

    /** Published on the looper after every change; read on any thread. */
    @Volatile
    var status: Status = snapshot()
        private set

    private val active: Boolean get() = generation != 0 && ended == null
    private val queue: Boolean get() = mode != MODE_SINGLE

    /** Play [path] alone from its start as play [generation] (SINGLE), replacing what plays. */
    fun play(generation: Int, path: String) = play(generation, MODE_SINGLE, listOf(Item(path)), 0)

    /**
     * Play [items] from [start] as play [generation] in [mode], replacing what plays. Items that break [Item]'s rules
     * fail the play. SINGLE plays only the item at [start].
     */
    fun play(generation: Int, mode: Int, items: List<Item>, start: Int = 0) {
        val replaced = if (active && this.mode == MODE_PLAYLIST && !endNotified) this.generation else 0
        close()
        if (replaced != 0) {
            endNotified = true
            service.ended(replaced, ENDED_REPLACED)
        }
        this.generation = generation
        latestGeneration = maxOf(latestGeneration, generation)
        this.mode = mode
        this.items = items
        failed = BooleanArray(items.size)
        wanted = true
        endNotified = mode != MODE_PLAYLIST
        if (!valid(mode, items, start)) {
            this.items = emptyList()
            end(FAILED, ERROR_FAILED, ENDED_FAILED)
            return
        }
        callStarts = callStarts(items)
        if (mode == MODE_PLAYLIST && !admit(generation)) {
            return
        }
        beginMove(Move(span(start, if (mode == MODE_SINGLE) start else items.size - 1), forward = true))
    }

    /** Pause, keeping the position; the focus goes back. While interrupted, it cancels the carry-on. */
    fun pause(generation: Int = 0) = command(generation) {
        if (wanted) {
            hold(PAUSED_USER)
        }
    }

    /** Play again. While interrupted it does nothing: the play carries on by itself when the focus comes back. */
    fun resume(generation: Int = 0) = command(generation) {
        if (!wanted) {
            wanted = true
            pausedReason = ""
            startRequest = false
            if (gap) {
                resumeFromGap()
            } else {
                advance()
            }
        }
    }

    /** Seek the open part; dropped unless [Status.canSeek]. */
    fun seek(positionMs: Long) = command(0) {
        if (canSeek()) {
            applySeek(positionMs)
        }
    }

    /** The next call's first playable part; dropped when there is none (or SINGLE). Paused stays paused. */
    fun next(generation: Int = 0) = command(generation) {
        if (queue && hasCallAfter(currentCall())) {
            beginMove(Move(after(callEnd(currentCall())), forward = true))
        }
    }

    /** The previous call, or a restart of this one (see the class's moves); dropped for SINGLE. */
    fun previous(generation: Int = 0) = command(generation) {
        if (queue && index >= 0) {
            moveBack()
        }
    }

    /** [pause] for the item whose play token the host captured; dropped when that item is not current. */
    fun pauseFor(generation: Int, token: Long) {
        if (isCurrent(generation, token)) {
            pause(generation)
        }
    }

    fun resumeFor(generation: Int, token: Long) {
        if (isCurrent(generation, token)) {
            resume(generation)
        }
    }

    fun seekFor(generation: Int, token: Long, positionMs: Long) {
        if (isCurrent(generation, token)) {
            seek(positionMs)
        }
    }

    /**
     * Stop. With [generation] 0 (the host's own stop, the app ending), whatever plays stops and the next status is
     * Stopped with no generation. With a generation, only that play stops, and it keeps its terminal reading
     * ([ENDED_STOPPED]) until the next play.
     */
    fun stop(generation: Int = 0) {
        if (generation != 0) {
            if (generation == this.generation && active && !expireIfDue()) {
                end(STOPPED, "", ENDED_STOPPED)
            }
            return
        }
        val stopped = if (active && mode == MODE_PLAYLIST && !endNotified) this.generation else 0
        close()
        this.generation = 0
        items = emptyList()
        endNotified = true
        publish()
        if (stopped != 0) {
            service.ended(stopped, ENDED_STOPPED)
        }
    }

    /** The app came to the foreground or left it; leaving pauses SINGLE and PARTS (D10). PLAYLIST ignores it. */
    fun setForeground(foreground: Boolean) {
        this.foreground = foreground
        if (!active || expireIfDue() || mode == MODE_PLAYLIST) {
            return
        }
        if (foreground) {
            advance()
        } else if (wanted) {
            hold(PAUSED_BACKGROUND)
        }
    }

    /** Headphones unplugged: pause whatever would make sound (playing, opening, the gap, interrupted). */
    fun onNoisy() {
        if (active && !expireIfDue() && wanted) {
            hold(PAUSED_NOISY)
        }
    }

    /** The service of PLAYLIST [generation] is in the foreground: audio may start. Before or after the play. */
    fun onPromoted(generation: Int) {
        if (generation > latestGeneration) {
            earlyPromoted = maxOf(earlyPromoted, generation)
            return
        }
        if (generation != this.generation || !active || mode != MODE_PLAYLIST || promoted || expireIfDue()) {
            return
        }
        promoted = true
        cancel(Slot.PROMOTION)
        advance()
    }

    /** The service of PLAYLIST [generation] could not start: the play ends "start". Before or after the play. */
    fun onPromotionRefused(generation: Int) {
        if (generation > latestGeneration) {
            earlyRefused = maxOf(earlyRefused, generation)
            return
        }
        if (generation == this.generation && active && mode == MODE_PLAYLIST && !promoted) {
            end(FAILED, ERROR_FAILED, ENDED_START)
        }
    }

    fun onPrepared(token: Long) {
        val current = player ?: return
        if (token != this.token || prepared || expireIfDue()) {
            return
        }
        prepared = true
        durationMs = current.durationMs.coerceAtLeast(0)
        positionMs = 0
        releaseHeld()
        move = null
        cancel(Slot.SEARCH)
        advance()
    }

    /** It played to its end: the next part at once, the gap after a call, or the end, whatever its duration. */
    fun onCompleted(token: Long) {
        if (player == null || token != this.token || !prepared || expireIfDue()) {
            return
        }
        playing = false
        playingSinceMs = -1
        positionMs = durationMs
        val call = currentCall()
        when {
            mode == MODE_SINGLE -> end(STOPPED, "", ENDED_FINISHED)
            (index + 1..callEnd(call)).any { !failed[it] } ->
                beginMove(Move(span(index + 1, callEnd(call)), forward = true, gapAfter = true))
            hasCallAfter(call) -> enterGap()
            else -> end(STOPPED, "", ENDED_FINISHED)
        }
    }

    /** The player failed: [REASON_PLAYER] ends the play; anything else is this recording's fault ([REASON_DAMAGED]). */
    fun onFailed(token: Long, reason: String) {
        if (player == null || token != this.token || expireIfDue()) {
            return
        }
        failItem(if (reason == REASON_PLAYER) REASON_PLAYER else REASON_DAMAGED)
    }

    fun onFocusChange(epoch: Int, change: FocusChange) {
        if (!focusHeld || epoch != focusEpoch || !active || expireIfDue()) {
            return
        }
        when (change) {
            FocusChange.GAINED -> {
                interrupted = false
                advance()
            }
            FocusChange.LOST_TRANSIENT -> {
                interrupted = true
                halt()
                freezeGap()
                publish()
            }
            FocusChange.LOST -> if (mode == MODE_PLAYLIST) end(STOPPED, "", ENDED_FOCUS) else hold(PAUSED_FOCUS)
        }
    }

    // ---- moves

    private fun beginMove(next: Move) {
        cancel(Slot.GAP)
        cancel(Slot.SEARCH)
        gap = false
        gapSinceMs = -1
        halt()
        val current = player
        if (current != null && prepared) {
            releaseHeld()
            held = Held(current, index, token, durationMs, positionMs)
        } else {
            current?.release()
        }
        player = null
        prepared = false
        positionMs = 0
        durationMs = 0
        move = next
        tryNext()
    }

    /** Open the move's next candidate not failed; restarting the held player's own part is a seek on it. */
    private fun tryNext() {
        val search = move ?: return
        while (search.next < search.candidates.size && failed[search.candidates[search.next]]) {
            search.next++
        }
        if (search.next >= search.candidates.size) {
            exhausted(search)
            return
        }
        val candidate = search.candidates[search.next++]
        val open = held
        if (open != null && open.index == candidate) {
            adopt(open, positionMs = 0)
            open.player.seekTo(0)
            advance()
            return
        }
        open(candidate)
    }

    private fun exhausted(search: Move) {
        move = null
        val open = held
        when {
            search.gapAfter && hasCallAfter(currentCall()) -> {
                // A held player is the part that completed; with none (it failed while playing), the gap has none.
                open?.let { adopt(it, it.durationMs) }
                enterGap()
            }
            !search.forward && open != null -> {
                adopt(open, positionMs = 0)
                open.player.seekTo(0)
                advance()
            }
            !search.forward -> beginMove(Move(after(search.resumeAfter), forward = true))
            failed.all { it } -> end(FAILED, ERROR_FAILED, ENDED_UNPLAYABLE)
            else -> {
                if (open != null) {
                    index = open.index
                    token = open.token
                    durationMs = open.durationMs
                    positionMs = open.positionMs
                }
                end(STOPPED, "", ENDED_FINISHED)
            }
        }
    }

    /** Make the held player current again, prepared, at [positionMs]. */
    private fun adopt(open: Held, positionMs: Long) {
        held = null
        move = null
        player = open.player
        index = open.index
        token = open.token
        prepared = true
        durationMs = open.durationMs
        this.positionMs = positionMs
    }

    private fun moveBack() {
        val call = currentCall()
        // A playable part of this call before the current one (never the current one itself, which may have just
        // failed and wait for its skip): restart the call.
        val playableBefore = (callStarts[call] until index).any { !failed[it] }
        val restart = gap || playableBefore || currentPositionMs() > RESTART_MS
        val earlier = if (restart) emptyList() else (call - 1 downTo 0).flatMap { callStarts[it]..callEnd(it) }
        val candidates = (earlier + span(callStarts[call], callEnd(call)).asList()).toIntArray()
        if (candidates.firstOrNull { !failed[it] } == index && player != null && !prepared) {
            publish() // It is already opening that part, at 0.
            return
        }
        beginMove(Move(candidates, forward = false, resumeAfter = callEnd(call)))
    }

    private fun open(candidate: Int) {
        index = candidate
        token = tokens.incrementAndGet()
        val result = try {
            opener.open(items[candidate])
        } catch (_: Exception) {
            OpenResult.Refused(REASON_CHANGED)
        }
        when (result) {
            is OpenResult.Refused -> failItem(if (result.reason == REASON_GONE) REASON_GONE else REASON_CHANGED)
            is OpenResult.Opened -> prepareFrom(result.source)
        }
    }

    /** Build the player and hand it the file, all guarded: a player that cannot be built ends the play. */
    private fun prepareFrom(source: Source) {
        var made: Player? = null
        var damaged = false
        try {
            made = players(token)
            made.prepare(source)
        } catch (_: Exception) {
            damaged = made != null
        } finally {
            try {
                source.close()
            } catch (_: Exception) {
                // The player has its own descriptor; a failed close loses nothing.
            }
        }
        when {
            made == null -> failItem(REASON_PLAYER)
            damaged -> {
                made.release()
                failItem(REASON_DAMAGED)
            }
            else -> {
                player = made
                publish()
            }
        }
    }

    /** The current recording cannot be played: end the play, or (PLAYLIST) mark it and search on, next turn. */
    private fun failItem(reason: String) {
        val wasOpen = prepared
        positionMs = currentPositionMs()
        player?.release()
        player = null
        prepared = false
        playing = false
        playingSinceMs = -1
        gap = false
        gapSinceMs = -1
        cancel(Slot.GAP)
        if (reason == REASON_PLAYER) {
            end(FAILED, ERROR_FAILED, ENDED_FAILED)
            return
        }
        val item = items[index]
        if (mode != MODE_SINGLE && (reason == REASON_GONE || reason == REASON_CHANGED)) {
            recheck.add(item.path)
        }
        if (mode != MODE_PLAYLIST) {
            end(FAILED, if (reason == REASON_GONE) ERROR_MISSING else ERROR_FAILED, ENDED_FAILED)
            return
        }
        if (!failed[index]) {
            failed[index] = true
            skipped++
        }
        if (move == null) {
            // Outside a move it failed while it played (or held the gap): go on as if it had completed.
            move = if (wasOpen) {
                Move(span(index + 1, callEnd(currentCall())), forward = true, gapAfter = true)
            } else {
                Move(after(index), forward = true)
            }
        }
        publish()
        schedule(Slot.SEARCH, 0) { tryNext() }
    }

    // ---- playing

    /** Start what may start: prepared, wanted, allowed by the foreground or the service, with the focus. */
    private fun advance() {
        if (!active || !wanted || !promoted || (mode != MODE_PLAYLIST && !foreground)) {
            publish()
            return
        }
        if (gap) {
            if (ensureFocus()) {
                if (!interrupted && gapSinceMs < 0) {
                    runGap()
                }
                publish()
            }
            return
        }
        val current = player
        if (current == null || !prepared || playing) {
            publish()
            return
        }
        if (!ensureFocus()) {
            return
        }
        if (!interrupted) {
            current.start()
            playing = true
            playingSinceMs = clock.elapsedMs()
        }
        publish()
    }

    private fun ensureFocus(): Boolean {
        if (focusHeld) {
            return true
        }
        val epoch = ++focusEpoch
        if (focus.request(if (mode == MODE_PLAYLIST) FocusMode.GAIN else FocusMode.GAIN_TRANSIENT, epoch)) {
            focusHeld = true
            startRequest = false
            interrupted = false
            return true
        }
        if (startRequest) {
            end(FAILED, ERROR_UNAVAILABLE, ENDED_FAILED)
        } else {
            hold(PAUSED_UNAVAILABLE)
        }
        return false
    }

    /** Nothing is wanted now, for [reason]: halt, freeze the gap, give the focus back. */
    private fun hold(reason: String) {
        wanted = false
        pausedReason = reason
        halt()
        freezeGap()
        dropFocus()
        publish()
    }

    /** Pause the player if it plays, keeping its position. */
    private fun halt() {
        val current = player ?: return
        if (!playing) {
            return
        }
        current.pause()
        positionMs = currentPositionMs()
        playing = false
        playingSinceMs = -1
    }

    private fun currentPositionMs(): Long {
        val position = if (playing) positionMs + (clock.elapsedMs() - playingSinceMs).coerceAtLeast(0) else positionMs
        return if (durationMs > 0) position.coerceAtMost(durationMs) else position
    }

    private fun applySeek(target: Long) {
        val current = player ?: return
        val clamped = target.coerceIn(0, durationMs.coerceAtLeast(0))
        current.seekTo(clamped)
        positionMs = clamped
        if (playing) {
            playingSinceMs = clock.elapsedMs()
        }
        publish()
    }

    private fun enterGap() {
        gap = true
        gapRemainingMs = GAP_MS
        gapSinceMs = -1
        advance()
    }

    /** Play in a paused gap: the next call at once, never the rest of the gap, with the focus asked for now. */
    private fun resumeFromGap() {
        if (!promoted || (mode != MODE_PLAYLIST && !foreground)) {
            publish()
            return
        }
        if (ensureFocus()) {
            beginMove(Move(after(callEnd(currentCall())), forward = true))
        }
    }

    private fun runGap() {
        gapSinceMs = clock.elapsedMs()
        schedule(Slot.GAP, gapRemainingMs) {
            beginMove(Move(after(callEnd(currentCall())), forward = true))
        }
    }

    /** Stop the gap's clock, keeping what is left of it. */
    private fun freezeGap() {
        if (!gap || gapSinceMs < 0) {
            return
        }
        gapRemainingMs = (gapRemainingMs - (clock.elapsedMs() - gapSinceMs)).coerceIn(0, GAP_MS)
        gapSinceMs = -1
        cancel(Slot.GAP)
    }

    private fun dropFocus() {
        if (focusHeld) {
            focus.abandon()
            focusEpoch++
        }
        focusHeld = false
        interrupted = false
    }

    // ---- start, deadline, end

    /** PLAYLIST: settle the start with the service; false when the play already ended. */
    private fun admit(generation: Int): Boolean {
        when {
            earlyRefused == generation -> {
                end(FAILED, ERROR_FAILED, ENDED_START)
                return false
            }
            earlyPromoted == generation -> promoted = true
            else -> {
                promoted = false
                schedule(Slot.PROMOTION, PROMOTION_MS) {
                    if (!promoted) {
                        end(FAILED, ERROR_FAILED, ENDED_START)
                    }
                }
            }
        }
        return true
    }

    /** PLAYLIST held (nothing wanted, or interrupted) for [PAUSED_LIMIT_MS] ends; true when it did. */
    private fun expireIfDue(): Boolean {
        if (!active || pausedSinceMs < 0 || clock.elapsedMs() - pausedSinceMs < PAUSED_LIMIT_MS) {
            return false
        }
        end(STOPPED, "", ENDED_TIMEOUT)
        return true
    }

    /** Start the paused deadline when a PLAYLIST stops wanting audio or is interrupted; clear it when it carries on. */
    private fun syncDeadline() {
        val holding = active && mode == MODE_PLAYLIST && (!wanted || interrupted)
        if (holding && pausedSinceMs < 0) {
            pausedSinceMs = clock.elapsedMs()
            scheduleDeadline(PAUSED_LIMIT_MS)
        } else if (!holding && pausedSinceMs >= 0) {
            pausedSinceMs = -1
            cancel(Slot.DEADLINE)
        }
    }

    /** The deadline's task: it ends the play, or (woken early) waits again for what is left. */
    private fun scheduleDeadline(delayMs: Long) {
        schedule(Slot.DEADLINE, delayMs) {
            if (active && pausedSinceMs >= 0) {
                scheduleDeadline((PAUSED_LIMIT_MS - (clock.elapsedMs() - pausedSinceMs)).coerceAtLeast(0))
            }
        }
    }

    /** The play is over: let everything go and keep its terminal reading. */
    private fun end(state: Int, error: String, reason: String) {
        positionMs = currentPositionMs()
        cancelAll()
        player?.release()
        player = null
        releaseHeld()
        move = null
        prepared = false
        playing = false
        playingSinceMs = -1
        gap = false
        wanted = false
        pausedReason = ""
        pausedSinceMs = -1
        dropFocus()
        ended = Ended(state, error, reason)
        publish()
        if (mode == MODE_PLAYLIST && !endNotified) {
            endNotified = true
            service.ended(generation, reason)
        }
    }

    /** Release the players, the focus and the tasks, and forget the play's readings (not its generation). */
    private fun close() {
        cancelAll()
        player?.release()
        player = null
        releaseHeld()
        move = null
        prepared = false
        playing = false
        playingSinceMs = -1
        positionMs = 0
        durationMs = 0
        index = -1
        token = 0
        gap = false
        gapSinceMs = -1
        wanted = false
        pausedReason = ""
        pausedSinceMs = -1
        promoted = true
        skipped = 0
        startRequest = true
        ended = null
        dropFocus()
    }

    private fun releaseHeld() {
        held?.player?.release()
        held = null
    }

    // ---- tasks

    /** Post [action] as [slot]'s task, superseding the one there; it runs only while it is still the slot's task. */
    private fun schedule(slot: Slot, delayMs: Long, action: () -> Unit) {
        cancel(slot)
        val id = ++taskIds
        val handle = scheduler.post(delayMs) {
            if (pending[slot]?.id == id) {
                pending.remove(slot)
                if (!expireIfDue()) {
                    action()
                }
            }
        }
        pending[slot] = Pending(id, handle)
    }

    private fun cancel(slot: Slot) {
        pending.remove(slot)?.handle?.cancel()
    }

    private fun cancelAll() {
        Slot.values().forEach(::cancel)
    }

    // ---- reading

    private inline fun command(generation: Int, action: () -> Unit) {
        if (!active || (generation != 0 && generation != this.generation) || expireIfDue()) {
            return
        }
        action()
    }

    private fun isCurrent(generation: Int, token: Long) =
        token != 0L && generation == this.generation && token == this.token && player != null

    private fun currentCall(): Int = items[index].call

    private fun callEnd(call: Int): Int = callStarts[call + 1] - 1

    /** The items after [index], in order. */
    private fun after(index: Int): IntArray = span(index + 1, items.size - 1)

    private fun span(first: Int, last: Int): IntArray = IntArray((last - first + 1).coerceAtLeast(0)) { first + it }

    private fun hasCallAfter(call: Int): Boolean = (callEnd(call) + 1 until items.size).any { !failed[it] }

    private fun canSeek(): Boolean =
        active && promoted && player != null && prepared && !gap && !interrupted && durationMs > 0

    private fun actions(): Int {
        if (!active) {
            return 0
        }
        var bits = ACTION_STOP or if (wanted && !interrupted) ACTION_PAUSE else ACTION_PLAY
        if (queue && index >= 0) {
            bits = bits or ACTION_PREVIOUS
            if (hasCallAfter(currentCall())) {
                bits = bits or ACTION_NEXT
            }
        }
        if (canSeek()) {
            bits = bits or ACTION_SEEK
        }
        return bits
    }

    private fun state(): Int {
        val over = ended
        return when {
            over != null -> over.state
            generation == 0 -> STOPPED
            gap -> if (wanted && !interrupted) PLAYING else PAUSED
            !promoted || player == null || !prepared -> PREPARING
            playing -> PLAYING
            else -> PAUSED
        }
    }

    private fun sessionState(): Int = when {
        generation == 0 -> SESSION_NONE
        ended != null -> SESSION_STOPPED
        !promoted -> SESSION_NONE
        interrupted || !wanted -> SESSION_PAUSED
        else -> SESSION_PLAYING
    }

    private fun shownPausedReason(): String = when {
        !active -> ""
        !wanted -> pausedReason
        mode != MODE_PLAYLIST && !foreground -> PAUSED_BACKGROUND
        else -> ""
    }

    private fun snapshot(): Status {
        val item = items.getOrNull(index)
        return Status(
            generation = generation,
            state = state(),
            positionMs = positionMs,
            playingSinceMs = if (playing) playingSinceMs else -1,
            durationMs = durationMs,
            error = ended?.error ?: "",
            mode = mode,
            itemIndex = if (item == null) -1 else index,
            item = item,
            playToken = token,
            call = item?.call ?: -1,
            calls = items.lastOrNull()?.let { it.call + 1 } ?: 0,
            partOrdinal = item?.partOrdinal ?: 0,
            partCount = item?.partCount ?: 0,
            gap = gap && active,
            interrupted = interrupted,
            canSeek = canSeek(),
            pausedReason = shownPausedReason(),
            endedReason = ended?.reason ?: "",
            skipped = skipped,
            recheckSerial = recheck.serial,
            sessionState = sessionState(),
            actions = actions(),
        )
    }

    private fun publish() {
        syncDeadline()
        status = snapshot()
        if (mode == MODE_PLAYLIST && generation != 0) {
            service.changed(status)
        }
    }

    private fun valid(mode: Int, items: List<Item>, start: Int): Boolean {
        if (mode !in MODE_SINGLE..MODE_PLAYLIST || items.isEmpty() || start !in items.indices || items[0].call != 0) {
            return false
        }
        return items.indices.all { i ->
            val item = items[i]
            val step = if (i == 0) 0 else item.call - items[i - 1].call
            step in 0..1 && item.partOrdinal in 1..item.partCount
        }
    }

    /** Each call's first item, and the item count after the last call. */
    private fun callStarts(items: List<Item>): IntArray {
        val starts = IntArray(items.last().call + 2)
        for (i in items.indices.reversed()) {
            starts[items[i].call] = i
        }
        starts[starts.size - 1] = items.size
        return starts
    }
}
