// SPDX-License-Identifier: GPL-3.0-or-later
package android.content

import android.view.accessibility.AccessibilityManager

// Just enough Context for the accessibility bridge: its application context and the accessibility manager.
open class Context {
    open val applicationContext: Context get() = this

    fun <T> getSystemService(type: Class<T>): T? =
        if (type == AccessibilityManager::class.java) type.cast(AccessibilityManager.device) else null
}
