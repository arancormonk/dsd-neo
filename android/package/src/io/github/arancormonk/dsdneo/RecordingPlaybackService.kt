// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.app.ForegroundServiceStartNotAllowedException
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import android.view.ViewConfiguration
import androidx.core.content.IntentCompat

/**
 * The History playlist's foreground service (#909, D3/D10): it keeps a playlist playing with the app in the
 * background, with a media notification and one framework MediaSession for the lock screen, Bluetooth, the headset and
 * the car. The queue itself stays in RecordingPlayback's player; this service only shows it and forwards commands.
 *
 * Two kinds of delivery reach it. A start ([ACTION_PROMOTE], from startForegroundService) always enters the foreground
 * first, with the current owner's notification when the service serves a playlist (a stale start never overwrites the
 * owner's controls) and else its own "Opening…" one, then promotes the request if it is the outstanding start. A
 * transport delivery ([ACTION_TRANSPORT], the notification's buttons on API 29-32) names the playlist it was posted
 * for, and the looper applies it only while that playlist still owns the service. After every delivery, and after
 * every asynchronous end the player posts here, the instance asks whether it should retire (PlaybackServicePhases).
 *
 * The session is made in [onCreate], so the first startForeground() already carries its token: MediaStyle with a
 * session is what exempts the notification from the notification permission. Its callbacks run on the playback looper.
 * A callback is installed for each playlist the session shows ([SessionTarget], R36), never for each part, and acts
 * only on that playlist, so a command the framework had queued for an earlier playlist cannot reach a later one, while
 * keys queued behind a move (each move opens its part under a new token) all reach the queue. The headset's
 * play/pause taps are counted here ([PlayPauseTaps], for the playlist, across its parts), not by the framework; the
 * transport keys (Play, Pause, Next, Previous, Stop) are applied here too ([SessionKeys]), against the player's state
 * on the looper.
 */
class RecordingPlaybackService : Service() {
    /** This instance, as PlaybackServicePhases knows it. */
    private var serial = 0

    /** The newest start id this instance has seen: it stops against it, so a later start keeps it alive. */
    private var lastStartId = 0

    private lateinit var session: MediaSession

    /** The playlist the session's state was last published for, and its callback installed for. */
    private var published = SessionTarget.NONE

    /** The headset's play/pause taps, waiting for a double tap on the playback looper; for the instance's playlists. */
    internal val taps = PlayPauseTaps(
        RecordingPlayback.playbackScheduler(),
        ViewConfiguration.getDoubleTapTimeout().toLong(),
        { generation -> RecordingPlayback.sessionCommand(generation) { player, g -> PlayPauseTaps.toggle(player, g) } },
        { generation -> RecordingPlayback.sessionCommand(generation) { player, g -> PlayPauseTaps.next(player, g) } },
    )

    /** The media buttons the session takes itself: the taps, and the transport keys as queue commands. */
    private val keys = SessionKeys(taps) { generation, command ->
        RecordingPlayback.sessionCommand(generation, command)
    }

    /** The metadata last set on the session, so a state change with the same item sets none. */
    private var shownMetadata: List<Any>? = null

    /** The buttons' PendingIntents for [intentsFor], built once per playlist. */
    private val intents = HashMap<Int, PendingIntent>()
    private var intentsFor = 0

