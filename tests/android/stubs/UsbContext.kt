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
    var targetPackage: String? = null
    fun setPackage(name: String): Intent { targetPackage = name; return this }
    fun putExtra(name: String, value: Int): Intent { extras[name] = value; return this }
    fun putExtra(name: String, value: Boolean): Intent { extras[name] = value; return this }
    fun putExtra(name: String, value: UsbDevice): Intent { extras[name] = value; return this }
    fun hasExtra(name: String) = name in extras
    fun getIntExtra(name: String, fallback: Int) = extras[name] as? Int ?: fallback
    fun getBooleanExtra(name: String, fallback: Boolean) = extras[name] as? Boolean ?: fallback
    @Suppress("UNCHECKED_CAST")
    fun <T> getParcelableExtra(name: String): T? = extras[name] as T?
    fun <T> getParcelableExtra(name: String, type: Class<T>): T? = extras[name]?.let { type.cast(it) }
    /** What the framework does to a PendingIntent's intent before it sends it: a copy, with its own extras added. */
    fun copy(): Intent = Intent(action).also { it.extras.putAll(extras); it.targetPackage = targetPackage }
}

open class Context {
    companion object {
        const val USB_SERVICE = "usb"
        const val RECEIVER_NOT_EXPORTED = 4
    }
    val usb = UsbManager()
    val receivers = mutableListOf<BroadcastReceiver>()
    /** App-private storage: a directory of the test's own, or the one a "previous process" left (#782). */
    var noBackupFilesDir: java.io.File = java.nio.file.Files.createTempDirectory("dsd-usb-test").toFile()
    open val applicationContext: Context get() = this
    val packageName = "io.github.arancormonk.dsdneo"
    fun getSystemService(name: String): Any? = if (name == USB_SERVICE) usb else null
    fun registerReceiver(receiver: BroadcastReceiver, filter: IntentFilter): Intent? { receivers += receiver; return null }
    fun registerReceiver(receiver: BroadcastReceiver, filter: IntentFilter, flags: Int): Intent? = registerReceiver(receiver, filter)
    fun broadcast(intent: Intent) { receivers.toList().forEach { it.onReceive(this, intent) } }
}
