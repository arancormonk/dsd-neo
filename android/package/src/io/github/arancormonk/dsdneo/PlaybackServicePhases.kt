// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * The call recording playlist's service order (#909): which play the playback service serves, when a start is issued,
 * what each delivery does and when the service retires. RecordingPlayback and RecordingPlaybackService carry out what
 * it decides; it holds no Android state, so the JVM tests drive it directly.
 *
 * Everything is under one lock: admission (closed for good by [release]), the service's [Phase], the reserved request
 * (the playlist a start will promote) and the owner (the playlist the promoted service serves). At most one start that
 * can promote is outstanding; a start given up ([abandonStart], R53) may still be delivered beside a fresh one, and its
 * delivery, fenced by its generation, promotes nothing and retires. A playlist accepted while the service is promoted
 * is handed to it in the process; one accepted while a start is issued becomes that start's request; only one accepted
 * with no service issues a start ([Start.ISSUE]). Issuing, handing over, retiring and closing admission all take the
 * lock, so a play, a retire and the teardown never interleave.
 *
 * A start carries this process's token and the generation it was issued for. Its delivery ([promote]) promotes the
 * current request only when it is that start; anything else (a start from an earlier process, redelivered after it
 * died) leaves the instance in the foreground with nothing to serve. A transport delivery (a notification button)
 * names the owner it was posted for and is applied only while that playlist still owns the service. After every
 * delivery and every asynchronous end the instance asks [retire]: with no owner, no request and no start outstanding
 * it stops against the newest start id it has seen, so a start that arrived after the decision keeps it alive.
 */
class PlaybackServicePhases(private val processToken: Long) {
    enum class Phase {
        /** No service, and no start outstanding that can promote (an abandoned one may still arrive, R53). */
        NONE,

        /** A start was issued and not delivered yet. */
        ISSUED,

        /** An instance is in the foreground. */
        PROMOTED,

        /** The instance stopped itself (past stopSelfResult) and is on its way out. */
        RETIRING,
    }

    /** What an accepted play does about the service. */
    enum class Start {
        /** Not a playlist: no service. */
        NONE,

        /** No service runs: issue a start for the play. */
        ISSUE,

        /** A start is outstanding: the play becomes its request, and no other start is issued. */
        JOIN,

        /** The service is in the foreground: it serves the play at once, in the process. */
        HAND_OVER,
    }

    /** What a start's delivery did: the playlist it promoted, or the one whose start was refused (0: none). */
    class Promotion(val promoted: Int, val refused: Int)

    /**
     * What an instance's destroy settles: the playlist it served, for the caller to end (0: none), and whether it was
     * still the instance alive ([current]). Its own session and taps always go with it; the state the process shares
     * between instances (the media key handoff's active session, the notification id) is cleared only when [current],
     * since a destroy reported after the next instance was created would otherwise take the new one's.
     */
    class Destroyed(val served: Int, val current: Boolean)

    private val lock = Any()
    private var open = true
    private var generations = 0
    private var phase = Phase.NONE

    /** The generation the outstanding start was issued for: its delivery names it. */
    private var issuedFor = 0
    private var reserved = 0
    private var owner = 0
    private var instances = 0

    /** The service instance alive now (one at a time); 0 when none. */
    private var alive = 0

    val currentPhase: Phase get() = synchronized(lock) { phase }

    /** The playlist the promoted service serves, or 0. */
    val currentOwner: Int get() = synchronized(lock) { owner }

    /** The playlist waiting for the outstanding start, or 0. */
    val currentRequest: Int get() = synchronized(lock) { reserved }

    /**
     * Accept a play, a [playlist] or not, replacing what played: its generation, or 0 once admission is closed. [then]
     * runs under the lock with the generation and what to do about the service, so what it posts is ordered against a
     * teardown. A play that is not a playlist ends the service's request and ownership at once.
     */
    fun accept(playlist: Boolean, then: (generation: Int, start: Start) -> Unit): Int = synchronized(lock) {
        if (!open) {
            return 0
        }
        val generation = ++generations
        val start = when {
            !playlist -> {
                owner = 0
                reserved = 0
                Start.NONE
            }
            phase == Phase.PROMOTED -> {
                owner = generation
                reserved = 0
                Start.HAND_OVER
            }
            phase == Phase.ISSUED -> {
                reserved = generation
                Start.JOIN
            }
            else -> {
                owner = 0
                reserved = generation
                issuedFor = generation
                phase = Phase.ISSUED
                Start.ISSUE
            }
        }
        then(generation, start)
        generation
    }

