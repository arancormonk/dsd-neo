// SPDX-License-Identifier: GPL-3.0-or-later
package androidx.core.content
import android.content.Context
import android.net.Uri
import java.io.File

// The URI the provider hands out names its authority and the file; the test checks which file a share offered. A test
// can make it refuse every path, as the platform's does for one outside the roots it read.
open class FileProvider {
    companion object {
        @Volatile
        var refuses = false

        @JvmStatic
        fun getUriForFile(context: Context, authority: String, file: File): Uri {
            if (refuses) throw IllegalArgumentException("Failed to find configured root that contains ${file.path}")
            return Uri.parse("content://$authority${file.path}")
        }
    }
}
