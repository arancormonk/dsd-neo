// SPDX-License-Identifier: GPL-3.0-or-later
package android.provider
import android.content.ContentResolver
import android.net.Uri

object OpenableColumns {
    const val DISPLAY_NAME = "_display_name"
    const val SIZE = "_size"
}

// The platform's deleteDocument() is a call into the document's provider; the stub resolver plays that provider.
object DocumentsContract {
    @JvmStatic
    fun deleteDocument(content: ContentResolver, documentUri: Uri): Boolean = content.deleteDocument(documentUri)
}