    /**
     * The start issued for [generation] was refused as it was asked for: nothing is outstanding, and whatever request
     * waited for that start is settled with it. The plays to tell they could not start: [generation] and the request
     * that waited, if another (each once, none 0).
     */
    fun startFailed(generation: Int): IntArray = synchronized(lock) {
        if (phase != Phase.ISSUED || issuedFor != generation) {
            return intArrayOf(generation)
        }
        phase = Phase.NONE
        issuedFor = 0
        val waited = reserved
        reserved = 0
        if (waited == 0 || waited == generation) intArrayOf(generation) else intArrayOf(generation, waited)
    }

    /**
     * A service instance was created: its serial, which its later calls name. The instance before it is gone, so a
     * retiring phase ends here, even when that instance's destroy is reported late.
     */
    fun created(): Int = synchronized(lock) {
        alive = ++instances
        if (phase == Phase.RETIRING) {
            phase = Phase.NONE
        }
        alive
    }

    /** The playlist whose notification a start's delivery shows as it enters the foreground: the owner, or 0. */
    fun foregroundOwner(): Int = synchronized(lock) { owner }

    /**
     * A start delivered to [instance] with the [token] and [generation] it carries, after the instance tried to enter
     * the foreground ([foreground]: whether it did). Only the outstanding start of this process promotes, and it
     * promotes the request standing now, which may be a later play than the one it was issued for, or none (settled).
     */
    fun promote(instance: Int, token: Long, generation: Int, foreground: Boolean): Promotion = synchronized(lock) {
        if (token != processToken || phase != Phase.ISSUED || generation != issuedFor || instance != alive) {
            return Promotion(0, 0)
        }
        issuedFor = 0
        val request = reserved
        reserved = 0
        if (!foreground) {
            phase = Phase.NONE
            return Promotion(0, request)
        }
        phase = Phase.PROMOTED
        owner = request
        Promotion(request, 0)
    }

    /** A transport delivery for [generation] from [token]: the playlist to apply it to, or 0 when it is stale. */
    fun transport(token: Long, generation: Int): Int = synchronized(lock) {
        if (token == processToken && generation != 0 && generation == owner) generation else 0
    }

    /** Whether [generation] owns the service now: a command is applied only then, as it runs. */
    fun isOwner(generation: Int): Boolean = synchronized(lock) { generation != 0 && generation == owner }

    /** Playlist [generation] ended (finished, stopped, timed out, refused, replaced): it no longer owns or waits. */
    fun ended(generation: Int) = synchronized(lock) {
        if (generation == owner) {
            owner = 0
        }
        if (generation == reserved) {
            reserved = 0
        }
    }

    /**
     * Playlist [generation], the request waiting for the start issued, gave that start up (its promotion timed out,
     * R53): the start no longer promotes, so the next play issues a fresh one instead of joining a start that may never
     * be delivered. Called on the looper before the play's end is published. A late delivery of the abandoned start is
     * still fenced by its generation: it promotes nothing and retires, as a stale start does.
     */
    fun abandonStart(generation: Int) = synchronized(lock) {
        if (generation == reserved && phase == Phase.ISSUED) {
            reserved = 0
            phase = Phase.NONE
            issuedFor = 0
        }
    }

    /** The playlist the service serves or waits for, for an end the instance asks for (task removed); 0 when none. */
    fun current(): Int = synchronized(lock) { if (owner != 0) owner else reserved }

    /**
     * The retire check, after every delivery and every asynchronous end: with no owner, no request and no start
     * outstanding, [stop] (stopSelfResult with the newest start id) runs under the lock; true when it stopped.
     */
    fun retire(instance: Int, stop: () -> Boolean): Boolean = synchronized(lock) {
        if (instance != alive || owner != 0 || reserved != 0 || phase == Phase.ISSUED || phase == Phase.RETIRING) {
            return false
        }
        if (!stop()) {
            return false
        }
        phase = Phase.RETIRING
        true
    }

