// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Which attached RTL-SDRs owe a USB reset before their next open (#782).
 *
 * A stall can wedge a dongle's bulk stream so that closing and reopening the same device brings no samples back; a
 * USB reset through the app's own descriptor clears it. The engine says which descriptor owes one
 * ([DsdNative.nativeUsbResetOwed]), and [UsbSourceManager] records that here on the physical attachment it handed the
 * descriptor with: the UsbDevice name, which a re-enumeration changes, and the broker's attach epoch, with the VID:PID
 * and serial that make an entry this device's. Each attachment keeps its own until a reset of it succeeds, samples show
 * it streams without one, or it is detached; selecting another receiver meanwhile never applies it there.
 *
 * Entries outlive the process ([encode], [restore]): a new process can meet a dongle the last one left wedged. At
 * process start an entry whose device is no longer attached with that VID:PID is dropped, since its detach means it
 * re-enumerated, and one still attached is honoured on its first open. The worst a stale entry costs is one reset that
 * was not needed. Plain Kotlin; [UsbSourceManager] makes every call on the main thread.
 */
internal class UsbResetObligations {
    /**
     * One physical attachment: the device [name], its attach [epoch] (0 when it was already attached when the process
     * started), its VID:PID, and its [serial], or "" when it could not be read.
     */
    data class Attachment(val name: String, val epoch: Long, val vendorId: Int, val productId: Int, val serial: String)

    private val owed = LinkedHashMap<String, Attachment>()

    /** The attachments that owe a reset, oldest first. */
    val entries: List<Attachment> get() = owed.values.toList()

    /**
     * Whether the device [name], at attach [epoch], with [vendorId]:[productId] and [serial] ("" when unreadable),
     * owes a reset before it is opened. An entry under that name that turns out to be another device (another VID:PID,
     * a serial both sides read and that differs, or another attachment) is dropped.
     */
    fun owes(name: String, epoch: Long, vendorId: Int, productId: Int, serial: String): Boolean {
        val entry = owed[name] ?: return false
        val serialDiffers = entry.serial.isNotEmpty() && serial.isNotEmpty() && !entry.serial.equals(serial, ignoreCase = true)
        if (entry.vendorId != vendorId || entry.productId != productId || serialDiffers || entry.epoch != epoch) {
            owed.remove(name)
            return false
        }
        return true
    }

    /** Records whether [attachment] owes a reset now; true when that changed anything. */
    fun record(attachment: Attachment, owes: Boolean): Boolean {
        if (!owes) {
            return owed.remove(attachment.name) != null
        }
        val entry = attachment.copy(serial = clean(attachment.serial))
        return owed.put(attachment.name, entry) != entry
    }

    /** The device [name] was detached: whatever it owed went with it. True when it owed something. */
    fun detach(name: String): Boolean = owed.remove(name) != null

    /**
     * A new attach edge for [name], at [epoch], with no detach seen in between. A device keeps its name while it stays
     * attached, so this is the same attachment reported again (the Activity's launch intent after a process restart,
     * for one): it keeps what it owes, under its new epoch.
     */
    fun attached(name: String, epoch: Long) {
        owed[name]?.let { owed[name] = it.copy(epoch = epoch) }
    }

    /** The entries as app-private storage keeps them: one line each, the attach epoch left out. */
    fun encode(): String =
        owed.values.joinToString("") { "${it.name}\t${it.vendorId}\t${it.productId}\t${it.serial}\n" }

    /**
     * At process start: take the stored [text], keeping each entry whose device is attached now with the same VID:PID
     * ([attached], by device name), under the epoch the broker has for it ([epochOf]). Returns true when the store
     * held anything that was dropped, so it should be written again.
     */
    fun restore(text: String, attached: Map<String, Pair<Int, Int>>, epochOf: (String) -> Long): Boolean {
        owed.clear()
        var dropped = false
        for (line in text.lineSequence().filter { it.isNotEmpty() }) {
            val fields = line.split('\t')
            val vendorId = fields.getOrNull(1)?.toIntOrNull()
            val productId = fields.getOrNull(2)?.toIntOrNull()
            val name = fields[0]
            if (fields.size != 4 || vendorId == null || productId == null || attached[name] != (vendorId to productId)) {
                dropped = true
                continue
            }
            owed[name] = Attachment(name, epochOf(name), vendorId, productId, fields[3])
        }
        return dropped
    }

    // One line per entry, tab-separated: a serial can never break the record.
    private fun clean(text: String) = text.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')
}
