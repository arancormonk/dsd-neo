// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Thin binding to the JNI lifecycle surface in `android/dsdneo_jni.cpp`.
 *
 * The library loaded here is the very same one Qt loads for the UI, so the service
 * and the UI share one instance of the engine's globals. Loading it from a companion
 * object matters: the service can be started with no QtActivity ever created, and
 * relying on Qt's activity bootstrap to have loaded the library ends in
 * UnsatisfiedLinkError.
 */
object DsdNative {
    const val STATUS_OK = 0
    const val STATUS_ERROR = -1
    const val STATUS_BAD_STATE = -2
    const val STATUS_CONFIG_EXIT = 1
    // Must match android/run_status.h. Terminal results are retained per session.
    const val RUN_PENDING = 0
    const val RUN_COMPLETED = 1
    const val RUN_CANCELLED = 2
    const val RUN_FAILED = 3

    init {
        System.loadLibrary("dsd-neo-app_arm64-v8a")
        HostDiagnostics.nativeReady()
    }

    external fun nativeInit(configDir: String): Int

    external fun nativeConfigure(args: Array<String>, sessionId: Long): Int

    /**
     * session id, initialized, terminal reason, run code, native USB open/claim error, input failure kind and code,
     * end cause (none, user, detached, receiver failed, lifecycle: 0-4; the run's first accepted ending) and the
     * session's configured receiver (none, RTL-SDR, Airspy: 0-2). Must match android/run_status.h.
     */
    external fun nativeLifecycleStatus(): LongArray?

    /** Lifecycle/USB messages only, never argv, credentials or key material. */
    external fun nativeHostDiagnostic(line: String)

    /** Re-arms the diagnostics startup window at a device boundary (issue #745). */
    external fun nativeArmStartupDiagnostics()

    /** Starts a flush of the persistent diagnostics before the process may go, without waiting for it (#782). */
    external fun nativeFlushDiagnostics()

    /**
     * Gives up a configured session that will never run: its start attempt was retired during configure (#782). Clears
     * the native configuration and the run record's receiver, and ends the record cancelled.
     */
    external fun nativeDiscardConfigured(): Int

    external fun nativeRun(): Int

    /**
     * Raises the engine's stop flag. [userRequested] is true only for the user's own Stop, which
     * also abandons the rdio uploads still pending instead of waiting for them (#718 D4). [cause] is
     * the [StopCause.wire] value; the session keeps the first ending it accepts, so an unplug stays
     * an unplug after a later Stop (#647). Call it through [EngineStop.request], which takes the
     * reason from the caller.
     */
    external fun nativeStop(userRequested: Boolean, cause: String): Int

    /**
     * Whether the session is configured or running with a local RTL-SDR (not rtl_tcp) or Airspy
     * input. A USB connection outlives the sessions that used it, so a detach concerns the session
     * only when this (or [nativeIsUsbFdInUse]) is true.
     */
    external fun nativeSessionUsesLocalReceiver(): Boolean

    external fun nativeDestroy(): Int

    external fun nativeIsRunning(): Boolean

    /**
     * Hand the engine an already-open USB device descriptor, or -1 to clear it.
     *
     * An app cannot open `/dev/bus/usb` nodes, so the descriptor comes from
     * [android.hardware.usb.UsbDeviceConnection]. Java keeps ownership: the
     * connection must stay open for as long as the engine is running. [resetFirst]
     * is true when the attachment owes a USB reset (#782): the RTL-SDR open that
     * takes this descriptor resets the device first. It is bound to [fd], so every
     * call replaces it.
     */
    external fun nativeSetUsbFd(fd: Int, resetFirst: Boolean): Int

    @JvmStatic
    external fun nativeSetAirspyUsbFd(fd: Int): Int

    /**
     * The RTL-SDR descriptor whose dongle owes a USB reset before its next open, or -1 (#782).
     *
     * Set with [nativeSetUsbFd], and by the engine when a stall stopped the dongle's samples (before any grace and
     * whatever Stop follows); cleared by samples since, or a reset that succeeded. [UsbSourceManager] records it on
     * the attachment it handed that descriptor with.
     */
    external fun nativeUsbResetOwed(): Int

    /**
     * The descriptors the engine holds, as { RTL-SDR, Airspy }, each -1 when none; null when the call failed (#782).
     *
     * A descriptor is held from just before libusb wraps it until the device is closed. [UsbSourceManager] closes a
     * released connection once its own descriptor is no longer held, whatever a later session holds.
     */
    external fun nativeHeldUsbFds(): IntArray?

    /**
     * Whether the engine currently has the descriptor wrapped in libusb.
     *
     * The window this reports is narrower than a run: the engine takes the
     * descriptor during input setup and gives it back when the device closes, both
     * well inside [nativeRun]. Closing the connection while this is true would pull
     * the file out from under an in-flight transfer, so [UsbSourceManager] waits for
     * it to clear before releasing.
     */
    external fun nativeIsUsbFdInUse(): Boolean

    /**
     * The decoder's current status as one versioned, tab-separated record, or null when
     * nothing has been published yet.
     *
     * One call rather than a field at a time: the service polls this off the decode
     * thread, and reading fields separately would let a publish land between them and
     * produce a status describing two different moments.
     */
    external fun nativeNotificationStatus(): String?

    /**
     * Asks the Qt event loop to quit so its `main()` returns.
     *
     * Qt's own teardown waits for that to happen but only raises the quit when the
     * Android event dispatcher is already stopped, which it is not when the task is
     * swiped out of recents — so without this the wait never ends. Called from
     * [DsdNeoActivity.onDestroy]; see the JNI side in `android/decoder_host_android.cpp`
     * for the full account.
     *
     * A no-op when no QCoreApplication exists yet, so it is safe on an Activity torn
     * down before Qt finished starting.
     */
    external fun nativeQuitUi()
}
