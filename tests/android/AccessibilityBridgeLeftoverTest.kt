// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.Activity
import android.system.ErrnoException
import android.system.Os
import android.view.accessibility.AccessibilityManager

// A launch under TalkBack whose write of 0 fails over a 1 already in the environment (#782 T4 N3): Qt reads that 1
// and keeps the bridge off, so the gate must know it is off and owe the user the notice.
fun main() {
    val am = AccessibilityManager.device
    bind("com.google.android.marvin.talkback/.TalkBackService")
    am.setState(enabled = true, touchExploration = true)
    Os.env[VARIABLE] = "1"
    Os.failure = ErrnoException("setenv", 12)
    Os.failuresLeft = 1
    val a = Activity("main")
    AccessibilityBridge.effects = Recorder
    AccessibilityBridge.beforeQt(a)
    check(Os.getenv(VARIABLE) == "1")
    Qt.onCreate()
    check(!Qt.bridgeOn) { "Qt reads the leftover 1 and keeps the bridge off" }
    AccessibilityBridge.afterQt(a)
    check(Os.calls == listOf("setenv $VARIABLE=0")) { "already 1: nothing more is written: ${Os.calls}" }
    check(Recorder.records == listOf(
        "accessibility: setting QT_ANDROID_DISABLE_ACCESSIBILITY failed (errno 12); Qt's default applies at launch",
        "accessibility: bridge off at launch (touch exploration on; accessibility on, 1 service bound); " +
            "restart notice pending",
    )) { "the record must match Qt's real state: ${Recorder.records}" }
    AccessibilityBridge.onResume(a)
    check(Recorder.shown == listOf(a)) { "the TalkBack user is told to reopen the app" }
    println("PASS: a failed write over a leftover 1 leaves the bridge off, and the gate owes the notice")
}
