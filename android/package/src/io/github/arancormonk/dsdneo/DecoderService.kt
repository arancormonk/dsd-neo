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
import android.os.Handler
import android.os.IBinder
import android.os.Looper
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
        // First, and on the same thread the poll runs on: once removeCallbacks() has
        // returned here, no pending tick can still fire and post a notification behind a
        // service that is on its way out.
        stopStatusPolling()
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
        startStatusPolling()
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
        stopStatusPolling()
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
        // Drops the cached record with it, which is what makes the update below visible:
        // the phase text is only rendered when no status is cached, so leaving the last
        // polled call in place would answer the user's stop with a call line and a
        // chronometer still counting up.
        stopStatusPolling()
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

    /**
     * The stop action's PendingIntent, built once for this service instance.
     *
     * [buildNotification] runs on every status poll — once a second for the length of
     * every call — and each PendingIntent.getService/getActivity is a binder round trip
     * on the main thread. A PendingIntent is a process-lifetime token and this one's
     * identity (request code, target, action) never varies, so building it once costs
     * nothing in freshness.
     *
     * On the instance and not in the companion object, unlike [state] and [engineThread]:
     * a PendingIntent built from `this` holds the Context, and a companion field would
     * keep a destroyed service alive for the life of the process.
     */
    private val stopPendingIntent: PendingIntent by lazy {
        val stopIntent = Intent(this, DecoderService::class.java).setAction(ACTION_STOP)
        PendingIntent.getService(
            this, REQUEST_STOP, stopIntent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
    }

    /**
     * The tap target, built once alongside [stopPendingIntent].
     *
     * The launcher's own intent, not an explicit QtActivity component: ACTION_MAIN +
     * CATEGORY_LAUNCHER + NEW_TASK is what resumes an existing task, whereas an explicit
     * component can start a second Activity instance and with it a second Qt main(). Null
     * only if no launcher activity resolves, in which case the body stays inert rather
     * than taking the service down — and a package cannot gain or lose its launcher
     * activity without the process being killed, so resolving once is enough.
     */
    private val openPendingIntent: PendingIntent? by lazy {
        packageManager.getLaunchIntentForPackage(packageName)?.let { launch ->
            PendingIntent.getActivity(
                this, REQUEST_OPEN, launch, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
        }
    }

    /**
     * Renders the notification from the last polled status, falling back to [text].
     *
     * [text] is the phase wording — "Starting…", "Decoding" — and is what shows before
     * the engine has published anything, which includes the unconditional promotion at
     * the top of onStartCommand.
     */
    private fun buildNotification(text: String): Notification {
        val status = DecoderStatus.parse(lastStatusRecord)
        val lead = status?.leadSlot

        val builder = Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_dsdneo)
            .setOngoing(true)
            // Updates must never re-alert: this posts again on every call boundary.
            .setOnlyAlertOnce(true)
            .addAction(Notification.Action.Builder(null, getString(R.string.action_stop), stopPendingIntent).build())

        // No setAutoCancel: this is an ongoing foreground-service notification and must
        // survive the tap that opens the app.
        openPendingIntent?.let { builder.setContentIntent(it) }

        if (status == null) {
            return builder.setContentTitle(getString(R.string.app_name))
                .setContentText(text)
                .setShowWhen(false)
                .build()
        }

        if (status.protocol.isNotEmpty()) {
            builder.setSubText(status.protocol)
        }

        if (lead != null && lead.state == DecoderStatus.LINE_ACTIVE) {
            builder.setWhen(System.currentTimeMillis() - lead.elapsedMs)
                .setShowWhen(true)
                .setUsesChronometer(true)
        } else {
            // A Notification chronometer always counts, so an ended call cannot hold it
            // at its final duration. Hiding it is the honest option; the call's identity
            // stays for the 3s hold so a late glance still reads who it was.
            builder.setShowWhen(false).setUsesChronometer(false)
        }

        builder.setContentTitle(if (lead != null) lead.name else getString(R.string.decoder_listening))
        // Back to the phase wording when the status has nothing to say. The record's two
        // halves are published by separate calls, so a poll can catch a status whose call
        // and frequency are both still empty, and a blank second line reads as a bug.
        builder.setContentText(bodyText(status, lead).ifEmpty { text })

        if (status.slots.any { it.hasContent }) {
            // Only when there is something to expand into: a BigTextStyle whose text is
            // just the collapsed line again turns the expand chevron into a lie.
            builder.setStyle(Notification.BigTextStyle().bigText(expandedText(status)))
        }
        return builder.build()
    }

    private fun sep() = getString(R.string.notif_separator)

    /** `851.012500 MHz`, or null when there is no frequency to show. */
    private fun freqText(hz: Long): String? =
        if (hz <= 0L) null else getString(R.string.notif_freq_mhz, hz / 1_000_000.0)

    /**
     * The frequency line, by first-match precedence.
     *
     * The tuner centre is never a stand-in for the voice channel: on a trunked system a
     * retune moves the centre out from under the call.
     */
    private fun frequencyPart(status: DecoderStatus, lead: SlotCall?): String? {
        if (!status.radioInput) {
            return null
        }
        if (lead?.state == DecoderStatus.LINE_ACTIVE && status.trunking && status.trunkTuned) {
            freqText(status.vcFreqHz)?.let { return it }
        }
        if (status.trunking) {
            freqText(status.ccFreqHz)?.let { return getString(R.string.notif_control_freq, it) }
        }
        return freqText(status.centerFreqHz)
    }

    /**
     * Parts in priority order, because Android truncates the tail: why there is no audio
     * first, who is talking second, where third.
     */
    private fun bodyText(status: DecoderStatus, lead: SlotCall?): String {
        val parts = mutableListOf<String>()
        if (lead != null && lead.enc) {
            parts += getString(R.string.notif_encrypted)
            // algid 0 is "encrypted, but no crypto header decoded yet"; the ENC word
            // alone is the whole of what is known. Same rule the Qt panel applies.
            if (lead.algid != 0) {
                parts += getString(R.string.notif_alg, "%02X".format(lead.algid))
                parts += getString(R.string.notif_kid, "%04X".format(lead.kid))
            }
        }
        // "0" is what the encoder writes for a source that never decoded, not a unit.
        if (lead != null && lead.srcText.isNotEmpty() && lead.srcText != "0") {
            parts += getString(R.string.notif_unit, lead.srcText)
        }
        frequencyPart(status, lead)?.let { parts += it }
        return parts.joinToString(sep())
    }

    /**
     * One row per slot with content, plus the frequency. Prose, not columns: notification
     * body text is proportional, so an aligned table would ragged out.
     */
    private fun expandedText(status: DecoderStatus): String {
        val rows = mutableListOf<String>()
        status.slots.forEachIndexed { index, call ->
            if (!call.hasContent) {
                return@forEachIndexed
            }
            val parts = mutableListOf(getString(R.string.notif_slot, index + 1), call.name)
            if (call.srcText.isNotEmpty() && call.srcText != "0") {
                parts += getString(R.string.notif_unit, call.srcText)
            }
            if (call.enc) {
                parts += getString(R.string.notif_encrypted)
            }
            rows += parts.joinToString(sep())
        }
        frequencyPart(status, status.leadSlot)?.let { rows += it }
        return rows.joinToString("\n")
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

    private val statusHandler = Handler(Looper.getMainLooper())

    /**
     * The record [statusPoll] read last, and the only input [buildNotification] renders.
     *
     * On the instance rather than in the companion, unlike [state] and [engineThread]:
     * this is a rendering cache, not lifecycle state. A service instance that replaces
     * one destroyed mid-run should render its own first poll rather than inherit a
     * predecessor's record and skip it as unchanged.
     */
    private var lastStatusRecord: String? = null

    /**
     * Re-reads the published status once a second, re-posting only on a change.
     *
     * Re-posts itself from the tail and only while RUNNING, so the loop unwinds on its
     * own the moment a session ends or a stop lands — there is no timer left pointing at
     * a service that has been told to go away.
     */
    private val statusPoll = object : Runnable {
        override fun run() {
            if (synchronized(lock) { state } != State.RUNNING) {
                return
            }
            val record = try {
                DsdNative.nativeNotificationStatus()
            } catch (e: UnsatisfiedLinkError) {
                // A native method with nothing bound behind it never acquires an
                // implementation later in the same process, so this drops the loop
                // instead of re-posting: retrying would only log the same failure every
                // second for the rest of the session. The notification keeps whatever it
                // last rendered.
                Log.e(TAG, "status accessor unavailable; stopping status polling", e)
                return
            }
            if (record != lastStatusRecord) {
                lastStatusRecord = record
                // Only when the record itself changed, which on a quiet channel is
                // almost never. A live call does re-render each second — its elapsed_ms
                // advances — but setOnlyAlertOnce keeps every one of those silent.
                updateNotification(currentStatusText())
            }
            statusHandler.postDelayed(this, STATUS_POLL_MS)
        }
    }

    private fun startStatusPolling() {
        // removeCallbacks first so a restart cannot leave two loops posting each other.
        statusHandler.removeCallbacks(statusPoll)
        statusHandler.post(statusPoll)
    }

    /**
     * Stops the loop and forgets the cached record, so the notification falls back to the
     * phase wording rather than freezing on the last call it saw.
     *
     * Safe against a tick that is already queued: the poll runs on the main looper, and
     * every caller of this is on the main thread too, so removeCallbacks() cannot race
     * one that is halfway through.
     */
    private fun stopStatusPolling() {
        statusHandler.removeCallbacks(statusPoll)
        lastStatusRecord = null
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
        /**
         * How long [onDestroy] waits for the engine thread before giving up on it.
         *
         * Deliberately short, because this runs on the main thread. The ordinary
         * teardown does not spend it at all: the engine thread calls stopSelf() only
         * after nativeRun has returned, so by the time onDestroy is delivered the
         * thread is already at the tail of its lambda and the join is immediate. The
         * budget is only ever drawn on when the service is destroyed *while* a run is
         * live — a swipe-away, or a stop the engine is slow to honour — and there a
         * longer wait buys nothing: joinEngineThread() already handles the timeout
         * correctly by leaving the thread, the state and the wake lock for the run to
         * unwind on its own. Trading a frozen UI for a slightly better chance of a
         * tidy join is the wrong way round.
         */
        private const val ENGINE_JOIN_TIMEOUT_MS = 1000L

        /**
         * How often the service re-reads the published status.
         *
         * A second is well under the shortest transmission worth showing and well over
         * the cost of the read, which is one short mutex and a string copy. Faster would
         * only buy sub-second precision on a chronometer that shows whole seconds.
         */
        private const val STATUS_POLL_MS = 1000L

        /**
         * Request codes for the two pending intents.
         *
         * Distinct so FLAG_UPDATE_CURRENT can never have one rewrite the other: the
         * request code is part of a PendingIntent's identity, the extras are not.
         */
        private const val REQUEST_STOP = 0
        private const val REQUEST_OPEN = 1

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
