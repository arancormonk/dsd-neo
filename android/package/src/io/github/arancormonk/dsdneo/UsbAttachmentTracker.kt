// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/** Android-free attach dedupe. The manager serializes access with its USB lock. */
internal object UsbAttachmentTracker {
    data class Identity(val deviceName: String, val vendorId: Int, val productId: Int)
    private val seen = mutableSetOf<Identity>()
    private var pending: Identity? = null

    fun reconcile(current: Set<Identity>) {
        seen.retainAll(current)
        if (pending !in current) pending = null
    }

    fun report(device: Identity, current: Set<Identity>): Boolean {
        reconcile(current)
        if (device !in current || !seen.add(device)) return false
        // One pending attachment: the newest validated device wins.
        pending = device
        return true
    }

    fun detach(device: Identity) {
        seen.remove(device)
        if (pending == device) pending = null
    }

    fun take(current: Set<Identity>): String {
        reconcile(current)
        val name = pending?.deviceName.orEmpty()
        pending = null
        return name
    }
}
