// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Holds the one live instance of something recreated, such as the activity, by identity. Android can create the new
 * activity before the old one's onDestroy runs, so a detach of anything but the current instance is ignored.
 */
class ActivitySlot<T : Any> {
    var current: T? = null
        private set

    fun attach(instance: T) {
        current = instance
    }

    /** Clears the slot if [instance] is what it holds; returns whether it did. */
    fun detach(instance: T): Boolean {
        if (current !== instance) {
            return false
        }
        current = null
        return true
    }
}
