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
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Call recording playback (#708, D6/D7/D10; #909): the Android half that DecoderHostAndroid calls over JNI.
 *
 * One looper thread owns a [RecordingPlayer] and the MediaPlayer it drives, so the player's calls and callbacks never
 * race. The Qt thread's calls only post there and return; the host polls [status], which reads the state the looper
 * published. Blocking file I/O never runs on the looper: each part's MediaPlayer is made there (its callbacks come
 * there) and handed to one setup thread, which opens the file, sets it as the data source, starts the prepare and posts
 * the result back with the part's token. The audio focus is speech, transient for a sheet's play and lasting for a
 * History playlist: other media
 * pauses while a recording plays, and the decoder's live audio, which asks for no focus, plays on beside it (D7).
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

    private val main: Handler by lazy { Handler(Looper.getMainLooper()) }
    private val renderPosted = AtomicBoolean()

    /** The playback service instance alive; on the main thread only. */
    private var service: RecordingPlaybackService? = null

    /** The playlist whose media session the service last rendered active (#909): the Activity's media key handoff. */
    private val activeSession = ActiveSession()

    /**
     * The setup thread, beside the looper and as long-lived: each part's blocking open, data source and prepare start
     * ([RecordingPlayer.Opener.setup]), one after another.
     */
    private val setupThread: ExecutorService by lazy {
        Executors.newSingleThreadExecutor { work -> Thread(work, "dsd-neo-playback-setup").apply { isDaemon = true } }
    }

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

    /** Play [path] alone, replacing what plays (SINGLE): the play's generation, which [status] names; 0 refused. */
    @JvmStatic
    fun play(context: Context, path: String): Int =
        begin(context, RecordingPlayer.MODE_SINGLE) { machine, generation -> machine.play(generation, path) }

    /**
     * Play the items the arrays list, one entry per recording, from [start] in [mode] (RecordingPlayer.MODE_*),
     * replacing what plays: the play's positive generation once accepted, also when its service could not be started
     * (that generation then reads FAILED, ended "start"). 0 only for a refusal before acceptance: arrays of different
     * lengths, no items, [start] or [mode] out of range, or the app ending. The machine checks the items themselves.
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
            mode !in RecordingPlayer.MODE_SINGLE..RecordingPlayer.MODE_PLAYLIST
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
        return begin(context, mode) { machine, generation -> machine.play(generation, mode, items, start) }
    }

    /** Queue-wide commands: the current play, whatever its item. */
    @JvmStatic
    fun next() = post { it.next() }

    @JvmStatic
    fun previous() = post { it.previous() }

    @JvmStatic
    fun pause() = post { it.pause() }

    @JvmStatic
    fun resume() = post { it.resume() }

    /** The host's own stop: whatever plays stops, and the next status has no generation. */
    @JvmStatic
    fun stop() = post { it.stop() }

    /** Stop play [generation] only (the bar's Stop names its playlist); it keeps its terminal reading. */
    @JvmStatic
    fun stopGeneration(generation: Int) = post { it.stop(generation) }

    /** Item commands: dropped on the looper unless the item whose play [token] the host captured is still current. */
    @JvmStatic
    fun seekFor(token: Long, positionMs: Long) = post { it.seekFor(it.status.generation, token, positionMs) }

    @JvmStatic
    fun pauseFor(token: Long) = post { it.pauseFor(it.status.generation, token) }

    @JvmStatic
    fun resumeFor(token: Long) = post { it.resumeFor(it.status.generation, token) }

    /**
     * The player's status record ([RecordingPlayer.Status.record], 21 fields), which DecoderHostAndroid parses; the
     * idle record before the first play.
     */
    @JvmStatic
    fun status(): String = player?.status?.record(SystemClock.elapsedRealtime()) ?: idleRecord()

    /** The recordings found gone or changed since the last take, oldest first; each is handed over once. */
    @JvmStatic
    fun takeRecheck(): Array<String> = recheck.take()

    /**
     * Whether the media session is active for the History playlist that owns the promoted playback service: the media
     * keys the Activity then hands past Qt reach it. Not before the service has rendered the owner's session active,
     * so a key in between stays with Qt rather than going to whichever app's session Android finds. Any thread.
     */
    fun playlistSessionOpen(): Boolean = activeSession.activeFor(phases.currentOwner)

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
    internal fun sessionRendered(generation: Int, active: Boolean) = activeSession.rendered(generation, active)

    internal fun sessionEnded(generation: Int) = activeSession.ended(generation)

    internal fun sessionReleased() = activeSession.released()

    internal fun serviceCreated(instance: RecordingPlaybackService): Int {
        service = instance
        return phases.created()
    }

    /** [instance] is destroyed: the playlist it served, if still open, ends (never a newer one). */
    internal fun serviceDestroyed(instance: RecordingPlaybackService, serial: Int) {
        if (service === instance) {
            service = null
        }
        val served = phases.destroyed(serial)
        if (served != 0) {
            post { it.stop(served) }
        }
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
            post { if (phases.isOwner(target)) command(it, target) }
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
        listOf(0, 0, 0, 0, "", -1, 0, -1, 0, 0, 0, 0, 0, 0, "", "", 0, recheck.serial, 0, -1, 0).joinToString("\t")

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
                onLooper { it.onNoisy() }
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

    private object Opener : RecordingPlayer.Opener {
        override fun open(item: RecordingPlayer.Item): RecordingPlayer.OpenResult {
            if (item.size < 0) {
                return openUnchecked(item.path)
            }
            return when (
                val opened = RecordingDescriptor.open(item.path, item.size, item.mtimeSec, item.mtimeNanos.toLong())
            ) {
                is RecordingDescriptor.Result.Opened -> RecordingPlayer.OpenResult.Opened(CheckedSource(opened.fd))
                RecordingDescriptor.Result.Gone -> RecordingPlayer.OpenResult.Refused(RecordingPlayer.REASON_GONE)
                RecordingDescriptor.Result.Changed -> RecordingPlayer.OpenResult.Refused(RecordingPlayer.REASON_CHANGED)
            }
        }

        /** [work] on the setup thread, its result posted to the looper; a refused hand-off fails as the player. */
        override fun setup(
            work: () -> RecordingPlayer.SetupResult,
            deliver: (RecordingPlayer.SetupResult) -> Unit,
        ) {
            try {
                setupThread.execute {
                    val result = work()
                    post { deliver(result) }
                }
            } catch (e: RejectedExecutionException) {
                Log.w(TAG, "could not hand a call recording to its setup", e)
                post { deliver(RecordingPlayer.SetupResult(RecordingPlayer.REASON_PLAYER)) }
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
     * One recording in a MediaPlayer, made (inside the player's guard) and called on the looper, which its callbacks
     * also arrive on; only [prepare] runs on the setup thread, before the looper touches it. A media server that died
     * is the player's fault and ends the play; any other error is the file's.
     */
    private class MediaItem(private var token: Long) : RecordingPlayer.Player {
        private val media = MediaPlayer()

        override fun prepare(source: RecordingPlayer.Source) {
            media.setAudioAttributes(attributes)
            media.setDataSource((source as DescriptorSource).fd)
            media.setOnPreparedListener { onLooper { it.onPrepared(token) } }
            media.setOnCompletionListener { onLooper { it.onCompleted(token) } }
            media.setOnErrorListener { _, what, _ ->
                val reason = if (what == MediaPlayer.MEDIA_ERROR_SERVER_DIED) {
                    RecordingPlayer.REASON_PLAYER
                } else {
                    RecordingPlayer.REASON_DAMAGED
                }
                onLooper { it.onFailed(token, reason) }
                true
            }
            media.prepareAsync()
        }

        override fun start() = media.start()

        override fun pause() = media.pause()

        override fun seekTo(positionMs: Long) = media.seekTo(positionMs, MediaPlayer.SEEK_CLOSEST)

        override fun restart(token: Long) {
            media.seekTo(0, MediaPlayer.SEEK_CLOSEST)
            // Behind what the looper already holds: a completion queued before the restart keeps the old token.
            synchronized(lock) { handler }?.post { this.token = token }
        }

        override val durationMs: Long
            get() = media.duration.toLong()

        override fun release() = media.release()
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
