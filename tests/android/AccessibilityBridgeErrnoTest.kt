// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.Activity
import android.system.ErrnoException
import android.system.Os
import android.view.accessibility.AccessibilityManager

// A failed setenv is recorded and leaves Qt's default, the behaviour before #782; it never crashes the launch.
fun main() {
    val am = AccessibilityManager.device
    bind("com.anydesk.anydeskandroid/.AnyDeskAccessibilityService")
    am.setState(enabled = true, touchExploration = false)
    Os.failure = ErrnoException("setenv", 22)
    val a = Activity("main")
    create(a)
    check(Os.getenv(VARIABLE) == null)
    check(Qt.bridgeOn) { "with the variable not set, Qt's default applies" }
    check(Recorder.records == listOf(
        "accessibility: setting QT_ANDROID_DISABLE_ACCESSIBILITY failed (errno 22); Qt's default applies at launch",
        "accessibility: setting QT_ANDROID_DISABLE_ACCESSIBILITY failed (errno 22); the bridge is not frozen, so Qt " +
            "follows later changes",
        "accessibility: bridge on at launch (touch exploration off; accessibility on, 1 service bound)",
    )) { "records: ${Recorder.records}" }
    // The process goes on: the notice still works.
    Os.failure = null
    AccessibilityBridge.onResume(a)
    am.setState(enabled = true, touchExploration = true)
    check(Recorder.shown == listOf(a))
    // And a notice that cannot be shown waits for the next resume.
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onStop(a)
    Recorder.showSucceeds = false
    AccessibilityBridge.onResume(a)
    check(Recorder.records.last() == "accessibility: notice could not be shown; it waits for the next resume")
    Recorder.showSucceeds = true
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onResume(a)
    check(Recorder.shown == listOf(a, a))
    println("PASS: a failed setenv is recorded and the launch goes on")
}
