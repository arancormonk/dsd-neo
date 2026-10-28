// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.util.Log

/**
 * Host diagnostics from Kotlin that may come before the native library is ready (#782): the main-looper watchdog, the
 * screen trace and the teardown markers. Each record also goes to logcat at once.
 *
 * Until native diagnostics are ready, records wait in a [PendingHostRecords] of [CAPACITY]; then they go out through
 * [DsdNative.nativeHostDiagnostic], which never waits for the native log. Ready means either of two things happened:
 * native code asked [ProcessFacts] for the process facts, which it does as the diagnostics log installs, or [DsdNative]
 * loaded the library. Until then nothing here touches [DsdNative], so a record never loads the library early.
 */
object HostDiagnostics {
    private const val TAG = "dsd-neo"

    /** How many records wait for the native side; later ones are counted, and the count is reported. */
    const val CAPACITY = 64

    private val pending = PendingHostRecords(CAPACITY) { line ->
        try {
            DsdNative.nativeHostDiagnostic(line)
        } catch (e: LinkageError) {
            Log.w(TAG, "host diagnostic not delivered: ${e.javaClass.simpleName}")
        }
    }

    /** Any thread. */
    fun submit(line: String) {
        Log.i(TAG, line)
        pending.submit(line)
    }

    /**
     * Native diagnostics can take records. The ones waiting go out on the watchdog's thread, so neither the library's
     * loading nor the diagnostics log's install calls back into native code from here; with no watchdog running, they
     * go ahead of the next record.
     */
    fun nativeReady() {
        if (pending.isReady) {
            return
        }
        pending.markReady()
        MainLooperWatchdog.post { pending.drain() }
    }
}
