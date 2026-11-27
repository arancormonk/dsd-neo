// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.session.MediaSession
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import io.github.arancormonk.dsdneo.PlaybackServicePhases.Start
import java.io.File
import java.io.FileDescriptor
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Call recording playback (#708, D6/D7/D10; #909): the Android half that DecoderHostAndroid calls over JNI.
 *
 * One looper thread owns a [RecordingPlayer], so the machine's calls and callbacks never race. The Qt thread's calls
 * only post there and return; the host polls [status], which reads the state the looper published. Each part's
 * MediaPlayer lives on a thread of its own ([MediaItem]), which makes it, calls it and receives its callbacks, as
 * MediaPlayer's threading contract asks; the looper never touches it. Blocking file I/O never runs on the looper: the
 * part's own thread opens the file, sets it as the data source and starts the prepare, and posts the result back with
 * the part's token. The audio focus is speech, transient for a sheet's play and lasting for a History playlist: other
 * media pauses while a recording plays, and the decoder's live audio, which asks for no focus, plays on beside it (D7).
 * DsdNeoActivity reports the foreground, whose loss pauses a sheet's play (D10), and [release]s playback when the app
 * ends. Headphones unplugged pause any play.
 *
 * A History playlist plays on in the background under [RecordingPlaybackService], in the order [PlaybackServicePhases]
 * keeps: the play is posted to the player, then a start is issued while the app is visible, and audio begins once the
 * service has promoted it. A partial wake lock is held while a playlist wants audio (opening, playing, the gap); while
 * a playlist is open the screen turning on checks its paused deadline, whose task counts only the device's awake time.
 * The recordings found gone or changed at their turn wait in a list for the store ([takeRecheck]).
 */
object RecordingPlayback {
    private const val TAG = "dsd-neo"

    private val phases = PlaybackServicePhases(DecoderService.processToken)
    private val lock = Any()

    /** The looper's Handler, made by the first play (or the first service instance); under [lock]. */
    private var handler: Handler? = null

    /** Whether the Activity is started; read when the player is made, then posted to it. */
    private var foreground = true

    /** Made with the first play, before anything is posted for it; then used only on the looper. */
    @Volatile
    private var player: RecordingPlayer? = null

    /** The application, kept from the first play for the service's start and stop. */
    @Volatile
    private var application: Context? = null

    /** The recordings found gone or changed at their turn (#909), for the store to look at again; for the process. */
    private val recheck = RecordingRecheck()

    /**
     * The players' threads let go and not ended yet, for the process (#909): at [AbandonedSetups.LIMIT] of them (a
     * player's own thread and its MediaPlayer each, stuck in a setup's read or a release's that may never return) an
     * open waits, bounded by its prepare timeout, so stalled storage cannot pile up threads; Stop never waits for one.
     * A thread leaves the count when its release task ends ([MediaItem.release]'s acknowledgement on the looper).
     */
    private val abandonedSetups = AbandonedSetups()

    private val main: Handler by lazy { Handler(Looper.getMainLooper()) }
    private val renderPosted = AtomicBoolean()

    /** The playback service instance alive; on the main thread only. */
    private var service: RecordingPlaybackService? = null

    /** The service instance's headset taps, for the explicit commands on the looper (R67); null with none. */
    @Volatile
    private var serviceTaps: PlayPauseTaps? = null

    /** The playlist whose media session the service last rendered active (#909): the Activity's media key handoff. */
    private val activeSession = ActiveSession<MediaSession.Token>()

    /** Looper only: the playlist's wake lock and the receivers' registrations. */
    private var wakeLock: PowerManager.WakeLock? = null
    private val noisy = Listening(
        Noisy,
        AudioManager.ACTION_AUDIO_BECOMING_NOISY,
        "the headphones receiver",
        "this play will not pause on unplugging",
    )
    private val screenOn = Listening(
        ScreenOn,
        Intent.ACTION_SCREEN_ON,
        "the screen-on receiver",
        "a paused playlist ends on its deadline's task",
    )

    internal val attributes: AudioAttributes by lazy {
        AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
            .build()
    }

    /**
     * Play [path] alone, replacing what plays (SINGLE): the play's generation, which [status] names; 0 refused, before
     * it is accepted, for a request [RecordingPlayer.validRequest] refuses (an empty path, R70), what plays playing on.
     */
    @JvmStatic
    fun play(context: Context, path: String): Int {
        if (!RecordingPlayer.validRequest(RecordingPlayer.MODE_SINGLE, listOf(RecordingPlayer.Item(path)), 0)) {
            return 0
        }
        return begin(context, RecordingPlayer.MODE_SINGLE) { machine, generation -> machine.play(generation, path) }
    }

    /**
     * Play the items the arrays list, one entry per recording, from [start] in [mode] (RecordingPlayer.MODE_*),
     * replacing what plays: the play's positive generation once accepted, also when its service could not be started
     * (that generation then reads FAILED, ended "start"). 0 only for a refusal before acceptance, which leaves what
     * plays untouched: arrays of different lengths, no items, [start] or [mode] out of range, a PARTS or PLAYLIST item
     * with no identity (a negative size, [RecordingPlayer.identified]; the host refuses one first), any other request
     * [RecordingPlayer.validRequest] refuses (calls out of order, a part outside its call's count), or the app ending.
     */
    @JvmStatic
    fun play(
        context: Context,
        paths: Array<String>,
        sizes: LongArray,
        mtimeSecs: LongArray,
        mtimeNanos: IntArray,
        calls: IntArray,
        partOrdinals: IntArray,
        partCounts: IntArray,
        titles: Array<String>,
        lines: Array<String>,
        keys: Array<String>,
        start: Int,
        mode: Int,
    ): Int {
        val count = paths.size
        val lengths = intArrayOf(
            sizes.size, mtimeSecs.size, mtimeNanos.size, calls.size, partOrdinals.size, partCounts.size, titles.size,
            lines.size, keys.size,
        )
        if (count == 0 || lengths.any { it != count } || start !in 0 until count ||
            mode !in RecordingPlayer.MODE_SINGLE..RecordingPlayer.MODE_PLAYLIST ||
            !RecordingPlayer.identified(mode, sizes)
        ) {
            return 0
        }
        val items = List(count) { i ->
            RecordingPlayer.Item(
                path = paths[i],
                size = sizes[i],
                mtimeSec = mtimeSecs[i],
                mtimeNanos = mtimeNanos[i],
                call = calls[i],
                partOrdinal = partOrdinals[i],
                partCount = partCounts[i],
                key = keys[i],
                title = titles[i],
                line = lines[i],
            )
        }
        // Refused before it is accepted, so what plays plays on (R63): the machine would refuse it anyway, but only
        // after the service's request and ownership had moved to it.
        if (!RecordingPlayer.validRequest(mode, items, start)) {
            return 0
        }
        return begin(context, mode) { machine, generation -> machine.play(generation, mode, items, start) }
    }

    /**
     * Queue-wide commands: the current play, whatever its item. Each is explicit (R67): a waiting headset tap is
     * cancelled first, so the command acts alone.
     */
    @JvmStatic
    fun next() = explicit { it.next() }

    @JvmStatic
    fun previous() = explicit { it.previous() }

    @JvmStatic
    fun pause() = explicit { it.pause() }

    @JvmStatic
    fun resume() = explicit { it.resume() }

    /** The host's own stop: whatever plays stops, and the next status has no generation. */
    @JvmStatic
    fun stop() = explicit { it.stop() }

    /** Stop play [generation] only (the bar's Stop names its playlist); it keeps its terminal reading. */
    @JvmStatic
    fun stopGeneration(generation: Int) = explicit { it.stop(generation) }

    private fun explicit(command: (RecordingPlayer) -> Unit) =
        post { machine -> PlayPauseTaps.explicit(serviceTaps, 0, machine) { player, _ -> command(player) } }

    /** Item commands: dropped on the looper unless the item whose play [token] the host captured is still current. */
    @JvmStatic
    fun seekFor(token: Long, positionMs: Long) = post { it.seekFor(it.status.generation, token, positionMs) }

    @JvmStatic
    fun pauseFor(token: Long) = explicit { it.pauseFor(it.status.generation, token) }

    @JvmStatic
    fun resumeFor(token: Long) = explicit { it.resumeFor(it.status.generation, token) }

    /**
     * The player's status record ([RecordingPlayer.Status.record], 22 fields), which DecoderHostAndroid parses; the
     * idle record before the first play.
     */
    @JvmStatic
    fun status(): String = player?.status?.record(SystemClock.elapsedRealtime()) ?: idleRecord()

    /** The recordings found gone or changed since the last take, oldest first; each is handed over once. */
    @JvmStatic
    fun takeRecheck(): Array<String> = recheck.take()

    /**
     * The media session of the History playlist that owns the promoted playback service, while it is active: the
     * Activity hands media keys past Qt to it (a MediaController on this token). Null before the service has rendered
     * the owner's session active, so a key in between stays with Qt. Any thread.
     */
    fun playlistSession(): MediaSession.Token? = activeSession.sessionFor(phases.currentOwner)

    /** The Activity started or stopped (not for a configuration change). */
    fun setForeground(foreground: Boolean) {
        synchronized(lock) {
            this.foreground = foreground
            handler?.post { onLooper { it.setForeground(foreground) } }
        }
    }

    /**
     * The app is ending: admission closes, so a later play is refused, and whatever plays stops (no audio, the focus
     * given back). A promoted playlist service is stopped here; a start issued and not promoted yet is left to its
     * delivery, which promotes the service and retires it, since stopping it before its startForeground() would make
     * ActivityManager crash the app.
     */
    fun release() {
        val promoted = phases.release { post { it.stop() } }
        val context = application ?: return
        if (promoted) {
            try {
                // An ActivityManager call, so it lands although the main thread is about to block in Qt's teardown.
                context.stopService(Intent(context, RecordingPlaybackService::class.java))
            } catch (e: RuntimeException) {
                Log.w(TAG, "could not stop the call recording playback service", e)
            }
        }
    }

    // ---- the service's side (RecordingPlaybackService, main thread unless noted)

    /** The looper's Handler, made if no play made it yet: the service's session callbacks run there. */
    internal fun playbackHandler(): Handler = synchronized(lock) { looperLocked() }

    /** Tasks on the playback looper (the session's tap timeout), cancelled by taking their message back. */
    internal fun playbackScheduler(): RecordingPlayer.Scheduler = LooperScheduler(playbackHandler())

    /** The service rendered playlist [generation] with its session [active] or not, or the session went ([ended]). */
    internal fun sessionRendered(generation: Int, active: Boolean, session: MediaSession.Token) =
        activeSession.rendered(generation, active, session)

    internal fun sessionEnded(generation: Int) = activeSession.ended(generation)

    internal fun sessionReleased() = activeSession.released()

    internal fun serviceCreated(instance: RecordingPlaybackService): Int {
        service = instance
        serviceTaps = instance.taps
        return phases.created()
    }

    /**
     * [instance] is destroyed: the playlist it served, if still open, ends (never a newer one). True when it was still
     * the instance alive, so the state instances share (the session handoff, the notification) is its to clear.
     */
    internal fun serviceDestroyed(instance: RecordingPlaybackService, serial: Int): Boolean {
        if (service === instance) {
            service = null
            serviceTaps = null
        }
        val destroyed = phases.destroyed(serial)
        if (destroyed.served != 0) {
            post { it.stop(destroyed.served) }
        }
        return destroyed.current
    }

    /** The owner's status, for the notification a start's delivery enters the foreground with; null: none owns it. */
    internal fun foregroundStatus(): RecordingPlayer.Status? {
        val owner = phases.foregroundOwner()
        return player?.status?.takeIf { owner != 0 && it.generation == owner }
    }

    /** A start reached [serial]'s instance, which entered the foreground or not ([foreground]): tell the player. */
    internal fun promote(serial: Int, token: Long, generation: Int, foreground: Boolean) {
        val promotion = phases.promote(serial, token, generation, foreground)
        if (promotion.promoted != 0) {
            post { it.onPromoted(promotion.promoted) }
        }
        if (promotion.refused != 0) {
            post { it.onPromotionRefused(promotion.refused) }
        }
    }

    /** A notification button for playlist [generation] from process [token]: applied only while that one owns it. */
    internal fun transport(token: Long, generation: Int, command: (RecordingPlayer, Int) -> Unit) {
        val target = phases.transport(token, generation)
        if (target != 0) {
            // Explicit (R67): that playlist's waiting headset tap is cancelled first, on the looper, as a session
            // callback's is (SessionKeys.direct()).
            post { if (phases.isOwner(target)) PlayPauseTaps.explicit(serviceTaps, target, it, command) }
        }
    }

    /** A session callback, on the looper, for the playlist its callback was installed for, while that one owns it. */
    internal fun sessionCommand(generation: Int, command: (RecordingPlayer, Int) -> Unit) {
        val machine = player ?: return
        if (phases.isOwner(generation)) {
            command(machine, generation)
            sync(machine)
        }
    }

    /** The task was removed: the playlist the service serves or waits for ends. */
    internal fun endCurrent() {
        val generation = phases.current()
        if (generation != 0) {
            post { it.stop(generation) }
        }
    }

    internal fun isOwner(generation: Int): Boolean = phases.isOwner(generation)

    /** The retire check of [serial]'s instance; [stop] runs under the phases' lock. */
    internal fun retire(serial: Int, stop: () -> Boolean): Boolean = phases.retire(serial, stop)

    // ---- inside

    /**
     * Accept a play and start what it needs. Plays arrive from one thread, the host's (Qt) thread, so no other play is
     * accepted between a playlist's ISSUE and its startForegroundService(); only the teardown can come in between,
     * which the phases order.
     */
    private fun begin(context: Context, mode: Int, play: (RecordingPlayer, Int) -> Unit): Int {
        val app = context.applicationContext
        application = app
        var issue = false
        val generation = phases.accept(mode == RecordingPlayer.MODE_PLAYLIST) { generation, start ->
            // Under the phases' lock, so a teardown is ordered after it; the machine exists before any promotion for
            // this play can be posted, and the play is posted before its start is issued.
            val (looper, machine) = ensure(app)
            looper.post {
                play(machine, generation)
                sync(machine)
            }
            when (start) {
                Start.HAND_OVER -> looper.post {
                    machine.onPromoted(generation)
                    sync(machine)
                }
                Start.ISSUE -> issue = true
                Start.JOIN, Start.NONE -> Unit
            }
        }
        if (issue && !issueStart(app, generation)) {
            for (refused in phases.startFailed(generation)) {
                post { it.onPromotionRefused(refused) }
            }
        }
        return generation
    }

    /** Start the playlist service for [generation] (PROMOTE); false when Android refused it as it was asked. */
    private fun issueStart(context: Context, generation: Int): Boolean = try {
        context.startForegroundService(RecordingPlaybackService.promoteIntent(context, generation)) != null
    } catch (e: RuntimeException) {
        // ForegroundServiceStartNotAllowedException (API 31+, the app is in the background) is an
        // IllegalStateException; a SecurityException or a vendor's own refusal is a start that did not happen too.
        Log.w(TAG, "could not start the call recording playback service", e)
        false
    }

    private fun ensure(context: Context): Pair<Handler, RecordingPlayer> = synchronized(lock) {
        val looper = looperLocked()
        val machine = player ?: RecordingPlayer(
            { token -> MediaItem(token) },
            Opener,
            AudioFocus(context, looper),
            RecordingPlayer.Clock(SystemClock::elapsedRealtime),
            LooperScheduler(looper),
            ServiceSide,
            recheck,
            foreground,
            abandonedSetups,
        ).also { player = it }
        looper to machine
    }

    private fun looperLocked(): Handler =
        handler ?: Handler(HandlerThread("dsd-neo-playback").apply { start() }.looper).also { handler = it }

    /** Run [action] on the looper with the player; nothing to do before the first play. */
    private fun post(action: (RecordingPlayer) -> Unit) {
        synchronized(lock) { handler }?.post { onLooper(action) }
    }

    /** On the looper: [action] on the player, then the wake lock and the noisy receiver follow what it does. */
    private fun onLooper(action: (RecordingPlayer) -> Unit) {
        val machine = player ?: return
        action(machine)
        sync(machine)
    }

    /**
     * Looper only: a play open listens for headphones unplugged; a playlist open, for the screen turning on (its paused
     * deadline, #909); a playlist that wants audio keeps the CPU awake.
     */
    private fun sync(machine: RecordingPlayer) {
        val status = machine.status
        val open = status.allows(RecordingPlayer.ACTION_STOP)
        val playlist = open && status.mode == RecordingPlayer.MODE_PLAYLIST
        noisy.follow(open, status.generation)
        screenOn.follow(playlist, status.generation)
        keepAwake(playlist && status.allows(RecordingPlayer.ACTION_PAUSE))
    }

    /** A receiver of one system broadcast, registered on the looper while [follow] is told to listen. Looper only. */
    private class Listening(
        private val receiver: BroadcastReceiver,
        private val action: String,
        private val name: String,
        private val without: String,
    ) {
        private var registered = false

        /** The play whose registration was refused: not tried again (nor logged again) until the next. */
        private var refusedFor = 0

        fun follow(listen: Boolean, generation: Int) {
            val context = application ?: return
            if (listen == registered || (listen && generation == refusedFor)) {
                return
            }
            try {
                if (listen) {
                    // A protected broadcast only the system sends: registered with no export flag (Android 14's
                    // exception), delivered on the looper.
                    context.registerReceiver(receiver, IntentFilter(action), null, synchronized(lock) { handler })
                } else {
                    context.unregisterReceiver(receiver)
                }
                registered = listen
            } catch (e: RuntimeException) {
                if (listen) {
                    refusedFor = generation
                    Log.w(TAG, "could not register $name; $without", e)
                } else {
                    registered = false // gone anyway: never try to unregister it again
                    Log.w(TAG, "could not unregister $name", e)
                }
            }
        }
    }

    private fun keepAwake(awake: Boolean) {
        val held = wakeLock
        if (awake && held == null) {
            val power = application?.getSystemService(PowerManager::class.java) ?: return
            wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "dsd-neo:playback").apply {
                setReferenceCounted(false)
                acquire()
            }
        } else if (!awake && held != null) {
            wakeLock = null
            if (held.isHeld) {
                held.release()
            }
        }
    }

    /** The record [status] gives before the first play, as after a host stop. */
    private fun idleRecord(): String =
        listOf(0, 0, 0, 0, "", -1, 0, -1, 0, 0, 0, 0, 0, 0, "", "", 0, recheck.serial, 0, -1, 0, 0).joinToString("\t")

    /** Once per state change, coalesced: the service shows the player's current status on the main thread. */
    private fun requestRender() {
        if (renderPosted.compareAndSet(false, true)) {
            main.post {
                renderPosted.set(false)
                val status = player?.status ?: return@post
                service?.render(status)
            }
        }
    }

    /** The player's link to the playlist's service, called on the looper. */
    private object ServiceSide : RecordingPlayer.ServiceLink {
        override fun changed(status: RecordingPlayer.Status) = requestRender()

        // On the looper, under the phases' lock, before the end is published: no main-thread hop (R53).
        override fun startAbandoned(generation: Int) = phases.abandonStart(generation)

        override fun ended(generation: Int, reason: String) {
            main.post {
                phases.ended(generation)
                service?.ended(generation)
                service?.retireCheck()
            }
        }
    }

    private object Noisy : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == AudioManager.ACTION_AUDIO_BECOMING_NOISY) {
                // The instance's taps, read here on the main thread; its waiting tap goes before the pause (R54).
                val taps = service?.taps
                onLooper { PlayPauseTaps.disconnected(taps, it) }
            }
        }
    }

    /** The screen turned on: a paused playlist whose ten minutes ran out in deep sleep ends now (on the looper). */
    private object ScreenOn : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == Intent.ACTION_SCREEN_ON) {
                onLooper { it.onAwake() }
            }
        }
    }

    /** A recording opened for the player: the descriptor MediaPlayer copies. */
    private interface DescriptorSource : RecordingPlayer.Source {
        val fd: FileDescriptor
    }

    /** A recording opened by path, unchecked (a single play). */
    private class StreamSource(private val stream: FileInputStream) : DescriptorSource {
        override val fd: FileDescriptor get() = stream.fd

        override fun close() = stream.close()
    }

    /** A recording opened and checked against the identity the store listed (Play all parts, a playlist). */
    private class CheckedSource(override val fd: FileDescriptor) : DescriptorSource {
        override fun close() = RecordingDescriptor.close(fd)
    }

    /** Opens a part's file on its player's own thread ([MediaItem]), inside its setup. */
    private object Opener : RecordingPlayer.Opener {
        override fun open(item: RecordingPlayer.Item, checked: Boolean): RecordingPlayer.OpenResult {
            if (!checked) {
                return openUnchecked(item.path) // SINGLE, as #708 opened it
            }
            return when (
                val opened = RecordingDescriptor.open(item.path, item.size, item.mtimeSec, item.mtimeNanos.toLong())
            ) {
                is RecordingDescriptor.Result.Opened -> RecordingPlayer.OpenResult.Opened(CheckedSource(opened.fd))
                RecordingDescriptor.Result.Gone -> RecordingPlayer.OpenResult.Refused(RecordingPlayer.REASON_GONE)
                RecordingDescriptor.Result.Changed -> RecordingPlayer.OpenResult.Refused(RecordingPlayer.REASON_CHANGED)
            }
        }

        private fun openUnchecked(path: String): RecordingPlayer.OpenResult = try {
            // A descriptor rather than the path: the file opened is the one that plays, and a gone one says so here.
            RecordingPlayer.OpenResult.Opened(StreamSource(FileInputStream(path)))
        } catch (_: FileNotFoundException) {
            // Also thrown for a file there that cannot be opened (no access, a folder): only one gone is missing.
            val reason = if (File(path).exists()) RecordingPlayer.REASON_CHANGED else RecordingPlayer.REASON_GONE
            RecordingPlayer.OpenResult.Refused(reason)
        }
    }

    /** Tasks on the looper, cancelled by taking their message back. */
    private class LooperScheduler(private val looper: Handler) : RecordingPlayer.Scheduler {
        override fun post(delayMs: Long, task: () -> Unit): RecordingPlayer.Cancellable {
            val message = Runnable { onLooper { task() } }
            looper.postDelayed(message, delayMs)
            return RecordingPlayer.Cancellable { looper.removeCallbacks(message) }
        }
    }

    /**
     * The Handler of a player's owner [thread], just started. One that cannot be made (an Error, memory tight after the
     * thread started) quits the thread before the failure goes on to the factory's guard, so no owner thread is left
     * looping on an empty queue with nothing to quit it.
     */
    private fun handlerOf(thread: HandlerThread): Handler = try {
        Handler(thread.looper)
    } catch (failure: Throwable) {
        thread.quit()
        throw failure
    }

    /**
     * One recording's MediaPlayer on a thread of its own, its owner (#909). "MediaPlayer is not thread-safe. Creation
     * of and all access to player instances should be on the same thread. If registering callbacks, the thread must
     * have a Looper." So the owner does everything for this player: it runs the setup (the file opened and checked,
     * then [create] and [prepare]), and every later call is posted to it, in order. The player's callbacks arrive on
     * the owner's Looper and are forwarded to the playback looper with the part's token; the duration is read there in
     * onPrepared, before the forward. The playback looper never touches the MediaPlayer. [release] posts the release,
     * and the thread ends from inside it, once the MediaPlayer is released: a setup stuck in a read releases and ends
     * when the read returns. A start runs on the owner too: it is made only while its request is still the start
     * wanted ([wantedStart], taken back at once on the looper by a pause or the release), and acknowledged to the
     * looper ([RecordingPlayer.onStarted]), which plays from then, never from the request. Anything a start, a pause
     * or a seek throws there is the player's failure ([RecordingPlayer.REASON_PLAYER]); one the release throws is
     * logged and the thread still ends. A media server that died is the player's fault and ends the play; any other
     * error is the file's.
     */
    private class MediaItem(private val token: Long) : RecordingPlayer.Player {
        private val thread = HandlerThread("dsd-neo-playback-player").apply { start() }
        private val owner = handlerOf(thread)

        /** The owner's only. */
        private var media: MediaPlayer? = null

        /** Written on the owner in onPrepared, before its forward; read on the looper after it. */
        @Volatile
        private var duration = 0L

        /** The start the looper wants made (its request serial), 0 for none: set and taken back on the looper. */
        @Volatile
        private var wantedStart = 0L

        /**
         * The owner's only: its seeks (one platform seek in flight, the newest held, R59) and its silence (only once
         * a pause has returned, or the release has, R62).
         */
        private val state = PlayerOwnerState(
            object : PlayerOwnerState.Platform {
                override fun seekTo(positionMs: Long) {
                    media?.seekTo(positionMs, MediaPlayer.SEEK_CLOSEST)
                }

                override fun pause() {
                    media?.pause()
                }
            },
            { serial -> post { machine -> machine.onSeekCompleted(token, serial) } },
            { post { it.onSilenced(token) } },
        ) { delayMs, task ->
            // A seek's 2 s bound (M1), on the owner. What it then passes on (the held seek, a start that waited) may
            // throw: the player's failure, never the app's.
            val run = Runnable { guarded(task) }
            owner.postDelayed(run, delayMs)
            val cancel: () -> Unit = { owner.removeCallbacks(run) }
            cancel
        }

        override fun setup(
            work: () -> RecordingPlayer.SetupResult,
            deliver: (RecordingPlayer.SetupResult) -> Unit,
        ) {
            // The work never throws (the machine turns anything thrown into a failed setup), so the owner always posts.
            val handed = owner.post {
                val result = work()
                post { deliver(result) }
            }
            if (!handed) {
                Log.w(TAG, "could not hand a call recording to its player's thread")
                post { deliver(RecordingPlayer.SetupResult(RecordingPlayer.REASON_PLAYER)) }
            }
        }

        override fun create() {
            val made = MediaPlayer()
            media = made // Released with the item, also when what follows throws.
            made.setAudioAttributes(attributes)
            made.setOnPreparedListener { prepared ->
                // On the owner, outside every other guard: a platform throw here (reading the length of a player that
                // went wrong) fails the part as the player's, never the app (R47).
                val length = try {
                    prepared.duration.toLong()
                } catch (e: Throwable) {
                    Log.w(TAG, "could not read a call recording's length", e)
                    null
                }
                if (length == null) {
                    post { it.onFailed(token, RecordingPlayer.REASON_PLAYER) }
                } else {
                    duration = length
                    post { it.onPrepared(token) }
                }
            }
            // Marked on the owner first (R66): a start queued behind the completion never plays the recording again.
            made.setOnCompletionListener {
                state.onCompleted()
                post { it.onCompleted(token) }
            }
            // On the owner: the seek in flight landed, and names its own request (R59). The held seek it then passes on
            // may throw, outside onOwner's guard: the player's failure, never the app's.
            made.setOnSeekCompleteListener { guarded { state.seekCompleted() } }
            made.setOnErrorListener { _, what, _ ->
                val reason = if (what == MediaPlayer.MEDIA_ERROR_SERVER_DIED) {
                    RecordingPlayer.REASON_PLAYER
                } else {
                    RecordingPlayer.REASON_DAMAGED
                }
                post { it.onFailed(token, reason) }
                true
            }
        }

        override fun prepare(source: RecordingPlayer.Source) {
            val made = checkNotNull(media) { "no MediaPlayer to prepare" }
            made.setDataSource((source as DescriptorSource).fd)
            made.prepareAsync()
        }

        override fun start(serial: Long) {
            wantedStart = serial
            onOwner { made ->
                // After a held seek has gone to the platform (M2); checked right before the start: a pause, a release
                // or a move since takes it back.
                state.start {
                    if (wantedStart == serial) {
                        made.start()
                        state.onStarted()
                        post { machine -> machine.onStarted(token, serial) }
                    }
                }
            }
        }

        override fun pause() {
            wantedStart = 0L
            onOwner { state.pause() }
        }

        override fun seekTo(positionMs: Long, serial: Long) = onOwner { state.seek(positionMs, serial) }

        override fun silence() {
            // Behind the move's pause on the owner, ahead of the release posted after it: silent at once when that
            // pause returned (or nothing had started), else once the release has returned (R62).
            val posted = owner.post { state.silence() }
            if (!posted) {
                post { it.onSilenced(token) } // its thread had ended: released, so silent
            }
        }

        override val durationMs: Long
            get() = duration

        override fun release(ended: () -> Unit) {
            wantedStart = 0L
            val posted = owner.post {
                val returned = try {
                    media?.release()
                    true
                } catch (e: Throwable) {
                    Log.w(TAG, "could not release a call recording's player", e)
                    false
                }
                media = null
                state.released(returned)
                // Only now: until release() returns, the native player may still post an event (one a pause just
                // before it raised), and a Looper that has quit refuses it ("sending message to a Handler on a dead
                // thread"). Release clears the native listener, so no new event is posted after it (a callback already
                // running can still be refused, harmlessly); an event already queued is for a released player, and
                // quit() drops it unread. Nothing of ours is posted after this.
                thread.quit()
                // The thread ends with this task: the looper stops counting it (a release stuck in native code on a
                // stalled read keeps it counted until it returns).
                post { ended() }
            }
            if (!posted) {
                post { ended() } // its thread had ended already
            }
        }

        /** [task] on the owner: anything it throws is the player's failure, never a crashed thread. */
        private fun guarded(task: () -> Unit) {
            try {
                task()
            } catch (e: Throwable) {
                Log.w(TAG, "a call recording's player refused a command", e)
                post { it.onFailed(token, RecordingPlayer.REASON_PLAYER) }
            }
        }

        /**
         * [call] on the owner, after everything posted before it; nothing once the player is released. Anything it
         * throws is the player's failure, reported to the looper under this part's token, never a crashed thread.
         */
        private fun onOwner(call: (MediaPlayer) -> Unit) {
            owner.post {
                val made = media ?: return@post
                try {
                    call(made)
                } catch (e: Throwable) {
                    Log.w(TAG, "a call recording's player refused a command", e)
                    post { it.onFailed(token, RecordingPlayer.REASON_PLAYER) }
                }
            }
        }
    }

    /**
     * The audio focus, one request per ask (lasting for a playlist, transient for a sheet's play), its changes
     * delivered on the looper with the epoch of the request they answer. Speech pauses when it would be ducked. Only a
     * granted request is kept; a refused one is abandoned at once ([FocusRequests]).
     */
    private class AudioFocus(context: Context, private val looper: Handler) : RecordingPlayer.Focus {
        private val audio = context.getSystemService(AudioManager::class.java)
        private val requests = FocusRequests<AudioFocusRequest> { audio?.abandonAudioFocusRequest(it) }

        override fun request(mode: RecordingPlayer.FocusMode, epoch: Int): Boolean {
            val gain = when (mode) {
                RecordingPlayer.FocusMode.GAIN -> AudioManager.AUDIOFOCUS_GAIN
                RecordingPlayer.FocusMode.GAIN_TRANSIENT -> AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
            }
            val made = AudioFocusRequest.Builder(gain)
                .setAudioAttributes(attributes)
                .setWillPauseWhenDucked(true)
                .setOnAudioFocusChangeListener({ change -> onLooper { it.onFocusChange(epoch, kind(change)) } }, looper)
                .build()
            return requests.request(made) { audio?.requestAudioFocus(it) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED }
        }

        override fun abandon() = requests.abandon()

        private fun kind(change: Int): RecordingPlayer.FocusChange = when (change) {
            AudioManager.AUDIOFOCUS_LOSS -> RecordingPlayer.FocusChange.LOST
            // Speech is not ducked under other audio: a recording pauses for a navigation prompt too.
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT, AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK ->
                RecordingPlayer.FocusChange.LOST_TRANSIENT
            else -> RecordingPlayer.FocusChange.GAINED
        }
    }
}
