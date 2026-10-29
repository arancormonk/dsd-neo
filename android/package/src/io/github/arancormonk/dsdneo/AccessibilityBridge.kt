// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.accessibilityservice.AccessibilityServiceInfo
import android.app.Activity
import android.system.ErrnoException
import android.system.Os
import android.view.accessibility.AccessibilityManager

/**
 * Keeps Qt's Android accessibility bridge off unless touch exploration is on at launch (#782), and tells the user when
 * touch exploration turns on later. The decisions are [AccessibilityGate]'s; this feeds it and carries them out.
 *
 * Qt 6.11.2 turns its bridge on for any enabled accessibility service, and while the app scans the bridge floods the
 * main thread. Qt reads `QT_ANDROID_DISABLE_ACCESSIBILITY` synchronously inside the activity's `super.onCreate()`, and
 * again on each later change of the enabled state; only `1` or `true` keeps the bridge off, any other value leaves Qt's
 * default (QtAccessibilityDelegate.java:94-95 at v6.11.2). So:
 * - [beforeQt], just before `super.onCreate()`, sets the variable from the launch's facts: `1` keeps the bridge off,
 *   `0` under touch exploration lets Qt turn it on;
 * - [afterQt], just after, makes it `1` for the rest of the process: turning the bridge on mid-session crashes Qt
 *   6.11.2 (QTBUG-144911), and turning it off would leave a screen reader with nothing to read. Touch exploration
 *   turned on later gets the notice instead, through [Effects].
 *
 * The environment rule: once `super.onCreate()` has started Qt's `main()` on the Qt thread, nothing on any thread adds,
 * removes or reallocates an environment entry, since that thread reads the environment (Qt's own variables, the app's
 * config) with no lock. Bionic's `setenv` of an absent name grows `environ` with `reallocarray`, which can free the
 * array a concurrent `getenv` is walking. So:
 * - [beforeQt] writes `0` rather than unsetting, so the entry exists before Qt starts, and it writes only on the
 *   process's first launch, before any Qt thread exists;
 * - [afterQt] writes only into an entry that exists and is not already `1`. That is one in-place pointer swap, in a
 *   launch under touch exploration only: the old string is leaked, not freed, and a concurrent `getenv` of that one
 *   name may briefly see the new, still-filling string. That window is accepted. Every other launch, and every later
 *   activity, writes nothing after Qt starts. An entry that is absent because the launch write failed stays absent:
 *   the bridge is then not frozen, Qt's default applies, and that is recorded.
 * - `QV4_GC_TIMELIMIT`, which the Qt side used to set from `main()`, is set by [QmlGcPolicy] before Qt starts.
 *
 * The notice follows the activity: it is shown only while the attached activity is resumed, closed at its stop or
 * destroy and shown again at the next resume, until the user's OK ([noticeAcknowledged]).
 *
 * Main thread only: the activity's callbacks, and the touch exploration listener, which the one-argument
 * `addTouchExplorationStateChangeListener` runs on the main looper. A failed `setenv` is recorded and leaves Qt's
 * default, which is the behaviour before #782.
 */
object AccessibilityBridge {
    /** The variable Qt's accessibility listener reads on every call. */
    const val DISABLE_VARIABLE = "QT_ANDROID_DISABLE_ACCESSIBILITY"

    /** The platform side: the notice dialog and the host diagnostics. */
    interface Effects {
        /** Shows the notice on [activity]'s window; false if it could not. */
        fun showNotice(activity: Activity): Boolean

        /** Closes the notice without the user's OK. */
        fun dismissNotice()

        /** A host diagnostics record. */
        fun record(text: String)
    }

    /** Set once, before [beforeQt]: [AccessibilityRestartNotice] in the app. */
    lateinit var effects: Effects

    private val gate = AccessibilityGate<Activity> { effects.record(it) }
    private var manager: AccessibilityManager? = null
    private var launchFacts: A11yFacts? = null
    private var launchRecorded = false

    private var listening = false

    private val listener = AccessibilityManager.TouchExplorationStateChangeListener { refresh() }

