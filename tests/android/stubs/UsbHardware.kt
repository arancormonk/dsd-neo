// SPDX-License-Identifier: GPL-3.0-or-later
package android.hardware.usb

import android.app.PendingIntent

class UsbDevice(val deviceName: String, val vendorId: Int, val productId: Int, private val serial: String?, val productName: String?) {
    // Android hides the serial from an app without permission for the device (API 29+).
    var permitted: () -> Boolean = { false }
    val serialNumber: String?
        get() {
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
    val deviceList: HashMap<String, UsbDevice> = LinkedHashMap()
    val granted = mutableSetOf<String>()
    val prompts = mutableListOf<Pair<UsbDevice, PendingIntent>>()
    val opened = mutableListOf<UsbDeviceConnection>()
    private var nextFd = 40
    fun plug(device: UsbDevice) {
        device.permitted = { device.deviceName in granted }
        deviceList[device.deviceName] = device
    }
    fun hasPermission(device: UsbDevice) = device.deviceName in granted
    fun openDevice(device: UsbDevice): UsbDeviceConnection? = UsbDeviceConnection(nextFd++).also { opened += it }
    fun requestPermission(device: UsbDevice, intent: PendingIntent) { prompts += device to intent }
}
