// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.system.ErrnoException
import android.system.Os

/**
 * The QML garbage collector's policy for the app (`src/ui/qt/qml_gc_policy.h`): `QV4_GC_TIMELIMIT=0`, so each
 * collection runs to completion, which keeps Qt 6.11.2 from freeing a QML object's function storage mid-creation.
 *
 * Set here, before the activity's `super.onCreate()`, rather than by `applyQmlGcPolicy()` in `main()`: once
 * `super.onCreate()` has started `main()` on the Qt thread, no thread may add an environment entry (#782; see
 * [AccessibilityBridge]), and `main()` adding this one could free the `environ` array under the main thread's own
 * reads. Only the process's first call writes, before any Qt thread exists; later activities write nothing.
 *
 * A debuggable build's `extraenvvars` are applied by Qt's loader inside `super.onCreate()`, after this, so a developer
 * can still bring the time slices back. Main thread only.
 */
object QmlGcPolicy {
    const val VARIABLE = "QV4_GC_TIMELIMIT"

    private var applied = false

    /** Just before the activity's `super.onCreate()`. A failure is recorded; the collector then keeps its slices. */
    fun beforeQt(record: (String) -> Unit) {
        if (applied) {
            return
        }
        applied = true
        try {
            Os.setenv(VARIABLE, "0", true)
        } catch (e: ErrnoException) {
            record("qml: setting $VARIABLE failed (errno ${e.errno}); the collector keeps its time slices")
        }
    }
}
