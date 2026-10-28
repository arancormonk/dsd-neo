// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

// #782: which attachments owe a USB reset before their next open, kept per physical attachment and across processes.

private const val RTL_A = "/dev/bus/usb/001/002"
private const val RTL_B = "/dev/bus/usb/001/003"
private const val RTL_A_REPLUGGED = "/dev/bus/usb/001/009"
private const val VID = 0x0bda
private const val PID = 0x2838

private fun a(serial: String = "00000001") = UsbResetObligations.Attachment(RTL_A, VID, PID, serial)

private fun b() = UsbResetObligations.Attachment(RTL_B, VID, PID, "00000002")

private fun UsbResetObligations.owes(attachment: UsbResetObligations.Attachment) =
    owes(attachment.name, attachment.vendorId, attachment.productId, attachment.serial)

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
    check(!owed.owes(a().copy(name = RTL_A_REPLUGGED))) { "the re-plugged device owes nothing" }
}

// An entry under the same name that is another device (VID:PID, a readable serial) is dropped.
private fun anotherDeviceUnderTheNameOwesNothing() {
    val owed = UsbResetObligations()
    owed.record(a(), owes = true)
    check(!owed.owes(a().copy(productId = 0x2832))) { "another VID:PID is another device" }
    check(owed.entries.isEmpty()) { "and its entry goes" }
    owed.record(a(), owes = true)
    check(owed.owes(a(serial = ""))) { "a serial that cannot be read yet does not tell them apart" }
    check(!owed.owes(a(serial = "99999999"))) { "a serial both sides read and that differs does" }
    check(owed.entries.isEmpty()) { "and its entry goes" }
}

// Something recorded in this process before the store was read wins over the stored entry under its name.
private fun theRecordWinsOverTheStore() {
    val before = UsbResetObligations()
    before.record(a(serial = "OLD"), owes = true)
    val owed = UsbResetObligations()
    owed.record(a(), owes = true)
    owed.restore(before.encode(), mapOf(RTL_A to (VID to PID)))
    check(owed.entries.single().serial == "00000001") { "${owed.entries}" }
}

// Gate r1 G2: a change made before the store was read leaves storage behind the record, so the restore says to write the
// merged record even when it dropped nothing from the store.
private fun aChangeBeforeTheStoreIsReadIsWrittenAfter() {
    val owed = UsbResetObligations()
    owed.record(a(), owes = true)
    check(owed.restore("", mapOf(RTL_A to (VID to PID)))) { "the obligation recorded before the read is stored after it" }
    check(owed.encode().startsWith("$RTL_A\t")) { owed.encode() }
    check(!owed.record(a(), owes = true) && !owed.restore("", mapOf(RTL_A to (VID to PID)))) {
        "once the store was read, a restore with nothing new writes nothing"
    }
    val cleared = UsbResetObligations()
    check(!cleared.record(b(), owes = false)) { "a clear of nothing changes nothing in memory" }
    check(!cleared.restore("", emptyMap())) { "and leaves nothing to write" }
}

// G2: so does a clear, which the stored entry under that name must not undo: the engine's answer in this process (a
// reset that succeeded, samples) is newer than anything the last process stored.
private fun aClearBeforeTheStoreIsReadWins() {
    val before = UsbResetObligations()
    before.record(a(), owes = true)
    val owed = UsbResetObligations()
    owed.record(a(), owes = true)
    owed.record(a(), owes = false)
    check(owed.restore(before.encode(), mapOf(RTL_A to (VID to PID)))) { "the stale stored entry is written away" }
    check(owed.entries.isEmpty()) { "the clear wins: ${owed.entries}" }
    val detached = UsbResetObligations()
    detached.detach(RTL_A)
    detached.restore(before.encode(), mapOf(RTL_A to (VID to PID)))
    check(detached.entries.isEmpty()) { "a detach in this process wins too: ${detached.entries}" }
}

// A new process reads the store back: A, still attached, keeps its obligation (honoured on its first open); an entry
// for a device no longer attached, or attached with another VID:PID, is dropped and the store rewritten.
private fun aNewProcessKeepsOnlyWhatIsStillAttached() {
    val before = UsbResetObligations()
    before.record(a(), owes = true)
    before.record(b(), owes = true)
    val stored = before.encode()

    val after = UsbResetObligations()
    val dropped = after.restore(stored, mapOf(RTL_A to (VID to PID)))
    check(dropped) { "B, gone, is dropped, so the store is written again" }
    check(after.entries.map { it.name } == listOf(RTL_A)) { "only A is kept: ${after.entries}" }
    check(after.owes(a())) { "A, attached since before the process started, owes its reset on its first open" }

    val replugged = UsbResetObligations()
    check(replugged.restore(stored, mapOf(RTL_A to (VID to 0x2832), RTL_B to (VID to PID)))) { "A is another device now" }
    check(replugged.entries.map { it.name } == listOf(RTL_B) && replugged.owes(b()))

    val untouched = UsbResetObligations()
    check(!untouched.restore(after.encode(), mapOf(RTL_A to (VID to PID)))) { "nothing dropped, nothing to write" }
    check(!UsbResetObligations().restore("", emptyMap())) { "an empty store is no change" }
    check(UsbResetObligations().restore("garbage\n", mapOf(RTL_A to (VID to PID)))) { "an unreadable line is dropped" }
}

// A serial can never break the stored record.
private fun aSerialStaysOneField() {
    val owed = UsbResetObligations()
    owed.record(a(serial = "A\tB\nC"), owes = true)
    val again = UsbResetObligations()
    again.restore(owed.encode(), mapOf(RTL_A to (VID to PID)))
    check(again.entries.single().serial == "A B C") { "${again.entries}" }
}

fun main() {
    eachAttachmentKeepsItsOwn()
    aRefusedResetKeepsIt()
    aDetachDropsIt()
    anotherDeviceUnderTheNameOwesNothing()
    theRecordWinsOverTheStore()
    aChangeBeforeTheStoreIsReadIsWrittenAfter()
    aClearBeforeTheStoreIsReadWins()
    aNewProcessKeepsOnlyWhatIsStillAttached()
    aSerialStaysOneField()
    println("UsbResetObligationsTest: OK")
}
