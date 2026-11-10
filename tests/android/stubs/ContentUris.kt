// SPDX-License-Identifier: GPL-3.0-or-later
package android.content
import android.net.Uri

// A row's URI under its collection, as the platform builds it.
object ContentUris {
    @JvmStatic
    fun withAppendedId(contentUri: Uri, id: Long): Uri = Uri.parse("$contentUri/$id")
}
