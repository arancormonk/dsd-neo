// SPDX-License-Identifier: GPL-3.0-or-later
package androidx.core.content
import android.content.Context
import android.net.Uri
import java.io.File

// The URI the provider hands out names its authority and the file; the test checks which file a share offered.
open class FileProvider {
    companion object {
        @JvmStatic
        fun getUriForFile(context: Context, authority: String, file: File): Uri = Uri.parse("content://$authority${file.path}")
    }
}
