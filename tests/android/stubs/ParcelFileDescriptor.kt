// SPDX-License-Identifier: GPL-3.0-or-later
package android.os
import java.io.Closeable
import java.io.File
import java.io.FileDescriptor
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.RandomAccessFile

// A descriptor over a real file, so the streams, the trim and the sync act on bytes a test reads back. [truncate]
// is what mode "wt" does when the provider honours it; [pipe] makes the descriptor report what a pipe does (no size,
// statSize -1). Closing is idempotent, as the platform's is; closeWithError() keeps the message it was given, for a
// test to check what the provider at the other end would have been told.
class ParcelFileDescriptor(file: File, mode: String, truncate: Boolean = false, private val pipe: Boolean = false) :
    Closeable {
    private val opened = RandomAccessFile(file, if (mode == "r") "r" else "rw")
    var closed = false
        private set
    var closedWithError: String? = null
        private set

    init {
        if (truncate) opened.setLength(0)
    }

    val fileDescriptor: FileDescriptor get() = opened.fd
    val statSize: Long get() = if (pipe) -1 else opened.length()

    override fun close() {
        if (!closed) {
            closed = true
            opened.close()
        }
    }

    fun closeWithError(msg: String?) {
        if (!closed) {
            closedWithError = msg ?: ""
        }
        close()
    }

    class AutoCloseInputStream(private val owner: ParcelFileDescriptor) : FileInputStream(owner.fileDescriptor) {
        override fun close() {
            try {
                super.close()
            } finally {
                owner.close()
            }
        }
    }

    class AutoCloseOutputStream(private val owner: ParcelFileDescriptor) : FileOutputStream(owner.fileDescriptor) {
        override fun close() {
            try {
                super.close()
            } finally {
                owner.close()
            }
        }
    }
}
