// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.util.Log

/**
 * Owns the USB-OTG side of a locally attached RTL-SDR.
 *
 * An app cannot open `/dev/bus/usb` nodes, so the descriptor has to come from
 * [UsbDeviceConnection]. This object obtains it — prompting for permission when
 * needed — and hands it to the engine through [DsdNative.nativeSetUsbFd]; librtlsdr
 * then wraps it instead of enumerating, which is why the engine also skips its own
 * device scan while a descriptor is set.
 *
 * The connection outlives a single decode run and is released only on detach or when
 * the service goes away: Java keeps ownership of the descriptor for as long as the
 * engine might use it.
 */
object UsbSourceManager {
    private const val TAG = "dsd-neo"
    private const val ACTION_USB_PERMISSION = "io.github.arancormonk.dsdneo.action.USB_PERMISSION"

    /**
     * RTL2832U vendor/product ids, the same set librtlsdr recognises. Kept in sync
     * with `res/xml/device_filter.xml`, which drives the attach intent filter.
     */
    private val KNOWN_IDS: Set<Int> = intArrayOf(
        0x0bda2832, 0x0bda2838,
        0x04136680, 0x04136f0f,
        0x0458707f,
        0x0ccd00a9, 0x0ccd00b3, 0x0ccd00b4, 0x0ccd00b5, 0x0ccd00b7, 0x0ccd00b8,
        0x0ccd00b9, 0x0ccd00c0, 0x0ccd00c6, 0x0ccd00d3, 0x0ccd00d7, 0x0ccd00e0,
        0x15545020,
        0x15f40131, 0x15f40133,
        0x185b0620, 0x185b0650, 0x185b0680,
        0x1b80d393, 0x1b80d394, 0x1b80d395, 0x1b80d397, 0x1b80d398, 0x1b80d39d,
        0x1b80d3a4, 0x1b80d3a8, 0x1b80d3af, 0x1b80d3b0,
        0x1d191101, 0x1d191102, 0x1d191103, 0x1d191104,
        0x1f4da803, 0x1f4db803, 0x1f4dc803, 0x1f4dd286, 0x1f4dd803,
    ).toHashSet()

    private val lock = Any()
    private var connection: UsbDeviceConnection? = null
    private var attachedName: String? = null
    private var status: String = ""
    private var receiverRegistered = false
    private var requestPending = false

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION -> {
                    synchronized(lock) { requestPending = false }
                    val device = usbDeviceExtra(intent)
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    if (device == null) {
                        setStatus("Permission result had no device")
                    } else if (granted) {
                        open(context, device)
                    } else {
                        // Not an error state: the user said no, and rtl_tcp still works.
                        setStatus("USB permission denied")
                    }
                }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    val device = usbDeviceExtra(intent)
                    if (device == null || device.deviceName == synchronized(lock) { attachedName }) {
                        Log.i(TAG, "SDR detached; stopping the engine")
                        // The engine is mid-transfer on a descriptor that just went
                        // away: stop first, release once it has unwound.
                        DsdNative.nativeStop()
                        release()
                        setStatus("Device detached")
                    }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    val device = usbDeviceExtra(intent) ?: return
                    if (isKnown(device)) {
                        setStatus("Found ${describe(device)}")
                    }
                }
            }
        }
    }

    /** Short human-readable attachment/permission state for the UI. */
    @JvmStatic
    fun statusText(): String = synchronized(lock) { status }

    /** Whether a descriptor has been handed to the engine. */
    @JvmStatic
    fun isReady(): Boolean = synchronized(lock) { connection != null }

    /**
     * Find an attached SDR and make it usable, prompting for permission if needed.
     *
     * Safe to call repeatedly; the outcome shows up in [statusText] and [isReady]
     * because the permission dialog answer arrives asynchronously.
     */
    @JvmStatic
    fun requestAccess(context: Context) {
        val appContext = context.applicationContext
        ensureReceiver(appContext)

        if (isReady()) {
            return
        }

        val manager = appContext.getSystemService(Context.USB_SERVICE) as UsbManager
        val device = manager.deviceList.values.firstOrNull { isKnown(it) }
        if (device == null) {
            setStatus("No RTL-SDR attached")
            return
        }

        if (manager.hasPermission(device)) {
            open(appContext, device)
            return
        }

        synchronized(lock) {
            if (requestPending) {
                return
            }
            requestPending = true
        }
        setStatus("Requesting permission for ${describe(device)}")
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(appContext.packageName)
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
        manager.requestPermission(device, PendingIntent.getBroadcast(appContext, 0, intent, flags))
    }

    /**
     * Drop the descriptor and close the connection.
     *
     * Must not run while the engine is still using it — callers stop the engine
     * first. Clearing the native slot also puts device enumeration back the way it
     * was, so a later rtl_tcp or host-side run is unaffected.
     */
    @JvmStatic
    fun release() {
        val open = synchronized(lock) {
            val current = connection
            connection = null
            attachedName = null
            current
        }
        if (open == null) {
            return
        }
        DsdNative.nativeSetUsbFd(-1)
        open.close()
    }

    private fun open(context: Context, device: UsbDevice) {
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val opened = manager.openDevice(device)
        if (opened == null) {
            setStatus("Could not open ${describe(device)}")
            return
        }

        val fd = opened.fileDescriptor
        if (fd < 0) {
            opened.close()
            setStatus("No descriptor for ${describe(device)}")
            return
        }

        val rc = DsdNative.nativeSetUsbFd(fd)
        if (rc != DsdNative.STATUS_OK) {
            opened.close()
            setStatus("Engine rejected the descriptor ($rc)")
            return
        }

        synchronized(lock) {
            connection = opened
            attachedName = device.deviceName
        }
        setStatus("Ready: ${describe(device)}")
    }

    private fun ensureReceiver(context: Context) {
        synchronized(lock) {
            if (receiverRegistered) {
                return
            }
            receiverRegistered = true
        }
        val filter = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            context.registerReceiver(receiver, filter)
        }
    }

    // Deliberately id-only. Matching on "has a vendor-specific interface" would drag
    // in hubs, ddocks and audio devices, and handing librtlsdr one of those
    // descriptors is worse than not finding a dongle at all. An unlisted rebadge
    // belongs in this table and in res/xml/device_filter.xml, not in a loose match.
    private fun isKnown(device: UsbDevice): Boolean =
        KNOWN_IDS.contains((device.vendorId shl 16) or device.productId)

    private fun describe(device: UsbDevice): String =
        device.productName ?: String.format("%04x:%04x", device.vendorId, device.productId)

    private fun setStatus(text: String) {
        synchronized(lock) { status = text }
        Log.i(TAG, "usb: $text")
    }

    @Suppress("DEPRECATION")
    private fun usbDeviceExtra(intent: Intent): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }
}
