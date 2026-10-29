// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.accessibilityservice.AccessibilityServiceInfo
import android.app.Activity
import android.system.Os
import android.view.accessibility.AccessibilityManager

// AccessibilityBridge is process state, so each scenario that needs a fresh process is its own test file, run in its
// own JVM: this one (launched with another service bound), AccessibilityBridgeLaunchTest (TalkBack on at launch) and
// AccessibilityBridgeErrnoTest (setenv fails). The shared rig is here.

internal const val VARIABLE = "QT_ANDROID_DISABLE_ACCESSIBILITY"

/** The bound services as the platform names them; no record may carry these. */
internal val SERVICE_NAMES = listOf(
    "com.anydesk.anydeskandroid/.AnyDeskAccessibilityService",
    "com.catalinagroup.callrecorder/.service.AccessibilityHelperService",
)

/** The platform side, recorded. */
internal object Recorder : AccessibilityBridge.Effects {
    val shown = mutableListOf<Activity>()
    var dismissed = 0
    val records = mutableListOf<String>()
    var showSucceeds = true

    override fun showNotice(activity: Activity): Boolean {
        if (showSucceeds) shown += activity
        return showSucceeds
    }

    override fun dismissNotice() {
        dismissed++
    }

    override fun record(text: String) {
        records += text
    }
}

/**
 * Qt 6.11.2's accessibility listener, as QtAccessibilityDelegate has it (:66-140): registered inside super.onCreate()
 * and called at once if accessibility is enabled; on every call it reads the variable first and does nothing at all
 * when it is "1" or "true", else turns the bridge on or off with the enabled state.
 */
internal object Qt {
    var bridgeOn = false
    var activations = 0
    var envSeen = mutableListOf<String?>()

    private val listener = AccessibilityManager.AccessibilityStateChangeListener { enabled -> changed(enabled) }

    fun onCreate() {
        val am = AccessibilityManager.device
        am.addAccessibilityStateChangeListener(listener)
        if (am.isEnabled) changed(true)
    }

    private fun changed(enabled: Boolean) {
        val value = Os.getenv(VARIABLE)
        envSeen += value
        if (value == "1" || value == "true") return
        if (enabled && !bridgeOn) activations++
        bridgeOn = enabled
    }
}

/** The activity's onCreate, around Qt's. */
internal fun create(activity: Activity) {
    AccessibilityBridge.effects = Recorder
    AccessibilityBridge.beforeQt(activity)
    Qt.onCreate()
    AccessibilityBridge.afterQt(activity)
}

internal fun bind(vararg names: String) {
    AccessibilityManager.device.services = names.map(::AccessibilityServiceInfo)
}

private val am get() = AccessibilityManager.device

private fun aServiceAloneLeavesTheBridgeOffAndFrozen(a: Activity) {
    bind(*SERVICE_NAMES.toTypedArray())
    am.setState(enabled = true, touchExploration = false)
    AccessibilityBridge.effects = Recorder
    AccessibilityBridge.beforeQt(a)
    check(Os.env[VARIABLE] == "1") { "touch exploration off: the variable is 1 before Qt starts" }
    Qt.onCreate()
    check(!Qt.bridgeOn && Qt.activations == 0) { "Qt's synchronous check at onCreate must see 1" }
    AccessibilityBridge.afterQt(a)
    check(Os.env[VARIABLE] == "1")
    check(am.touchListeners.size == 1) { "one touch exploration listener" }
    check(Recorder.records == listOf(
        "accessibility: bridge off at launch (touch exploration off; accessibility on, 2 services bound)"
    )) { "launch record: ${Recorder.records}" }
    // A second afterQt (a second instance in the process) registers nothing more and records the launch once.
    AccessibilityBridge.afterQt(a)
    check(am.touchListeners.size == 1 && Recorder.records.size == 1)
    check(Os.env[VARIABLE] == "1")
}