    /**
     * [instance] is destroyed: the playlist it served, for the caller to settle, and whether it is still the instance
     * alive ([Destroyed]). A start issued since it retired is the next instance's and stays outstanding.
     */
    fun destroyed(instance: Int): Destroyed = synchronized(lock) {
        if (instance != alive) {
            return Destroyed(0, false)
        }
        alive = 0
        val served = owner
        owner = 0
        if (phase == Phase.PROMOTED || phase == Phase.RETIRING) {
            phase = Phase.NONE
        }
        Destroyed(served, true)
    }

    /**
     * The app is ending: admission closes, the request and the owner are settled, and [then] runs under the lock (the
     * player's stop). True when a promoted service must be stopped (stopService). A start issued and not promoted is
     * left to its delivery, which promotes and retires: stopping it before its startForeground() would crash.
     */
    fun release(then: () -> Unit): Boolean = synchronized(lock) {
        open = false
        owner = 0
        reserved = 0
        val promoted = phase == Phase.PROMOTED
        if (promoted) {
            phase = Phase.RETIRING
        }
        then()
        promoted
    }
}

/**
 * The pace of the playlist notification's updates (#909). Android sheds an app's notification updates while its
 * enqueue rate is over the limit (NotificationManagerService.DEFAULT_MAX_NOTIFICATION_ENQUEUE_RATE, 5 per second for
 * the package, an exponentially weighted estimate that a quick pair alone can push over it), so a burst of renders (a
 * skip through missing recordings) could lose the last one and leave its controls stale: Pause shown on a paused
 * playlist on API 29-32. The decoder's notification shares that budget and is updated at most once a second (its
 * status poll). So the playlist's is posted at most once per [intervalMs]: the first of a burst at once, the rest as
 * one trailing post of the latest value [offer] was given. The media session follows every render at once; only the
 * notification waits. [post] posts a value and says whether it did; a value is posted only while it is still [current]
 * (its playlist owns the service), so a trailing post never shows a playlist another has replaced. Not thread-safe:
 * the service's main thread only.
 */
class NotificationPace<T : Any>(
    private val scheduler: RecordingPlayer.Scheduler,
    private val clock: RecordingPlayer.Clock,
    private val current: (T) -> Boolean,
    private val post: (T) -> Boolean,
    private val intervalMs: Long = INTERVAL_MS,
) {
    private var lastPostMs = 0L
    private var posted = false
    private var latest: T? = null
    private var trailing: RecordingPlayer.Cancellable? = null

    /** [value] is the newest to show: posted now, or by the trailing post, which then shows the newest given. */
    fun offer(value: T) {
        latest = value
        if (trailing != null) {
            return
        }
        val waitMs = if (posted) lastPostMs + intervalMs - clock.elapsedMs() else 0
        if (waitMs <= 0) {
            flush()
        } else {
            trailing = scheduler.post(waitMs) {
                trailing = null
                flush()
            }
        }
    }

    /**
     * The notification was posted another way (startForeground): it counts as a post. When it showed the owner's
     * status as it is now ([covers]), it covers what a trailing post waits to show, which is dropped.
     */
    fun postedNow(covers: Boolean = false) {
        lastPostMs = clock.elapsedMs()
        posted = true
        if (covers) {
            trailing?.cancel()
            trailing = null
            latest = null
        }
    }

    /** The notification is taken down (the service retires or goes): nothing waiting is posted. */
    fun cancel() {
        trailing?.cancel()
        trailing = null
        latest = null
    }

    private fun flush() {
        val value = latest ?: return
        latest = null
        if (current(value) && post(value)) {
            postedNow()
        }
    }

    companion object {
        /** At most two a second: with the decoder's one a second, well under the package's five. */
        const val INTERVAL_MS = 500L
    }
}

/**
 * What a media session's callback acts on (#909, R36): the playlist [generation] the session showed when the callback
 * was installed. RecordingPlaybackService installs a callback of its own for each playlist, never for each part: every
 * move opens its part under a new token, so a callback swapped per part dropped the keys queued behind a move (Next,
 * Next, Stop lost the Stop). A command the framework queued for an earlier playlist carries that one's generation, and
 * the player drops it. The commands are queue-wide and act on the queue's state when the looper runs them; a seek
 * applies to the item current then (a seek sent as a part changes lands on the new part).
 */
class SessionTarget(val generation: Int) {
    fun play(player: RecordingPlayer) = on { player.resume(generation) }

    fun pause(player: RecordingPlayer) = on { player.pause(generation) }

    fun next(player: RecordingPlayer) = on { player.next(generation) }

    fun previous(player: RecordingPlayer) = on { player.previous(generation) }

