// SPDX-License-Identifier: GPL-3.0-or-later
package android.content
import android.content.res.AssetFileDescriptor
import android.database.Cursor
import android.net.Uri
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor

// The resolver surface the backup's document copies use, with the platform's signatures. A test's subclass plays the
// provider; deleteDocument() stands in for the provider call DocumentsContract.deleteDocument() makes.
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
}
