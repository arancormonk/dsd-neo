// SPDX-License-Identifier: GPL-3.0-or-later
package android.content

import android.app.NotificationManager
import android.content.pm.PackageManager
import android.os.PowerManager
import java.io.File
import java.util.concurrent.ConcurrentLinkedQueue

class ComponentName

class Intent(context: Context? = null, cls: Class<*>? = null) {
    var action: String? = null
    private val extras = mutableMapOf<String, Any>()
    fun setAction(value: String): Intent { action = value; return this }
    fun putExtra(name: String, value: String): Intent { extras[name] = value; return this }
    fun putExtra(name: String, value: Long): Intent { extras[name] = value; return this }
    fun putExtra(name: String, value: Array<String>): Intent { extras[name] = value; return this }
    fun getStringExtra(name: String) = extras[name] as? String
    fun getLongExtra(name: String, fallback: Long) = extras[name] as? Long ?: fallback
    @Suppress("UNCHECKED_CAST")
    fun getStringArrayExtra(name: String) = extras[name] as? Array<String>
}

// Delivery is explicitly driven by the tests. Returning from startService never runs onStartCommand.
open class Context {
    companion object { const val POWER_SERVICE = "power" }
    open val applicationContext: Context get() = this
    val filesDir = File("/unused/files")
    val cacheDir = File("/unused/cache")
    val packageName = "io.github.arancormonk.dsdneo"
    val packageManager = PackageManager()
    val power = PowerManager()
    val notifications: NotificationManager get() = NotificationManager.device
    val intents = ConcurrentLinkedQueue<Intent>()
    var dispatchFailure: RuntimeException? = null
    var dispatchReturnsNull = false
    open fun startService(intent: Intent): ComponentName? {
        dispatchFailure?.let { throw it }
        if (dispatchReturnsNull) return null
        intents.add(intent)
        return ComponentName()
    }
    fun startForegroundService(intent: Intent) = startService(intent)
    fun getString(id: Int, vararg args: Any): String = "resource-$id"
    fun getSystemService(name: String): Any = power
    fun <T> getSystemService(type: Class<T>): T = type.cast(notifications)
}
