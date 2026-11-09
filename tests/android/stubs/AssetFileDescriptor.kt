// SPDX-License-Identifier: GPL-3.0-or-later
package android.content.res
import android.os.ParcelFileDescriptor
import java.io.Closeable
import java.io.FileInputStream

// A whole-file asset over a ParcelFileDescriptor stub; its stream closes the descriptor, as the platform's does. Open,
// as the platform's is, so a test's provider can hand out a stream it controls.
open class AssetFileDescriptor(val parcelFileDescriptor: ParcelFileDescriptor, val startOffset: Long, val length: Long) :
    Closeable {
    companion object {
        const val UNKNOWN_LENGTH = -1L
    }

    open fun createInputStream(): FileInputStream = ParcelFileDescriptor.AutoCloseInputStream(parcelFileDescriptor)

    override fun close() = parcelFileDescriptor.close()
}
