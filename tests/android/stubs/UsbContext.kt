// SPDX-License-Identifier: GPL-3.0-or-later
package android.content

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager

// The USB broker's side of the platform: one context that owns the USB service and delivers broadcasts to the
// receivers registered on it, when the test says so.
abstract class BroadcastReceiver {
    abstract fun onReceive(context: Context, intent: Intent)
}

class IntentFilter {
    val actions = mutableListOf<String>()
    fun addAction(action: String) { actions += action }
}

class Intent(var action: String? = null) {
    private val extras = mutableMapOf<String, Any>()
    /** Extras another app put in that no class loader here knows: every getter for one throws, as the platform's do. */
    private val unreadable = mutableSetOf<String>()
    var targetPackage: String? = null
    fun setPackage(name: String): Intent { targetPackage = name; return this }
    fun putExtra(name: String, value: Int): Intent { extras[name] = value; return this }
    fun putExtra(name: String, value: Boolean): Intent { extras[name] = value; return this }
    fun putExtra(name: String, value: UsbDevice): Intent { extras[name] = value; return this }
    fun putUnreadableExtra(name: String): Intent { unreadable += name; return this }
    fun hasExtra(name: String) = name in extras
    fun getIntExtra(name: String, fallback: Int) = extras[name] as? Int ?: fallback
    fun getBooleanExtra(name: String, fallback: Boolean) = extras[name] as? Boolean ?: fallback
    @Suppress("UNCHECKED_CAST")
    fun <T> getParcelableExtra(name: String): T? = read(name) as T?
    fun <T> getParcelableExtra(name: String, type: Class<T>): T? = read(name)?.let { type.cast(it) }
    private fun read(name: String): Any? {
        if (name in unreadable) throw android.os.BadParcelableException("ClassNotFoundException when unmarshalling: x.Evil")
        return extras[name]
    }
    /** What the framework does to a PendingIntent's intent before it sends it: a copy, with its own extras added. */
    fun copy(): Intent = Intent(action).also { it.extras.putAll(extras); it.targetPackage = targetPackage }
}

/** The system property naming the folder UsbSourceManagerTest's main() makes for the run and removes at its end. */
const val USB_TEST_SCRATCH = "dsdneo.test.usb.scratch"

/** A new temp folder for a USB test, in the run's scratch folder (or java.io.tmpdir outside a run). */
fun usbTestDir(prefix: String): java.io.File {
    val parent = System.getProperty(USB_TEST_SCRATCH) ?: System.getProperty("java.io.tmpdir")
    return java.nio.file.Files.createTempDirectory(java.nio.file.Path.of(parent), prefix).toFile()
}

open class Context {
    companion object {
        const val USB_SERVICE = "usb"
        const val RECEIVER_NOT_EXPORTED = 4
    }
    val usb = UsbManager()
    val receivers = mutableListOf<BroadcastReceiver>()
    /** App-private storage: a directory of the test's own, or the one a "previous process" left (#782). */
    var noBackupFilesDir: java.io.File = usbTestDir("dsd-usb-test")
    open val applicationContext: Context get() = this
    val packageName = "io.github.arancormonk.dsdneo"
    fun getSystemService(name: String): Any? = if (name == USB_SERVICE) usb else null
    fun registerReceiver(receiver: BroadcastReceiver, filter: IntentFilter): Intent? { receivers += receiver; return null }
    fun registerReceiver(receiver: BroadcastReceiver, filter: IntentFilter, flags: Int): Intent? = registerReceiver(receiver, filter)
    fun broadcast(intent: Intent) { receivers.toList().forEach { it.onReceive(this, intent) } }
}
