// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.Activity
import android.system.ErrnoException
import android.system.Os
import android.view.accessibility.AccessibilityManager

// The launch's writes fail with accessibility off at launch (#782 T4 r3 M1). Nothing freezes the bridge, so when
// TalkBack later turns on as the first service, Qt turns the bridge on itself, as before #782. The gate must know
// that: its record says Qt follows later changes, and it never tells the user to restart.
fun main() {
    val am = AccessibilityManager.device
    bind()
    am.setState(enabled = false, touchExploration = false)
    Os.failure = ErrnoException("setenv", 12)
    Os.failuresLeft = 2 // The collector's write and the bridge's launch write.
    val a = Activity("main")
    create(a)
    check(Os.calls == listOf("setenv QV4_GC_TIMELIMIT=0", "setenv $VARIABLE=1")) { "no freeze write: ${Os.calls}" }
    check(Os.getenv(VARIABLE) == null && !Qt.bridgeOn)
    check(Recorder.records == listOf(
        "qml: setting QV4_GC_TIMELIMIT failed (errno 12); the collector keeps its time slices",
        "accessibility: setting QT_ANDROID_DISABLE_ACCESSIBILITY failed (errno 12); Qt's default applies at launch",
        "accessibility: bridge off at launch (touch exploration off; accessibility off, 0 services bound); " +
            "not frozen, so Qt follows later changes",
    )) { "the record must say what Qt will do: ${Recorder.records}" }
    AccessibilityBridge.onResume(a)
    bind("com.google.android.marvin.talkback/.TalkBackService")
    am.setState(enabled = true, touchExploration = true)
    check(Qt.bridgeOn && Qt.activations == 1) { "unfrozen, Qt turns the bridge on mid-session itself" }
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.isEmpty()) { "the bridge is on: no restart notice" }
    check(Recorder.records.last() ==
        "accessibility: touch exploration turned on; the bridge is not frozen, so Qt follows it") {
        "records: ${Recorder.records}"
    }
    check(Os.calls.size == 2) { "nothing more is written: ${Os.calls}" }
    println("PASS: a failed launch write with accessibility off leaves Qt following, and asks for no restart")
}
