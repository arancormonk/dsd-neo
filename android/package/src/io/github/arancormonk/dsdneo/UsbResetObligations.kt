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
 * descriptor with: the UsbDevice name, which a re-enumeration changes, with the VID:PID and serial that make an entry
 * this device's. Each attachment keeps its own until a reset of it succeeds, samples show it streams without one, or it
 * is detached; selecting another receiver meanwhile never applies it there.
 *
 * The name stands for the attachment: a device keeps it while it stays attached, and only a detach ends an attachment
 * (the broker drops the entry then, and a re-plug gets a new name). An attach event for a device that never left, such
 * as the Activity's launch intent reported again after a process restart, is the same attachment and keeps its entry.
 *
 * Entries outlive the process ([encode], [restore]): a new process can meet a dongle the last one left wedged. At
 * process start an entry whose device is no longer attached with that VID:PID is dropped, since its detach means it
 * re-enumerated, and one still attached is honoured on its first open. The worst a stale entry costs is one reset that
 * was not needed. What this process decided before the store was read (an entry set or cleared, a detach) is newer than
 * the store, so it wins over the stored entry under that name, and the restore asks for the merged record to be written
 * (#782 gate r1 G2). Plain Kotlin; [UsbSourceManager] makes every call on the main thread.
 */
internal class UsbResetObligations {
    /** One physical attachment: the device [name], its VID:PID, and its [serial], or "" when it could not be read. */
    data class Attachment(val name: String, val vendorId: Int, val productId: Int, val serial: String)

    private val owed = LinkedHashMap<String, Attachment>()

    /** Whether [restore] ran: until then, storage has not been read, and nothing has been written to it. */
    private var restored = false

    /** Before [restore]: the names this process set, cleared or saw detached, whose stored entries are older. */
    private val decided = HashSet<String>()

    /** Before [restore]: whether this process changed the record, which storage then still lacks. */
    private var changedBeforeRestore = false

    /** Notes a decision about [name] made before the store was read; [changed] when it changed the record. */
    private fun noteBeforeRestore(name: String, changed: Boolean) {
        if (restored) return
        decided += name
        if (changed) changedBeforeRestore = true
    }

    /** The attachments that owe a reset, oldest first. */
    val entries: List<Attachment> get() = owed.values.toList()

    /**
     * Whether the device [name], with [vendorId]:[productId] and [serial] ("" when unreadable), owes a reset before it
     * is opened. An entry under that name that turns out to be another device (another VID:PID, or a serial both sides
     * read and that differs) is dropped, which [entries] then shows.
     */
    fun owes(name: String, vendorId: Int, productId: Int, serial: String): Boolean {
        val entry = owed[name] ?: return false
        val serialDiffers = entry.serial.isNotEmpty() && serial.isNotEmpty() && !entry.serial.equals(serial, ignoreCase = true)
        if (entry.vendorId != vendorId || entry.productId != productId || serialDiffers) {
            owed.remove(name)
            noteBeforeRestore(name, changed = true)
            return false
        }
        return true
    }

    /** Records whether [attachment] owes a reset now; true when that changed anything. */
    fun record(attachment: Attachment, owes: Boolean): Boolean {
        val changed = if (!owes) {
            owed.remove(attachment.name) != null
        } else {
            val entry = attachment.copy(serial = clean(attachment.serial))
            owed.put(attachment.name, entry) != entry
        }
        noteBeforeRestore(attachment.name, changed)
        return changed
    }

    /** The device [name] was detached: whatever it owed went with it. True when it owed something. */
    fun detach(name: String): Boolean {
        val changed = owed.remove(name) != null
        noteBeforeRestore(name, changed)
        return changed
    }

    /** The entries as app-private storage keeps them: one line each. */
    fun encode(): String =
        owed.values.joinToString("") { "${it.name}\t${it.vendorId}\t${it.productId}\t${it.serial}\n" }

    /**
     * At process start: add the stored [text]'s entries whose device is attached now with the same VID:PID
     * ([attached], by device name). Whatever this process decided about a name before the store was read (an entry
     * recorded or cleared, a detach) wins over the stored entry under it. Returns true when storage should be written
     * again: it held anything that was dropped, or this process changed the record before it was read (G2).
     */
    fun restore(text: String, attached: Map<String, Pair<Int, Int>>): Boolean {
        var dropped = false
        val merged = LinkedHashMap<String, Attachment>()
        for (line in text.lineSequence().filter { it.isNotEmpty() }) {
            val fields = line.split('\t')
            val vendorId = fields.getOrNull(1)?.toIntOrNull()
            val productId = fields.getOrNull(2)?.toIntOrNull()
            val name = fields[0]
            if (fields.size != 4 || vendorId == null || productId == null || attached[name] != (vendorId to productId) ||
                name in decided) {
                dropped = true
                continue
            }
            merged[name] = Attachment(name, vendorId, productId, fields[3])
        }
        merged.putAll(owed)
        owed.clear()
        owed.putAll(merged)
        val rewrite = dropped || changedBeforeRestore
        restored = true
        decided.clear()
        changedBeforeRestore = false
        return rewrite
    }

    // One line per entry, tab-separated: a serial can never break the record.
    private fun clean(text: String) = text.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')
}
