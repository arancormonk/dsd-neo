// SPDX-License-Identifier: GPL-3.0-or-later
package android.os
import java.io.File

// Shared storage's path, which the platform answers mounted or not.
object Environment {
    @JvmStatic
    @Deprecated("the platform's is deprecated too")
    fun getExternalStorageDirectory(): File = File("/storage/emulated/0")
}