private fun talkBackWhileResumedShowsTheNoticeOnce(a: Activity) {
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.isEmpty())
    am.setState(enabled = true, touchExploration = true)
    check(Recorder.shown == listOf(a)) { "touch exploration on while resumed shows the notice once" }
    check(!Qt.bridgeOn) { "the frozen variable keeps Qt's bridge off" }
    am.setState(enabled = true, touchExploration = true)
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.size == 1) { "no second dialog while one shows" }
}

private fun aStopClosesTheNoticeAndTheResumeBringsItBack(a: Activity) {
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onStop(a)
    check(Recorder.dismissed == 1) { "onStop closes the showing dialog" }
    check(Recorder.records.last() == "accessibility: notice dropped")
    AccessibilityBridge.onResume(a)
    check(Recorder.shown == listOf(a, a)) { "shown again on the next resume" }
    AccessibilityBridge.noticeAcknowledged()
    check(Recorder.records.last() == "accessibility: notice acknowledged")
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onStop(a)
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.size == 2 && Recorder.dismissed == 1) { "nothing after OK" }
}

private fun whilePausedNothingUntilResume(a: Activity) {
    am.setState(enabled = true, touchExploration = false)
    AccessibilityBridge.onPause(a)
    am.setState(enabled = true, touchExploration = true)
    check(Recorder.shown.size == 2) { "never shown to a paused activity" }
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.size == 3 && Recorder.shown.last() === a)
    AccessibilityBridge.noticeAcknowledged()
}

private fun theFirstServiceTurnedOnAfterLaunchFindsQtFrozen(a: Activity) {
    // No service left, then TalkBack as the first one: the enabled state really flips, so Qt's listener runs (run T1).
    am.setState(enabled = false, touchExploration = false)
    bind()
    am.setState(enabled = true, touchExploration = true)
    check(Qt.envSeen.isNotEmpty() && Qt.envSeen.all { it == "1" }) { "Qt's listener must see 1 on every call" }
    check(!Qt.bridgeOn && Qt.activations == 0) { "the bridge never turns on mid-session" }
    check(Recorder.shown.size == 4) { "and the notice says to reopen the app" }
}

private fun aDestroyedActivityHandsTheNoticeToTheNext(a: Activity) {
    check(Recorder.shown.last() === a)
    AccessibilityBridge.onPause(a)
    AccessibilityBridge.onDestroy(a)
    check(Recorder.dismissed == 2) { "onDestroy closes the dialog" }
    val b = Activity("second")
    Os.calls.clear()
    create(b)
    check(Os.calls == listOf("setenv $VARIABLE=1")) { "a later instance only refreezes: ${Os.calls}" }
    check(Os.env[VARIABLE] == "1" && !Qt.bridgeOn)
    AccessibilityBridge.onResume(a)
    check(Recorder.shown.size == 4) { "the destroyed activity's late resume is ignored" }
    AccessibilityBridge.onResume(b)
    check(Recorder.shown.size == 5 && Recorder.shown.last() === b) { "the pending notice reaches the next activity" }
}

private fun noRecordNamesAService() {
    for (record in Recorder.records) {
        for (name in SERVICE_NAMES) {
            val app = name.substringBefore('/')
            check(!record.contains(app) && !record.contains(name.substringAfter("/."))) { "a record names a service: $record" }
        }
    }
    check(am.serviceListReads == 1) { "the services are counted once, for the launch record" }
    check(Recorder.records.contains("accessibility: notice shown"))
}

fun main() {
    val a = Activity("main")
    check(Os.env[VARIABLE] == null)
    aServiceAloneLeavesTheBridgeOffAndFrozen(a)
    talkBackWhileResumedShowsTheNoticeOnce(a)
    aStopClosesTheNoticeAndTheResumeBringsItBack(a)
    whilePausedNothingUntilResume(a)
    theFirstServiceTurnedOnAfterLaunchFindsQtFrozen(a)
    aDestroyedActivityHandsTheNoticeToTheNext(a)
    noRecordNamesAService()
    println("PASS: the accessibility bridge stays off for a bound service, freezes after Qt starts, and tells TalkBack")
}
