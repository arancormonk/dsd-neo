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

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        ensureNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        synchronized(lock) { lastStartId = startId }

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
        val stopped = joinEngineThread()
        if (stopped && initialized) {
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
        // The USB connection is deliberately not released here. onDestroy runs at the
        // end of every session — the engine thread calls stopSelf() when nativeRun
        // returns — so releasing would drop the descriptor after every run and leave
        // the user reconnecting the dongle before each one. UsbSourceManager holds it
        // for the process instead and releases it on detach; process death closes it.
        if (stopped) {
            releaseWakeLock()
        } else {
            // The engine is still decoding with this service on its way out. Releasing
            // here would drop the one thing keeping the CPU awake on screen-off, in the
            // middle of a run that is still producing audio. The wake lock belongs to
            // the run, not to this instance: the engine thread releases it when
            // nativeRun finally returns. See joinEngineThread().
            Log.e(TAG, "engine still running; keeping the wake lock until it returns")
        }
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
        acquireWakeLock(this)

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
            // Released before the state drops to IDLE. acquireWakeLock is a no-op while
            // a lock is held, so a start that observes IDLE first would find the lock
            // still ours and decode with nothing keeping the CPU awake.
            releaseWakeLock()
            synchronized(lock) {
                state = State.IDLE
                // Only if it is still ours: a start that raced a timed-out join has
                // already installed its own thread here.
                if (engineThread === Thread.currentThread()) {
                    engineThread = null
                }
            }
            stopSelfLatest()
        }, "dsd-neo-engine")
        synchronized(lock) {
            engineThread = thread
            state = State.RUNNING
        }
        updateNotification(getString(R.string.decoder_running))
        thread.start()
    }

    /**
     * Abandons a start. [reason] goes to logcat; [userMessage] is what the UI shows,
     * because dropping back to IDLE with nothing on screen reads as "nothing happened".
     */
    private fun failStart(reason: String, userMessage: String) {
        Log.e(TAG, reason)
        // Released before IDLE, for the same reason as the engine thread does it.
        releaseWakeLock()
        synchronized(lock) {
            state = State.IDLE
            lastError = userMessage
        }
        stopForegroundCompat()
        stopSelfLatest()
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

    /** @return true when no engine thread is left running. */
    private fun joinEngineThread(): Boolean {
        val thread = synchronized(lock) { engineThread } ?: return true
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
            // Both live in the companion object, so a service created after this one
            // is destroyed still sees this thread and can join it in turn.
            Log.e(TAG, "engine thread did not stop within ${ENGINE_JOIN_TIMEOUT_MS}ms")
            return false
        }
        // The thread clears these on its way out; this only settles a join that
        // returned before the tail of that lambda ran.
        synchronized(lock) {
            if (engineThread === thread) {
                engineThread = null
                state = State.IDLE
            }
        }
        return true
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
        stopSelfLatest()
    }

    /**
     * Stops the service against the start it was last handed.
     *
     * Bare stopSelf() stops regardless of how many starts have landed since. A run that
     * ends just as the user begins another one would tear the new session down from
     * onDestroy, because the destroy is delivered after the new start has already
     * installed its engine thread. Handing AMS the newest id makes the stop a no-op
     * once a newer start has arrived.
     */
    private fun stopSelfLatest() {
        stopSelf(synchronized(lock) { lastStartId })
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

        /**
         * The running engine's thread, held with [state] rather than on the instance.
         *
         * A join that times out leaves the engine running and this service destroyed.
         * The instance that replaces it has to be able to find that thread and join it
         * in turn, which an instance field cannot express: it would come back null and
         * strand a lifecycle the static [state] still describes as busy.
         */
        private var engineThread: Thread? = null
        private var initialized = false
        private var lastError = ""

        /** Newest start id handed to onStartCommand; see [stopSelfLatest]. */
        private var lastStartId = 0

        /**
         * The wake lock, held for as long as a run is in flight.
         *
         * Kept with [engineThread] rather than on the instance for the same reason: a
         * join that times out destroys this service while the engine keeps decoding, and
         * an instance field would leave nobody able to release it — or, worse, invite
         * onDestroy to release it out from under a run that is still going.
         */
        private var wakeLock: PowerManager.WakeLock? = null

        private fun acquireWakeLock(context: Context) {
            synchronized(lock) {
                if (wakeLock != null) {
                    return
                }
                val power = context.applicationContext.getSystemService(Context.POWER_SERVICE) as PowerManager
                val held = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "dsd-neo:decode")
                held.setReferenceCounted(false)
                held.acquire()
                wakeLock = held
            }
        }

        private fun releaseWakeLock() {
            val held = synchronized(lock) {
                val current = wakeLock
                wakeLock = null
                current
            }
            if (held != null && held.isHeld) {
                held.release()
            }
        }

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
