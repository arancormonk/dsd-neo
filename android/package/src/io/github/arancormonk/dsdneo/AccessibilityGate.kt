// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * What [AccessibilityManager][android.view.accessibility.AccessibilityManager] says (#782): whether any accessibility
 * service is on, whether touch exploration (a screen reader such as TalkBack) is on, and how many services are bound.
 * The count only goes into the launch record; no other app's name is ever kept.
 */
data class A11yFacts(val enabled: Boolean, val touchExploration: Boolean, val boundServices: Int)

/**
 * Whether the app wants Qt's accessibility bridge: only under touch exploration (#782 D-A1). Any other bound service,
 * such as a remote-control app or a call recorder, would turn Qt's bridge on too, and the bridge floods the main thread
 * while the app scans.
 */
fun bridgeWanted(f: A11yFacts): Boolean = f.touchExploration

/**
 * The launch's decision. [env] is what `QT_ANDROID_DISABLE_ACCESSIBILITY` holds while Qt starts: `"1"` keeps the bridge
 * off; `"0"` leaves Qt's default, since Qt keeps it off only for `1` or `true`. It is written rather than unset so the
 * freeze after Qt starts never adds an environment entry (see [AccessibilityBridge]). [activated] is whether Qt turns
 * the bridge on as it starts, which it does only when accessibility is enabled at that moment.
 */
data class LaunchPlan(val wanted: Boolean, val activated: Boolean, val env: String) {
    companion object {
        fun of(f: A11yFacts): LaunchPlan {
            val wanted = bridgeWanted(f)
            return LaunchPlan(wanted, wanted && f.enabled, if (wanted) "0" else "1")
        }
    }
}

/** What the platform side does with the notice: nothing, show it now, or keep it for the owner's next resume. */
enum class Notice { NONE, SHOW, PENDING }

/**
 * The accessibility bridge's process state (#782). Qt reads `QT_ANDROID_DISABLE_ACCESSIBILITY` as it starts and on each
 * later change of the enabled state, and turning the bridge on later is unsafe on Qt 6.11.2, so the first [launch]
 * decides for the whole process and the variable is frozen right after. Touch exploration turned on after a launch that
 * left the bridge off owes the user a notice: the app has to be closed and opened again.
 *
 * The notice is owned by the process, not by an activity: it is shown only to the attached activity while resumed,
 * waits ([Notice.PENDING]) while there is none, and goes back to waiting when its dialog is closed without OK
 * ([noticeDropped]). After OK nothing is shown until touch exploration goes off and on again. Callbacks from an
 * activity other than the attached one are ignored, as Android may create the next activity before the last one's
 * onDestroy.
 *
 * Main thread only. Every notice transition goes to [log], for the host diagnostics.
 */
class AccessibilityGate<A : Any>(private val log: (String) -> Unit) {
    private enum class State { IDLE, PENDING, SHOWING }

    private val slot = ActivitySlot<A>()
    private var ownerResumed = false
    private var state = State.IDLE
    private var shownOn: A? = null
    private var lastWanted = false

    /** The process's plan, once [launch] has decided it. */
    var plan: LaunchPlan? = null
        private set

    /** The attached activity, if any. */
    val owner: A? get() = slot.current

    /** Whether a notice is owed and not on screen. */
    val noticePending: Boolean get() = state == State.PENDING

    /**
     * The first call decides for the process; later calls return that plan, whatever [f] says now. [decided] is the
     * plan Qt actually starts with: [LaunchPlan.of] unless the variable could not be written.
     */
    fun launch(f: A11yFacts, decided: LaunchPlan = LaunchPlan.of(f)): LaunchPlan {
        plan?.let { return it }
        plan = decided
        lastWanted = decided.wanted
        if (decided.wanted && !decided.activated) {
            state = State.PENDING
        }
        return decided
    }

    fun attach(activity: A) {
        slot.attach(activity)
        ownerResumed = false
    }

    fun detach(activity: A) {
        if (!slot.detach(activity)) {
            return
        }
        ownerResumed = false
        if (state == State.SHOWING && shownOn === activity) {
            // The platform side drops a showing notice before it detaches; this only keeps the notice from being lost.
            state = State.PENDING
            shownOn = null
        }
    }

    /** The attached activity resumed: [Notice.SHOW] if a notice waits for it. */
    fun resumed(activity: A): Notice {
        if (slot.current !== activity) {
            return Notice.NONE
        }
        ownerResumed = true
        return if (state == State.PENDING) Notice.SHOW else Notice.NONE
    }

    fun paused(activity: A) {
        if (slot.current === activity) {
            ownerResumed = false
        }
    }

    /** New facts. Only a rising edge of [bridgeWanted] on a process whose bridge is off asks for the notice. */
    fun onFacts(f: A11yFacts): Notice {
        val launched = plan ?: return Notice.NONE
        val wanted = bridgeWanted(f)
        if (wanted == lastWanted) {
            return Notice.NONE
        }
        lastWanted = wanted
        if (launched.activated) {
            log(
                if (wanted) "accessibility: touch exploration turned on"
                else "accessibility: touch exploration turned off; the bridge stays on until the app restarts"
            )
            return Notice.NONE
        }
        if (!wanted) {
            log("accessibility: touch exploration turned off")
            // A notice not yet on screen is moot now; one on screen stays until its OK.
            if (state == State.PENDING) {
                state = State.IDLE
            }
            return Notice.NONE
        }
        log("accessibility: touch exploration turned on; the bridge stays off until the app restarts")
        if (state == State.IDLE) {
            state = State.PENDING
        }
        if (state != State.PENDING) {
            return Notice.NONE
        }
        return if (slot.current != null && ownerResumed) Notice.SHOW else Notice.PENDING
    }

    /** The platform side put the notice on [activity]'s window. */
    fun noticeShown(activity: A) {
        if (state != State.PENDING || slot.current !== activity) {
            return
        }
        state = State.SHOWING
        shownOn = activity
        log("accessibility: notice shown")
    }

    /** Whether the notice is on [activity]'s window, so its stop or destroy has to close it. */
    fun showingOn(activity: A): Boolean = state == State.SHOWING && shownOn === activity

    /** The user dismissed the notice. Nothing more is shown until touch exploration goes off and on again. */
    fun noticeAcknowledged() {
        if (state != State.SHOWING) {
            return
        }
        state = State.IDLE
        shownOn = null
        log("accessibility: notice acknowledged")
    }

    /**
     * The notice's dialog was closed without the user (a stop or destroy): it waits for the next resume, unless touch
     * exploration has gone off since, which makes it moot.
     */
    fun noticeDropped() {
        if (state != State.SHOWING) {
            return
        }
        state = if (lastWanted) State.PENDING else State.IDLE
        shownOn = null
        log("accessibility: notice dropped")
    }

    /** The launch record (#782 D-A7): flags and counts only, never another app's name. */
    fun record(f: A11yFacts, plan: LaunchPlan): String {
        val bridge = if (plan.activated) "on" else "off"
        val touch = if (f.touchExploration) "on" else "off"
        val enabled = if (f.enabled) "on" else "off"
        val services = if (f.boundServices == 1) "1 service" else "${f.boundServices} services"
        val notice = if (plan.wanted && !plan.activated) "; restart notice pending" else ""
        return "accessibility: bridge $bridge at launch (touch exploration $touch; accessibility $enabled, " +
            "$services bound)$notice"
    }
}
