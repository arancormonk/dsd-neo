// SPDX-License-Identifier: GPL-3.0-or-later
package android.content
import android.net.Uri
import java.io.File

// The context, intents and clip a share uses, with the platform's names. A test's context sets its folders and keeps
// the activities it was asked to start.
open class Context {
    open val packageName: String = "io.github.arancormonk.dsdneo"
    open val filesDir: File = File("/data/user/0/io.github.arancormonk.dsdneo/files")
    open fun getExternalFilesDir(type: String?): File? = null
    val started = mutableListOf<Intent>()
    open fun startActivity(intent: Intent) {
        started += intent
    }
}

class ClipData private constructor(val label: CharSequence, val uri: Uri) {
    companion object {
        @JvmStatic
        fun newRawUri(label: CharSequence, uri: Uri) = ClipData(label, uri)
    }
}

class Intent(val action: String) {
    var type: String? = null
    var clipData: ClipData? = null
    var flags = 0
        private set
    val extras = mutableMapOf<String, Any?>()

    fun putExtra(name: String, value: Uri): Intent = apply { extras[name] = value }
    fun putExtra(name: String, value: String): Intent = apply { extras[name] = value }
    fun putExtra(name: String, value: Intent): Intent = apply { extras[name] = value }
    fun addFlags(flags: Int): Intent = apply { this.flags = this.flags or flags }

    companion object {
        const val ACTION_SEND = "android.intent.action.SEND"
        const val ACTION_CHOOSER = "android.intent.action.CHOOSER"
        const val EXTRA_STREAM = "android.intent.extra.STREAM"
        const val EXTRA_SUBJECT = "android.intent.extra.SUBJECT"
        const val EXTRA_INTENT = "android.intent.extra.INTENT"
        const val EXTRA_TITLE = "android.intent.extra.TITLE"
        const val FLAG_GRANT_READ_URI_PERMISSION = 0x00000001
        const val FLAG_GRANT_WRITE_URI_PERMISSION = 0x00000002
        const val FLAG_ACTIVITY_NEW_TASK = 0x10000000

        @JvmStatic
        fun createChooser(target: Intent, title: CharSequence?): Intent =
            Intent(ACTION_CHOOSER).putExtra(EXTRA_INTENT, target).also { it.extras[EXTRA_TITLE] = title }
    }
}
