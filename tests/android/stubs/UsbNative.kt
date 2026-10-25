// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import java.util.concurrent.CopyOnWriteArrayList

// The engine as the USB broker sees it: descriptor slots, whether a session uses the local receiver, and stops.
object DsdNative {
    const val STATUS_OK = 0
    const val STATUS_ERROR = -1
    @Volatile var running = false
    @Volatile var sessionUsesLocalReceiver = false
    @Volatile var fdInUse = false
    @Volatile var usbFd = -1
    @Volatile var airspyFd = -1
    val stopCauses = CopyOnWriteArrayList<String>()
    val diagnostics = CopyOnWriteArrayList<String>()
    fun nativeSetUsbFd(fd: Int): Int { usbFd = fd; return STATUS_OK }
    fun nativeSetAirspyUsbFd(fd: Int): Int { airspyFd = fd; return STATUS_OK }
    fun nativeIsRunning() = running
    fun nativeIsUsbFdInUse() = fdInUse
    fun nativeSessionUsesLocalReceiver() = sessionUsesLocalReceiver
    fun nativeStop(userRequested: Boolean, cause: String): Int { stopCauses += cause; return STATUS_OK }
    fun nativeHostDiagnostic(line: String) { diagnostics += line }
    fun nativeArmStartupDiagnostics() {}
}
