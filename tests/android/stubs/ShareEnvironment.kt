// SPDX-License-Identifier: GPL-3.0-or-later
package android.os
import java.io.File

// Shared storage's path, which the platform answers mounted or not, and the Download folder's name.
object Environment {
    const val DIRECTORY_DOWNLOADS = "Download"

    @JvmStatic
    @Deprecated("the platform's is deprecated too")
    fun getExternalStorageDirectory(): File = File("/storage/emulated/0")
}
