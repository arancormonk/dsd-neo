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
    Os.env[VARIABLE] = "1" // Left over from anywhere: a launch under TalkBack must replace it.
    val a = Activity("main")
    AccessibilityBridge.effects = Recorder
    AccessibilityBridge.beforeQt(a)
    // 0, not unset: the entry exists before Qt starts, so the freeze below only replaces it in place (#782 T4 I1).
    check(Os.getenv(VARIABLE) == "0") { "touch exploration on: the variable is 0 before Qt starts" }
    check(Os.calls == listOf("setenv $VARIABLE=0")) { "calls: ${Os.calls}" }
    Qt.onCreate()
    check(Qt.bridgeOn && Qt.activations == 1) { "Qt's synchronous check at onCreate reads 0 as on" }
    // TalkBack goes off while Qt starts, before our listener exists: afterQt reads it.
    am.setStateSilently(enabled = true, touchExploration = false)
    AccessibilityBridge.afterQt(a)
    check(Os.getenv(VARIABLE) == "1") { "frozen after Qt starts" }
    check(Os.calls == listOf("setenv $VARIABLE=0", "setenv $VARIABLE=1")) { "0 replaced by 1: ${Os.calls}" }
    check(am.touchListeners.size == 1)
    check(Recorder.records == listOf(
        "accessibility: bridge on at launch (touch exploration on; accessibility on, 1 service bound)",
        "accessibility: touch exploration turned off; the bridge stays on until the app restarts",
    )) { "afterQt must read the facts: ${Recorder.records}" }
    AccessibilityBridge.onResume(a)
    // TalkBack off with no other service: Qt would tear the bridge down; frozen, it stays on (the documented limit).
    am.setState(enabled = false, touchExploration = false)
    check(Qt.bridgeOn) { "the freeze keeps a bridge that started on" }
    am.setState(enabled = true, touchExploration = true)
    check(Qt.bridgeOn && Qt.activations == 1) { "and Qt never activates mid-session" }
    check(Qt.envSeen.drop(1).all { it == "1" }) { "Qt's later calls all see 1: ${Qt.envSeen}" }
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.isEmpty()) { "a bridge on since launch never asks for a restart" }
    check(Recorder.records.drop(2) == listOf("accessibility: touch exploration turned on")) {
        "records: ${Recorder.records}"
    }
    check(Recorder.records.none { it.contains("talkback", ignoreCase = true) })
    check(Os.calls.size == 2) { "nothing more is written: ${Os.calls}" }
    println("PASS: a launch under TalkBack writes 0 before Qt, then replaces it with 1, and the freeze holds the bridge")
}
