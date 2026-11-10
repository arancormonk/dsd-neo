// SPDX-License-Identifier: GPL-3.0-or-later
package android.content
import android.content.pm.PackageManager
import android.net.Uri
import java.io.File

// The context, intents and results the document picker and the file browser use, with the platform's names. A test's
// context carries the resolver, package manager, services and permissions it arranged; an intent keeps what was set on
// it for the test to read.
open class Context {
    open val applicationContext: Context get() = this
    open val contentResolver: ContentResolver = ContentResolver()
    open val packageManager: PackageManager = PackageManager()
    open val packageName: String = "io.github.arancormonk.dsdneo"
    val grantedPermissions = mutableSetOf<String>()

    open fun getExternalFilesDir(type: String?): File? = null

    open fun <T> getSystemService(serviceClass: Class<T>): T? = null

    fun checkSelfPermission(permission: String): Int =
        if (permission in grantedPermissions) PackageManager.PERMISSION_GRANTED else PackageManager.PERMISSION_DENIED
}

class ActivityNotFoundException(message: String? = null) : RuntimeException(message)

data class ComponentName(val packageName: String, val className: String)

class ClipData(vararg uris: Uri?) {
    class Item(val uri: Uri?)

    private val items = uris.map { Item(it) }
    val itemCount: Int get() = items.size

    fun getItemAt(index: Int) = items[index]
}

class Intent(val action: String? = null) {
    constructor(action: String?, uri: Uri?) : this(action) {
        data = uri
    }

    val categories = mutableSetOf<String>()
    val extras = mutableMapOf<String, Any?>()
    var data: Uri? = null
    var clipData: ClipData? = null
    var mimeType: String? = null
        private set
    var target: ComponentName? = null
        private set

    fun addCategory(category: String): Intent = apply { categories += category }
    fun setType(type: String?): Intent = apply { mimeType = type }
    fun setComponent(component: ComponentName?): Intent = apply { target = component }
    fun putExtra(name: String, value: String?): Intent = apply { extras[name] = value }
    fun putExtra(name: String, value: Array<out Any>): Intent = apply { extras[name] = value }

    companion object {
        const val ACTION_OPEN_DOCUMENT = "android.intent.action.OPEN_DOCUMENT"
        const val ACTION_CREATE_DOCUMENT = "android.intent.action.CREATE_DOCUMENT"
        const val ACTION_GET_CONTENT = "android.intent.action.GET_CONTENT"
        const val ACTION_CHOOSER = "android.intent.action.CHOOSER"
        const val CATEGORY_OPENABLE = "android.intent.category.OPENABLE"
        const val EXTRA_TITLE = "android.intent.extra.TITLE"
        const val EXTRA_INTENT = "android.intent.extra.INTENT"
        const val EXTRA_EXCLUDE_COMPONENTS = "android.intent.extra.EXCLUDE_COMPONENTS"
        const val FLAG_GRANT_READ_URI_PERMISSION = 0x00000001
        const val FLAG_GRANT_WRITE_URI_PERMISSION = 0x00000002

        // As the platform's: the target goes in EXTRA_INTENT, and a title only when there is one.
        @JvmStatic
        fun createChooser(target: Intent, title: CharSequence?): Intent = Intent(ACTION_CHOOSER).also {
            it.extras[EXTRA_INTENT] = target
            if (title != null) it.extras[EXTRA_TITLE] = title
        }
    }
}
