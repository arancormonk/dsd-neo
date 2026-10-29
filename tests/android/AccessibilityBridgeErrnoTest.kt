// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.Activity
import android.system.ErrnoException
import android.system.Os
import android.view.accessibility.AccessibilityManager

// A failed setenv is recorded and leaves Qt's default, the behaviour before #782; it never crashes the launch, and the
// gate follows what Qt actually did.
fun main() {
    val am = AccessibilityManager.device
    bind("com.anydesk.anydeskandroid/.AnyDeskAccessibilityService")
    am.setState(enabled = true, touchExploration = false)
    Os.failure = ErrnoException("setenv", 22)
    val a = Activity("main")
    create(a)
    check(Os.getenv(VARIABLE) == null)
    check(Qt.bridgeOn) { "with the variable not set, Qt's default turns the bridge on for the bound service" }
    check(Recorder.records == listOf(
        "accessibility: setting QT_ANDROID_DISABLE_ACCESSIBILITY failed (errno 22); Qt's default applies at launch",
        "accessibility: setting QT_ANDROID_DISABLE_ACCESSIBILITY failed (errno 22); the bridge is not frozen, so Qt " +
            "follows later changes",
        "accessibility: bridge on at launch (touch exploration off; accessibility on, 1 service bound)",
    )) { "records: ${Recorder.records}" }
    // The process goes on. TalkBack can already read it, so turning TalkBack on asks for no restart.
    Os.failure = null
    AccessibilityBridge.onResume(a)
    am.setState(enabled = true, touchExploration = true)
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.isEmpty()) { "no restart notice for a bridge Qt's default turned on" }
    println("PASS: a failed setenv is recorded, the launch goes on, and the gate follows Qt's default")
}
