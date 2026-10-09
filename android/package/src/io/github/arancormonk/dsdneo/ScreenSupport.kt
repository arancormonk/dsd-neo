// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.app.Activity
import android.app.NotificationManager
import android.content.BroadcastReceiver
import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioManager
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.provider.Settings
import android.util.Log
import android.view.MotionEvent
import android.view.WindowManager
import android.view.accessibility.AccessibilityManager
import androidx.core.content.ContextCompat
import kotlin.math.abs

/**
 * The Android side of the screen modes: feeds one [ScreenController] from the activity, the decoder service's status
 * poll and the screen broadcasts, and carries its effects out on the activity's window and [ScreenLocks]. Every
 * decision is the controller's; each entry point here is one delegation.
 *
 * Main thread only. The activity's callbacks, the service's poll, the receiver and the scheduled tick all run there,
 * and Qt reaches [configure] through runOnAndroidMainThread; an entry point called from anywhere else is logged and
 * ignored rather than racing the policy.
 *
 * What feeds what:
 * - Qt's Screen and delay settings: [configure].
 * - The activity: [attach]/[detach] (created, destroyed), [onStart], [onStop], [onPause], [onTopResumedChanged],
 *   [onFocusChanged], [onMultiWindowChanged], [dispatchTouch] and [userInteraction]. Callbacks from an activity
 *   instance other than the attached one are ignored.
 * - The decoder service: [statusTick] on every running poll tick, [statusStopped] wherever the poll stops.
 * - The screen broadcasts: screen on, screen off and user present, on an exported receiver (see [bind]).
 *
 * Before the screen-off broadcast and each callback that can lose the foreground, a synchronous status sample goes
 * straight to the controller, so audio that began since the last poll is known when the policy judges a snooze or an
 * arming. [StatusFeed] keeps no stamp of its own, so such a sample cannot be undone by a later tick.
 */
object ScreenSupport {
    private const val TAG = "dsd-neo"

    /** The window's brightness override while dimmed, as a fraction of full; compared within the tolerance. */
    private const val DIM_BRIGHTNESS = 0.01f
    private const val BRIGHTNESS_TOLERANCE = 0.001f

    /** The phone's screen-off timeout when the setting cannot be read: Android's default. */
    private const val DEFAULT_SCREEN_OFF_TIMEOUT_MS = 30_000L

    private const val WAKE_REFUSED = "Screen wake refused by Android; calls cannot turn the screen on"

    private val main = Handler(Looper.getMainLooper())
    private val slot = ActivitySlot<Activity>()

    // The application's services, taken once; no Context is kept.
    private var bound = false
    private var settings: ContentResolver? = null
    private var power: PowerManager? = null
    private var audio: AudioManager? = null
    private var notifications: NotificationManager? = null
    private var accessibility: AccessibilityManager? = null
    private var locks: ScreenLocks? = null
    private var receiverRegistered = false
    private var receiverFailureLogged = false

    /** The window state the controller last asked for, re-applied against the live window on every sample. */
    private var keepScreenOn = false
    private var dimmed = false

    /** Whether a refused wake was reported, and for which session: a refusal is reported once per session. */
    private var refusalReported = false
    private var refusalSession: Long? = null

    private object Platform : ScreenController.Effects {
        override fun setKeepScreenOn(on: Boolean) {
            keepScreenOn = on
            applyWindow()
        }

        override fun setDimmed(dimmed: Boolean) {
            ScreenSupport.dimmed = dimmed
            applyWindow()
        }

        override fun renewLease() {
            screenLocks()?.renewLease()
        }

        override fun releaseLease() {
            locks?.releaseLease()
        }

        override fun pulseWake(): Boolean {
            val screen = screenLocks() ?: return false
            val on = screen.pulseWake()
            if (!on) {
                reportRefusal()
            }
            return on
        }

        override fun schedule(atMs: Long?) {
            main.removeCallbacks(tick)
            if (atMs != null) {
                // The controller's clock is uptimeMillis, the same base postAtTime uses.
                main.postAtTime(tick, atMs)
            }
        }
    }

    private val controller = ScreenController(SystemClock::uptimeMillis, Platform)
    private val feed = StatusFeed(controller)
    private val tick = Runnable { controller.tick() }

