// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import java.util.concurrent.CopyOnWriteArrayList

// The engine as the USB broker sees it: descriptor slots, whether a session uses the local receiver, which
// descriptors it holds, and stops.
object DsdNative {
    const val STATUS_OK = 0
    const val STATUS_ERROR = -1
    @Volatile var running = false
    @Volatile var sessionUsesLocalReceiver = false
    @Volatile var fdInUse = false
    @Volatile var usbFd = -1
    @Volatile var airspyFd = -1
    @Volatile var resetFirst = false
    /** What nativeHeldUsbFds() answers: { RTL-SDR, Airspy }, -1 for none. */
    @Volatile var held = intArrayOf(-1, -1)
    /** Every descriptor published to a slot, in order (clears included). */
    val published = CopyOnWriteArrayList<Int>()
    val stopCauses = CopyOnWriteArrayList<String>()
    val diagnostics = CopyOnWriteArrayList<String>()
    fun nativeSetUsbFd(fd: Int): Int { usbFd = fd; published += fd; return STATUS_OK }
    fun nativeSetAirspyUsbFd(fd: Int): Int { airspyFd = fd; published += fd; return STATUS_OK }
    fun nativeSetUsbResetFirst(value: Boolean) { resetFirst = value }
    fun nativeHeldUsbFds(): IntArray? = held.copyOf()
    fun nativeIsRunning() = running
    fun nativeIsUsbFdInUse() = fdInUse
    fun nativeSessionUsesLocalReceiver() = sessionUsesLocalReceiver
    fun nativeStop(userRequested: Boolean, cause: String): Int { stopCauses += cause; return STATUS_OK }
    fun nativeHostDiagnostic(line: String) { diagnostics += line }
    fun nativeArmStartupDiagnostics() {}
}
