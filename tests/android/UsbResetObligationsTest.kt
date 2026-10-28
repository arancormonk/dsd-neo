// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

// #782: which attachments owe a USB reset before their next open, kept per physical attachment and across processes.

private const val RTL_A = "/dev/bus/usb/001/002"
private const val RTL_B = "/dev/bus/usb/001/003"
private const val RTL_A_REPLUGGED = "/dev/bus/usb/001/009"
private const val VID = 0x0bda
private const val PID = 0x2838

private fun a(epoch: Long = 1L, serial: String = "00000001") = UsbResetObligations.Attachment(RTL_A, epoch, VID, PID, serial)

private fun b(epoch: Long = 2L) = UsbResetObligations.Attachment(RTL_B, epoch, VID, PID, "00000002")

private fun UsbResetObligations.owes(attachment: UsbResetObligations.Attachment) =
    owes(attachment.name, attachment.epoch, attachment.vendorId, attachment.productId, attachment.serial)

// A owes one, B is selected and opened, then A again: B never gets A's, and A still owes its own.
private fun eachAttachmentKeepsItsOwn() {
    val owed = UsbResetObligations()
    check(owed.record(a(), owes = true)) { "a stall records it on A" }
    check(!owed.record(a(), owes = true)) { "recording it again changes nothing" }
    check(!owed.owes(b())) { "B, selected next, owes nothing" }
    check(!owed.record(b(), owes = false)) { "B's clean session leaves A's alone" }
    check(owed.owes(a())) { "back on A, it still owes its reset" }
    check(owed.record(a(), owes = false)) { "a reset that succeeded clears it" }
    check(!owed.owes(a()) && owed.entries.isEmpty())
}

// A reset the kernel refused keeps the obligation: the engine still names the descriptor, so it is recorded again.
private fun aRefusedResetKeepsIt() {
    val owed = UsbResetObligations()
    owed.record(a(), owes = true)
    check(owed.owes(a())) { "the next open is asked to reset it" }
    check(!owed.record(a(), owes = true)) { "the refused reset left it owed; nothing changes" }
    check(owed.owes(a())) { "and the open after that is asked again" }
}

// A detach takes the obligation with it; the device comes back as a new enumeration.
private fun aDetachDropsIt() {
    val owed = UsbResetObligations()
    owed.record(a(), owes = true)
    check(owed.detach(RTL_A)) { "the detach drops it" }
    check(!owed.detach(RTL_A)) { "once" }
    check(!owed.owes(a(epoch = 2L).copy(name = RTL_A_REPLUGGED))) { "the re-plugged device owes nothing" }
}

// An entry under the same name that is another device (VID:PID, a readable serial, another attachment) is dropped.
private fun anotherDeviceUnderTheNameOwesNothing() {
    val owed = UsbResetObligations()
    owed.record(a(), owes = true)
    check(!owed.owes(a().copy(productId = 0x2832))) { "another VID:PID is another device" }
    check(owed.entries.isEmpty()) { "and its entry goes" }
    owed.record(a(), owes = true)
    check(owed.owes(a(serial = ""))) { "a serial that cannot be read yet does not tell them apart" }
    check(!owed.owes(a(serial = "99999999"))) { "a serial both sides read and that differs does" }
    owed.record(a(), owes = true)
    check(!owed.owes(a(epoch = 7L))) { "another attachment under the name owes nothing" }
}

// The Activity's launch intent reports a device that was attached when the process started: the same attachment,
// which keeps what it owes under its new epoch.
private fun anAttachEdgeKeepsItUnderTheNewEpoch() {
    val owed = UsbResetObligations()
    owed.record(a(epoch = 0L), owes = true)
    owed.attached(RTL_A, 1L)
    check(owed.owes(a(epoch = 1L))) { "the reported device still owes its reset" }
    owed.attached(RTL_B, 2L)
    check(owed.entries.size == 1) { "an edge for a device that owes nothing records nothing" }
}

// A new process reads the store back: A, still attached, keeps its obligation (honoured on its first open); an entry
// for a device no longer attached, or attached with another VID:PID, is dropped and the store rewritten.
private fun aNewProcessKeepsOnlyWhatIsStillAttached() {
    val before = UsbResetObligations()
    before.record(a(), owes = true)
    before.record(b(), owes = true)
    val stored = before.encode()

    val after = UsbResetObligations()
    val dropped = after.restore(stored, mapOf(RTL_A to (VID to PID))) { 0L }
    check(dropped) { "B, gone, is dropped, so the store is written again" }
    check(after.entries.map { it.name } == listOf(RTL_A)) { "only A is kept: ${after.entries}" }
    check(after.owes(a(epoch = 0L))) { "A, attached since before the process started, owes its reset on its first open" }

    val replugged = UsbResetObligations()
    check(replugged.restore(stored, mapOf(RTL_A to (VID to 0x2832), RTL_B to (VID to PID))) { 3L }) { "A is another device now" }
    check(replugged.entries.map { it.name } == listOf(RTL_B) && replugged.owes(b(epoch = 3L)))

    val untouched = UsbResetObligations()
    check(!untouched.restore(after.encode(), mapOf(RTL_A to (VID to PID))) { 0L }) { "nothing dropped, nothing to write" }
    check(!UsbResetObligations().restore("", emptyMap()) { 0L }) { "an empty store is no change" }
    check(UsbResetObligations().restore("garbage\n", mapOf(RTL_A to (VID to PID))) { 0L }) { "an unreadable line is dropped" }
}

// A serial can never break the stored record.
private fun aSerialStaysOneField() {
    val owed = UsbResetObligations()
    owed.record(a(serial = "A\tB\nC"), owes = true)
    val again = UsbResetObligations()
    again.restore(owed.encode(), mapOf(RTL_A to (VID to PID))) { 1L }
    check(again.entries.single().serial == "A B C") { "${again.entries}" }
}

fun main() {
    eachAttachmentKeepsItsOwn()
    aRefusedResetKeepsIt()
    aDetachDropsIt()
    anotherDeviceUnderTheNameOwesNothing()
    anAttachEdgeKeepsItUnderTheNewEpoch()
    aNewProcessKeepsOnlyWhatIsStillAttached()
    aSerialStaysOneField()
    println("UsbResetObligationsTest: OK")
}
