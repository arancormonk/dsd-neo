// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.Activity
import android.system.ErrnoException
import android.system.Os
import android.view.accessibility.AccessibilityManager

// The launch's writes fail and the freeze would succeed (#782 T4 N1). Each failure is recorded and leaves Qt's
// default, the behaviour before #782; nothing crashes, the gate follows what Qt actually did, and the freeze never
// creates the absent entry once Qt's main() runs.
fun main() {
    val am = AccessibilityManager.device
    bind("com.anydesk.anydeskandroid/.AnyDeskAccessibilityService")
    am.setState(enabled = true, touchExploration = false)
    Os.failure = ErrnoException("setenv", 22)
    Os.failuresLeft = 2 // The collector's write and the bridge's launch write; any later write would succeed.
    val a = Activity("main")
    create(a)
    check(Os.calls == listOf("setenv QV4_GC_TIMELIMIT=0", "setenv $VARIABLE=1")) {
        "the freeze must not create the absent entry after Qt starts: ${Os.calls}"
    }
    check(Os.getenv(VARIABLE) == null && Os.getenv(QmlGcPolicy.VARIABLE) == null)
    check(Qt.bridgeOn) { "with the variable not set, Qt's default turns the bridge on for the bound service" }
    check(Recorder.records == listOf(
        "qml: setting QV4_GC_TIMELIMIT failed (errno 22); the collector keeps its time slices",
        "accessibility: setting QT_ANDROID_DISABLE_ACCESSIBILITY failed (errno 22); Qt's default applies at launch",
        "accessibility: the variable is unset; the bridge is not frozen, so Qt follows later changes",
        "accessibility: bridge on at launch (touch exploration off; accessibility on, 1 service bound)",
    )) { "records: ${Recorder.records}" }
    // The process goes on. TalkBack can already read it, so turning TalkBack on asks for no restart.
    AccessibilityBridge.onResume(a)
    am.setState(enabled = true, touchExploration = true)
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.isEmpty()) { "no restart notice for a bridge Qt's default turned on" }
    // A later activity writes nothing either, and does not record the missing freeze again.
    val records = Recorder.records.size
    create(Activity("second"))
    check(Os.calls.size == 2) { "a later activity writes nothing: ${Os.calls}" }
    check(Recorder.records.size == records)
    println("PASS: failed launch writes are recorded, and the freeze never creates the entry once Qt runs")
}
