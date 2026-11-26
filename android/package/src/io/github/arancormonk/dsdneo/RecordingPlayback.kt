// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaPlayer
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import java.io.File
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.util.concurrent.atomic.AtomicInteger

/**
 * Call recording playback (#708, D6/D7/D10): the Android half that DecoderHostAndroid calls over JNI.
 *
 * One looper thread owns a [RecordingPlayer] and the MediaPlayer it drives, so the player's calls and callbacks never
 * race. The Qt thread's calls only post there and return; the host polls [status], which reads the state the looper
 * published. The audio focus is transient and speech: other media pauses while a recording plays, and the decoder's
 * live audio, which asks for no focus, plays on beside it (D7). DsdNeoActivity reports the foreground, whose loss
 * pauses (D10), and releases the player when the app ends.
 */
object RecordingPlayback {
    private val generations = AtomicInteger()
    private val lock = Any()
    private var handler: Handler? = null

    /** Whether the Activity is started; read when the player is made, then posted to it. */
    @Volatile
    private var foreground = true

    /** Made on the looper by the first play, then used only there. */
    @Volatile
    private var player: RecordingPlayer? = null

    /** The recordings found gone or changed at their turn, for the store to look at again (#909); for the process. */
    private val recheck = RecordingRecheck()

    private val attributes: AudioAttributes by lazy {
        AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
            .build()
    }

    /** Play [path], replacing what plays: the play's generation, which [status] names. */
    @JvmStatic
    fun play(context: Context, path: String): Int {
        val application = context.applicationContext
        val generation = generations.incrementAndGet()
        val looper = synchronized(lock) {
            handler ?: Handler(HandlerThread("dsd-neo-playback").apply { start() }.looper).also { handler = it }
        }
        looper.post {
            val machine = player ?: RecordingPlayer(
                ::MediaItem,
                FileOpener,
                AudioFocus(application, looper),
                RecordingPlayer.Clock(SystemClock::elapsedRealtime),
                LooperScheduler(looper),
                NoService,
                recheck,
                foreground,
            ).also { player = it }
            machine.play(generation, path)
        }
        return generation
    }

    @JvmStatic
    fun pause() = post { it.pause() }

    @JvmStatic
    fun resume() = post { it.resume() }

    @JvmStatic
    fun seek(positionMs: Long) = post { it.seek(positionMs) }

    @JvmStatic
    fun stop() = post { it.stop() }

    /**
     * The first five fields of the player's status record ([RecordingPlayer.Status.record]: generation, state,
     * position, duration, error), which DecoderHostAndroid parses; empty before the first play.
     */
    @JvmStatic
    fun status(): String =
        player?.status?.record(SystemClock.elapsedRealtime())?.split('\t')?.take(5)?.joinToString("\t") ?: ""

    /** The Activity started or stopped (not for a configuration change). */
    fun setForeground(foreground: Boolean) {
        synchronized(lock) {
            this.foreground = foreground
            handler?.post { player?.setForeground(foreground) }
        }
    }

    /** The app is ending: let the player and the focus go. */
    fun release() = post { it.stop() }

    /** Run [action] on the looper with the player; nothing to do before the first play. */
    private fun post(action: (RecordingPlayer) -> Unit) {
        synchronized(lock) { handler }?.post { player?.let(action) }
    }

    /** A recording opened by path, unchecked (a single play): its descriptor, which MediaPlayer copies. */
    private class StreamSource(val stream: FileInputStream) : RecordingPlayer.Source {
        override fun close() = stream.close()
    }

    private object FileOpener : RecordingPlayer.Opener {
        override fun open(item: RecordingPlayer.Item): RecordingPlayer.OpenResult = try {
            // A descriptor rather than the path: the file opened is the one that plays, and a gone one says so here.
            RecordingPlayer.OpenResult.Opened(StreamSource(FileInputStream(item.path)))
        } catch (_: FileNotFoundException) {
            // Also thrown for a file there that cannot be opened (no access, a folder): only one gone is missing.
            val reason = if (File(item.path).exists()) RecordingPlayer.REASON_CHANGED else RecordingPlayer.REASON_GONE
            RecordingPlayer.OpenResult.Refused(reason)
        }
    }

    /** Tasks on the looper, cancelled by taking their message back. */
    private class LooperScheduler(private val looper: Handler) : RecordingPlayer.Scheduler {
        override fun post(delayMs: Long, task: () -> Unit): RecordingPlayer.Cancellable {
            val message = Runnable(task)
            looper.postDelayed(message, delayMs)
            return RecordingPlayer.Cancellable { looper.removeCallbacks(message) }
        }
    }

    /** Only single plays start here, which no service follows. */
    private object NoService : RecordingPlayer.ServiceLink {
        override fun changed(status: RecordingPlayer.Status) = Unit

        override fun ended(generation: Int, reason: String) = Unit
    }

    /** One recording in a MediaPlayer, made and called on the looper, which its callbacks also arrive on. */
    private class MediaItem(private val token: Long) : RecordingPlayer.Player {
        private val media = MediaPlayer()

        override fun prepare(source: RecordingPlayer.Source) {
            media.setAudioAttributes(attributes)
            media.setDataSource((source as StreamSource).stream.fd)
            media.setOnPreparedListener { player?.onPrepared(token) }
            media.setOnCompletionListener { player?.onCompleted(token) }
            media.setOnErrorListener { _, _, _ ->
                player?.onFailed(token, RecordingPlayer.REASON_DAMAGED)
                true
            }
            media.prepareAsync()
        }

        override fun start() = media.start()

        override fun pause() = media.pause()

        override fun seekTo(positionMs: Long) = media.seekTo(positionMs, MediaPlayer.SEEK_CLOSEST)

        override val durationMs: Long
            get() = media.duration.toLong()

        override fun release() = media.release()
    }

    /** The audio focus, its changes delivered on the looper with the epoch of the request they answer. */
    private class AudioFocus(context: Context, private val looper: Handler) : RecordingPlayer.Focus {
        private val audio = context.getSystemService(AudioManager::class.java)
        private var request: AudioFocusRequest? = null

        override fun request(mode: RecordingPlayer.FocusMode, epoch: Int): Boolean {
            val gain = when (mode) {
                RecordingPlayer.FocusMode.GAIN -> AudioManager.AUDIOFOCUS_GAIN
                RecordingPlayer.FocusMode.GAIN_TRANSIENT -> AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
            }
            val made = AudioFocusRequest.Builder(gain)
                .setAudioAttributes(attributes)
                .setOnAudioFocusChangeListener({ change -> player?.onFocusChange(epoch, kind(change)) }, looper)
                .build()
            request = made
            return audio?.requestAudioFocus(made) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        }

        override fun abandon() {
            request?.let { audio?.abandonAudioFocusRequest(it) }
            request = null
        }

        private fun kind(change: Int): RecordingPlayer.FocusChange = when (change) {
            AudioManager.AUDIOFOCUS_LOSS -> RecordingPlayer.FocusChange.LOST
            // Speech is not ducked under other audio: a recording pauses for a navigation prompt too.
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT, AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK ->
                RecordingPlayer.FocusChange.LOST_TRANSIENT
            else -> RecordingPlayer.FocusChange.GAINED
        }
    }
}
