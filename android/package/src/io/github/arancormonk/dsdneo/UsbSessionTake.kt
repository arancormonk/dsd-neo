// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * What a start found when it took its local receiver's USB connection (#782); see [UsbSourceManager.takeForSession].
 *
 * [failureKind] is empty when the start may go on, else the attempt's failure kind as DecoderService publishes it
 * (android/run_status.h attempt_failure_from_wire()), with [reason] for the diagnostics and, when the kind's own text
 * would not say it (a start naming an RTL-SDR serial that is not connected), [message] for the UI. A start that took a
 * connection carries its identity: the attachment ([device], the UsbDevice name, and [epoch], the broker's count of
 * attach events when it was plugged in; 0 when it was already attached at process start) and the [connection]
 * generation. The run's receiver error belongs to exactly that connection, so the host never shows it for another
 * dongle (#782).
 */
internal data class UsbSessionTake(
    val failureKind: String = "",
    val reason: String = "",
    val device: String = "",
    val epoch: Long = 0L,
    val connection: Long = 0L,
    val message: String = "",
) {
    companion object {
        const val PREVIOUS_STOPPING = "previous_stopping"
        const val USB_PERMISSION = "usb_permission"
        const val USB_OPEN_FAILED = "usb_open_failed"
        const val USB_GONE = "usb_gone"

        /** A start naming an RTL-SDR serial that two readable dongles carry; [message] names it and the fix. */
        const val USB_SERIAL_DUPLICATE = "usb_serial_duplicate"

        /** A start naming an RTL-SDR serial no attached dongle carries; [message] names it and the fix. */
        const val USB_SERIAL_MISSING = "usb_serial_missing"

        fun failed(kind: String, reason: String, message: String = "") =
            UsbSessionTake(failureKind = kind, reason = reason, message = message)
    }
}
