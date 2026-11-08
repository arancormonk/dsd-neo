// SPDX-License-Identifier: GPL-3.0-or-later
package android.content
import android.content.res.AssetFileDescriptor
import android.database.Cursor
import android.net.Uri
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor

// The resolver surface the document copies and the Download folder save use, with the platform's signatures. A
// test's subclass plays the provider; deleteDocument() stands in for the provider call
// DocumentsContract.deleteDocument() makes.
open class ContentResolver {
    open fun query(
        uri: Uri,
        projection: Array<String>?,
        selection: String?,
        selectionArgs: Array<String>?,
        sortOrder: String?,
        cancellationSignal: CancellationSignal?,
    ): Cursor? = error("no provider for $uri")

    open fun openAssetFileDescriptor(uri: Uri, mode: String, cancellationSignal: CancellationSignal?): AssetFileDescriptor? =
        error("no provider for $uri")

    open fun openFileDescriptor(uri: Uri, mode: String, cancellationSignal: CancellationSignal?): ParcelFileDescriptor? =
        error("no provider for $uri")

    open fun deleteDocument(uri: Uri): Boolean = error("no provider for $uri")

    // MediaStore's rows (the Download folder save, #829) and the app's persisted grants.
    open fun insert(url: Uri, values: ContentValues?): Uri? = error("no provider for $url")

    open fun update(uri: Uri, values: ContentValues?, where: String?, selectionArgs: Array<String>?): Int =
        error("no provider for $uri")

    open fun delete(url: Uri, where: String?, selectionArgs: Array<String>?): Int = error("no provider for $url")

    open val persistedUriPermissions: List<UriPermission> get() = error("no persisted grants")

    open fun releasePersistableUriPermission(uri: Uri, modeFlags: Int): Unit = error("no grant for $uri")
}

// A persisted grant, as the platform lists it.
class UriPermission(val uri: Uri, val isReadPermission: Boolean, val isWritePermission: Boolean)
