// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * The stream half of the document copies (the backup's, #580, and the imports', #832), free of Android so the JVM tests
 * run it.
 *
 * [DocumentCopies] opens the document and the local file; this moves the bytes. The size cap is counted on what was
 * actually read, because a provider's declared size is only a hint, and every chunk first checks the process-wide
 * abort that teardown raises, then the copy's own [Steps].
 */
object DocumentTransfer {
    /** DocumentCopies.copyDocumentToFile() and copyImportDocument() results. Must match DecoderHost::DocumentCopy. */
    const val COPIED = 0
    const val TOO_LARGE = 1
    const val FAILED = 2

    /** The document could be read, but this device could not write the copy: a temp file, a write or the rename. */
    const val LOCAL_FAILED = 3

    /** The import request the copy belongs to was cancelled. Only import copies answer it; an abort is FAILED. */
    const val CANCELLED = 4

    /** A failure on the local side of a copy (the file DSD-neo writes), not on the document's. */
    class LocalFailure(cause: IOException) : IOException(cause.message, cause)

    /** [target] with every failure it raises reported as a [LocalFailure]. */
    class LocalOutput(private val target: OutputStream) : OutputStream() {
        override fun write(b: Int) = local { target.write(b) }

        override fun write(b: ByteArray, off: Int, len: Int) = local { target.write(b, off, len) }

        override fun flush() = local { target.flush() }

        override fun close() = local { target.close() }

        private inline fun local(operation: () -> Unit) {
            try {
                operation()
            } catch (e: LocalFailure) {
                throw e
            } catch (e: IOException) {
                throw LocalFailure(e)
            }
        }
    }

    const val CHUNK_BYTES = 64 * 1024

    /** What one copy does around each chunk: its own stop, and a count of the bytes it wrote. */
    interface Steps {
        /** Throw to stop the copy; called before every chunk is read. */
        fun check()

        /** [count] more bytes were written. */
        fun copied(count: Int)
    }

    /** The steps of a copy with no stop of its own and nobody counting: the backup's. */
    object NoSteps : Steps {
        override fun check() {}

        override fun copied(count: Int) {}
    }

    @Volatile
    private var aborted = false

    /** Whether [abort] was called. */
    val isAborted: Boolean
        get() = aborted

    /**
     * Stop every copy at its next chunk, and fail every later one, for the rest of the process.
     *
     * Only teardown raises it, through DocumentCopies.abort() (DecoderHostAndroid::abortDocumentTransfers(), from
     * BackupModel's destructor), which also cancels the provider calls in flight. A read or write on a descriptor the
     * provider holds blocked is not interrupted: the copy stops once that call returns.
     */
    fun abort() {
        aborted = true
    }

    /**
     * Copy [input] to [output], at most [maxBytes] of it, telling [steps] of each chunk.
     *
     * @return the bytes copied, or -1 once [input] holds more than [maxBytes]; no chunk that crosses the cap is
     *     written.
     * @throws IOException on a read or write error, or once [abort] was called (checked before every chunk); whatever
     *     [Steps.check] throws.
     */
    fun copyCapped(input: InputStream, output: OutputStream, maxBytes: Long, steps: Steps = NoSteps): Long {
        val buffer = ByteArray(CHUNK_BYTES)
        var total = 0L
        while (true) {
            if (aborted) {
                throw IOException("document transfers were aborted")
            }
            steps.check()
            val n = input.read(buffer)
            if (n < 0) {
                return total
            }
            if (n > maxBytes - total) {
                return -1
            }
            output.write(buffer, 0, n)
            total += n
            steps.copied(n)
        }
    }
}
