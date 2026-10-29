// SPDX-License-Identifier: GPL-3.0-or-later
package android.view.accessibility

import android.accessibilityservice.AccessibilityServiceInfo

// The process's accessibility manager, driven by the test. [setState] does what AOSP's setStateLocked does: it writes
// both cached flags first, then runs the state listeners if the enabled flag changed, then the touch exploration
// listeners if that flag changed (AccessibilityManager.java:1523-1549), each on the main looper, which here is the
// test's own thread.
class AccessibilityManager {
    fun interface AccessibilityStateChangeListener {
        fun onAccessibilityStateChanged(enabled: Boolean)
    }

    fun interface TouchExplorationStateChangeListener {
        fun onTouchExplorationStateChanged(enabled: Boolean)
    }

    var isEnabled = false
        private set
    var isTouchExplorationEnabled = false
        private set
    var services: List<AccessibilityServiceInfo> = emptyList()
    val stateListeners = linkedSetOf<AccessibilityStateChangeListener>()
    val touchListeners = linkedSetOf<TouchExplorationStateChangeListener>()
    var serviceListReads = 0

    fun setState(enabled: Boolean, touchExploration: Boolean) {
        val enabledChanged = enabled != isEnabled
        val touchChanged = touchExploration != isTouchExplorationEnabled
        isEnabled = enabled
        isTouchExplorationEnabled = touchExploration
        if (enabledChanged) stateListeners.toList().forEach { it.onAccessibilityStateChanged(enabled) }
        if (touchChanged) touchListeners.toList().forEach { it.onTouchExplorationStateChanged(touchExploration) }
    }

    fun getEnabledAccessibilityServiceList(feedbackTypeFlags: Int): List<AccessibilityServiceInfo> {
        check(feedbackTypeFlags == AccessibilityServiceInfo.FEEDBACK_ALL_MASK) { "every feedback type is counted" }
        serviceListReads++
        return services
    }

    fun addAccessibilityStateChangeListener(listener: AccessibilityStateChangeListener): Boolean =
        stateListeners.add(listener)

    fun addTouchExplorationStateChangeListener(listener: TouchExplorationStateChangeListener): Boolean =
        touchListeners.add(listener)

    companion object {
        val device = AccessibilityManager()
    }
}