    fun seek(player: RecordingPlayer, positionMs: Long) =
        on { player.seekFor(generation, player.status.playToken, positionMs) }

    fun stop(player: RecordingPlayer) = on { player.stop(generation) }

    /** Whether this is the target of [status]: its playlist, whatever its item. */
    fun shows(status: RecordingPlayer.Status): Boolean = generation == status.generation

    /** Generation 0 is no playlist: the player would read it as "whatever plays", so nothing is sent for it. */
    private inline fun on(command: () -> Unit) {
        if (generation != 0) {
            command()
        }
    }

    companion object {
        /** Before anything was published: no generation, which no command reaches. */
        val NONE = SessionTarget(0)
    }
}

/**
 * The headset's play/pause taps on the playlist's media session (#909), held here instead of by the framework, which
 * keeps a single tap waiting for a double tap on the callback's own handler and toggles against the state it last
 * published, posted to the main thread later (see [SessionKeys]). The rules are the framework's
 * (MediaSession.Callback.onMediaButtonEvent): a tap waits [timeoutMs] (the double-tap timeout), a
 * second within it is Next, and once it runs out the tap toggles play and pause; a long press (a repeat) or another
 * media key counts a waiting tap as single at once, so keys act in order. A tap belongs to its playlist (generation),
 * never to a part: a part change keeps it, while another playlist, the end or the service going drops it, and its
 * timeout with it. [toggle] and [next] run on the scheduler's thread or the caller's, outside the lock.
 */
class PlayPauseTaps(
    private val scheduler: RecordingPlayer.Scheduler,
    private val timeoutMs: Long,
    private val toggle: (generation: Int) -> Unit,
    private val next: (generation: Int) -> Unit,
) {
    private class Tap(val generation: Int) {
        var timeout: RecordingPlayer.Cancellable? = null
    }

    private val lock = Any()
    private var pending: Tap? = null

    /** A play/pause key (headset hook, play/pause) went down for playlist [generation], [repeat] its repeat count. */
    fun keyDown(generation: Int, repeat: Int) {
        if (repeat > 0) {
            settle()
            return
        }
        val second = synchronized(lock) {
            val waiting = pending
            if (waiting != null && waiting.generation == generation) {
                clear(waiting)
                true
            } else {
                waiting?.let(::clear) // another playlist's tap: not this one's to count
                if (generation != 0) {
                    val tap = Tap(generation)
                    pending = tap
                    tap.timeout = scheduler.post(timeoutMs) { timedOut(tap) }
                }
                false
            }
        }
        if (second) {
            next(generation)
        }
    }

    /** Another media key went down: a waiting tap counts as single first. */
    fun otherKey() = settle()

    /** The session now shows playlist [generation]: a tap of another one is dropped. */
    fun follow(generation: Int) = synchronized(lock) {
        pending?.takeIf { it.generation != generation }?.let(::clear)
    }

    /** Playlist [generation] ended: its tap is dropped. */
    fun ended(generation: Int) = synchronized(lock) {
        pending?.takeIf { it.generation == generation }?.let(::clear)
    }

    /** The service is going: any tap is dropped. */
    fun drop() = synchronized(lock) {
        pending?.let(::clear)
    }

    private fun settle() {
        val tap = synchronized(lock) { pending?.also(::clear) } ?: return
        toggle(tap.generation)
    }

    private fun timedOut(tap: Tap) {
        val mine = synchronized(lock) {
            (pending === tap).also { if (it) pending = null }
        }
        if (mine) {
            toggle(tap.generation)
        }
    }

    private fun clear(tap: Tap) {
        pending = null
        tap.timeout?.cancel()
    }

    companion object {
        /**
         * A single tap, as the framework's: Pause while the session plays (or buffers a start) and offers it, else Play
         * when offered.
         */
        fun toggle(player: RecordingPlayer, generation: Int) {
            val status = player.status
            if (status.generation != generation) {
                return
            }
            // A start still pending counts as playing: the tap takes it back and pauses (R49).
            val playing = status.sessionState == RecordingPlayer.SESSION_PLAYING ||
                status.sessionState == RecordingPlayer.SESSION_BUFFERING
            if (playing) {
                if (status.allows(RecordingPlayer.ACTION_PAUSE)) {
                    player.pause(generation)
                }
            } else if (status.allows(RecordingPlayer.ACTION_PLAY)) {
                player.resume(generation)
            }
        }

        /**
         * An explicit transport command, on the looper (R67): a notification button for playlist [generation] (API
         * 29-32), or the app's own Pause, Resume, Next, Previous or Stop (generation 0: whatever plays). The waiting
         * tap ([taps], the service instance's, null with none) is cancelled, not counted, then [command] runs, so it
         * acts alone and the tap's timeout never undoes it. A media-button key settles a waiting tap first instead, as
         * AOSP's MediaSession.Callback does ([SessionKeys.onKey]).
         */
        fun explicit(
            taps: PlayPauseTaps?,
            generation: Int,
            player: RecordingPlayer,
            command: (RecordingPlayer, Int) -> Unit,
        ) {
            if (generation != 0) {
                taps?.ended(generation)
            } else {
                taps?.drop()
            }
            command(player, generation)
        }

        /**
         * The headphones were disconnected (AUDIO_BECOMING_NOISY), on the looper: the instance's waiting tap ([taps],
         * null with no service) is dropped first, then the player pauses for the noise (R54). Left armed, the tap's
         * timeout would find the play paused and resume it through the speaker.
         */
        fun disconnected(taps: PlayPauseTaps?, player: RecordingPlayer) {
            taps?.drop()
            player.onNoisy()
        }

        /** A double tap: Next when the session offers it. */
        fun next(player: RecordingPlayer, generation: Int) {
            val status = player.status
            if (status.generation == generation && status.allows(RecordingPlayer.ACTION_NEXT)) {
                player.next(generation)
            }
        }
    }
}

