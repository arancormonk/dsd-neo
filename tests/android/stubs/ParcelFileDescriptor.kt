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
    private val opened = RandomAccessFile(file, if (mode == "r") "r" else "rw").also { handles.incrementAndGet() }
    var closed = false
        private set
    var closedWithError: String? = null
        private set

    init {
        // A truncate that throws (a descriptor a fake opened read-only for "wt") leaves no object to close: close the
        // file here, or its handle outlives the test and Windows cannot remove it.
        if (truncate) {
            try {
                opened.setLength(0)
            } catch (e: Throwable) {
                opened.close()
                handles.decrementAndGet()
                throw e
            }
        }
    }

    companion object {
        private val handles = java.util.concurrent.atomic.AtomicInteger()

        /** How many file handles the descriptors hold still: none once a test is done, so none keeps a file open. */
        fun openCount(): Int = handles.get()
    }

    val fileDescriptor: FileDescriptor get() = opened.fd
    val statSize: Long get() = if (pipe) -1 else opened.length()

    override fun close() {
        if (!closed) {
            closed = true
            handles.decrementAndGet()
            opened.close()
        }
    }

    fun closeWithError(msg: String?) {
        if (!closed) {
            closedWithError = msg ?: ""
        }
        close()
    }

    open class AutoCloseInputStream(private val owner: ParcelFileDescriptor) : FileInputStream(owner.fileDescriptor) {
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
