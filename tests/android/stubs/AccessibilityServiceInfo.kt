// SPDX-License-Identifier: GPL-3.0-or-later
package android.accessibilityservice

// A bound service, named as the platform names it; the app may count these but never record their names.
class AccessibilityServiceInfo(val id: String) {
    companion object {
        const val FEEDBACK_ALL_MASK = -1
    }
}