    /** Android's wake verdict for the policy, which asks it only for a sample that would otherwise wake. */
    private val wakeVerdict: () -> Boolean = { wakeAllowed() }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                Intent.ACTION_SCREEN_ON -> controller.screenOn(screenOffTimeoutMs())
                Intent.ACTION_SCREEN_OFF -> screenOff()
                Intent.ACTION_USER_PRESENT -> controller.userPresent()
            }
        }
    }

    /** Qt's Screen and delay settings, at startup and on every change. Called from C++ over JNI. */
    @JvmStatic
    fun configure(context: Context, mode: Int, delaySeconds: Int) {
        if (!onMain("configure")) {
            return
        }
        bind(context)
        controller.configure(mode, delaySeconds)
        applyWindow()
    }

    /** The activity was created; from here on its window carries the controller's effects. */
    fun attach(activity: Activity) {
        if (!onMain("attach")) {
            return
        }
        bind(activity)
        slot.attach(activity)
        applyWindow()
    }

    /** The activity is being destroyed. Only the attached instance counts: a recreated one may already be attached. */
    fun detach(activity: Activity) {
        if (!onMain("detach")) {
            return
        }
        if (slot.detach(activity)) {
            controller.destroyed()
        }
    }

    fun onStart(activity: Activity) = fromActivity(activity, "onStart") {
        // The mode changed callback only reports changes, so an activity started in split screen is told here.
        controller.multiWindowChanged(activity.isInMultiWindowMode)
        controller.started()
    }

    /**
     * The caller skips a stop that is part of a configuration change. The policy may wake here, for a call heard while
     * the display went off in front, so the stop carries the verdict too.
     */
    fun onStop(activity: Activity) = fromActivity(activity, "onStop") {
        sampleNow()
        controller.stopped(interactive(), wakeVerdict)
    }

    fun onPause(activity: Activity) = fromActivity(activity, "onPause") {
        sampleNow()
        controller.paused(interactive())
    }

    fun onTopResumedChanged(activity: Activity, top: Boolean) = fromActivity(activity, "onTopResumedChanged") {
        if (!top) {
            sampleNow()
        }
        controller.topResumedChanged(top, interactive())
    }

    fun onFocusChanged(activity: Activity, focused: Boolean) = fromActivity(activity, "onFocusChanged") {
        if (!focused) {
            sampleNow()
        }
        controller.focusChanged(focused, interactive())
    }

    fun onMultiWindowChanged(activity: Activity, multiWindow: Boolean) =
        fromActivity(activity, "onMultiWindowChanged") { controller.multiWindowChanged(multiWindow) }

    /** Whether the activity should swallow this touch event: one that belongs to a tap brightening a dimmed screen. */
    fun dispatchTouch(activity: Activity, event: MotionEvent): Boolean {
        if (!onMain("dispatchTouch") || slot.current !== activity) {
            return false
        }
        return controller.touch(event.actionMasked, accessibility?.isTouchExplorationEnabled == true)
    }

    /** A key press, a generic motion event or Back. Never swallowed. */
    fun userInteraction(activity: Activity) {
        if (!onMain("userInteraction") || slot.current !== activity) {
            return
        }
        controller.userInteraction()
    }

    /** One status poll tick while the decoder runs, with the record the service read on it (null if none). */
    fun statusTick(context: Context, sessionId: Long, status: DecoderStatus?) {
        if (!onMain("statusTick")) {
            return
        }
        bind(context)
        feed.onTick(running = true, sessionId = sessionId, status = status, wakeAllowed = wakeVerdict)
        applyWindow()
    }

    /** The status poll stopped, by whatever path. Ends the session once however many paths report it. */
    fun statusStopped() {
        if (!onMain("statusStopped")) {
            return
        }
        feed.onTerminated()
    }

    private inline fun fromActivity(activity: Activity, entry: String, event: () -> Unit) {
        if (!onMain(entry) || slot.current !== activity) {
            return
        }
        event()
        applyWindow()
    }

    private fun onMain(entry: String): Boolean {
        if (Looper.myLooper() === Looper.getMainLooper()) {
            return true
        }
        Log.e(TAG, "ScreenSupport.$entry called off the main thread; ignored")
        return false
    }

    /** Takes the application's services from the first caller, and registers the screen receiver once. */
    private fun bind(context: Context) {
        if (bound && receiverRegistered) {
            return
        }
        val app = context.applicationContext
        if (!bound) {
            bound = true
            settings = app.contentResolver
            power = app.getSystemService(PowerManager::class.java)
            audio = app.getSystemService(AudioManager::class.java)
            notifications = app.getSystemService(NotificationManager::class.java)
            accessibility = app.getSystemService(AccessibilityManager::class.java)
        }
        if (receiverRegistered) {
            return
        }
        val filter = IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_ON)
            addAction(Intent.ACTION_SCREEN_OFF)
            addAction(Intent.ACTION_USER_PRESENT)
        }
        try {
            // Exported on purpose: all three actions are <protected-broadcast>s that only system-side senders can send,
            // and USER_PRESENT comes from SystemUI, not the system uid, so a not-exported receiver would never get it.
            ContextCompat.registerReceiver(app, receiver, filter, ContextCompat.RECEIVER_EXPORTED)
            receiverRegistered = true
        } catch (e: RuntimeException) {
            // Left unregistered so the next entry point tries again, rather than running without screen events; the
            // poll would retry every second, so only the first failure is logged.
            if (!receiverFailureLogged) {
                receiverFailureLogged = true
                Log.e(TAG, "could not register the screen receiver; will retry", e)
            }
        }
    }

    private fun screenLocks(): ScreenLocks? {
        locks?.let { return it }
        val power = power ?: return null
        return ScreenLocks(power).also { locks = it }
    }

    private fun interactive(): Boolean = power?.isInteractive ?: true

    /**
     * D5: a call may wake the screen only in the normal audio mode with no Do Not Disturb filter. Two binder reads on
     * the main thread, so only the policy asks, through [wakeVerdict], and only for a sample that would otherwise wake.
     */
    private fun wakeAllowed(): Boolean {
        val audioMode = audio?.mode ?: return false
        val filter = notifications?.currentInterruptionFilter ?: return false
        return ScreenPolicy.wakeAllowed(audioMode, filter)
    }

    private fun screenOffTimeoutMs(): Long {
        val resolver = settings ?: return DEFAULT_SCREEN_OFF_TIMEOUT_MS
        return Settings.System.getLong(resolver, Settings.System.SCREEN_OFF_TIMEOUT, DEFAULT_SCREEN_OFF_TIMEOUT_MS)
    }

    private fun screenOff() {
        sampleNow()
        controller.screenOff()
    }

    /**
     * Reads the decoder status now and samples it, so audio that began since the last poll counts. Only for the session
     * the poll is feeding: before its first tick, and after the engine stopped, there is nothing to add.
     */
    private fun sampleNow() {
        val session = DecoderService.runningSessionId() ?: return
        if (session != controller.state.session) {
            return
        }
        val record = try {
            DsdNative.nativeNotificationStatus()
        } catch (_: UnsatisfiedLinkError) {
            return
        }
        val status = DecoderStatus.parse(record) ?: return
        controller.sample(status.audibleStamp, status.audibleAgeMs, wakeVerdict)
        applyWindow()
    }

    private fun reportRefusal() {
        val session = controller.state.session
        if (refusalReported && refusalSession == session) {
            return
        }
        refusalReported = true
        refusalSession = session
        Log.w(TAG, WAKE_REFUSED)
        try {
            DsdNative.nativeHostDiagnostic(WAKE_REFUSED)
        } catch (_: UnsatisfiedLinkError) {
            // The log line above is all there is without the native side.
        }
    }

    /**
     * Puts the wanted keep-on flag and brightness on the attached activity's window, writing only what differs from the
     * live attributes: Qt may replace them, and a write that changes nothing still relayouts the window.
     */
    private fun applyWindow() {
        val window = slot.current?.window ?: return
        val flag = WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
        if (((window.attributes.flags and flag) != 0) != keepScreenOn) {
            if (keepScreenOn) {
                window.addFlags(flag)
            } else {
                window.clearFlags(flag)
            }
        }
        val brightness = if (dimmed) DIM_BRIGHTNESS else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        val attributes = window.attributes
        if (abs(attributes.screenBrightness - brightness) > BRIGHTNESS_TOLERANCE) {
            attributes.screenBrightness = brightness
            window.attributes = attributes
        }
    }
}
