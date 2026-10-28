// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The app's [LooperWatchdog], on a HandlerThread of its own, and the facts its incidents record (#782).
 *
 * Started once, from [ProcessFacts.attach], so it runs in every process start the Activity or the service makes. Its
 * records go through [HostDiagnostics], which holds them until native diagnostics are ready. The facts are published
 * here by the main thread, as the watchdog reads them while that thread may be blocked: [activities] (the lifecycle
 * phase, the activity and window generations, and the touch count) and [screenInputs] (the screen policy's latest
 * inputs, from [ScreenSupport]).
 */
object MainLooperWatchdog {
    private val started = AtomicBoolean(false)

    @Volatile
    private var handler: Handler? = null

    /** Main thread writes; the watchdog reads. */
    val activities = ActivityTrace()

    private class ScreenInputs(val at: Long, val text: String)

    @Volatile
    private var screen: ScreenInputs? = null

    /** Starts the watchdog; any later call does nothing. */
    fun start() {
        if (!started.compareAndSet(false, true)) {
            return
        }
        val thread = HandlerThread("dsd-main-watchdog")
        thread.start()
        val watchdogHandler = Handler(thread.looper)
        // A plain Handler posts synchronous messages, the kind a sync barrier holds back.
        val main = Handler(Looper.getMainLooper())
        val watchdog = LooperWatchdog(
            clock = SystemClock::uptimeMillis,
            timer = { atMs, task -> watchdogHandler.postAtTime(task, atMs) },
            postToMain = { main.post(it) },
            touches = { activities.touches },
            capture = ::capture,
            emit = HostDiagnostics::submit,
        )
        handler = watchdogHandler
        watchdog.start()
    }

    /** Runs [task] on the watchdog's thread; false when the watchdog has not started. */
    fun post(task: () -> Unit): Boolean = handler?.post { task() } ?: false

    /** The screen policy's inputs as [ScreenSupport] last applied them; main thread. */
    fun screenInputs(text: String) {
        screen = ScreenInputs(SystemClock.uptimeMillis(), text)
    }

    private fun capture(): LooperWatchdog.Capture {
        // Thread.getStackTrace() names native methods too, as Java declares them; that is all it can see. The state is
        // read first: taking the stack suspends the thread for a moment.
        val thread = Looper.getMainLooper().thread
        val state = thread.state.name
        val stack = thread.stackTrace.map { it.toString() }
        val inputs = screen
        val screenText = if (inputs == null) {
            "not applied yet"
        } else {
            "as of ${SystemClock.uptimeMillis() - inputs.at} ms before: ${inputs.text}"
        }
        return LooperWatchdog.Capture(stack, activities.snapshot.toString(), screenText, state)
    }
}
