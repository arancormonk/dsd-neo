// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log

/**
 * Foreground service that owns the decoder.
 *
 * The engine lives here, not in the Qt UI: Android can destroy the Activity (and with
 * it Qt's `main()`) while this process keeps decoding, and a relaunched Activity
 * re-attaches to the running engine.
 *
 * One engine per process, so the state machine rejects a start unless it is IDLE.
 */
class DecoderService : Service() {

    private var engineThread: Thread? = null
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        ensureNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Unconditional, and before anything that can return early. startDecoder()
        // uses startForegroundService(), which obliges this service to call
        // startForeground() for that start — including the ones we go on to reject
        // (a duplicate START while already RUNNING) and the ones we do not
        // recognise. Skipping it there risks ForegroundServiceDidNotStartInTime.
        startForegroundNotification(currentStatusText())

        when (intent?.action) {
            ACTION_START -> {
                val args = intent.getStringArrayExtra(EXTRA_ARGS) ?: emptyArray()
                startDecoding(args)
            }
            ACTION_STOP -> {
                stopDecoding()
                // A stop aimed at a service that is not running still promoted us
                // above; drop back out of the foreground rather than sitting there
                // with no decoder behind the notification.
                stopIfIdle()
            }
            else -> {
                Log.w(TAG, "ignoring intent with action ${intent?.action}")
                stopIfIdle()
            }
        }
        // A dead process stays dead until the user relaunches: restarting the engine
        // without its configuration would be worse than not restarting at all.
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopDecoding()
        joinEngineThread()
        if (initialized) {
            // Only a successful teardown frees the native side. nativeDestroy refuses
            // while the engine is still running, and clearing the flag anyway would
            // wedge the service: the next start calls nativeInit, which refuses in
            // turn because the old objects are still alive.
            val rc = DsdNative.nativeDestroy()
            if (rc == DsdNative.STATUS_OK) {
                initialized = false
            } else {
                Log.e(TAG, "nativeDestroy rejected ($rc); leaving the engine initialized")
            }
        }
        // release() waits for the engine to let go of the descriptor on its own. The
        // connection is held across runs so a second start does not re-prompt for
        // permission.
        UsbSourceManager.release()
        releaseWakeLock()
        super.onDestroy()
    }

    private fun startDecoding(args: Array<String>) {
        synchronized(lock) {
            if (state != State.IDLE) {
                Log.w(TAG, "start rejected in state $state")
                return
            }
            state = State.STARTING
            lastError = ""
        }

        updateNotification(getString(R.string.decoder_starting))
        acquireWakeLock()

        if (!initialized) {
            val rc = DsdNative.nativeInit(filesDir.absolutePath, cacheDir.absolutePath)
            if (rc != DsdNative.STATUS_OK) {
                failStart("nativeInit failed ($rc)", getString(R.string.error_init_failed, rc))
                return
            }
            initialized = true
        }

        val configureRc = DsdNative.nativeConfigure(args)
        if (configureRc != DsdNative.STATUS_OK) {
            failStart(
                "nativeConfigure failed ($configureRc)",
                getString(R.string.error_configure_failed, configureRc)
            )
            return
        }

        val thread = Thread({
            val rc = DsdNative.nativeRun()
            Log.i(TAG, "engine run returned $rc")
            synchronized(lock) { state = State.IDLE }
            stopSelf()
        }, "dsd-neo-engine")
        engineThread = thread
        synchronized(lock) { state = State.RUNNING }
        updateNotification(getString(R.string.decoder_running))
        thread.start()
    }

    /**
     * Abandons a start. [reason] goes to logcat; [userMessage] is what the UI shows,
     * because dropping back to IDLE with nothing on screen reads as "nothing happened".
     */
    private fun failStart(reason: String, userMessage: String) {
        Log.e(TAG, reason)
        synchronized(lock) {
            state = State.IDLE
            lastError = userMessage
        }
        releaseWakeLock()
        stopForegroundCompat()
        stopSelf()
    }

    private fun stopDecoding() {
        val shouldStop = synchronized(lock) {
            if (state != State.RUNNING) {
                false
            } else {
                state = State.STOPPING
                true
            }
        }
        if (!shouldStop) {
            return
        }
        updateNotification(getString(R.string.decoder_stopping))
        DsdNative.nativeStop()
    }

    private fun joinEngineThread() {
        val thread = engineThread ?: return
        try {
            thread.join(ENGINE_JOIN_TIMEOUT_MS)
        } catch (e: InterruptedException) {
            Log.w(TAG, "interrupted joining engine thread", e)
            Thread.currentThread().interrupt()
        }
        if (thread.isAlive) {
            // Leave both the handle and the state as the still-running engine thread
            // will find them: forcing IDLE here advertises a service that can be
            // started again, and the native side would then reject every attempt.
            Log.e(TAG, "engine thread did not stop within ${ENGINE_JOIN_TIMEOUT_MS}ms")
            return
        }
        engineThread = null
        synchronized(lock) { state = State.IDLE }
    }

    private fun acquireWakeLock() {
        if (wakeLock != null) {
            return
        }
        val power = getSystemService(Context.POWER_SERVICE) as PowerManager
        val lock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "dsd-neo:decode")
        lock.setReferenceCounted(false)
        lock.acquire()
        wakeLock = lock
    }

    private fun releaseWakeLock() {
        wakeLock?.let {
            if (it.isHeld) {
                it.release()
            }
        }
        wakeLock = null
    }

    /**
     * Creates the notification channel. Idempotent by contract, so it is called
     * unconditionally rather than guarded by getNotificationChannel(): reading a channel
     * back copies it, and on some vendor builds copying a channel with no vibration
     * pattern throws inside the framework and kills the process.
     */
    private fun ensureNotificationChannel() {
        val channel =
            NotificationChannel(CHANNEL_ID, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW)
        channel.enableVibration(false)
        channel.vibrationPattern = null
        getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    private fun buildNotification(text: String): Notification {
        val stopIntent = Intent(this, DecoderService::class.java).setAction(ACTION_STOP)
        val stopPending = PendingIntent.getService(
            this, 0, stopIntent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_stat_dsdneo)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, getString(R.string.action_stop), stopPending).build())
            .build()
    }

    private fun startForegroundNotification(text: String) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID, buildNotification(text), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            )
        } else {
            startForeground(NOTIFICATION_ID, buildNotification(text))
        }
    }

    private fun updateNotification(text: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, buildNotification(text))
    }

    private fun stopForegroundCompat() {
        stopForeground(STOP_FOREGROUND_REMOVE)
    }

    /** Notification text matching the current phase, for the unconditional promotion. */
    private fun currentStatusText(): String = when (synchronized(lock) { state }) {
        State.STARTING -> getString(R.string.decoder_starting)
        State.RUNNING -> getString(R.string.decoder_running)
        State.STOPPING -> getString(R.string.decoder_stopping)
        State.IDLE -> getString(R.string.decoder_starting)
    }

    /**
     * Undoes the promotion above for an intent that started nothing, so an
     * unrecognised action cannot leave a foreground service with no decoder behind it.
     */
    private fun stopIfIdle() {
        if (synchronized(lock) { state } != State.IDLE) {
            return
        }
        releaseWakeLock()
        stopForegroundCompat()
        stopSelf()
    }

    private enum class State { IDLE, STARTING, RUNNING, STOPPING }

    companion object {
        private const val TAG = "dsd-neo"
        private const val CHANNEL_ID = "dsdneo_decoder"
        private const val NOTIFICATION_ID = 1
        private const val ENGINE_JOIN_TIMEOUT_MS = 5000L

        const val ACTION_START = "io.github.arancormonk.dsdneo.action.START"
        const val ACTION_STOP = "io.github.arancormonk.dsdneo.action.STOP"
        const val EXTRA_ARGS = "io.github.arancormonk.dsdneo.extra.ARGS"

        private val lock = Any()
        private var state = State.IDLE
        private var initialized = false
        private var lastError = ""

        /** Called from the Qt host (DecoderHostAndroid) to start decoding. */
        @JvmStatic
        fun startDecoder(context: Context, args: Array<String>) {
            val intent = Intent(context, DecoderService::class.java)
                .setAction(ACTION_START)
                .putExtra(EXTRA_ARGS, args)
            context.startForegroundService(intent)
        }

        /** Called from the Qt host to request a graceful stop. */
        @JvmStatic
        fun stopDecoder(context: Context) {
            val intent = Intent(context, DecoderService::class.java).setAction(ACTION_STOP)
            context.startService(intent)
        }

        /** Service-side view of the lifecycle, for UI status text. */
        @JvmStatic
        fun stateName(): String = synchronized(lock) { state.name }

        /**
         * Why the last start was abandoned, or "" if none was. Read by the Qt host when
         * it sees a start end without the engine ever running; cleared by the next start.
         */
        @JvmStatic
        fun lastError(): String = synchronized(lock) { lastError }
    }
}