/**
 * The media buttons the playlist's session takes itself (#909), as RecordingPlaybackService.onMediaButtonEvent hands
 * them over on the playback looper. The headset hook and play/pause are counted by [taps]. Play, Pause, Next, Previous
 * and Stop are each sent ([send]) as the queue command for the playlist [generation] names, applied to the player's
 * state as it is then; not left to the framework, which checks the actions the session last published, and those are
 * posted to the main thread later: a Pause right after a waiting tap resumed the play would find the paused session's
 * actions and be dropped. A waiting tap counts as single first, so the keys act in order. These keys' ups and repeats
 * are taken and ignored; any other key is left to the framework ([onKey] false), after a waiting tap counts as single.
 */
class SessionKeys(
    private val taps: PlayPauseTaps,
    private val send: (generation: Int, command: (RecordingPlayer, Int) -> Unit) -> Unit,
) {
    /** A media key event for playlist [generation]: true when it is taken here. */
    fun onKey(generation: Int, keyCode: Int, down: Boolean, repeat: Int): Boolean {
        if (keyCode == MediaKeys.HEADSETHOOK || keyCode == MediaKeys.MEDIA_PLAY_PAUSE) {
            if (down) {
                taps.keyDown(generation, repeat)
            }
            return true
        }
        val command = transport(keyCode)
        if (command == null) {
            if (down) {
                taps.otherKey()
            }
            return false
        }
        if (down && repeat == 0) {
            taps.otherKey()
            send(generation, command)
        }
        return true
    }

    /**
     * A direct transport callback of the session (onPlay, onPause, onStop, onSkipToNext, onSkipToPrevious, R60): the
     * playlist [generation]'s waiting tap is cancelled, not counted, then [command] is sent, so it acts alone. A
     * lock-screen Pause in a tap's double-tap wait is never undone by the tap's timeout, and a paused playlist's tap
     * never resumes it (taking another app's audio) before the Pause.
     */
    fun direct(generation: Int, command: (RecordingPlayer, Int) -> Unit) {
        taps.ended(generation)
        send(generation, command)
    }

    companion object {
        /** The queue command of a transport key, or null for another key. Each is dropped where the state has none. */
        fun transport(keyCode: Int): ((RecordingPlayer, Int) -> Unit)? = when (keyCode) {
            MediaKeys.MEDIA_PLAY -> { player, generation -> player.resume(generation) }
            MediaKeys.MEDIA_PAUSE -> { player, generation -> player.pause(generation) }
            MediaKeys.MEDIA_NEXT -> { player, generation -> player.next(generation) }
            MediaKeys.MEDIA_PREVIOUS -> { player, generation -> player.previous(generation) }
            MediaKeys.MEDIA_STOP -> { player, generation -> player.stop(generation) }
            else -> null
        }
    }
}
