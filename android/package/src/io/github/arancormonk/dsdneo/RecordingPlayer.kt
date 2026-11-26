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
 * The audio focus requests the app holds (#909), for RecordingPlayback's focus: a request is kept only when it is
 * granted. Android registers a request's listener before it answers, so a refused one is given back ([give]) at once,
 * and repeated refusals (Resume pressed during a phone call) leave no listener behind. [T] is the platform's request,
 * so the JVM tests drive it with their own.
 */
class FocusRequests<T : Any>(private val give: (T) -> Unit) {
    private var held: T? = null

    /** Send [made] through [ask]; true when it was granted, and then it is the request [abandon] gives back. */
    fun request(made: T, ask: (T) -> Boolean): Boolean {
        abandon() // one at a time: an earlier request is superseded
        if (!ask(made)) {
            give(made)
            return false
        }
        held = made
        return true
    }

    /** Give back the request granted, if one is held. */
    fun abandon() {
        val granted = held ?: return
        held = null
        give(granted)
    }
}

/**
 * The setups given up while their work still runs (#909), counted for the process: a setup dropped while it runs (its
 * part timed out, was moved off, stopped or replaced) keeps its thread and its MediaPlayer until its read returns, and
 * on storage that stopped answering it may never return. While [limit] of them are still running, a new open makes no
 * player and starts no setup: it waits, under its own prepare timeout, for one to return. One leaves the count when
 * its work returns on its setup thread, before its result is posted, so a setup that is done and only waits for the
 * looper to take its result counts for nothing; the looper hears it when that result is delivered, which releases its
 * player and lets a waiting open go on. RecordingPlayback owns one for the process. [abandoned] and [full] run on the
 * playback looper, [returned] on the setup thread.
 */
class AbandonedSetups(val limit: Int = LIMIT) {
    /** One setup's work, as the count follows it: given up or not, returned or not. */
    class Work internal constructor() {
        internal var abandoned = false
        internal var returned = false
    }

    private val lock = Any()
    private var running = 0

    /** The setups given up whose work has not returned. */
    val outstanding: Int get() = synchronized(lock) { running }

    val full: Boolean get() = synchronized(lock) { running >= limit }

    /** A setup's work, as it is handed to its setup thread. */
    fun begin(): Work = Work()

    /** The machine gave the setup up: counted while its work still runs. */
    fun abandoned(work: Work) = synchronized(lock) {
        if (!work.abandoned) {
            work.abandoned = true
            if (!work.returned) {
                running++
            }
        }
    }

    /** Its work returned (or its result came with no work run): it no longer counts. Once; any thread. */
    fun returned(work: Work) = synchronized(lock) {
        if (!work.returned) {
            work.returned = true
            if (work.abandoned) {
                running--
            }
        }
    }

    companion object {
        /** At most this many stuck setups (threads and players) are left behind. */
        const val LIMIT = 2
    }
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
 * granted. Every open gets a fresh play token and a fresh player: a player is never reused, and it is released as soon
 * as its part completes, fails or is left. The token is unique in the process and never reused, and the player's
 * callbacks and the host's item commands carry it; each scheduled task (gap, skip search,
 * paused deadline, promotion timeout, prepare timeout) has its own id; focus callbacks carry the epoch of the request
 * they answer. A callback, task or command whose identity is not current is dropped.
 *
 * Opening a part never blocks the machine's thread: the player is made there, then handed to the setup thread
 * ([Opener.setup]), which opens the file, gives it to the player and starts its prepare. The machine does not touch
 * that player until the setup's result comes back with the part's token, and releases it then if the part is no longer
 * current (it timed out, was moved off, stopped or ended); a setup that found its file gone or changed puts it on the
 * recheck list whether or not its part is still current. A part not prepared within [PREPARE_TIMEOUT_MS] of its open,
 * setup included, fails (a stalled read or a MediaPlayer that never answers would leave the play opening for good):
 * SINGLE ends, PARTS and PLAYLIST skip it. While [AbandonedSetups.LIMIT] setups given up still run, an open waits, with
 * no player made, until one returns; its prepare timeout bounds the wait.
 *
 * Moves: [next] opens the next call's first playable part (also from the gap). [previous] restarts the current call
 * when a playable part of the call comes before the current one, the position is past [RESTART_MS] or it is in the
 * gap; otherwise it opens the previous call's first playable part, and at the first call it restarts. A restart
 * opens the part again, as any move does (a new player and token, through the setup thread), so the pass before can
 * deliver nothing to it; a part gone since is a failed open, skipped and counted. A move that finds nothing forward
 * ends the play: "unplayable" when every recording of the play failed, else finished. A backward move whose every
 * candidate fails goes on forward. Each search visits each recording at most once; the next candidate after a failure
 * opens on the next looper turn. A recording that fails while it plays is followed as if it had completed: the rest
 * of its call at once, then the gap before the next call. The gap holds no player, only the readings of the part that
 * completed (its item, token, duration and position). Play in a paused gap opens the next call at once.
 *
 * A PLAYLIST paused or interrupted ends after [PAUSED_LIMIT_MS], counted from the start of the continuous stretch in
 * which nothing was wanted or the focus was away, checked by a task, at every event and when the device wakes
 * ([onAwake]) against the elapsed clock.
 * Neither navigating while paused, the app's Pause during an interruption nor a refused resume restarts or extends
 * it; only audio carrying on ends the stretch: a resume, from a part, from the gap or after an interruption, ends it
 * when a part starts playing, not when it is pressed nor when the focus is granted. One pressed in time holds the
 * deadline off, across parts that fail on the way, until a part plays, the focus is refused or lost for a while, or it
 * is paused again (the deadline then counts from the start), or the search ends. A focus request
 * refused for the play's own start ends it as unavailable; one refused for a resume leaves it paused (any mode). A
 * transient focus loss interrupts (the request stays outstanding, Play is a no-op, the app's Pause cancels the
 * carry-on); a lasting one ends a PLAYLIST and pauses SINGLE and PARTS. Headphones unplugged ([onNoisy]) pause every
 * mode. An ended play keeps its terminal reading (generation, reason, the item it ended on) until the next play, so
 * the host reads a Stop it did not ask for.
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
    private val abandoned: AbandonedSetups = AbandonedSetups(),
) {
    /**
     * One recording's player, made by the factory with its play token; it reports back through [onPrepared],
     * [onCompleted] and [onFailed] with that token.
     */
    interface Player {
        /**
         * Take [source]'s file and start preparing it, asynchronously; throws when the file cannot be set (a damaged
         * file). Called on the setup thread ([Opener.setup]), never on the machine's, which does not touch the player
         * meanwhile. The setup closes [source] once this returns: the player keeps its own copy of the descriptor.
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
     * What a part's setup came to: [failure] is empty when the player took the file and prepares, else REASON_*.
     * [path] is the recording it tried, for the recheck list.
     */
    class SetupResult(val failure: String = "", val path: String = "")

    /**
     * Opens an item's file and, for an item with a size (not SINGLE's -1), checks it is the regular file the store
     * listed (size and modification time). Gone: no file there. Changed: another file, a link, or any other open error.
     */
    interface Opener {
        /** Called on the setup thread only. */
        fun open(item: Item): OpenResult

        /**
         * Run [work] on the setup thread, one setup after another, then hand what it returned to [deliver] on the
         * machine's thread. [work] does the blocking part of an open (the file, the player's data source and the start
         * of its prepare) and touches nothing of the machine's; one stuck in a read holds the setups behind it, which
         * time out with it.
         */
        fun setup(work: () -> SetupResult, deliver: (SetupResult) -> Unit)

        /**
         * The setup running now was dropped with its result still out (its part timed out, was moved off, stopped,
         * failed or replaced), so its read may never return: later setups go to a fresh setup thread, and that one
         * takes no new work. It finishes, if ever, on its own; its result is delivered and released as any late one.
         * Healthy setups finish in milliseconds, so this costs at most a short-lived thread per quick move.
         */
        fun abandonSetup()
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
        /** The focus requests this play was refused: it moves on each, so a refusal read again is news. */
        val refusals: Int,
        /**
         * The skip notice: the call that played after recordings were skipped ([skipNoticeCount] of them since the
         * last notice), while that call is current; -1 and 0 with none.
         */
        val skipNoticeCall: Int,
        val skipNoticeCount: Int,
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
         * The status record DecoderHostAndroid parses: 21 fields, tab-separated, in this order:
         * generation, state, position ms, duration ms, error, item index, play token, call, calls, part ordinal,
         * part count, gap, interrupted, can seek, paused reason, ended reason, skipped count, recheck serial,
         * refusals, skip notice call, skip notice count.
         *
         * Numbers are decimal (the play token is a 64-bit count from 1; 0 = none). The flags are 0 or 1. With no item
         * (before the first play, after a host stop): item index -1, call -1, calls 0, part ordinal and count 0. Error
         * is empty or ERROR_*, set only while the state is FAILED. Paused reason is empty or PAUSED_*, set only while
         * nothing is wanted (or SINGLE/PARTS wait for the foreground). Ended reason is empty or ENDED_* (never
         * ENDED_REPLACED), set only once the play has ended; the state is then STOPPED or FAILED. Refusals count from 0
         * for each play. The skip notice's call is -1 (its count 0) with none.
         */
        fun record(nowMs: Long): String = listOf(
            generation, state, positionMs(nowMs), durationMs, error, itemIndex, playToken, call, calls, partOrdinal,
            partCount, flag(gap), flag(interrupted), flag(canSeek), pausedReason, endedReason, skipped, recheckSerial,
            refusals, skipNoticeCall, skipNoticeCount,
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

        /**
         * PLAYLIST: every recording of the play failed (one that played in part included); PARTS: every part was
         * skipped for its prepare's timeout. FAILED, error "failed".
         */
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

        /**
         * A part whose player has not prepared this long after its open has failed, as a player error on it would
         * have: a MediaPlayer that never answers would otherwise leave the play opening, its service up, for good.
         */
        const val PREPARE_TIMEOUT_MS = 10_000L

        /** Play tokens, unique in the process. */
        private val tokens = AtomicLong()

        /**
         * A part's setup, on the setup thread: open [item], hand it to [player] and close what was opened. It touches
         * nothing of the machine's; the result goes back to the machine's thread.
         */
        private fun setUp(opener: Opener, item: Item, player: Player): SetupResult {
            val opened = try {
                opener.open(item)
            } catch (_: Exception) {
                OpenResult.Refused(REASON_CHANGED)
            }
            val source = when (opened) {
                is OpenResult.Refused ->
                    return SetupResult(if (opened.reason == REASON_GONE) REASON_GONE else REASON_CHANGED, item.path)
                is OpenResult.Opened -> opened.source
            }
            return try {
                player.prepare(source)
                SetupResult(path = item.path)
            } catch (_: Exception) {
                SetupResult(REASON_DAMAGED, item.path)
            } finally {
                try {
                    source.close()
                } catch (_: Exception) {
                    // The player has its own descriptor; a failed close loses nothing.
                }
            }
        }
    }

    private enum class Slot { GAP, SEARCH, DEADLINE, PROMOTION, PREPARE }

    private class Pending(val id: Int, val handle: Cancellable)

    /**
     * The readings of the part that played last (completed, or left by a move), its player already released: the gap
     * after its call and a forward move that finds nothing name it.
     */
    private class Left(val index: Int, val token: Long, val durationMs: Long, val positionMs: Long)

    /**
     * A search for the next playable recording: [candidates] in order, each visited once. Exhausted, a forward move
     * finishes, a backward one goes on forward after [resumeAfter], and one with [gapAfter] (the rest of a call after
     * its part completed) enters the gap before the next call, on the part that completed.
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

    /**
     * A part's player while its setup runs on the setup thread: not touched here until the setup's result is back.
     * [checked]: its play checks identities (not SINGLE), so a file found gone or changed is rechecked.
     */
    private class Opening(
        val token: Long,
        val player: Player,
        val checked: Boolean,
        /** Its work, which [AbandonedSetups] counts once it is given up, until the work returns. */
        val work: AbandonedSetups.Work,
    ) {
        /** Its prepare answered before the setup's result came back: it is prepared as soon as it is adopted. */
        var prepared = false
    }

    private var generation = 0
    private var latestGeneration = 0
    private var mode = MODE_SINGLE
    private var items: List<Item> = emptyList()
    private var callStarts = IntArray(1)
    private var failed = BooleanArray(0)
    private var skipped = 0

    /** Skips no part has played after yet, and the notice of the ones a part played after (its call, their count). */
    private var skipsPending = 0
    private var skipNoticeCall = -1
    private var skipNoticeCount = 0
    private var refusals = 0
    private var index = -1
    private var token = 0L
    private var player: Player? = null

    /** The current part's setup, while it runs; dropped (and its player released when it comes back) if it is not. */
    private var opening: Opening? = null

    /**
     * The current part's token while its open waits for a setup thread ([AbandonedSetups] at its limit), with no player
     * and no setup yet; 0 with none. Stop, a move and the end drop the wait as they drop an opening.
     */
    private var waiting = 0L
    private var prepared = false
    private var playing = false
    private var positionMs = 0L
    private var playingSinceMs = -1L
    private var durationMs = 0L
    private var left: Left? = null
    private var move: Move? = null
    private var wanted = false
    private var pausedReason = ""
    private var promoted = true
    private var earlyPromoted = 0
    private var earlyRefused = 0
    private var focusHeld = false
    /** The next focus request is the play's own start (not a resume): refused, it ends the play. */
    private var startRequest = true

    /**
     * A PLAYLIST resume pressed before its paused deadline (from a part, the gap, an interruption's end or a media
     * key) holds the deadline off: the play does not end "timeout" while that resume waits, across parts that fail or
     * are skipped on the way, and the stretch goes on. It ends when a part starts playing (the stretch ends with it),
     * when the focus is refused or lost for a while or the play is paused again (the deadline is back from the
     * stretch's start, and ends the play at once if it has passed), or with the search (finished, unplayable). Each
     * recording is visited once and each open waits at most [PREPARE_TIMEOUT_MS], so the search bounds it.
     */
    private var resumeSuspended = false
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
            // Pressed before the deadline (command() checked it): the deadline waits for a part to play.
            awaitPlayback()
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

    /**
     * The next call's first playable part. With calls after this one but none of them playable the play ends, as the
     * automatic advance ends it at the end (finished, or "unplayable" when every recording failed); dropped at the last
     * call (or SINGLE). Paused stays paused.
     */
    fun next(generation: Int = 0) = command(generation) {
        if (queue && callEnd(currentCall()) + 1 < items.size) {
            beginMove(Move(after(callEnd(currentCall())), forward = true))
        }
    }

    /** The previous call, or a restart of this one (see the class's moves); dropped for SINGLE. */
    fun previous(generation: Int = 0) = command(generation) {
        if (queue && index >= 0) {
            moveBack()
        }
    }

    /**
     * [pause] for the item whose play token the host captured; dropped when that item is not current. The item is
     * current while its player is open, and in the gap after its call even with none (its call's last part failed
     * while it played): the status names it there, and Pause and Resume act on the play.
     */
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

    /** [seek] for the item whose play token the host captured: only on that item's own open player. */
    fun seekFor(generation: Int, token: Long, positionMs: Long) {
        if (isCurrent(generation, token) && player != null) {
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

    /**
     * The device woke for the user (the screen turned on): the paused deadline is checked now. The deadline's task
     * counts the device's awake time, so a PLAYLIST whose ten minutes ran out in deep sleep ends here, not on a later
     * task.
     */
    fun onAwake() {
        if (active) {
            expireIfDue()
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
        val setup = opening
        if (setup != null && setup.token == token) {
            setup.prepared = true // The setup's result is still on its way: prepared once it is adopted.
            return
        }
        val current = player ?: return
        if (token != this.token || prepared || expireIfDue()) {
            return
        }
        prepared = true
        cancel(Slot.PREPARE)
        durationMs = current.durationMs.coerceAtLeast(0)
        positionMs = 0
        left = null
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
        // Done with: never touched again (a later callback from it carries a token no open has again), and released
        // once the next part is handed off, so its release adds nothing to the joint between parts.
        val done = player
        leave(release = false)
        val call = currentCall()
        when {
            mode == MODE_SINGLE -> end(STOPPED, "", ENDED_FINISHED)
            (index + 1..callEnd(call)).any { !failed[it] } ->
                beginMove(Move(span(index + 1, callEnd(call)), forward = true, gapAfter = true))
            hasCallAfter(call) -> enterGap()
            else -> end(STOPPED, "", ENDED_FINISHED)
        }
        done?.release()
    }

    /**
     * The player failed: [REASON_PLAYER] ends the play; anything else is this recording's fault ([REASON_DAMAGED]).
     * An error while the part's setup runs drops the setup. A part that completed or was left has no player any more,
     * so nothing from it reaches a later play.
     */
    fun onFailed(token: Long, reason: String) {
        val open = player != null || opening?.token == token
        if (!open || token != this.token || expireIfDue()) {
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
                awaitPlayback() // The stretch ends when a part plays, not on the grant.
                advance()
            }
            FocusChange.LOST_TRANSIENT -> {
                interrupted = true
                halt()
                freezeGap()
                if (endHoldOff()) {
                    return
                }
                publish()
            }
            FocusChange.LOST -> if (mode == MODE_PLAYLIST) end(STOPPED, "", ENDED_FOCUS) else hold(PAUSED_FOCUS)
        }
    }

    // ---- moves

    private fun beginMove(next: Move) {
        cancel(Slot.GAP)
        cancel(Slot.SEARCH)
        cancel(Slot.PREPARE)
        gap = false
        gapSinceMs = -1
        halt()
        dropOpening()
        if (player != null && prepared) {
            leave()
        }
        player?.release()
        player = null
        prepared = false
        positionMs = 0
        durationMs = 0
        move = next
        tryNext()
    }

    /**
     * The current part's setup is dropped with its result still out: its thread is given up at once
     * ([Opener.abandonSetup]), so nothing ever queues behind a read that does not return, and it is counted until its
     * work returns.
     */
    private fun dropOpening() {
        waiting = 0L
        val dropped = opening ?: return
        opening = null
        abandoned.abandoned(dropped.work)
        opener.abandonSetup()
    }

    /** The current part is done with (completed, or left by a move): its player goes, its readings stay in [left]. */
    private fun leave(release: Boolean = true) {
        left = Left(index, token, durationMs, currentPositionMs())
        if (release) {
            player?.release()
        }
        player = null
        prepared = false
    }

    /** Open the move's next candidate not failed: always a new player, never the one left. */
    private fun tryNext() {
        val search = move ?: return
        while (search.next < search.candidates.size && failed[search.candidates[search.next]]) {
            search.next++
        }
        if (search.next >= search.candidates.size) {
            exhausted(search)
            return
        }
        open(search.candidates[search.next++])
    }

    private fun exhausted(search: Move) {
        move = null
        when {
            search.gapAfter && hasCallAfter(currentCall()) -> {
                // The gap names the part that completed (none when it failed while playing): no player is kept.
                showLeft()
                enterGap()
            }
            !search.forward -> beginMove(Move(after(search.resumeAfter), forward = true))
            failed.all { it } -> end(FAILED, ERROR_FAILED, ENDED_UNPLAYABLE)
            else -> {
                showLeft()
                end(STOPPED, "", ENDED_FINISHED)
            }
        }
    }

    /** The readings of the part that played last, for the gap or the end; it has no player. */
    private fun showLeft() {
        val last = left ?: return
        left = null
        index = last.index
        token = last.token
        durationMs = last.durationMs
        positionMs = last.positionMs
    }

    private fun moveBack() {
        val call = currentCall()
        // A playable part of this call before the current one (never the current one itself, which may have just
        // failed and wait for its skip): restart the call.
        val playableBefore = (callStarts[call] until index).any { !failed[it] }
        val restart = gap || playableBefore || currentPositionMs() > RESTART_MS
        val earlier = if (restart) emptyList() else (call - 1 downTo 0).flatMap { callStarts[it]..callEnd(it) }
        val candidates = (earlier + span(callStarts[call], callEnd(call)).asList()).toIntArray()
        // Always a new open, also of the part opening now: its file is checked again under a fresh token, and the
        // pass before can deliver nothing to it.
        beginMove(Move(candidates, forward = false, resumeAfter = callEnd(call)))
    }

    /**
     * Build the candidate's player here (its callbacks come to this thread), guarded: one that cannot be built ends
     * the play. Then hand it to the setup thread, with the prepare timeout already armed, so it covers the setup too.
     * With [AbandonedSetups.LIMIT] setups given up still running, no player is made and no setup started: the open
     * waits ([waiting]) until a setup returns ([onSetUp]), its prepare timeout armed as usual, so storage that stalls
     * costs one timeout per recording, never a burst of failures.
     */
    private fun open(candidate: Int) {
        index = candidate
        token = tokens.incrementAndGet()
        // Fenced by the slot's task id and this part's token: a prepare, an error, a move or the end cancels it,
        // and a later open's player is never failed by it. It covers a wait for a setup thread too.
        val opened = token
        schedule(Slot.PREPARE, PREPARE_TIMEOUT_MS) {
            // schedule() has checked the paused deadline first: a playlist past it ends "timeout", with no skip.
            val current = player != null || opening?.token == opened || waiting == opened
            if (token == opened && !prepared && current) {
                failItem(REASON_DAMAGED, timedOut = true)
            }
        }
        if (abandoned.full) {
            waiting = token
            publish()
            return
        }
        startSetup(candidate)
    }

    /** Make the current part's player here and hand it to the setup thread. */
    private fun startSetup(candidate: Int) {
        val made = try {
            players(token)
        } catch (_: Exception) {
            null
        }
        if (made == null) {
            failItem(REASON_PLAYER)
            return
        }
        val setup = Opening(token, made, mode != MODE_SINGLE, abandoned.begin())
        opening = setup
        publish()
        val item = items[candidate]
        // The work returning, on the setup thread, takes it off the count before its result is posted.
        opener.setup({
            try {
                setUp(opener, item, made)
            } finally {
                abandoned.returned(setup.work)
            }
        }) { result -> onSetUp(setup, result) }
    }

    /**
     * A setup's result, on this thread. A file it found gone or changed goes on the recheck list first, whether or not
     * its part is still current. A part no longer current (timed out, moved off, stopped, ended, replaced) only
     * releases its player; a current one adopts it, prepared already if its prepare answered first, or fails.
     */
    private fun onSetUp(setup: Opening, result: SetupResult) {
        abandoned.returned(setup.work) // a result that came with no work run (a refused hand-off)
        val serial = recheck.serial
        if (setup.checked && (result.failure == REASON_GONE || result.failure == REASON_CHANGED)) {
            recheck.add(result.path)
        }
        if (opening !== setup) {
            setup.player.release()
            if (waiting != 0L && waiting == token && !abandoned.full) {
                // A setup thread came free: the open waiting for one goes on, under its own token and timeout.
                waiting = 0L
                startSetup(index)
            } else if (recheck.serial != serial) {
                publish() // The host reads the recheck serial from the status.
            }
            return
        }
        opening = null
        if (result.failure.isNotEmpty()) {
            setup.player.release()
            if (!expireIfDue()) {
                failItem(result.failure)
            }
            return
        }
        player = setup.player
        if (expireIfDue()) {
            return
        }
        if (setup.prepared) {
            onPrepared(setup.token)
        } else {
            publish()
        }
    }

    /**
     * The current recording cannot be played: end the play, or (PLAYLIST, and PARTS for a part whose prepare timed
     * out, [timedOut]) mark it and search on, next turn.
     */
    private fun failItem(reason: String, timedOut: Boolean = false) {
        cancel(Slot.PREPARE)
        val wasOpen = prepared
        positionMs = currentPositionMs()
        player?.release()
        player = null
        dropOpening()
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
        if (mode == MODE_SINGLE || (mode == MODE_PARTS && !timedOut)) {
            end(FAILED, if (reason == REASON_GONE) ERROR_MISSING else ERROR_FAILED, ENDED_FAILED)
            return
        }
        if (!failed[index]) {
            failed[index] = true
            skipped++
            skipsPending++
        }
        if (move == null) {
            // Outside a move it failed while it played: go on as if it had completed.
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
            // Audio carries on: a resume's hold-off ends, and with it the paused stretch (publish).
            resumeSuspended = false
            noteSkips()
        }
        publish()
    }

    /**
     * A part plays: the skips before it are said on its call (added to a notice already on that call, a part of it
     * skipped), never on the gap after the call they happened on.
     */
    private fun noteSkips() {
        if (skipsPending == 0) {
            return
        }
        val call = currentCall()
        skipNoticeCount = if (call == skipNoticeCall) skipNoticeCount + skipsPending else skipsPending
        skipNoticeCall = call
        skipsPending = 0
    }

    /** The notice lasts while its call is current: another call (or none) ends it. */
    private fun followSkipNotice() {
        if (skipNoticeCall >= 0 && items.getOrNull(index)?.call != skipNoticeCall) {
            skipNoticeCall = -1
            skipNoticeCount = 0
        }
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
        refusals++
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
        if (endHoldOff()) {
            return
        }
        publish()
    }

    /**
     * Audio is wanted again (a resume, an interruption's end) while a PLAYLIST's paused stretch is open: the deadline
     * is held off, and the stretch goes on, until a part starts playing.
     */
    private fun awaitPlayback() {
        if (mode == MODE_PLAYLIST && pausedSinceMs >= 0) {
            resumeSuspended = true
            cancel(Slot.DEADLINE)
        }
    }

    /**
     * A resume came to nothing before a part played (refused, interrupted, or paused again): the stretch goes on from
     * its start, and a deadline that passed while the resume waited ends the play now (true).
     */
    private fun endHoldOff(): Boolean {
        if (!resumeSuspended) {
            return false
        }
        resumeSuspended = false
        if (expireIfDue()) {
            return true
        }
        scheduleDeadline((PAUSED_LIMIT_MS - (clock.elapsedMs() - pausedSinceMs)).coerceAtLeast(0))
        return false
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
        val due = pausedSinceMs >= 0 && clock.elapsedMs() - pausedSinceMs >= PAUSED_LIMIT_MS
        if (!active || resumeSuspended || !due) {
            return false
        }
        end(STOPPED, "", ENDED_TIMEOUT)
        return true
    }

    /**
     * Start the paused deadline when a PLAYLIST stops wanting audio or is interrupted; clear it when it carries on. A
     * resume still waiting for a part to play (opening it, asking for the focus, or refused it) has not carried on
     * yet, so the stretch, and its start, last until a part starts.
     */
    private fun syncDeadline() {
        val holding = active && mode == MODE_PLAYLIST && (!wanted || interrupted || resumeSuspended)
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
        dropOpening()
        left = null
        move = null
        prepared = false
        playing = false
        playingSinceMs = -1
        gap = false
        wanted = false
        pausedReason = ""
        pausedSinceMs = -1
        resumeSuspended = false
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
        dropOpening()
        left = null
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
        skipsPending = 0
        skipNoticeCall = -1
        skipNoticeCount = 0
        refusals = 0
        startRequest = true
        resumeSuspended = false
        ended = null
        dropFocus()
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
        token != 0L && generation == this.generation && token == this.token &&
            (player != null || opening != null || waiting != 0L || gap)

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
            refusals = refusals,
            skipNoticeCall = skipNoticeCall,
            skipNoticeCount = skipNoticeCount,
            sessionState = sessionState(),
            actions = actions(),
        )
    }

    private fun publish() {
        syncDeadline()
        followSkipNotice()
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
