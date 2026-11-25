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
    /**
     * One physical attachment: the device [name], its VID:PID, and its [serial], or "" when it could not be read.
     * [wholeSerial] is false only for an entry read back in the format written before the broker kept an RTL-SDR's
     * whole serial, whose [serial] may be the last 16 characters of a longer one ([sameSerial]).
     */
    data class Attachment(
        val name: String,
        val vendorId: Int,
        val productId: Int,
        val serial: String,
        val wholeSerial: Boolean = true,
    )

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
     * read and that differs) is dropped, which [entries] then shows. Serials compare exactly, as the engine matches an
     * RTL-SDR's ([UsbReceiverKind.sameSerial]), so a replacement whose serial differs only in case is another device.
     */
    fun owes(name: String, vendorId: Int, productId: Int, serial: String): Boolean {
        val entry = owed[name] ?: return false
        val serialDiffers = entry.serial.isNotEmpty() && serial.isNotEmpty() && !sameSerial(entry, serial)
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
            val entry = attachment.copy(wholeSerial = true)
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

    /**
     * The entries as app-private storage keeps them: one line each, `name, VID, PID, serial` and, for a whole serial,
     * a fifth field [WHOLE_SERIAL]. A whole serial is stored losslessly ([escape]), so two serials that differ only in
     * a tab, a line break or a '%' stay two. An entry read back without the fifth field keeps the four fields it was
     * read with, its serial as that format kept it (a tab or line break as a space).
     */
    fun encode(): String =
        owed.values.joinToString("") {
            val serial = if (it.wholeSerial) "${escape(it.serial)}\t$WHOLE_SERIAL" else it.serial
            "${it.name}\t${it.vendorId}\t${it.productId}\t$serial\n"
        }

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
            val whole = fields.size == 5 && fields[4] == WHOLE_SERIAL
            val serial = if (whole) unescape(fields[3]) else fields.getOrNull(3)
            if ((fields.size != 4 && !whole) || serial == null || vendorId == null || productId == null ||
                attached[name] != (vendorId to productId) || name in decided) {
                dropped = true
                continue
            }
            merged[name] = Attachment(name, vendorId, productId, serial, wholeSerial = whole)
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

    // The older format's serial: a tab or line break was stored as a space, so a live serial is read that way too.
    private fun clean(text: String) = text.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')

    /** A whole serial as one field of a line: '%', tab, line feed and carriage return as %25, %09, %0A and %0D. */
    private fun escape(text: String): String = buildString {
        for (c in text) {
            when (c) {
                '%' -> append("%25")
                '\t' -> append("%09")
                '\n' -> append("%0A")
                '\r' -> append("%0D")
                else -> append(c)
            }
        }
    }

    /** [escape] undone, or null for a field it never writes (a '%' not followed by one of its four codes). */
    private fun unescape(field: String): String? {
        val out = StringBuilder(field.length)
        var at = 0
        while (at < field.length) {
            val c = field[at]
            if (c != '%') {
                out.append(c)
                at += 1
                continue
            }
            out.append(ESCAPED[field.substring(at + 1, minOf(at + 3, field.length))] ?: return null)
            at += 3
        }
        return out.toString()
    }

    /**
     * Whether [entry]'s serial is the device's [live] one: exactly, for an entry this version recorded or stored. An
     * entry read back in the older format ([Attachment.wholeSerial] false) holds the last 16 characters of the serial:
     * for a serial of 16 characters or fewer that is the whole serial, so the two compare equal as they are; a
     * 16-character one is also read as the end of a longer live serial (the attachment name and VID:PID already
     * match). A whole 16-character serial this version recorded never matches a longer one that merely ends with it.
     */
    private fun sameSerial(entry: Attachment, live: String): Boolean {
        if (entry.wholeSerial) return UsbReceiverKind.sameSerial(UsbReceiverKind.RTL, entry.serial, live)
        val read = clean(live)
        return UsbReceiverKind.sameSerial(UsbReceiverKind.RTL, entry.serial, read) ||
            (entry.serial.length == LEGACY_SERIAL_LENGTH && read.length > LEGACY_SERIAL_LENGTH &&
                read.endsWith(entry.serial))
    }

    private companion object {
        /** How many trailing characters of a serial the broker kept before it kept an RTL-SDR's whole serial. */
        const val LEGACY_SERIAL_LENGTH = 16

        /** The fifth field of a stored entry whose serial is whole; the older format had four fields. */
        const val WHOLE_SERIAL = "whole"

        /** What [escape] writes, by the two characters after its '%'. */
        val ESCAPED = mapOf("25" to '%', "09" to '\t', "0A" to '\n', "0D" to '\r')
    }
}