    /**
     * The notification's updates, at most one per [NotificationPace.INTERVAL_MS] with a trailing post of the latest
     * status, so Android's rate limit never sheds the last one; the session follows every render at once.
     */
    private val main = Handler(Looper.getMainLooper())
    private val pace = NotificationPace<RecordingPlayer.Status>(
        { delayMs, task ->
            val message = Runnable(task)
            main.postDelayed(message, delayMs)
            RecordingPlayer.Cancellable { main.removeCallbacks(message) }
        },
        { SystemClock.elapsedRealtime() },
        { status -> RecordingPlayback.isOwner(status.generation) },
        { status ->
            getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(status))
            true
        },
    )

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        ensureNotificationChannel()
        session = MediaSession(this, SESSION_TAG)
        session.setCallback(SessionCallbacks(published, keys), RecordingPlayback.playbackHandler())
        serial = RecordingPlayback.serviceCreated(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        lastStartId = startId
        val token = intent?.getLongExtra(EXTRA_PROCESS, 0L) ?: 0L
        val generation = intent?.getIntExtra(EXTRA_GENERATION, 0) ?: 0
        when (intent?.action) {
            ACTION_PROMOTE -> RecordingPlayback.promote(serial, token, generation, enterForeground())
            ACTION_TRANSPORT -> command(intent.getIntExtra(EXTRA_COMMAND, 0))?.let {
                RecordingPlayback.transport(token, generation, it)
            }
            else -> Log.w(TAG, "ignoring a playback intent with action ${intent?.action}")
        }
        retireCheck()
        // A playlist does not outlive its process: a new process has no queue to play.
        return START_NOT_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        // The app swiped from Recents ends the playlist; its end retires the service.
        RecordingPlayback.endCurrent()
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        // Settles only the playlist this instance served, if it is still open: never a newer one. A destroy reported
        // after the next instance was created leaves the state they share, the key handoff and the notification, to
        // that one (PlaybackServicePhases.Destroyed).
        val current = RecordingPlayback.serviceDestroyed(this, serial)
        pace.cancel()
        // Released with the playlist: a later media key no longer restarts it.
        session.release()
        if (current) {
            RecordingPlayback.sessionReleased()
        }
        // After the release, which removes the callback, so no tap can be counted after the drop.
        taps.drop()
        if (current) {
            getSystemService(NotificationManager::class.java).cancel(NOTIFICATION_ID)
        }
        super.onDestroy()
    }

    /**
     * The retire check: with nothing to serve and no start outstanding, stop against the newest start id and take
     * this instance's notification down. Runs under the phases' lock, so a play and a retire never interleave.
     */
    internal fun retireCheck() {
        RecordingPlayback.retire(serial) {
            val stopped = stopSelfResult(lastStartId)
            if (stopped) {
                pace.cancel()
                stopForeground(STOP_FOREGROUND_REMOVE)
                getSystemService(NotificationManager::class.java).cancel(NOTIFICATION_ID)
            }
            stopped
        }
    }

    /**
     * Playlist [generation] ended, before the retire check: the session it was last published for goes inactive, so it
     * no longer takes media keys while the instance waits to retire or for the next owner. Main thread.
     */
    internal fun ended(generation: Int) {
        taps.ended(generation)
        RecordingPlayback.sessionEnded(generation)
        if (generation != 0 && generation == published.generation && session.isActive) {
            session.isActive = false
        }
    }

    /** Show [status] on the session and the notification, if its playlist still owns the service. Main thread. */
    internal fun render(status: RecordingPlayer.Status) {
        if (!RecordingPlayback.isOwner(status.generation)) {
            return
        }
        // A tap waiting for its double tap is the playlist's: another playlist drops it, a part change keeps it.
        taps.follow(status.generation)
        if (!published.shows(status)) {
            // A callback of its own for a new playlist, never for a new part (R36): what the framework queued for the
            // old one (setCallback() drops it, or it runs on the old callback) still names the old playlist.
            published = SessionTarget(status.generation)
            session.setCallback(SessionCallbacks(published, keys), RecordingPlayback.playbackHandler())
        }
        val title = title(status)
        val subtitle = subtitle(status)
        val metadata = listOf(status.generation, status.itemIndex, title, subtitle, status.durationMs)
        if (metadata != shownMetadata) {
            shownMetadata = metadata
            session.setMetadata(
                MediaMetadata.Builder()
                    .putString(MediaMetadata.METADATA_KEY_TITLE, title)
                    .putString(MediaMetadata.METADATA_KEY_ARTIST, subtitle)
                    .putString(MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE, subtitle)
                    .putLong(MediaMetadata.METADATA_KEY_DURATION, status.durationMs)
                    .build(),
            )
        }
        session.setPlaybackState(playbackState(status))
        val open = status.allows(RecordingPlayer.ACTION_STOP)
        if (!session.isActive && open) {
            // A playlist open takes the instance (again): media keys and transport controls reach it.
            session.isActive = true
        }
        // The Activity hands media keys past Qt only from here on, for this playlist (RecordingPlayback).
        RecordingPlayback.sessionRendered(status.generation, open && session.isActive, session.sessionToken)
        pace.offer(status)
    }

    /** Enter the foreground, first and always on a start; false when Android refused it (API 31+). */
    private fun enterForeground(): Boolean = try {
        val shown = RecordingPlayback.foregroundStatus()
        startForeground(NOTIFICATION_ID, notification(shown), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        // The owner's status as it is now covers a trailing post still waiting; the "Opening…" one does not.
        pace.postedNow(covers = shown != null)
        true
    } catch (e: IllegalStateException) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S || e !is ForegroundServiceStartNotAllowedException) {
            throw e
        }
        Log.w(TAG, "the call recording playback service could not enter the foreground", e)
        false
    }

    /**
     * The playback state from the player's table: the session state, the position at its update time and the speed,
     * exactly the actions the table offers (seeking only when the part can seek), and a Stop custom action, which a
     * system player shows beside Previous and Next and which does not route to onStop().
     */
    private fun playbackState(status: RecordingPlayer.Status): PlaybackState {
        val now = SystemClock.elapsedRealtime()
        val state = when (status.sessionState) {
            RecordingPlayer.SESSION_PLAYING -> PlaybackState.STATE_PLAYING
            RecordingPlayer.SESSION_PAUSED -> PlaybackState.STATE_PAUSED
            RecordingPlayer.SESSION_STOPPED -> PlaybackState.STATE_STOPPED
            // A start asked for and not acknowledged: Android 13+ shows a loading spinner, with Pause (R49).
            RecordingPlayer.SESSION_BUFFERING -> PlaybackState.STATE_BUFFERING
            else -> PlaybackState.STATE_NONE
        }
        val builder = PlaybackState.Builder()
            .setState(state, status.positionMs(now), status.speed, now)
            .setActions(sessionActions(status))
        if (status.allows(RecordingPlayer.ACTION_STOP)) {
            builder.addCustomAction(
                PlaybackState.CustomAction.Builder(
                    CUSTOM_STOP,
                    getString(R.string.playback_stop),
                    R.drawable.ic_playback_stop,
                ).build(),
            )
        }
        return builder.build()
    }

    private fun sessionActions(status: RecordingPlayer.Status): Long {
        var actions = 0L
        if (status.allows(RecordingPlayer.ACTION_PLAY)) {
            actions = actions or PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PLAY_PAUSE
        }
        if (status.allows(RecordingPlayer.ACTION_PAUSE)) {
            actions = actions or PlaybackState.ACTION_PAUSE or PlaybackState.ACTION_PLAY_PAUSE
        }
        if (status.allows(RecordingPlayer.ACTION_NEXT)) {
            actions = actions or PlaybackState.ACTION_SKIP_TO_NEXT
        }
        if (status.allows(RecordingPlayer.ACTION_PREVIOUS)) {
            actions = actions or PlaybackState.ACTION_SKIP_TO_PREVIOUS
        }
        if (status.allows(RecordingPlayer.ACTION_SEEK)) {
            actions = actions or PlaybackState.ACTION_SEEK_TO
        }
        if (status.allows(RecordingPlayer.ACTION_STOP)) {
            actions = actions or PlaybackState.ACTION_STOP
        }
        return actions
    }

    /**
     * The MediaStyle notification for [status], or the instance's own "Opening…" one when it serves no playlist yet
     * (null). On API 29-32 its buttons are the controls (compact Previous, Play or Pause, Next, plus Stop); on 33+ the
     * system builds them from the session's state. Ongoing while playing; a swipe, where offered, stops.
     */
    private fun notification(status: RecordingPlayer.Status?): Notification {
        val style = Notification.MediaStyle().setMediaSession(session.sessionToken)
        val builder = Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_dsdneo)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_TRANSPORT)
        openPendingIntent?.let { builder.setContentIntent(it) }
        if (status == null) {
            return builder.setStyle(style)
                .setOngoing(true)
                .setContentTitle(getString(R.string.playback_title))
                .setContentText(getString(R.string.playback_opening))
                .build()
        }
        // The buttons in order, each added only when the table offers it; all but Stop go in the compact view.
        val compact = ArrayList<Int>()
        var added = 0
        fun button(icon: Int, label: Int, command: Int, inCompact: Boolean) {
            val action = Notification.Action.Builder(
                Icon.createWithResource(this, icon),
                getString(label),
                transportIntent(status.generation, command),
            ).build()
            builder.addAction(action)
            if (inCompact) {
                compact += added
            }
            added++
        }
        if (status.allows(RecordingPlayer.ACTION_PREVIOUS)) {
            button(R.drawable.ic_playback_previous, R.string.playback_previous, COMMAND_PREVIOUS, true)
        }
        if (status.allows(RecordingPlayer.ACTION_PAUSE)) {
            button(R.drawable.ic_playback_pause, R.string.playback_pause, COMMAND_PAUSE, true)
        } else if (status.allows(RecordingPlayer.ACTION_PLAY)) {
            button(R.drawable.ic_playback_play, R.string.playback_play, COMMAND_PLAY, true)
        }
        if (status.allows(RecordingPlayer.ACTION_NEXT)) {
            button(R.drawable.ic_playback_next, R.string.playback_next, COMMAND_NEXT, true)
        }
        if (status.allows(RecordingPlayer.ACTION_STOP)) {
            button(R.drawable.ic_playback_stop, R.string.playback_stop, COMMAND_STOP, false)
            builder.setDeleteIntent(transportIntent(status.generation, COMMAND_STOP))
        }
        style.setShowActionsInCompactView(*compact.toIntArray())
        return builder.setStyle(style)
            .setOngoing(
                status.sessionState == RecordingPlayer.SESSION_PLAYING ||
                    status.sessionState == RecordingPlayer.SESSION_BUFFERING,
            )
            .setContentTitle(title(status))
            .setContentText(subtitle(status))
            .build()
    }

    /** The call's name, as History shows it. */
    private fun title(status: RecordingPlayer.Status): String =
        status.item?.title?.takeIf { it.isNotBlank() } ?: getString(R.string.playback_title)

    /** When the call was, and where it is in the playlist: "2026-11-25 12:04 · Call 3 of 41 · part 2 of 3". */
    private fun subtitle(status: RecordingPlayer.Status): String {
        val parts = ArrayList<String>()
        status.item?.line?.takeIf { it.isNotBlank() }?.let { parts += it }
        if (status.calls > 0 && status.call >= 0) {
            parts += getString(R.string.playback_call_of, status.call + 1, status.calls)
        }
        if (status.partCount > 1) {
            parts += getString(R.string.playback_part_of, status.partOrdinal, status.partCount)
        }
        return parts.joinToString(getString(R.string.notif_separator))
    }

    /**
     * A button's PendingIntent for playlist [generation]: immutable, to this service, with the process token and the
     * generation in its extras and in its data URI, so an old one never matches (or acts on) a newer playlist's.
     */
    private fun transportIntent(generation: Int, command: Int): PendingIntent {
        if (intentsFor != generation) {
            intents.clear()
            intentsFor = generation
        }
        return intents.getOrPut(command) {
            val intent = Intent(this, RecordingPlaybackService::class.java)
                .setAction(ACTION_TRANSPORT)
                .setData(dataUri(generation, COMMAND_NAMES.getValue(command)))
                .putExtra(EXTRA_PROCESS, DecoderService.processToken)
                .putExtra(EXTRA_GENERATION, generation)
                .putExtra(EXTRA_COMMAND, command)
            PendingIntent.getService(this, (generation shl 3) or command, intent, PendingIntent.FLAG_IMMUTABLE)
        }
    }

    /** The tap target: the launcher's own intent, which resumes the existing task (as the decoder's notification). */
    private val openPendingIntent: PendingIntent? by lazy {
        packageManager.getLaunchIntentForPackage(packageName)?.let { launch ->
            PendingIntent.getActivity(
                this, REQUEST_OPEN, launch, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
        }
    }

    /** Idempotent by contract, so made unconditionally (see DecoderService.ensureNotificationChannel). */
    private fun ensureNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.playback_channel_name),
            NotificationManager.IMPORTANCE_LOW,
        )
        channel.enableVibration(false)
        channel.vibrationPattern = null
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    /**
     * The session's commands, on the playback looper, for the [target] this callback was installed for only. The
     * headset hook and play/pause keys are counted (single, double and long press as the framework counts them) and
     * the transport keys applied as queue commands by [keys]; every other key goes to the framework, after a waiting
     * tap counts as single.
     */
    private class SessionCallbacks(
        private val target: SessionTarget,
        private val keys: SessionKeys,
    ) : MediaSession.Callback() {
        override fun onMediaButtonEvent(mediaButtonIntent: Intent): Boolean {
            val key = if (mediaButtonIntent.action == Intent.ACTION_MEDIA_BUTTON) {
                IntentCompat.getParcelableExtra(mediaButtonIntent, Intent.EXTRA_KEY_EVENT, KeyEvent::class.java)
            } else {
                null
            }
            val taken = key != null &&
                keys.onKey(target.generation, key.keyCode, key.action == KeyEvent.ACTION_DOWN, key.repeatCount)
            return taken || super.onMediaButtonEvent(mediaButtonIntent)
        }

        override fun onPlay() = transport(target::play)

        override fun onPause() = transport(target::pause)

        override fun onSkipToNext() = transport(target::next)

        override fun onSkipToPrevious() = transport(target::previous)

        override fun onSeekTo(pos: Long) = command { target.seek(it, pos) }

        // The system player's dismissal (API 33+) arrives here.
        override fun onStop() = transport(target::stop)

        override fun onCustomAction(action: String, extras: Bundle?) {
            if (action == CUSTOM_STOP) {
                transport(target::stop)
            }
        }

        private fun command(action: (RecordingPlayer) -> Unit) =
            RecordingPlayback.sessionCommand(target.generation) { player, _ -> action(player) }

        /** A transport command: a waiting headset tap counts first, so this later command wins (R60). */
        private fun transport(action: (RecordingPlayer) -> Unit) =
            keys.direct(target.generation) { player, _ -> action(player) }
    }

    companion object {
        private const val TAG = "dsd-neo"
        private const val CHANNEL_ID = "dsdneo_playback"

        /** Its own id: the decoder's notification is 1. */
        private const val NOTIFICATION_ID = 2
        private const val SESSION_TAG = "dsd-neo call recordings"
        private const val REQUEST_OPEN = 0
        private const val SCHEME = "dsdneo-playback"
        private const val CUSTOM_STOP = "io.github.arancormonk.dsdneo.playback.STOP"

        private const val ACTION_PROMOTE = "io.github.arancormonk.dsdneo.action.PLAYBACK_PROMOTE"
        private const val ACTION_TRANSPORT = "io.github.arancormonk.dsdneo.action.PLAYBACK_TRANSPORT"
        private const val EXTRA_PROCESS = "io.github.arancormonk.dsdneo.extra.PLAYBACK_PROCESS"
        private const val EXTRA_GENERATION = "io.github.arancormonk.dsdneo.extra.PLAYBACK_GENERATION"
        private const val EXTRA_COMMAND = "io.github.arancormonk.dsdneo.extra.PLAYBACK_COMMAND"

        private const val COMMAND_PLAY = 1
        private const val COMMAND_PAUSE = 2
        private const val COMMAND_NEXT = 3
        private const val COMMAND_PREVIOUS = 4
        private const val COMMAND_STOP = 5
        private val COMMAND_NAMES = mapOf(
            COMMAND_PLAY to "play",
            COMMAND_PAUSE to "pause",
            COMMAND_NEXT to "next",
            COMMAND_PREVIOUS to "previous",
            COMMAND_STOP to "stop",
        )

        /** What a button's command does to its playlist, or null for an unknown one. */
        private fun command(code: Int): ((RecordingPlayer, Int) -> Unit)? = when (code) {
            COMMAND_PLAY -> { player, generation -> player.resume(generation) }
            COMMAND_PAUSE -> { player, generation -> player.pause(generation) }
            COMMAND_NEXT -> { player, generation -> player.next(generation) }
            COMMAND_PREVIOUS -> { player, generation -> player.previous(generation) }
            COMMAND_STOP -> { player, generation -> player.stop(generation) }
            else -> null
        }

        /** dsdneo-playback://<process token>/<generation>/<action>: part of each intent's identity. */
        private fun dataUri(generation: Int, action: String): Uri = Uri.Builder()
            .scheme(SCHEME)
            .authority(java.lang.Long.toHexString(DecoderService.processToken))
            .appendPath(generation.toString())
            .appendPath(action)
            .build()

        /** The start RecordingPlayback issues for playlist [generation]: startForegroundService() delivers it. */
        internal fun promoteIntent(context: Context, generation: Int): Intent =
            Intent(context, RecordingPlaybackService::class.java)
                .setAction(ACTION_PROMOTE)
                .setData(dataUri(generation, "promote"))
                .putExtra(EXTRA_PROCESS, DecoderService.processToken)
                .putExtra(EXTRA_GENERATION, generation)
    }
}
