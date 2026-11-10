// SPDX-License-Identifier: GPL-3.0-or-later
package android.os
import java.io.File

// Shared storage's path, which the platform answers mounted or not, its state, the Download folder's name and whether
// the app holds All files access, which a test sets.
object Environment {
    const val DIRECTORY_DOWNLOADS = "Download"
    const val MEDIA_MOUNTED = "mounted"
    const val MEDIA_MOUNTED_READ_ONLY = "mounted_ro"
    const val MEDIA_UNMOUNTED = "unmounted"

    var externalStorageManager = false
    var storageState = MEDIA_MOUNTED

    @JvmStatic
    @Deprecated("the platform's is deprecated too")
    fun getExternalStorageDirectory(): File = File("/storage/emulated/0")

    @JvmStatic
    fun getExternalStorageState(): String = storageState

    @JvmStatic
    fun isExternalStorageManager(): Boolean = externalStorageManager
}
