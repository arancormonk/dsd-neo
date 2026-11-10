// SPDX-License-Identifier: GPL-3.0-or-later
package android.os
import java.io.File

// Shared storage's path, which the platform answers mounted or not, the Download folder's name, the volume states and
// whether the app holds All files access, which a test sets.
object Environment {
    const val DIRECTORY_DOWNLOADS = "Download"
    const val MEDIA_MOUNTED = "mounted"
    const val MEDIA_MOUNTED_READ_ONLY = "mounted_ro"
    const val MEDIA_UNMOUNTED = "unmounted"
    const val MEDIA_CHECKING = "checking"
    const val MEDIA_NOFS = "nofs"
    const val MEDIA_REMOVED = "removed"
    const val MEDIA_UNMOUNTABLE = "unmountable"
    const val MEDIA_BAD_REMOVAL = "bad_removal"
    const val MEDIA_EJECTING = "ejecting"

    var externalStorageManager = false

    @JvmStatic
    @Deprecated("the platform's is deprecated too")
    fun getExternalStorageDirectory(): File = File("/storage/emulated/0")

    @JvmStatic
    fun isExternalStorageManager(): Boolean = externalStorageManager
}
