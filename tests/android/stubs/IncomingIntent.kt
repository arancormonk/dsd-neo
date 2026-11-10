// SPDX-License-Identifier: GPL-3.0-or-later
package android.content

import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.os.Process

// The delivered intent and the context the incoming-document intake and the extras guard read, with the platform's
// names. As the platform's, getExtras() hands out a copy: a copy that fails to unparcel leaves the intent's own extras
// as they were, so only replaceExtras() makes the next read safe. The getters read the intent's own extras, which
// unparcel (and may throw) on first use.
class Intent(var action: String? = null) {
    var data: Uri? = null
    var flags = 0
    private var own: Bundle? = null

    val extras: Bundle? get() = own?.let { Bundle(it) }

    fun replaceExtras(extras: Bundle?): Intent = apply { own = extras?.let { Bundle(it) } }

    @Deprecated("API 33")
    fun <T> getParcelableExtra(name: String): T? = own?.getParcelable(name)

    fun <T> getParcelableExtra(name: String, clazz: Class<T>): T? = own?.get(name)?.let { if (clazz.isInstance(it)) clazz.cast(it) else null }

    @Deprecated("API 33")
    fun <T> getParcelableArrayListExtra(name: String): ArrayList<T>? = own?.getParcelableArrayList(name)

    fun <T> getParcelableArrayListExtra(name: String, clazz: Class<out T>): ArrayList<T>? {
        val list = own?.get(name) as? ArrayList<*> ?: return null
        @Suppress("UNCHECKED_CAST")
        return if (list.all { clazz.isInstance(it) }) list as ArrayList<T> else null
    }

    companion object {
        const val ACTION_MAIN = "android.intent.action.MAIN"
        const val ACTION_VIEW = "android.intent.action.VIEW"
        const val ACTION_SEND = "android.intent.action.SEND"
        const val ACTION_SEND_MULTIPLE = "android.intent.action.SEND_MULTIPLE"
        const val ACTION_EDIT = "android.intent.action.EDIT"
        const val EXTRA_STREAM = "android.intent.extra.STREAM"
        const val EXTRA_TEXT = "android.intent.extra.TEXT"
        const val FLAG_GRANT_READ_URI_PERMISSION = 0x00000001
        const val FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY = 0x00100000
    }
}

// The app's package, the package manager a test arranges, and the read grants recorded for this app, by URI text. Each
// grant check is kept, so a test can see which URI was asked about and how.
open class Context {
    open val packageName = "io.github.arancormonk.dsdneo"
    val packageManager = PackageManager()
    val grants = mutableSetOf<String>()
    val grantChecks = mutableListOf<String>()

    fun checkUriPermission(uri: Uri, pid: Int, uid: Int, modeFlags: Int): Int {
        check(pid == Process.myPid() && uid == Process.myUid()) { "the grant is checked for this process: $pid/$uid" }
        check(modeFlags == Intent.FLAG_GRANT_READ_URI_PERMISSION) { "the grant checked is read: $modeFlags" }
        grantChecks += uri.toString()
        return if (uri.toString() in grants) PackageManager.PERMISSION_GRANTED else PackageManager.PERMISSION_DENIED
    }
}
