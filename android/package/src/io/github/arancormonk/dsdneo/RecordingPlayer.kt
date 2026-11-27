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
 * The players' own threads the machine has let go and that have not ended yet (#909, R32/R35/R40), counted for the
 * process. A player is let go when its open is dropped with its setup still out (its part timed out, was moved off,
 * stopped or replaced) or when it is released; it leaves the count once its thread has ended, at the end of its
 * release ([Player.release]'s acknowledgement). Each thread counts once: one dropped during its setup and released
 * when its result lands is one. A setup stuck in a read, or a release stuck in native code on a stalled read, keeps its
 * thread and its MediaPlayer, and on storage that stopped answering may never end. While [limit] of them are still
 * running, a new open makes no player and starts no setup: it waits, under its own prepare timeout, for one to end,
 * and goes on when the machine hears that. RecordingPlayback owns one for the process. [letGo], [ended] and [full] run
 * on the playback looper.
 */
class AbandonedSetups(val limit: Int = LIMIT) {
    /** One player's thread, as the count follows it: let go or not, ended or not. */
    class Work internal constructor() {
        internal var letGo = false
        internal var ended = false
    }

    private val lock = Any()
    private var running = 0

    /** The threads let go that have not ended. */
    val outstanding: Int get() = synchronized(lock) { running }

    val full: Boolean get() = synchronized(lock) { running >= limit }

    /** A player's thread, as its player is made. */
    fun begin(): Work = Work()

    /** The machine let the player go (its open dropped, or released): counted until its thread ends. Once. */
    fun letGo(work: Work) = synchronized(lock) {
        if (!work.letGo) {
            work.letGo = true
            if (!work.ended) {
                running++
            }
        }
    }

    /** Its thread has ended: it no longer counts. Once. */
    fun ended(work: Work) = synchronized(lock) {
        if (!work.ended) {
            work.ended = true
            if (work.letGo) {
                running--
            }
        }
    }

    companion object {
        /** At most this many threads let go (each with its MediaPlayer) are left running. */
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
 * callbacks and the host's item commands carry it; each scheduled task (gap, skip search, paused deadline, promotion
 * timeout, prepare timeout, start watchdog) has its own id; focus callbacks carry the epoch of the request they
 * answer. A callback, task or command whose identity is not current is dropped.
 *
 * Opening a part never blocks the machine's thread: each part's player has a thread of its own, which does everything
 * for it ([Player.setup]): it opens the file, makes the platform player, gives it the file and starts its prepare. The
 * machine does not touch that player until the setup's result comes back with the part's token, and releases it then
 * if the part is no longer current (it timed out, was moved off, stopped or ended); a setup stuck in a read holds only
 * its own player's thread. A setup that found its file gone or changed puts it on the recheck list whether or not its
 * part is still current. A part not prepared within [PREPARE_TIMEOUT_MS] of its open,
 * setup included, fails (a stalled read or a MediaPlayer that never answers would leave the play opening for good):
 * SINGLE ends, PARTS and PLAYLIST skip it. While [AbandonedSetups.LIMIT] players let go still have their threads
 * running (a setup or a release stuck on a stalled read), an open waits, with no player made, until one ends; its
 * prepare timeout bounds the wait.
 *
 * Moves: [next] opens the next call's first playable part (also from the gap). [previous] restarts the current call
 * when a playable part of the call comes before the current one, the position is past [RESTART_MS] or it is in the
 * gap; otherwise it opens the previous call's first playable part, and at the first call it restarts. A restart
 * opens the part again, as any move does (a new player and token, set up on its own thread), so the pass before can
 * deliver nothing to it; a part gone since is a failed open, skipped and counted, and a restart that finds nothing
 * playable in its call goes on to the earlier calls, nearest first (R55). A move that finds nothing forward ends the
 * play: "unplayable" when every recording of the play failed, else finished. A backward move whose every candidate
 * fails goes on forward. Each search visits each recording at most once; the next candidate after a failure
 * opens on the next looper turn. A move keeps its direction and its candidates until its candidate plays (a start
 * acknowledged): one that prepared and fails before that is a candidate that failed, and the search goes on. A
 * recording that fails after it played is followed as if it had completed: the rest of its call at once, then the gap
 * before the next call. The gap holds no player, only the readings of the part that
 * completed or failed while it played (its item, token, duration and position). Every open starts at position 0 with
 * an unknown length, never the readings of the part tried before it. Play in a paused gap opens the next call at once.
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
     * One recording's player, made by the factory with its play token on the machine's thread; it reports back through
     * [onPrepared], [onCompleted] and [onFailed] with that token, on the machine's thread.
     *
     * Each player has one thread of its own, which does everything for it (on Android a HandlerThread: MediaPlayer is
     * not thread-safe, so the one thread makes it, calls it and receives its callbacks). [setup]'s work runs there and
     * calls [create] and [prepare]; [start], [pause], [seekTo] and [release] are called on the machine's thread and
     * passed to the player's own thread in the order they were called. [durationMs] is read on the machine's thread.
     * A start is made on the player's thread and acknowledged from there ([onStarted]); anything thrown there by a
     * start, a pause or a seek is reported as [onFailed] with [REASON_PLAYER].
     */
    interface Player {
        /**
         * Run [work] on this player's own thread, then hand what it returned to [deliver] on the machine's thread.
         * [work] does the blocking part of an open (the file, then [create] and [prepare]) and never throws: whatever
         * it meets is a result, so every setup delivers one. A hand-off the thread refuses (it has quit) delivers a
         * failed setup, [REASON_PLAYER]. The machine does not touch the player until the result is delivered.
         */
        fun setup(work: () -> SetupResult, deliver: (SetupResult) -> Unit)

        /** Make the platform player, on its own thread inside [setup]'s work; throws when it cannot be made. */
        fun create()

        /**
         * Take [source]'s file and start preparing it, asynchronously, on the player's own thread inside [setup]'s
         * work, after [create]; throws when the file cannot be set (a damaged file). The setup closes [source] once
         * this returns: the player keeps its own copy of the descriptor.
         */
        fun prepare(source: Source)

        /**
         * Start playing, as request [serial]: made on the player's own thread only while [serial] is still the start
         * wanted (a [pause] or [release] called since takes it back at once, on the machine's thread), and then
         * acknowledged with [onStarted] and the serial. Never called twice for one start.
         */
        fun start(serial: Long)

        /** Stop playing: a start not made yet is taken back, and one made is paused. */
        fun pause()

        /**
         * Seek to [positionMs] as seek request [serial]; once the platform player has landed it, [onSeekCompleted] is
         * posted with that request's own serial (R56, R59: its owner keeps one platform seek in flight and holds the
         * newest, so a completion for a superseded serial is not this one's landing).
         */
        fun seekTo(positionMs: Long, serial: Long)

        /**
         * Report silence (R52): once this player's own thread has run everything passed to it before (the move's
         * [pause]), [onSilenced] is posted with its token; at once if its thread has ended, which released it.
         */
        fun silence()

        /** The length, once prepared: read when [onPrepared] has come for this player. */
        val durationMs: Long

        /**
         * Let the player go; its own thread releases it after everything passed to it before, then ends, and [ended]
         * is called on the machine's thread once it has (at once, there, if the thread had ended already). A release
         * stuck in native code on a stalled read ends late or never.
         */
        fun release(ended: () -> Unit)
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
     * Opens an item's file and, when [checked] (a PARTS or PLAYLIST play, never SINGLE, whatever its size says),
     * checks it is the regular file the store listed (size and modification time); SINGLE opens it by path, unchecked.
     * Gone: no file there. Changed: another file, a link, or any other open error.
     */
    fun interface Opener {
        /** Called on the part's player's own thread only, inside its setup ([Player.setup]). */
        fun open(item: Item, checked: Boolean): OpenResult
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

        /**
         * PLAYLIST [generation] gives up the service start it waited for (its promotion timed out, R53): called on
         * the looper before the play's end is published, so a play the user asks for as soon as the end shows
         * issues a fresh start instead of joining this one.
         */
        fun startAbandoned(generation: Int) = Unit
    }

    /**
     * One recording of a play. [size], [mtimeSec] and [mtimeNanos] are the file's identity as the store listed it, a
     * size from 0, for PARTS and PLAYLIST, whose items all carry one ([identified]); SINGLE has none (size -1).
     * [call] is the 0-based call it belongs to (non-decreasing along the items, starting at 0, never skipping one);
     * [partOrdinal] is its 1-based place among all its call's parts and [partCount] their number, both as listed
     * (failed or away parts included). [key], [title] and [line] are carried for the service's notification.
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
        /**
         * The seeks this play has landed (R56): it moves only when the player confirms the latest seek, so a sheet can
         * tell a landing from a position read before the seek reached the player.
         */
        val seeksLanded: Int,
        /** SESSION_NONE, SESSION_PLAYING, SESSION_PAUSED, SESSION_STOPPED or SESSION_BUFFERING (the playlist table). */
        val sessionState: Int,
        /** The ACTION_* bits the session offers. */
        val actions: Int,
    ) {
        /** 1 while the player plays, else 0 (opening, starting, the gap, paused, interrupted). */
        val speed: Float get() = if (playingSinceMs < 0) 0f else 1f

        fun allows(action: Int): Boolean = actions and action != 0

        fun positionMs(nowMs: Long): Long {
            val position = if (playingSinceMs < 0) positionMs else positionMs + (nowMs - playingSinceMs).coerceAtLeast(0)
            return if (durationMs > 0) position.coerceAtMost(durationMs) else position
        }

        /**
         * The status record DecoderHostAndroid parses: 22 fields, tab-separated, in this order:
         * generation, state, position ms, duration ms, error, item index, play token, call, calls, part ordinal,
         * part count, gap, interrupted, can seek, paused reason, ended reason, skipped count, recheck serial,
         * refusals, skip notice call, skip notice count, seeks landed.
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
            refusals, skipNoticeCall, skipNoticeCount, seeksLanded,
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

        /**
         * A start asked of the part's player and not acknowledged yet (R49): wanted, so every control offers Pause, and
         * seek stays allowed, but nothing plays yet: the position holds, and the clock, the hold-off, the paused stretch
         * and the 3,000 ms rule start only on the acknowledgement. Its session state is [SESSION_BUFFERING].
         */
        const val STARTING = 5

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

        /**
         * Wanted with no audio yet, a part opening or its start pending (not the gap): Android's STATE_BUFFERING, the
         * frozen position at speed 0, Pause offered.
         */
        const val SESSION_BUFFERING = 4

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

        /** The largest nanoseconds a file identity's modification time has (dsd_file_identity_t). */
        private const val MAX_NANOS = 999_999_999

        /**
         * A part whose player has not prepared this long after its open has failed, as a player error on it would
         * have: a MediaPlayer that never answers would otherwise leave the play opening, its service up, for good.
         */
        const val PREPARE_TIMEOUT_MS = 10_000L

        /**
         * A start asked of a part's player and not acknowledged in this time has failed, as a start that throws: the
         * player's failure, which ends the play (R44). An owner stuck in MediaPlayer.start(), or queued behind a stalled
         * call, would otherwise hold the focus, the service and the wake lock with nothing playing.
         */
        const val START_TIMEOUT_MS = 10_000L

        /** Play tokens, unique in the process. */
        private val tokens = AtomicLong()

        /**
         * Whether a play's items carry what [mode] needs: every PARTS or PLAYLIST item its listed identity (the
         * store lists one for every recording, so a negative size is a malformed request); SINGLE needs none.
         * RecordingPlayback refuses a play without it before accepting it, so what plays plays on.
         */
        fun identified(mode: Int, sizes: LongArray): Boolean = mode == MODE_SINGLE || sizes.all { it >= 0 }

        /**
         * Whether [items] from [start] in [mode] is a request [play] takes (R63, R70: what the host admits): a mode,
         * items (exactly one for SINGLE), a start among them, a path for each, calls from 0 in order with no hole (one
         * call for PARTS), each part numbered within its call's count (1 of 1 for a path alone), and, but for SINGLE,
         * each file's identity. Checked before anything is replaced: by RecordingPlayback before it accepts the play,
         * the path-only play included, and by [play] again.
         */
        fun validRequest(mode: Int, items: List<Item>, start: Int): Boolean {
            val framed = mode in MODE_SINGLE..MODE_PLAYLIST && items.isNotEmpty() && start in items.indices &&
                (mode != MODE_SINGLE || items.size == 1)
            if (!framed || items[0].call != 0) {
                return false
            }
            return items.indices.all { i -> validItem(mode, items, i) }
        }

        /** A queued item's identity names a file (R75): a size from 0, and nanoseconds within a second. */
        private fun identifies(item: Item): Boolean = item.size >= 0 && item.mtimeNanos in 0..MAX_NANOS

        /**
         * Item [i] of a request as DecoderHost::playbackItemsValid() takes it (R70): a path, its call the one before's
         * or the next (0 for every part of PARTS), its part within its call's count, and, but for SINGLE, its identity.
         */
        private fun validItem(mode: Int, items: List<Item>, i: Int): Boolean {
            val item = items[i]
            val step = if (i == 0) 0 else item.call - items[i - 1].call
            return item.path.isNotEmpty() && step in 0..1 && (mode != MODE_PARTS || item.call == 0) &&
                item.partOrdinal in 1..item.partCount && (mode == MODE_SINGLE || identifies(item))
        }

        /**
         * A part's setup, on its player's own thread: open [item], make the platform player, hand it the file and close
         * what was opened. It touches nothing of the machine's; the result goes back to the machine's thread. A player
         * that cannot be made is the player's failure ([REASON_PLAYER]), a file it cannot take the file's.
         */
        private fun setUp(opener: Opener, item: Item, checked: Boolean, player: Player): SetupResult {
            val opened = try {
                opener.open(item, checked)
            } catch (_: Exception) {
                OpenResult.Refused(REASON_CHANGED)
            }
            val source = when (opened) {
                is OpenResult.Refused ->
                    return SetupResult(if (opened.reason == REASON_GONE) REASON_GONE else REASON_CHANGED, item.path)
                is OpenResult.Opened -> opened.source
            }
            return try {
                try {
                    player.create()
                } catch (_: Exception) {
                    return SetupResult(REASON_PLAYER, item.path)
                }
                try {
                    player.prepare(source)
                    SetupResult(path = item.path)
                } catch (_: Exception) {
                    SetupResult(REASON_DAMAGED, item.path)
                }
            } finally {
                try {
                    source.close()
                } catch (_: Exception) {
                    // The player has its own descriptor; a failed close loses nothing.
                }
            }
        }
    }

    private enum class Slot { GAP, SEARCH, DEADLINE, PROMOTION, PREPARE, START }

    private class Pending(val id: Int, val handle: Cancellable)

    /**
     * The readings of the part that played last (completed, failed while it played, or left by a move), its player
     * already released: the gap after its call and a forward move that finds nothing name it.
     */
    private class Left(val index: Int, val token: Long, val durationMs: Long, val positionMs: Long)

    /**
     * A search for the next playable recording: [candidates] in order, each visited once and made only as the search
     * reaches it (ranges walked by a cursor, never a copy of the queue's tail), so a move costs only the candidates it
     * tries. Exhausted, a forward move finishes, a backward one goes on forward after [resumeAfter], and one with
     * [gapAfter] (the rest of a call after its part completed) enters the gap before the next call, on the part that
     * completed.
     */
    private class Move(
        candidates: Sequence<Int>,
        val forward: Boolean,
        val resumeAfter: Int = -1,
        val gapAfter: Boolean = false,
    ) {
        private val cursor = candidates.iterator()

        /** The next candidate, or -1 once every one was visited. */
        fun next(): Int = if (cursor.hasNext()) cursor.next() else -1
    }

    private class Ended(val state: Int, val error: String, val reason: String)

    /**
     * A part's player while its setup runs on its own thread: not touched here until the setup's result is back.
     * [checked]: its play checks identities (not SINGLE), so a file found gone or changed is rechecked.
     */
    private class Opening(
        val token: Long,
        val player: Player,
        val checked: Boolean,
        /** Its player's thread, which [AbandonedSetups] counts once it is let go, until the thread ends. */
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

    /**
     * Seeks (R56): the latest one asked of the current player and not confirmed yet ([seekPending], its serial; 0 with
     * none) and its target. Until it lands the readings keep the confirmed position, and its clock, so Previous's
     * 3,000 ms rule and the record never read a target the player has not reached; a superseded seek's landing counts
     * for nothing. [seeksLanded] counts the landings of the play.
     */
    private var seekSerial = 0L
    private var seekPending = 0L
    private var seekTargetMs = 0L
    private var seeksLanded = 0
    private var index = -1
    private var token = 0L
    private var player: Player? = null

    /** The current [player]'s thread, as [AbandonedSetups] counts it once the player is let go. */
    private var playerWork: AbandonedSetups.Work? = null

    /** The current part's setup, while it runs; dropped (and its player released when it comes back) if it is not. */
    private var opening: Opening? = null

    /**
     * The current part's token while its open waits for a thread let go to end ([AbandonedSetups] at its limit), with
     * no player and no setup yet; 0 with none. Stop, a move and the end drop the wait as they drop an opening.
     */
    private var waiting = 0L
    private var prepared = false
    private var playing = false

    /**
     * The start asked of the current player and not acknowledged yet ([onStarted]): its serial, 0 with none. Until the
     * acknowledgement nothing plays as far as the play's readings go: the state reads [STARTING], the position and the
     * paused stretch stay as they were.
     */
    private var startPending = 0L
    private var startSerial = 0L

    /** A start was posted to the current player ([Player.start]): it may sound until its owner has paused it. */
    private var startMade = false

    /**
     * The players a move, the end or a replacement let go while they may have been sounding, by token, whose silence
     * ([onSilenced]) has not come yet (R52). A start asked for meanwhile, also a later play's first, waits
     * ([startDeferred]) under its watchdog. They outlive the play's end (finished, nothing left to try, Stop, the
     * host's own stop, a replacement: R61), which is published at once; only a start whose wait failed at its watchdog
     * drops them, so the next play does not wait for a stuck player again (the documented limit). Each silence is
     * fenced by its own token, so it counts only for the wait it belongs to. At most one is outstanding today, since no
     * start is posted while one is; the set keeps that true for any later path.
     */
    private val silencing = HashSet<Long>()
    private var startDeferred = false

    /**
     * The current part completed while paused (the platform played out what it had buffered at the Pause): it stays
     * current, paused at its end, with no seek, and moves on when resumed. Its player never starts again (R66).
     */
    private var completionHeld = false

    /** The current part has played (a start of it was acknowledged) since it opened (R46). */
    private var partPlayed = false
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

    /**
     * What an ended play's terminal reading names once its queue is let go (R43): the item it ended on and its call
     * count. The queue itself (items, failed marks, call starts) goes at the end; nothing reads it after, as every
     * command and task needs the play open.
     */
    private var endedItem: Item? = null
    private var endedCalls = 0
    private var endNotified = true
    private var taskIds = 0
    private val pending = HashMap<Slot, Pending>()

    /** The search candidates made so far, for the tests: each move makes only the ones it tries. */
    internal var candidatesMade = 0L
        private set

    /** Published on the looper after every change; read on any thread. */
    @Volatile
    var status: Status = snapshot()
        private set

    private val active: Boolean get() = generation != 0 && ended == null
    private val queue: Boolean get() = mode != MODE_SINGLE

    /** Play [path] alone from its start as play [generation] (SINGLE), replacing what plays. */
    fun play(generation: Int, path: String) = play(generation, MODE_SINGLE, listOf(Item(path)), 0)

    /**
     * Play [items] from [start] as play [generation] in [mode], replacing what plays: true. A request that breaks
     * [Item]'s rules ([validRequest]) is refused before anything is replaced (R63): false, and the play running plays
     * on untouched. RecordingPlayback refuses one before it is accepted, so none reaches here from the host. SINGLE
     * plays only the item at [start].
     */
    fun play(generation: Int, mode: Int, items: List<Item>, start: Int = 0): Boolean {
        if (!validRequest(mode, items, start)) {
            return false
        }
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
        callStarts = callStarts(items)
        if (mode == MODE_PLAYLIST && !admit(generation)) {
            return true
        }
        beginMove(Move(span(start, if (mode == MODE_SINGLE) start else items.size - 1), forward = true))
        return true
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
    /**
     * Whether a command scoped to play [generation] (and to the item whose play [token] it captured, 0 for the play
     * alone) would act now (R73): checked on the looper before an explicit command cancels a waiting tap, so a stale
     * one changes nothing, the tap included.
     */
    fun accepts(generation: Int, token: Long = 0L): Boolean = if (token != 0L) {
        isCurrent(generation, token)
    } else {
        generation != 0 && generation == this.generation && active
    }

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
        // The move that reached it (its direction, the candidates left) and the readings before it stay until it
        // plays (R46): prepared and failing before its first start, it is a candidate that failed, and the search
        // goes on in the move's direction.
        cancel(Slot.SEARCH)
        advance()
    }

    /**
     * The player's own thread made start [serial] of the part under [token]: it plays from now. A stale one (another
     * token, a start taken back since, or none wanted) is dropped.
     */
    fun onStarted(token: Long, serial: Long) {
        if (player == null || token != this.token || serial == 0L || serial != startPending || !prepared) {
            return
        }
        withdrawStart()
        if (expireIfDue()) {
            return
        }
        playing = true
        partPlayed = true
        left = null
        move = null
        playingSinceMs = clock.elapsedMs()
        // Audio carries on: a resume's hold-off ends, and with it the paused stretch (publish).
        resumeSuspended = false
        noteSkips()
        publish()
    }

    /**
     * The player landed seek [serial] (R56): the latest one asked of the current part becomes its position, and the
     * clock counts from there. One superseded since, or of a part gone, changes nothing.
     */
    fun onSeekCompleted(token: Long, serial: Long) {
        if (player == null || token != this.token || serial == 0L || serial != seekPending || !prepared) {
            return
        }
        seekPending = 0L
        if (completionHeld) {
            return // a held part stays at its end: a seek landing after its completion moves nothing
        }
        positionMs = seekTargetMs
        if (playing) {
            playingSinceMs = clock.elapsedMs()
        }
        seeksLanded++
        publish()
    }

    /**
     * It played to its end: the next part at once, the gap after a call, or the end, whatever its duration. Paused or
     * interrupted (the platform played out what it had buffered at the Pause), the completion is held: the part stays
     * current, paused at its end, and moves on when it is resumed ([completionHeld]).
     */
    fun onCompleted(token: Long) {
        if (player == null || token != this.token || !prepared || expireIfDue()) {
            return
        }
        playing = false
        withdrawStart()
        playingSinceMs = -1
        positionMs = durationMs
        if (!wanted || interrupted) {
            // Paused (the platform drained what it had buffered at the Pause): the part stays current, paused at its
            // end, and moves on when it is resumed, as if it had just completed. The paused deadline counts on.
            completionHeld = true
            publish()
            return
        }
        finishPart()
    }

    /** The current part completed while wanted, or was resumed after a held completion: on to what follows. */
    private fun finishPart() {
        completionHeld = false
        // Done with: never touched again (a later callback from it carries a token no open has again), and released
        // once the next part is handed off, so its release adds nothing to the joint between parts.
        val done = player
        val doneWork = playerWork
        leave(release = false)
        // Counted now, before the next part's open checks the limit; its release is posted after the hand-off.
        doneWork?.let(abandoned::letGo)
        val call = currentCall()
        when {
            mode == MODE_SINGLE -> end(STOPPED, "", ENDED_FINISHED)
            (index + 1..callEnd(call)).any { !failed[it] } ->
                beginMove(Move(span(index + 1, callEnd(call)), forward = true, gapAfter = true))
            hasCallAfter(call) -> enterGap()
            else -> end(STOPPED, "", ENDED_FINISHED)
        }
        if (done != null && doneWork != null) {
            letGo(done, doneWork)
        }
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
        askSilence()
        if (player != null && prepared) {
            leave()
        }
        releasePlayer()
        prepared = false
        positionMs = 0
        durationMs = 0
        move = next
        tryNext()
    }

    /**
     * The current part's setup is dropped with its result still out: its thread counts from now until it ends, and its
     * player is released when its result is delivered. Its read holds only its own player's thread.
     */
    private fun dropOpening() {
        waiting = 0L
        val dropped = opening ?: return
        opening = null
        abandoned.letGo(dropped.work)
    }

    /** The current part is done with (completed, or left by a move): its player goes, its readings stay in [left]. */
    private fun leave(release: Boolean = true) {
        left = Left(index, token, durationMs, currentPositionMs())
        if (release) {
            releasePlayer()
        }
        player = null
        playerWork = null
        prepared = false
    }

    /**
     * Release the current part's player, if one is adopted: let go, it counts until its thread ends. One that may still
     * sound (a start was posted to it) and was not asked for its silence yet is paused and asked first (R61, R62, I2),
     * whatever let it go (a failure too), so the next start waits for its silence.
     */
    private fun releasePlayer() {
        val current = player ?: return
        if (startMade && token !in silencing) {
            current.pause()
            askSilence()
        }
        val work = playerWork
        val released = token
        player = null
        playerWork = null
        if (work != null) {
            letGo(current, work, released)
        } else {
            current.release { onSilenced(released) }
        }
    }

    /**
     * Release [let]: its thread counts from now until it has ended ([onThreadEnded]). Its thread ended, it is silent:
     * a silence still outstanding for [silenced] (its token; 0 for none) is taken as come (M3), so a release that threw
     * or a thread that ended before answering costs the next start no 10 s wait.
     */
    private fun letGo(let: Player, work: AbandonedSetups.Work, silenced: Long = 0L) {
        abandoned.letGo(work)
        let.release {
            onThreadEnded(work)
            if (silenced != 0L) {
                onSilenced(silenced)
            }
        }
    }

    /** A player's thread let go has ended: it leaves the count, and an open waiting at the limit goes on. */
    private fun onThreadEnded(work: AbandonedSetups.Work) {
        abandoned.ended(work)
        if (waiting != 0L && waiting == token && !abandoned.full) {
            // Under its own token and prepare timeout, which already cover the wait.
            waiting = 0L
            startSetup(index)
        }
    }

    /** Open the move's next candidate not failed: always a new player, never the one left. */
    private fun tryNext() {
        val search = move ?: return
        var candidate = search.next()
        while (candidate >= 0 && failed[candidate]) {
            candidate = search.next()
        }
        if (candidate < 0) {
            exhausted(search)
            return
        }
        open(candidate)
    }

    private fun exhausted(search: Move) {
        move = null
        when {
            search.gapAfter && hasCallAfter(currentCall()) -> {
                // The gap names the part that completed or failed while it played ([left]): no player is kept.
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
        // The calls before, nearest first, each from its first part, and this call: made as the search reaches them. A
        // move tries the earlier calls first, then this one; a restart this call first, then the earlier calls (R55),
        // so a call gone since keeps the search backward. The forward fallback comes only after both.
        val earlier = (call - 1 downTo 0).asSequence().flatMap { span(callStarts[it], callEnd(it)) }
        val thisCall = span(callStarts[call], callEnd(call))
        val candidates = if (restart) thisCall + earlier else earlier + thisCall
        // Always a new open, also of the part opening now: its file is checked again under a fresh token, and the
        // pass before can deliver nothing to it.
        beginMove(Move(candidates, forward = false, resumeAfter = callEnd(call)))
    }

    /**
     * Build the candidate's player here, guarded: one that cannot be built (its thread cannot start) ends the play.
     * Then hand its setup to its own thread, with the prepare timeout already armed, so it covers the setup too.
     * With [AbandonedSetups.LIMIT] players let go still running, no player is made and no setup started: the open
     * waits ([waiting]) until one's thread ends ([onThreadEnded]), its prepare timeout armed as usual, so storage that
     * stalls costs one timeout per recording, never a burst of failures.
     */
    private fun open(candidate: Int) {
        index = candidate
        token = tokens.incrementAndGet()
        // The part opening has no readings of its own yet: never the position or length of the part tried before it
        // (one that failed while it played), which the Previous rule, the seek and the record would read as this one's.
        // What a gap needs of the part that played is kept apart, in [left].
        positionMs = 0
        durationMs = 0
        partPlayed = false
        startMade = false
        seekPending = 0L
        completionHeld = false
        // Fenced by the slot's task id and this part's token: a prepare, an error, a move or the end cancels it,
        // and a later open's player is never failed by it. It covers a wait for a thread let go to end too.
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

    /** Make the current part's player here and hand its setup to the player's own thread. */
    private fun startSetup(candidate: Int) {
        val made = try {
            players(token)
        } catch (_: Throwable) {
            null // An Error too: a thread that cannot start is out of memory for it.
        }
        if (made == null) {
            failItem(REASON_PLAYER)
            return
        }
        val setup = Opening(token, made, mode != MODE_SINGLE, abandoned.begin())
        opening = setup
        publish()
        val item = items[candidate]
        // The work never throws: anything thrown (an Error such as running out of memory in the data source) is a
        // failed setup, so a result is posted on every path and its player is released, and so its thread ends.
        made.setup({
            try {
                setUp(opener, item, setup.checked, made)
            } catch (_: Throwable) {
                SetupResult(REASON_PLAYER, item.path)
            }
        }) { result -> onSetUp(setup, result) }
    }

    /**
     * A setup's result, on this thread. A file it found gone or changed goes on the recheck list first, whether or not
     * its part is still current. A part no longer current (timed out, moved off, stopped, ended, replaced) only
     * releases its player; a current one adopts it, prepared already if its prepare answered first, or fails.
     */
    private fun onSetUp(setup: Opening, result: SetupResult) {
        val serial = recheck.serial
        if (setup.checked && (result.failure == REASON_GONE || result.failure == REASON_CHANGED)) {
            recheck.add(result.path)
        }
        if (opening !== setup) {
            // Let go when its open was dropped; its thread ends after this release, which may wake a waiting open.
            letGo(setup.player, setup.work)
            if (recheck.serial != serial) {
                publish() // The host reads the recheck serial from the status.
            }
            return
        }
        opening = null
        if (result.failure.isNotEmpty()) {
            letGo(setup.player, setup.work)
            if (!expireIfDue()) {
                failItem(result.failure)
            }
            return
        }
        player = setup.player
        playerWork = setup.work
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
        // It played (a start acknowledged), not only prepared: one that prepared and never played is a candidate that
        // failed, and the move that reached it goes on (R46).
        val wasOpen = prepared && partPlayed
        positionMs = currentPositionMs()
        if (wasOpen) {
            // It played: followed as if it had completed, so the gap after its call names it, whatever opens next.
            left = Left(index, token, durationMs, positionMs)
        }
        releasePlayer()
        dropOpening()
        prepared = false
        playing = false
        withdrawStart()
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
        if (completionHeld) {
            // A completion held while paused: resumed, the part moves on as if it had just completed.
            if (interrupted) publish() else finishPart()
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
        if (current == null || !prepared || playing || startPending != 0L) {
            publish()
            return
        }
        if (!ensureFocus()) {
            return
        }
        if (!interrupted) {
            requestStart(current)
            if (startPending == 0L || playing) {
                return // acknowledged (or failed) at once, and published there
            }
        }
        publish()
    }

    /**
     * Ask [current] to start, as a new request: the part plays from its acknowledgement ([onStarted]), never from
     * here, and a watchdog scoped to the part's token and the request's serial fails it as the player's if none comes
     * within [START_TIMEOUT_MS] (R44).
     */
    private fun requestStart(current: Player) {
        val serial = ++startSerial
        val asked = token
        startPending = serial
        schedule(Slot.START, START_TIMEOUT_MS) {
            if (startPending == serial && token == asked && player != null) {
                if (startDeferred) {
                    silencing.clear() // a silence that never came: the next play does not wait for it again (R61)
                }
                failItem(REASON_PLAYER)
            }
        }
        if (silencing.isEmpty()) {
            postStart(current, serial)
        } else {
            startDeferred = true // posted once every player let go is silent; the watchdog runs meanwhile (R52)
        }
    }

    private fun postStart(current: Player, serial: Long) {
        startDeferred = false
        startMade = true
        current.start(serial)
    }

    /** The pending start is taken back (a pause, a seek, a move, the end): no acknowledgement of it counts. */
    private fun withdrawStart() {
        startPending = 0L
        startDeferred = false
        cancel(Slot.START)
    }

    /**
     * A player a move let go is silent (R52): its owner has run the move's pause. Once every such player of the play
     * is, a start that waited for them is posted. One this play is not waiting for (an earlier play's) is ignored.
     */
    fun onSilenced(token: Long) {
        if (!silencing.remove(token) || silencing.isNotEmpty()) {
            return
        }
        val current = player
        if (startDeferred && startPending != 0L && current != null && prepared) {
            postStart(current, startPending)
        }
    }

    /** The current player, let go by a move or a replacement while it may sound, is asked for its silence (R52). */
    private fun askSilence() {
        val current = player ?: return
        if (startMade && silencing.add(token)) {
            current.silence()
        }
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

    /** The item the readings name: the current one, or the one an ended play ended on. */
    private fun shownItem(): Item? = if (ended != null) endedItem else items.getOrNull(index)

    /** The recordings a play holds, its queue's every entry, for the tests: none once it has ended. */
    internal val queueHeld: Int get() = items.size + failed.size + callStarts.size - 1

    /** The notice lasts while its call is current: another call (or none) ends it. */
    private fun followSkipNotice() {
        if (skipNoticeCall >= 0 && shownItem()?.call != skipNoticeCall) {
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
        if (startPending != 0L) {
            // Asked for and not acknowledged: taken back at once (a start the thread made already is paused).
            withdrawStart()
            current.pause()
            return
        }
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
        // A start still pending is taken back before the seek is posted, so the old request can never sound from the
        // old position; the wish to play is unchanged, so a new start is asked for behind the seek (R45).
        val restart = startPending != 0L
        if (restart) {
            withdrawStart()
            current.pause()
        }
        // Asked for, not landed: the readings keep the confirmed position until the player says it is there (R56).
        seekPending = ++seekSerial
        seekTargetMs = clamped
        current.seekTo(clamped, seekPending)
        if (restart) {
            requestStart(current)
            if (startPending == 0L || playing) {
                return // acknowledged (or failed) at once, and published there: nothing after an end
            }
            publish()
        }
        // A plain seek changes no reading until it lands, and the landing publishes (R56): publishing the confirmed
        // position now would snap a system scrubber the user just dragged back to it (M4).
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
                        service.startAbandoned(generation) // before the end shows (R53)
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
        completionHeld = false
        positionMs = currentPositionMs()
        cancelAll()
        // The end is published at once; a player that may still sound is paused and asked for its silence as it is
        // let go, which the next play's first start waits for (R61).
        releasePlayer()
        dropOpening()
        left = null
        move = null
        prepared = false
        playing = false
        withdrawStart()
        playingSinceMs = -1
        gap = false
        wanted = false
        pausedReason = ""
        pausedSinceMs = -1
        resumeSuspended = false
        dropFocus()
        ended = Ended(state, error, reason)
        endedItem = items.getOrNull(index)
        endedCalls = items.lastOrNull()?.let { it.call + 1 } ?: 0
        items = emptyList()
        failed = BooleanArray(0)
        callStarts = IntArray(1)
        publish()
        if (mode == MODE_PLAYLIST && !endNotified) {
            endNotified = true
            service.ended(generation, reason)
        }
    }

    /** Release the players, the focus and the tasks, and forget the play's readings (not its generation). */
    private fun close() {
        completionHeld = false
        cancelAll()
        // The next play's first start waits for this play's sounding player to fall silent (R52, M3, R61): it is
        // paused and asked as it is let go.
        releasePlayer()
        dropOpening()
        left = null
        move = null
        prepared = false
        playing = false
        withdrawStart()
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
        seeksLanded = 0
        startRequest = true
        resumeSuspended = false
        ended = null
        endedItem = null
        endedCalls = 0
        // The queue goes with the play: the host's own stop leaves nothing held, as every other end does.
        failed = BooleanArray(0)
        callStarts = IntArray(1)
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
    private fun after(index: Int): Sequence<Int> = span(index + 1, items.size - 1)

    /** The items [first] to [last] in order (none when [last] is before [first]), each made as a search reaches it. */
    private fun span(first: Int, last: Int): Sequence<Int> = (first..last).asSequence().map {
        candidatesMade++
        it
    }

    private fun hasCallAfter(call: Int): Boolean = (callEnd(call) + 1 until items.size).any { !failed[it] }

    private fun canSeek(): Boolean =
        active && promoted && player != null && prepared && !gap && !interrupted && durationMs > 0 && !completionHeld

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
            // Asked for, not acknowledged: the controls follow the wish (Pause), the clock waits for the audio (R49).
            startPending != 0L -> STARTING
            playing -> PLAYING
            else -> PAUSED
        }
    }

    private fun sessionState(): Int = when {
        generation == 0 -> SESSION_NONE
        ended != null -> SESSION_STOPPED
        !promoted -> SESSION_NONE
        interrupted || !wanted -> SESSION_PAUSED
        // Opening a wanted part or waiting for its start: the wish is to play, no audio yet (R49). The headset's taps
        // are counted by PlayPauseTaps, which reads BUFFERING as playing, so a tap still pauses. The gap keeps PLAYING.
        !gap && (player == null || !prepared || startPending != 0L) -> SESSION_BUFFERING
        else -> SESSION_PLAYING
    }

    private fun shownPausedReason(): String = when {
        !active -> ""
        !wanted -> pausedReason
        mode != MODE_PLAYLIST && !foreground -> PAUSED_BACKGROUND
        else -> ""
    }

    private fun snapshot(): Status {
        val item = shownItem()
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
            calls = if (ended != null) endedCalls else items.lastOrNull()?.let { it.call + 1 } ?: 0,
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
            seeksLanded = seeksLanded,
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