    /** Just before the activity's `super.onCreate()`: the first call decides for the process and sets the variable. */
    fun beforeQt(activity: Activity) {
        if (gate.plan != null) {
            // The variable stays frozen at "1" from the first afterQt.
            return
        }
        val facts = readFacts(activity)
        val wanted = LaunchPlan.of(facts)
        val written = setVariable(wanted.env, "Qt's default applies at launch")
        // Unwritten, Qt reads whatever value is there, and the gate is told what Qt actually does. Usually there is
        // none: the bridge turns on with any enabled service, and since afterQt never creates the entry, Qt goes on
        // following the enabled state, so no restart is ever owed. A value left over from elsewhere is frozen by
        // afterQt in place; the gate then asks for a restart exactly when that value keeps the bridge off under touch
        // exploration.
        val plan = if (written) {
            wanted
        } else {
            val present = Os.getenv(DISABLE_VARIABLE)
            wanted.copy(activated = facts.enabled && !qtKeepsBridgeOff(present), qtFollows = present == null)
        }
        gate.launch(facts, plan)
        launchFacts = facts
    }

    /** Just after the activity's `super.onCreate()`: freezes the variable, attaches [activity], records the launch. */
    fun afterQt(activity: Activity) {
        // Qt's main() may already be reading the environment: write only into an existing entry that is not 1 yet.
        // Absent only when the launch's write failed: the launch record says Qt follows later changes.
        val now = Os.getenv(DISABLE_VARIABLE)
        if (now != null && now != "1") {
            setVariable("1", "the bridge is not frozen, so Qt follows later changes")
        }
        gate.attach(activity)
        val am = manager
        if (!listening && am != null) {
            am.addTouchExplorationStateChangeListener(listener)
            listening = true
        }
        val facts = launchFacts
        val plan = gate.plan
        if (!launchRecorded && facts != null && plan != null) {
            launchRecorded = true
            effects.record(gate.record(facts, plan))
        }
        // Touch exploration may have changed while Qt started.
        refresh()
    }

    fun onResume(activity: Activity) {
        // The facts first: an edge missed while away becomes pending, and the resume below shows it once.
        refresh()
        if (gate.resumed(activity) == Notice.SHOW) {
            show(activity)
        }
    }

    fun onPause(activity: Activity) {
        gate.paused(activity)
    }

    fun onStop(activity: Activity) {
        drop(activity)
    }

    fun onDestroy(activity: Activity) {
        drop(activity)
        gate.detach(activity)
    }

    /** The notice's OK. */
    fun noticeAcknowledged() {
        gate.noticeAcknowledged()
    }

    private fun drop(activity: Activity) {
        gate.paused(activity)
        if (gate.showingOn(activity)) {
            effects.dismissNotice()
            gate.noticeDropped()
        }
    }

    private fun refresh() {
        val am = manager ?: return
        val notice = gate.onFacts(factsOf(am, countServices = false))
        if (notice == Notice.SHOW) {
            // SHOW only comes for the attached, resumed activity.
            gate.owner?.let(::show)
        }
    }

    private fun show(activity: Activity) {
        if (effects.showNotice(activity)) {
            gate.noticeShown(activity)
        } else {
            effects.record("accessibility: notice could not be shown; it waits for the next resume")
        }
    }

    private fun readFacts(activity: Activity): A11yFacts {
        val am = manager ?: activity.applicationContext.getSystemService(AccessibilityManager::class.java)
        manager = am
        return if (am == null) {
            A11yFacts(enabled = false, touchExploration = false, boundServices = 0)
        } else {
            factsOf(am, countServices = true)
        }
    }

    /**
     * The flags are cached in the process; the service list is a call into the system, made only for the launch record.
     * The gate never reads the count, so later reads leave it 0.
     */
    private fun factsOf(am: AccessibilityManager, countServices: Boolean): A11yFacts = A11yFacts(
        enabled = am.isEnabled,
        touchExploration = am.isTouchExplorationEnabled,
        boundServices = if (countServices) {
            am.getEnabledAccessibilityServiceList(AccessibilityServiceInfo.FEEDBACK_ALL_MASK).size
        } else {
            0
        },
    )

    /** Sets the variable; on failure records it with its [consequence] and returns false. */
    private fun setVariable(value: String, consequence: String): Boolean = try {
        Os.setenv(DISABLE_VARIABLE, value, true)
        true
    } catch (e: ErrnoException) {
        effects.record("accessibility: setting $DISABLE_VARIABLE failed (errno ${e.errno}); $consequence")
        false
    }
}
