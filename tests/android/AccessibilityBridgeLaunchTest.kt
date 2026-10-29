// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.Activity
import android.system.Os
import android.view.accessibility.AccessibilityManager

// A process launched with TalkBack on (runs D and T2): Qt turns the bridge on as it starts, and the freeze then holds
// it on through TalkBack going off and on, with no notice.
fun main() {
    val am = AccessibilityManager.device
    bind("com.google.android.marvin.talkback/.TalkBackService")
    am.setState(enabled = true, touchExploration = true)
    Os.env[VARIABLE] = "1" // Left over from anywhere: a launch under TalkBack must clear it.
    val a = Activity("main")
    AccessibilityBridge.effects = Recorder
    AccessibilityBridge.beforeQt(a)
    check(Os.getenv(VARIABLE) == null) { "touch exploration on: the variable is unset before Qt starts" }
    Qt.onCreate()
    check(Qt.bridgeOn && Qt.activations == 1) { "Qt's synchronous check at onCreate turns the bridge on" }
    AccessibilityBridge.afterQt(a)
    check(Os.getenv(VARIABLE) == "1") { "frozen after Qt starts" }
    check(am.touchListeners.size == 1)
    check(Recorder.records == listOf(
        "accessibility: bridge on at launch (touch exploration on; accessibility on, 1 service bound)"
    )) { "launch record: ${Recorder.records}" }
    AccessibilityBridge.onResume(a)
    // TalkBack off with no other service: Qt would tear the bridge down; frozen, it stays on (the documented limit).
    am.setState(enabled = false, touchExploration = false)
    check(Qt.bridgeOn) { "the freeze keeps a bridge that started on" }
    am.setState(enabled = true, touchExploration = true)
    check(Qt.bridgeOn && Qt.activations == 1) { "and Qt never activates mid-session" }
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.isEmpty()) { "a bridge on since launch never asks for a restart" }
    check(Recorder.records.drop(1) == listOf(
        "accessibility: touch exploration turned off; the bridge stays on until the app restarts",
        "accessibility: touch exploration turned on",
    )) { "records: ${Recorder.records}" }
    check(Recorder.records.none { it.contains("talkback", ignoreCase = true) })
    println("PASS: a launch under TalkBack turns the bridge on and the freeze holds it")
}
