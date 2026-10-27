// SPDX-License-Identifier: GPL-3.0-or-later
package android.hardware.usb

import android.app.PendingIntent

class UsbDevice(val deviceName: String, val vendorId: Int, val productId: Int, private val serial: String?, val productName: String?) {
    // Android hides the serial from an app without permission for the device (API 29+).
    var permitted: () -> Boolean = { false }
    /** Runs inside every serial read, which is a binder call on Android; a test blocks it here. */
    @Volatile var serialHook: (() -> Unit)? = null
    val serialNumber: String?
        get() {
            serialHook?.invoke()
            if (!permitted()) throw SecurityException("no permission for $deviceName")
            return serial
        }
}

class UsbDeviceConnection(val fileDescriptor: Int) {
    @Volatile var closed = false
    fun close() { closed = true }
}

// The bus, the permissions the user granted and the prompts the app raised; the test plugs, grants and answers.
class UsbManager {
    companion object {
        const val ACTION_USB_DEVICE_ATTACHED = "android.hardware.usb.action.USB_DEVICE_ATTACHED"
        const val ACTION_USB_DEVICE_DETACHED = "android.hardware.usb.action.USB_DEVICE_DETACHED"
        const val EXTRA_DEVICE = "device"
        const val EXTRA_PERMISSION_GRANTED = "permission"
    }
    // Bus order is plug order, so "first on the bus" is the test's to choose.
    private val devices = LinkedHashMap<String, UsbDevice>()
    /**
     * A copy, as the binder reply is: taken first, then [listHook] runs, so a test that blocks there holds a listing
     * that attaches and detaches applied meanwhile have made stale.
     */
    val deviceList: HashMap<String, UsbDevice>
        get() {
            val copy = synchronized(devices) { LinkedHashMap(devices) }
            listHook?.invoke()
            return copy
        }
    @Volatile var listHook: (() -> Unit)? = null
    val granted: MutableSet<String> = java.util.Collections.synchronizedSet(mutableSetOf<String>())
    val prompts = mutableListOf<Pair<UsbDevice, PendingIntent>>()
    val opened = mutableListOf<UsbDeviceConnection>()
    /** When set, openDevice() answers null, as Android does when it cannot open the device. */
    @Volatile var refuseOpen = false
    private var nextFd = 40
    fun plug(device: UsbDevice) {
        device.permitted = { device.deviceName in granted }
        synchronized(devices) { devices[device.deviceName] = device }
    }
    fun unplug(name: String) { synchronized(devices) { devices.remove(name) } }
    fun hasPermission(device: UsbDevice) = device.deviceName in granted
    fun openDevice(device: UsbDevice): UsbDeviceConnection? =
        if (refuseOpen) null else UsbDeviceConnection(nextFd++).also { opened += it }
    fun requestPermission(device: UsbDevice, intent: PendingIntent) { prompts += device to intent }
}
