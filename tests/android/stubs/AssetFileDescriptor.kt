// SPDX-License-Identifier: GPL-3.0-or-later
package android.content.res
import android.os.ParcelFileDescriptor
import java.io.Closeable
import java.io.FileInputStream

// A whole-file asset over a ParcelFileDescriptor stub; its stream closes the descriptor, as the platform's does.
class AssetFileDescriptor(val parcelFileDescriptor: ParcelFileDescriptor, val startOffset: Long, val length: Long) :
    Closeable {
    companion object {
        const val UNKNOWN_LENGTH = -1L
    }

    fun createInputStream(): FileInputStream = ParcelFileDescriptor.AutoCloseInputStream(parcelFileDescriptor)

    override fun close() = parcelFileDescriptor.close()
}
