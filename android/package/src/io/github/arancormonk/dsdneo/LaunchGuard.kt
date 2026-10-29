// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Records a second activity instance in a running process (#782). A record only: it changes nothing about the launch.
 *
 * Qt 6.11.2 cannot take a second activity in a process its first one started: the new instance's `super.onCreate()`
 * sees Qt already started and restarts the application, with `startActivity`, `finish()` and `Runtime.exit(0)` inside
 * `onCreate`, before the app's teardown can run. The manifest prevents that (`singleTask`, and every configuration
 * change handled in place; `tests/android/run_jvm_tests.py` `launch_policy` pins both). What still gets through is
 * recorded here, before `super.onCreate()`, as the process state summary Android keeps with the exit; the next process
 * prints it on that exit's line ([ExitFacts]). Nothing else is done: the reason and status on that line are Android's,
 * and the record says nothing about cleanup.
 *
 * Plain Kotlin with no Android imports, so the JVM tests run it as is; [ProcessFacts.setStateSummary] is the platform
 * call. Main thread only.
 */
object LaunchGuard {
    enum class Launch { FIRST, SECOND_IN_PROCESS }

    /** The process state summary of a process that saw a second instance; flags only (#782 D-A7). */
    const val SECOND_INSTANCE = "activity: second instance in a running process"

    /**
     * What Qt does with that second instance, appended to its exit line. Pinned to the Qt version the manifest's
     * policy was audited against: `launch_policy` fails when the pinned Qt moves, until the restart path is checked
     * again and this text with it.
     */
    const val QT_RESTART = "Qt 6.11.2 restarts the process"

    /** `ActivityManager.setProcessStateSummary` refuses a summary longer than this. */
    const val MAX_SUMMARY_BYTES = 128

    /** Set once the first instance's `super.onCreate()` has started Qt. */
    private var qtStarted = false

    /**
     * First in the activity's launch statements, before `super.onCreate()`: a second instance in this process hands
     * [SECOND_INSTANCE] to [setSummary] and nothing more.
     */
    fun onCreate(setSummary: (ByteArray) -> Unit): Launch {
        if (!qtStarted) {
            return Launch.FIRST
        }
        setSummary(SECOND_INSTANCE.toByteArray(Charsets.UTF_8))
        return Launch.SECOND_IN_PROCESS
    }

    /** Right after the activity's `super.onCreate()` returned: Qt has started in this process. */
    fun afterQt() {
        qtStarted = true
    }
}
