// SPDX-License-Identifier: GPL-3.0-or-later
package android.provider
import android.net.Uri

// The Download collection, the columns the Download folder save (#829) writes and those the file browser's Saved by
// DSD-neo reads and selects on (#830), with the platform's names.
object MediaStore {
    object Downloads {
        @JvmField
        val EXTERNAL_CONTENT_URI: Uri = Uri.parse("content://media/external/downloads")
    }

    object MediaColumns {
        const val _ID = "_id"
        const val DISPLAY_NAME = "_display_name"
        const val SIZE = "_size"
        const val DATE_MODIFIED = "date_modified"
        const val OWNER_PACKAGE_NAME = "owner_package_name"
        const val MIME_TYPE = "mime_type"
        const val RELATIVE_PATH = "relative_path"
        const val IS_PENDING = "is_pending"
        const val DATE_EXPIRES = "date_expires"
    }
}
