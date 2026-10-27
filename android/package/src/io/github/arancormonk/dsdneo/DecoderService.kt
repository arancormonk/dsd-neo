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
import android.os.SystemClock
import android.util.Log
import java.util.Locale
import org.json.JSONObject

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

    /** Which instance this is; start ids mean something only to the instance they were handed to. */
    private var serial = 0L

    override fun onCreate() {
        super.onCreate()
        serial = ++instancesCreated
        current = this
        ProcessFacts.attach(this)
        ensureNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        logReceipt(intent, startId)
        // A notification Stop must reach native cancellation before any notification binder
        // calls. The Qt sender has already signalled it; repeating the request is harmless.
        val stopping = intent?.action == ACTION_STOP && requestEngineStop(
            StopCause.fromWire(intent.getStringExtra(EXTRA_STOP_CAUSE)),
            intent.getLongExtra(EXTRA_STOP_SESSION, 0L).takeIf { it != 0L }
        )

        // Unconditional, and before anything that can return early. startDecoder()
        // uses startForegroundService(), which obliges this service to call
        // startForeground() for that start — including the ones we go on to reject
        // (a duplicate START while already RUNNING) and the ones we do not
        // recognise. Skipping it there risks ForegroundServiceDidNotStartInTime.
        val starting = intent?.action == ACTION_START
        if (starting) {
            // Each step a start takes into the platform is logged on entry and on return, so a phone that falls over
            // inside one still shows in the durable diagnostics how far the start got (issue #745).
            DsdNative.nativeHostDiagnostic("service: entering the foreground")
        }
        startForegroundNotification(currentStatusText())
        if (starting) {
            DsdNative.nativeHostDiagnostic("service: in the foreground")
        }

        when (intent?.action) {
            ACTION_START -> {
                val args = intent.getStringArrayExtra(EXTRA_ARGS) ?: emptyArray()
                startDecoding(args, startId, intent.getLongExtra(EXTRA_ATTEMPT, 0L))
            }
            ACTION_STOP -> {
                synchronized(lock) { lastStart = DeliveredStart(serial, startId) }
                // Both senders say who asked: the app's Stop listening and the notification's
                // Stop action are the user (#718 D4). Anything else drains.
                if (stopping) showStopping()
                // A stop aimed at a service that is not running still promoted us
                // above; drop back out of the foreground rather than sitting there
                // with no decoder behind the notification.
                stopIfIdle()
            }
            else -> {
                synchronized(lock) { lastStart = DeliveredStart(serial, startId) }
                Log.w(TAG, "ignoring intent with action ${intent?.action}")
                stopIfIdle()
            }
        }

        // Here rather than in startDecoding(), so it keys off "this instance is fronting a
        // live engine" instead of "this instance started it": a duplicate START that
        // startDecoding() rejects still has to leave the notification tracking the call it
        // is already fronting. startStatusPolling() removes any queued tick first, so
        // arriving here twice for one session cannot leave two loops running.
        //
        // RUNNING and not merely "not IDLE": STOPPING means nativeStop() has been called,
        // which is the state a timed-out joinEngineThread() leaves behind (onDestroy runs
        // stopDecoding() before the join, so the engine is always on its way out by then).
        // A replacement instance created in that window is fronting a session that is
        // shutting down, and "Stopping…" is what its notification should say. That run's
        // worker reaches this instance when it ends: see routeSessionEnd().
        if (synchronized(lock) { state } == State.RUNNING) {
            startStatusPolling()
        }

        // A dead process stays dead until the user relaunches: restarting the engine
        // without its configuration would be worse than not restarting at all.
        return START_NOT_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        // Removing the task is an explicit stop, even if Android retains the Activity or Qt's
        // teardown is delayed. Home/Back with background listening enabled never comes here.
        stopDecoding(StopCause.USER)
        stopIfIdle()
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {
        // First, and on the same thread the poll runs on: once removeCallbacks() has
        // returned here, no pending tick can still fire and post a notification behind a
        // service that is on its way out.
        stopStatusPolling()
        stopDecoding(StopCause.LIFECYCLE)
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
        // end of every session — the engine thread's session end calls stopSelf() when
        // nativeRun returns — so releasing would drop the descriptor after every run and
        // leave the user reconnecting the dongle before each one. UsbSourceManager holds
        // it for the process instead and releases it on detach; process death closes it.
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
        // Last, because the platform took the foreground notification down before it
        // scheduled this callback (ActiveServices.bringDownServiceLocked() cancels it, then
        // calls scheduleStopService()), so anything posted since — stopDecoding()'s
        // "Stopping…" above included — is an ordinary ongoing notification that nothing
        // else would remove. Only another live instance, fronting a run of its own, keeps it.
        val replaced = current != null && current !== this
        if (current === this) {
            current = null
        }
        if (!replaced) {
            cancelNotification()
        }
        super.onDestroy()
    }

    /**
     * Runs one start on the main thread. [attempt] is the host's id for it (0: a start without one): the host retires
     * an attempt it has given up on (retireAttempts()), and a retired or superseded attempt never runs (#782).
     */
    private fun startDecoding(args: Array<String>, startId: Int, attempt: Long) {
        var rejectedIn: State? = null
        val session = synchronized(lock) {
            // Publish the id with the decision to start or reject. Doing this before
            // foreground promotion lets an old worker take a new run's id while the
            // main thread is still in the notification binder call.
            lastStart = DeliveredStart(serial, startId)
            // Before the busy check: a start the host has given up on is nothing to the run that holds the service.
            if (attempt != 0L && (attempt <= retiredThrough || attempt <= newestAttempt)) {
                rejectedIn = state
                return@synchronized 0L
            }
            if (state != State.IDLE) {
                // Not a terminal failure: the run that holds the service keeps its
                // notification and its polling, which onStartCommand re-arms below.
                lastError = getString(R.string.error_busy)
                Log.w(TAG, "start rejected in state $state")
                return
            }
            state = State.STARTING
            sessionId += 1
            terminalReason = DsdNative.RUN_PENDING
            deviceError = 0
            lastError = ""
            attemptId = attempt
            if (attempt != 0L) {
                newestAttempt = attempt
            }
            cancelRequested = false
            attemptRan = false
            failureKind = ""
            sessionId
        }
        rejectedIn?.let { found ->
            // The host already shows what became of it, and a later attempt may own the screen.
            DsdNative.nativeHostDiagnostic(
                "service: start attempt $attempt rejected as retired or superseded (state $found)"
            )
            stopIfIdle()
            return
        }

        // The device is about to be opened: keep the diagnostics start window open through it.
        DsdNative.nativeArmStartupDiagnostics()
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

        DsdNative.nativeHostDiagnostic("service: configuring the engine")
        // On the main thread, as is the USB receiver: a detach is handled only once this returns, when the native
        // record already names the session's receiver (nativeSessionUsesLocalReceiver(), #647).
        val configureRc = DsdNative.nativeConfigure(args, session)
        DsdNative.nativeHostDiagnostic("service: engine configuration returned $configureRc")
        if (configureRc == DsdNative.STATUS_CONFIG_EXIT) {
            // Help and other successful one-shot configuration flows did not
            // initialize a session; publish completion without stamping recency.
            releaseWakeLock()
            synchronized(lock) {
                terminalReason = DsdNative.RUN_COMPLETED
                state = State.IDLE
            }
            finishIdle()
            return
        }
        if (configureRc != DsdNative.STATUS_OK) {
            failStart(
                "nativeConfigure failed ($configureRc)",
                getString(R.string.error_configure_failed, configureRc)
            )
            return
        }
        val thread = Thread({
            DsdNative.nativeHostDiagnostic("service: engine run starting")
            val rc = DsdNative.nativeRun()
            Log.i(TAG, "engine run returned $rc")
            DsdNative.nativeHostDiagnostic("engine run returned $rc")
            // Released before the state drops to IDLE. acquireWakeLock is a no-op while
            // a lock is held, so a start that observes IDLE first would find the lock
            // still ours and decode with nothing keeping the CPU awake.
            releaseWakeLock()
            val completedStart = synchronized(lock) {
                val result = DsdNative.nativeLifecycleStatus()
                if (result != null && result.size >= 5 && result[0] == sessionId) {
                    terminalReason = result[2].toInt()
                    deviceError = result[4].toInt()
                }
                if (terminalReason == DsdNative.RUN_FAILED || rc != DsdNative.STATUS_OK && terminalReason == DsdNative.RUN_PENDING) {
                    terminalReason = DsdNative.RUN_FAILED
                    // Codes only: raw configuration/argv text can contain keys.
                    lastError = "Decoder run failed ($rc)" + if (deviceError != 0) "; USB open/claim error $deviceError" else ""
                }
                state = State.IDLE
                // Only if it is still ours: a start that raced a timed-out join has
                // already installed its own thread here.
                if (engineThread === Thread.currentThread()) {
                    engineThread = null
                }
                // Capture before publishing IDLE to another start. Reading lastStart later
                // could stop the new run instead of the one this worker just finished.
                lastStart
            }
            routeSessionEnd(session, completedStart)
        }, "dsd-neo-engine")
        // Atomic with the host's retirement: a retireAttempts() that found this attempt STARTING latched the
        // cancellation, and one that comes after this block finds it RUNNING and leaves it running. The latch is
        // not the native stop flag, which nativeConfigure() clears.
        val cancelled = synchronized(lock) {
            if (cancelRequested) {
                true
            } else {
                engineThread = thread
                state = State.RUNNING
                attemptRan = true
                false
            }
        }
        if (cancelled) {
            cancelConfigured(attempt)
            return
        }
        updateNotification(getString(R.string.decoder_running))
        thread.start()
        // Polling is armed by onStartCommand() once the state settles; see the note there.
    }

    /**
     * Ends a configured start whose attempt the host retired during configuration: no worker starts, the native side
     * gives up the configuration and the receiver it named, and the session ends cancelled. The host already shows
     * what became of the attempt.
     */
    private fun cancelConfigured(attempt: Long) {
        val rc = DsdNative.nativeDiscardConfigured()
        DsdNative.nativeHostDiagnostic(
            "service: start attempt $attempt retired during configuration; cancelled (discard rc=$rc)"
        )
        releaseWakeLock()
        synchronized(lock) {
            terminalReason = DsdNative.RUN_CANCELLED
            state = State.IDLE
        }
        finishIdle()
    }

    /**
     * Abandons a start. [reason] goes to logcat; [userMessage] is what the UI shows,
     * because dropping back to IDLE with nothing on screen reads as "nothing happened".
     * [kind] is the attempt's failure kind for the record's "failureKind" (a wire value
     * android/run_status.h's attempt_failure_from_wire() reads), or "" when the message
     * alone says it.
     */
    private fun failStart(reason: String, userMessage: String, kind: String = "") {
        Log.e(TAG, reason)
        DsdNative.nativeHostDiagnostic(reason)
        // Released before IDLE, for the same reason as the engine thread does it.
        releaseWakeLock()
        synchronized(lock) {
            state = State.IDLE
            lastError = userMessage
            terminalReason = DsdNative.RUN_FAILED
            failureKind = kind
        }
        // No stopStatusPolling() here: leaveForeground() does it first thing, and it has
        // to, because stopIfIdle() reaches it by a path that does not come through here.
        // Two calls would only make it ambiguous which one is load-bearing.
        finishIdle()
    }

    private fun stopDecoding(cause: StopCause) {
        if (!requestEngineStop(cause)) {
            return
        }
        showStopping()
    }

    private fun showStopping() {
        // Drops the cached record with it, which is what makes the update below visible:
        // the phase text is only rendered when no status is cached, so leaving the last
        // polled call in place would answer the user's stop with a call line and a
        // chronometer still counting up.
        stopStatusPolling()
        updateNotification(getString(R.string.decoder_stopping))
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
        val stopIntent = Intent(this, DecoderService::class.java)
            .setAction(ACTION_STOP)
            .putExtra(EXTRA_STOP_CAUSE, StopCause.USER.wire)
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
        val status = lastStatus
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

        // "0" is the encoder's not-decoded sentinel, the same one srcText is filtered for
        // in bodyText() -- X2-TDMA and standalone ProVoice never parse a talkgroup at all
        // and their calls arrive carrying it, and a policy-resolved target with no CSV
        // alias does too. Empty is the other unrenderable case: the record's charset
        // filter drops a multi-byte sequence the field ended part-way through, which can
        // take the whole name with it. Either way there is a transmission, it just has no
        // name, so the phase wording is the honest headline -- "Listening" would say the
        // opposite of what is happening.
        val leadName = lead?.name?.takeIf { it.isNotEmpty() && it != "0" }
        builder.setContentTitle(
            (if (lead?.emergency == true) getString(R.string.decoder_emergency_prefix) else "") + when {
                leadName != null -> leadName
                lead != null -> text
                else -> getString(R.string.decoder_listening)
            }
        )
        // Resolved once and handed to both renderers: each would otherwise take the
        // Resources lock for the same two strings, and they must not disagree.
        val frequency = frequencyPart(status, lead)

        // Back to the phase wording when the status has nothing to say. The record's two
        // halves are published by separate calls, so a poll can catch a status whose call
        // and frequency are both still empty, and a blank second line reads as a bug.
        val body = bodyText(lead, frequency)
        builder.setContentText(body.ifEmpty { text })

        // Gated on the expanded text itself, not on "some slot has content": a slot whose
        // name and source are both the encoder's "0" sentinel — X2-TDMA and standalone
        // ProVoice, which call_view.c deliberately lets through with no identity —
        // contributes no row at all, so the guard would pass with nothing to show. An
        // empty big text, or one that just repeats the collapsed line, turns the expand
        // chevron into a lie.
        val expanded = expandedText(status, frequency)
        if (expanded.isNotEmpty() && expanded != body) {
            builder.setStyle(Notification.BigTextStyle().bigText(expanded))
        }
        return builder.build()
    }

    /**
     * Resolved once: it is a constant for the life of the process, and [expandedText]
     * would otherwise take the Resources lock once per rendered row.
     */
    private val separator: String by lazy(LazyThreadSafetyMode.NONE) { getString(R.string.notif_separator) }

    /**
     * `851.012500 MHz`, or null when there is no frequency to show.
     *
     * Formatted against Locale.ROOT, not the resource configuration's locale, which is
     * what `getString(id, args)` would use: Java's `%f` localises both the decimal
     * separator and the zero digit, so a device set to German would render
     * `851,012500 MHz` and one set to Arabic with native digits would render the number in
     * Arabic-Indic numerals beside English words. Every other frequency readout in the
     * project is C-locale `%.06lf`, and a frequency is a machine value the user reads back
     * into a config file.
     */
    private fun freqText(hz: Long): String? =
        if (hz <= 0L) null else String.format(Locale.ROOT, getString(R.string.notif_freq_mhz), hz / 1_000_000.0)

    /**
     * The frequency line, by first-match precedence.
     *
     * The tuner centre is never a stand-in for the voice channel: on a trunked system a
     * retune moves the centre out from under the call.
     */
    private fun frequencyPart(status: DecoderStatus, lead: SlotCall?): String? {
        // hasContent, not LINE_ACTIVE: the lead slot stays ENDED for the hold that follows
        // every transmission, and trunkTuned is still set for as much of it as the trunk
        // state machine takes to hand the tuner back. Testing for ACTIVE alone made the
        // body fall through to the branch below and label the control channel as where the
        // receiver was, for three seconds after every call, while it was still parked on
        // the voice channel.
        if (lead != null && lead.hasContent && status.trunking && status.trunkTuned) {
            freqText(status.vcFreqHz)?.let { return it }
        }
        if (status.trunking) {
            freqText(status.ccFreqHz)?.let { return getString(R.string.notif_control_freq, it) }
        }
        // Only the tuner centre is gated on the input being a radio, and only the tuner
        // centre should be: the publisher already zeroes it for every other input, whereas
        // the voice and control channels above are decoded off the air and are just as
        // true on a rigctl-tuned trunked session fed over UDP, TCP or from a file. Gating
        // the whole chain here left those sessions with no frequency line at all.
        return if (status.radioInput) freqText(status.centerFreqHz) else null
    }

    /**
     * `Encrypted`, plus `ALG xx` / `KID xxxx` once a crypto header has decoded.
     *
     * One helper for both renderers: written out twice, the expanded view had only the
     * bare `Encrypted` while the collapsed line carried the algorithm — so pulling the
     * notification open *removed* the one thing that tells AES from RC4 at a glance,
     * which is the whole reason the Qt panel formats it.
     *
     * algid 0 is "encrypted, but no crypto header decoded yet"; the ENC word alone is the
     * whole of what is known. Same rule the Qt panel applies.
     */
    private fun cryptoParts(call: SlotCall?): List<String> {
        if (call == null || !call.enc) {
            return emptyList()
        }
        if (call.algid == 0) {
            return listOf(getString(R.string.notif_encrypted))
        }
        return listOf(
            getString(R.string.notif_encrypted),
            getString(R.string.notif_alg, "%02X".format(call.algid)),
            getString(R.string.notif_kid, "%04X".format(call.kid))
        )
    }

    /**
     * Parts in priority order, because Android truncates the tail: why there is no audio
     * first, who is talking second, where third.
     */
    private fun bodyText(lead: SlotCall?, frequency: String?): String {
        val parts = mutableListOf<String>()
        parts += cryptoParts(lead)
        // "0" is what the encoder writes for a source that never decoded, not a unit.
        if (lead != null && lead.srcText.isNotEmpty() && lead.srcText != "0") {
            parts += getString(R.string.notif_unit, lead.srcText)
        }
        frequency?.let { parts += it }
        return parts.joinToString(separator)
    }

    /**
     * One row per slot with content, plus the frequency. Prose, not columns: notification
     * body text is proportional, so an aligned table would ragged out.
     */
    private fun expandedText(status: DecoderStatus, frequency: String?): String {
        val occupied = status.slots.count { it.hasContent }
        val rows = mutableListOf<String>()
        status.slots.forEachIndexed { index, call ->
            if (!call.hasContent) {
                return@forEachIndexed
            }
            val parts = mutableListOf<String>()
            // Only when the number tells the reader something. Slot 1 is the only slot
            // P25 Phase 1, D-STAR, M17, YSF, NXDN, dPMR, EDACS and ProVoice ever fill, and
            // labelling their single row "Slot 1" invents a TDMA concept those modes do
            // not have.
            if (occupied > 1) {
                parts += getString(R.string.notif_slot, index + 1)
            }
            // Same two unrenderable cases the content title guards against; a bare "0" or
            // an empty name would otherwise leave a row that is nothing but a separator.
            if (call.name.isNotEmpty() && call.name != "0") {
                parts += call.name
            }
            if (call.srcText.isNotEmpty() && call.srcText != "0") {
                parts += getString(R.string.notif_unit, call.srcText)
            }
            parts += cryptoParts(call)
            if (parts.isNotEmpty()) {
                rows += parts.joinToString(separator)
            }
        }
        frequency?.let { rows += it }
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

    private fun cancelNotification() {
        getSystemService(NotificationManager::class.java).cancel(NOTIFICATION_ID)
    }

    /**
     * Logs an intent as it arrives, before anything acts on it: which one, its start id, the
     * state it found, and how long it took to arrive when the sender said when it sent it.
     * The notification's Stop is an immutable PendingIntent built once, so it carries no
     * send time, and any stamp in it would be the time it was built rather than tapped.
     */
    private fun logReceipt(intent: Intent?, startId: Int) {
        val sent = intent?.getLongExtra(EXTRA_SENT_UPTIME, 0L) ?: 0L
        val now = SystemClock.uptimeMillis()
        val delay = if (sent in 1L..now) "delay=${now - sent} ms" else "delay unavailable"
        val action = when (intent?.action) {
            ACTION_START -> "START"
            ACTION_STOP -> "STOP"
            null -> "no action"
            else -> "unrecognised action"
        }
        val found = synchronized(lock) { state }
        DsdNative.nativeHostDiagnostic("service: received $action id=$startId state=$found $delay")
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
     * [lastStatusRecord] parsed, and the only input [buildNotification] renders.
     *
     * Parsed once where the record changes rather than once per notification build: a
     * build happens on every phase update too, and re-splitting the record to render the
     * same status again is work for nothing. Kept in step with [lastStatusRecord] — both
     * are written together and cleared together — so after every tick it holds that
     * tick's status, audible-audio age included, whether or not the tick re-posted.
     */
    private var lastStatus: DecoderStatus? = null

    /**
     * Re-reads the published status once a second, re-posting only when what the
     * notification shows changed, and feeds every running tick to the screen policy.
     *
     * Re-posts itself from the tail and only while RUNNING, so the loop unwinds on its
     * own the moment a session ends or a stop lands — there is no timer left pointing at
     * a service that has been told to go away. Both tails that end the loop end the
     * screen policy's session too, as [stopStatusPolling] does.
     */
    private val statusPoll = object : Runnable {
        override fun run() {
            if (synchronized(lock) { state } != State.RUNNING) {
                // Never a notify() here. A run that ended by itself is taken down by its
                // session end (finishSession()), and a tick queued ahead of it must not post
                // behind it: once the platform has dropped the foreground, a notify() is an
                // ordinary ongoing notification that nothing removes (#782). A stop already
                // posted "Stopping…" through showStopping(). The cache goes, so a later phase
                // update cannot render the finished call again.
                lastStatusRecord = null
                lastStatus = null
                ScreenSupport.statusStopped()
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
                ScreenSupport.statusStopped()
                return
            }
            var displayChanged = false
            if (record != lastStatusRecord) {
                val previous = lastStatus
                val wasUnreadable = lastStatusRecord != null && previous == null
                val status = DecoderStatus.parse(record)
                lastStatusRecord = record
                lastStatus = status
                if (record != null && status == null && !wasUnreadable) {
                    // A record this build cannot read — a field added on one side of JNI
                    // without the other, or a version it does not know. Silent otherwise:
                    // the notification would render the phase wording for the rest of the
                    // session with nothing on either side saying why. Once per run of
                    // unreadable records: the audible-audio age changes the record every
                    // second, and the first line already says all there is to say.
                    Log.w(TAG, "unreadable status record; notification limited to phase text")
                }
                // The record changes every second while audio is recent — its audible-audio
                // age advances — but that pair is for the screen policy, not the shade, so
                // re-post only when what the notification shows changed, which on a quiet
                // channel is almost never. A record turning unreadable or readable again
                // counts: the notification swaps between the phase wording and the status.
                // A live call does re-render each second — its elapsed_ms advances — but
                // setOnlyAlertOnce keeps every one of those silent.
                displayChanged = if (status == null) previous != null else !status.sameDisplay(previous)
            }
            // Every tick, changed record or not: the screen policy's lease is renewed per sample, and
            // [lastStatus] holds this tick's status either way.
            ScreenSupport.statusTick(this@DecoderService, synchronized(lock) { sessionId }, lastStatus)
            if (displayChanged) {
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
     * phase wording rather than freezing on the last call it saw, and ends the screen
     * policy's session.
     *
     * Every deliberate teardown comes through here first: [onDestroy], [stopDecoding] and
     * [leaveForeground] (and so every session end, [failStart] and [stopIfIdle]). The
     * poll's own two tails end the screen session themselves; ending it twice is harmless.
     *
     * Safe against a tick that is already queued: the poll runs on the main looper, and
     * every caller of this is on the main thread too, so removeCallbacks() cannot race
     * one that is halfway through.
     */
    private fun stopStatusPolling() {
        statusHandler.removeCallbacks(statusPoll)
        lastStatusRecord = null
        lastStatus = null
        ScreenSupport.statusStopped()
    }

    private fun leaveForeground() {
        // Before dropping the notification, not after: once it is gone a queued tick that
        // re-posted would put an ongoing notification back with no foreground service
        // behind it, and nothing left alive to take it down again.
        stopStatusPolling()
        stopForeground(STOP_FOREGROUND_REMOVE)
        // Also removes one posted while the service was not in the foreground.
        cancelNotification()
    }

    /**
     * The end of [session] on the main thread: takes the notification down and stops the
     * service against [start], the start the session was last handed.
     *
     * In that order, so nothing can post between the platform dropping the foreground and
     * this instance being destroyed: stopSelf() makes ActiveServices cancel the foreground
     * notification and only then schedule onDestroy() on this looper, so a status tick
     * already queued there would run in between (#782).
     *
     * A newer accepted run that owns the service again keeps its notification and its
     * polling. Its start came after [start], so the platform ignores the stopSelf():
     * stopServiceTokenLocked() stops only for the newest start id.
     *
     * A start handed to an earlier instance is not stopped here. Start ids restart with
     * each service record, so that id could name one of this instance's own, newer starts;
     * and an instance whose start came after the capture already settled it, with the
     * state that start found.
     */
    private fun finishSession(session: Long, start: DeliveredStart) {
        val newerRun = synchronized(lock) { state != State.IDLE && sessionId != session }
        if (!newerRun) {
            leaveForeground()
        }
        if (start.service == serial) {
            stopSelf(start.id)
        }
    }

    /** [finishSession] for a start that ends before any worker ran: failed, refused or one-shot. */
    private fun finishIdle() {
        val (session, start) = synchronized(lock) { sessionId to lastStart }
        finishSession(session, start)
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
        // Against the start it was last handed: only main-thread callers come here, where
        // another onStartCommand cannot interleave. The engine worker captures its id under
        // the lock that publishes IDLE instead, so a subsequent start cannot lend it a newer id.
        finishIdle()
    }

    private enum class State { IDLE, STARTING, RUNNING, STOPPING }

    /** A start id, with the [serial] of the instance it was handed to. */
    private class DeliveredStart(val service: Long, val id: Int)

    companion object {
        private const val TAG = "dsd-neo"
        private const val CHANNEL_ID = "dsdneo_decoder"
        private const val NOTIFICATION_ID = 1
        /**
         * How long [onDestroy] waits for the engine thread before giving up on it.
         *
         * Deliberately short, because this runs on the main thread. The ordinary
         * teardown does not spend it at all: the engine thread hands its session end
         * (and with it stopSelf()) to the main thread only after nativeRun has returned,
         * as the last thing it does, so the join is immediate. The
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
        /** A [StopCause.wire] value: who asked for an [ACTION_STOP]. */
        const val EXTRA_STOP_CAUSE = "io.github.arancormonk.dsdneo.extra.STOP_CAUSE"
        /** A delivered stop must not cancel a session started after the request. Zero means an ordered pending start. */
        const val EXTRA_STOP_SESSION = "io.github.arancormonk.dsdneo.extra.STOP_SESSION"
        /** SystemClock.uptimeMillis() as the sender dispatched the intent, so its receipt can log the delay. */
        const val EXTRA_SENT_UPTIME = "io.github.arancormonk.dsdneo.extra.SENT_UPTIME"
        /** The host's id for a START's attempt (#782); see [retireAttempts]. Absent or 0: a start without one. */
        const val EXTRA_ATTEMPT = "io.github.arancormonk.dsdneo.extra.ATTEMPT"

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
        private var sessionId = 0L
        private var terminalReason = DsdNative.RUN_PENDING
        private var deviceError = 0
        /** The newest session whose receiver ending the UI acknowledged; outlives the Activity's host (#647). */
        private var endingAcknowledged = 0L

        /** Newest start handed to onStartCommand; the worker captures it before releasing IDLE. */
        private var lastStart = DeliveredStart(0L, 0)

        /*
         * Start attempts (#782). The host gives every start an id, increasing for the life of the process, and gives
         * up on one (its start grace ran out, the user dismissed it, retried it or started something else) by retiring
         * every id up to it. A START carrying a retired id, or one no newer than the newest accepted, never runs.
         */
        /** The current session's attempt (0: started without one), published as the record's "attemptId". */
        private var attemptId = 0L
        /** The newest attempt accepted; an older one arriving after it is superseded. */
        private var newestAttempt = 0L
        /** Attempts up to this one were retired by the host. */
        private var retiredThrough = 0L
        /** The current attempt was retired while STARTING: checked with the RUNNING publication; it ends cancelled. */
        private var cancelRequested = false
        /** The current attempt was published RUNNING: a retirement after that leaves it running. */
        private var attemptRan = false
        /**
         * Why the current attempt failed before nativeConfigure(), as the wire value android/run_status.h maps to a
         * failure kind (attempt_failure_from_wire()); "" when it did not. Published as the record's "failureKind".
         */
        private var failureKind = ""

        /**
         * The live service instance, which a session's end reaches; null between an
         * instance's onDestroy() and the next one's onCreate(), so it never keeps a destroyed
         * service alive. Read and written on the main thread only.
         *
         * Not always the worker's own instance: a join that timed out destroyed that one,
         * and an instance created since — promoted while the run was still STOPPING, so it
         * started no poll — is the one in the foreground. Its own stopSelf() is the only one
         * that can stop it: Service.stopSelf() passes the instance's token, and
         * ActiveServices.findServiceLocked() matches only the service record it names.
         */
        private var current: DecoderService? = null

        /** Instances created so far, for [serial]; main thread only. */
        private var instancesCreated = 0L

        private val mainHandler = Handler(Looper.getMainLooper())

        /**
         * Ends [session] from any thread: [finishSession] runs on the main thread, on the
         * instance that is live by then. With none, the last one's onDestroy() already took
         * the notification down, and there is no service left to stop.
         */
        private fun routeSessionEnd(session: Long, start: DeliveredStart) {
            mainHandler.post { current?.finishSession(session, start) }
        }

        /**
         * Cancel a configured run synchronously, also when called from the Qt thread. nativeStop
         * only raises atomic flags: no device, notification, UI or configure lock is acquired.
         * Holding the service lock through that call prevents a new run from taking the stop.
         * STARTING still uses the ordered intent: nativeConfigure resets the flags, so cancelling
         * it here could be lost. RUNNING is published only after configure has returned; the JNI
         * startup latch covers the interval before nativeRun reaches the decode loop.
         */
        private fun requestEngineStop(cause: StopCause, expectedSession: Long? = null): Boolean = synchronized(lock) {
            if (expectedSession != null && expectedSession != sessionId) {
                stopDiagnostic("Ignoring stale stop for session $expectedSession; current session $sessionId")
                return false
            }
            if (state != State.RUNNING && state != State.STOPPING) {
                stopDiagnostic("Stop session $sessionId cause=${cause.wire} state=$state: no run to cancel")
                return false
            }
            // Repeated stops are deliberate: a user's Stop while a lifecycle stop is draining
            // must still cancel uploads, and a previously refused native stop can be retried.
            val rc = EngineStop.request(cause, DsdNative::nativeStop)
            stopDiagnostic("Stop session $sessionId cause=${cause.wire} state=$state nativeStop=$rc")
            if (rc != DsdNative.STATUS_OK) {
                return false
            }
            state = State.STOPPING
            true
        }

        private fun stopDiagnostic(message: String) {
            Log.i(TAG, message)
            DsdNative.nativeHostDiagnostic(message)
        }

        /**
         * The wake lock, held for as long as a run is in flight.
         *
         * Kept with [engineThread] rather than on the instance for the same reason: a
         * join that times out destroys this service while the engine keeps decoding, and
         * an instance field would leave nobody able to release it — or, worse, invite
         * onDestroy to release it out from under a run that is still going.
         */
        private var wakeLock: PowerManager.WakeLock? = null

        /**
         * Takes the run's wake lock. The acquire is a binder call into the power manager, so
         * it happens outside [lock]: held under it, a slow acquire would stall
         * [lifecycleStatus] on the Qt thread and [stopDecoder] with it. The lock is published
         * under [lock] after a re-check, and a second one released if another got there first.
         */
        private fun acquireWakeLock(context: Context) {
            if (synchronized(lock) { wakeLock != null }) {
                return
            }
            val power = context.applicationContext.getSystemService(Context.POWER_SERVICE) as PowerManager
            val fresh = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "dsd-neo:decode")
            fresh.setReferenceCounted(false)
            fresh.acquire()
            val published = synchronized(lock) {
                if (wakeLock == null) {
                    wakeLock = fresh
                    true
                } else {
                    false
                }
            }
            if (!published) {
                fresh.release()
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

        /**
         * Called from the Qt host (DecoderHostAndroid) to start decoding. [attempt] is the host's id for this start,
         * which retires every earlier attempt first ([retireAttempts]), so none of them can run once this one is asked
         * for.
         */
        @JvmStatic
        fun startDecoder(context: Context, args: Array<String>, attempt: Long = 0L) {
            val intent = Intent(context, DecoderService::class.java)
                .setAction(ACTION_START)
                .putExtra(EXTRA_ARGS, args)
                .putExtra(EXTRA_ATTEMPT, attempt)
            intent.putExtra(EXTRA_SENT_UPTIME, SystemClock.uptimeMillis())
            context.startForegroundService(intent)
        }

        /**
         * Retires every start attempt up to [through], synchronously, also from the Qt thread (#782): the host gave up
         * on it. A START for one still undelivered is rejected when it arrives. One the service is configuring is
         * cancelled once nativeConfigure() returns: no worker starts, and it ends cancelled. One already published
         * RUNNING keeps running.
         *
         * @return whether the service had already taken attempt [through] past the point a retirement can cancel it:
         *   it runs, or it ended on its own and its record says how. False when it never will run.
         */
        @JvmStatic
        fun retireAttempts(through: Long): Boolean = synchronized(lock) {
            if (through <= 0L) {
                return false
            }
            if (through > retiredThrough) {
                retiredThrough = through
            }
            if (attemptId != 0L && attemptId <= through && state == State.STARTING && !attemptRan && !cancelRequested) {
                cancelRequested = true
                stopDiagnostic("Retiring start attempt $attemptId during configuration of session $sessionId")
            }
            attemptId == through && !cancelRequested
        }

        /**
         * Called from the Qt host for every stop or restart the user starts: Stop listening,
         * switching systems, retrying after a failure, leaving Signal search or its Stop, Back or
         * closing the window with background listening off, and "Dismiss failure and choose a
         * site". A user's stop does not wait for pending rdio uploads (#718 D4).
         */
        @JvmStatic
        fun stopDecoder(context: Context?) {
            val requestedSession = synchronized(lock) {
                val current = sessionId.takeIf { state != State.IDLE }
                requestEngineStop(StopCause.USER, current)
                current
            }
            // The engine has already been signalled when it is running. The intent is for
            // main-thread notification/screen bookkeeping, or an ordered stop of a pending
            // start. A delayed callback must not be the only path to native cancellation.
            if (context == null) {
                stopDiagnostic("Stop session ${requestedSession ?: 0}: no Android context for service dispatch")
                return
            }
            val intent = Intent(context, DecoderService::class.java)
                .setAction(ACTION_STOP)
                .putExtra(EXTRA_STOP_CAUSE, StopCause.USER.wire)
            requestedSession?.let { intent.putExtra(EXTRA_STOP_SESSION, it) }
            try {
                intent.putExtra(EXTRA_SENT_UPTIME, SystemClock.uptimeMillis())
                if (context.startService(intent) == null) {
                    stopDiagnostic("Stop session ${requestedSession ?: 0}: service dispatch returned no component")
                }
            } catch (e: IllegalStateException) {
                stopDiagnostic("Stop session ${requestedSession ?: 0}: service dispatch failed (${e.javaClass.simpleName})")
            } catch (e: SecurityException) {
                stopDiagnostic("Stop session ${requestedSession ?: 0}: service dispatch failed (${e.javaClass.simpleName})")
            }
        }

        /** One coherent lifecycle record for the UI's existing tick. Reading the
         * native record here uses its short-held status lock, never the configure
         * lock or the single-consumer decoder snapshot. */
        @JvmStatic
        fun lifecycleStatus(): String = synchronized(lock) {
            val native = DsdNative.nativeLifecycleStatus()
            val fields = native?.takeIf { it.size >= 5 } ?: LongArray(5)
            val matches = fields.size >= 5 && fields[0] == sessionId && sessionId != 0L
            // A native result may arrive before the worker releases its wake lock
            // and publishes IDLE. Preserve it beside the actual service state;
            // the host must keep restart disabled until that state becomes IDLE.
            val reason = if (terminalReason != DsdNative.RUN_PENDING) terminalReason
                         else if (matches) fields[2].toInt() else DsdNative.RUN_PENDING
            val usbError = if (matches) fields[4].toInt() else deviceError
            val error = if (lastError.isNotEmpty()) lastError
                        else if (reason == DsdNative.RUN_FAILED) "Decoder run failed (${fields[3]})" +
                            if (usbError != 0) "; USB open/claim error $usbError" else ""
                        else ""
            JSONObject().put("sessionId", sessionId).put("state", state.name)
                .put("initialized", matches && fields[1] != 0L)
                .put("reason", reason).put("deviceError", usbError).put("lastError", error)
                .put("inputFailure", if (matches && fields.size >= 7) fields[5].toInt() else 0)
                .put("inputError", if (matches && fields.size >= 7) fields[6].toInt() else 0)
                // #647: why the session ended and its receiver, published while its cleanup still runs.
                .put("endCause", if (matches && fields.size >= 9) fields[7].toInt() else 0)
                .put("receiverSource", if (matches && fields.size >= 9) fields[8].toInt() else 0)
                .put("endingAcknowledged", endingAcknowledged)
                // #782: the attempt this session was started for, and its attempt-scoped failure.
                .put("attemptId", attemptId).put("failureKind", failureKind).toString()
        }

        /**
         * The UI acknowledged the receiver endings of sessions up to [session] (a start, or an access request once the
         * session ended). Kept here rather than in the Activity's host, so a recreated Activity does not show an
         * acknowledged ending again (#647).
         */
        @JvmStatic
        fun acknowledgeReceiverEnding(session: Long) = synchronized(lock) {
            endingAcknowledged = maxOf(endingAcknowledged, session)
        }

        /** Service-side view of the lifecycle, for UI status text. */
        @JvmStatic
        fun stateName(): String = synchronized(lock) { state.name }

        /** The running session's id, or null unless RUNNING; for the screen policy's synchronous samples. */
        fun runningSessionId(): Long? = synchronized(lock) { if (state == State.RUNNING) sessionId else null }

        /**
         * Why the last start was abandoned, or "" if none was. Read by the Qt host when
         * it sees a start end without the engine ever running; cleared by the next start.
         */
        @JvmStatic
        fun lastError(): String = synchronized(lock) { lastError }
    }
}
