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
import java.io.FileInputStream
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
                AudioFocus(application, looper),
                SystemClock::elapsedRealtime,
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

    /** The player's status record ([RecordingPlayer.Status.record]); empty before the first play. */
    @JvmStatic
    fun status(): String = player?.status?.record(SystemClock.elapsedRealtime()) ?: ""

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

    /** One recording in a MediaPlayer, made and called on the looper, which its callbacks also arrive on. */
    private class MediaItem(private val generation: Int) : RecordingPlayer.Player {
        private val media = MediaPlayer()

        override fun prepare(path: String) {
            media.setAudioAttributes(attributes)
            // A descriptor rather than the path: the file opened is the one that plays, and a gone one says so here.
            FileInputStream(path).use { media.setDataSource(it.fd) }
            media.setOnPreparedListener { player?.onPrepared(generation) }
            media.setOnCompletionListener { player?.onCompleted(generation) }
            media.setOnErrorListener { _, _, _ ->
                player?.onFailed(generation, RecordingPlayer.ERROR_FAILED)
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

    /** The transient audio focus, its changes delivered on the looper. */
    private class AudioFocus(context: Context, looper: Handler) : RecordingPlayer.Focus {
        private val audio = context.getSystemService(AudioManager::class.java)
        private val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
            .setAudioAttributes(attributes)
            .setOnAudioFocusChangeListener({ change -> player?.onFocusChange(kind(change)) }, looper)
            .build()

        override fun request(): Boolean =
            audio?.requestAudioFocus(request) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED

        override fun abandon() {
            audio?.abandonAudioFocusRequest(request)
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
