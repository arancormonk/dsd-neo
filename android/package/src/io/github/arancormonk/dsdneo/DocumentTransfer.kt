// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * The stream half of the backup's document copies (#580), free of Android so the JVM tests run it.
 *
 * [DocumentCopies] opens the document and the local file; this moves the bytes. The size cap is counted on what was
 * actually read, because a provider's declared size is only a hint, and every chunk first checks the process-wide
 * abort that teardown raises.
 */
object DocumentTransfer {
    /** DocumentCopies.copyDocumentToFile() results. Must match DecoderHost::DocumentCopy. */
    const val COPIED = 0
    const val TOO_LARGE = 1
    const val FAILED = 2

    const val CHUNK_BYTES = 64 * 1024

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
     * Copy [input] to [output], at most [maxBytes] of it.
     *
     * @return the bytes copied, or -1 once [input] holds more than [maxBytes]; no chunk that crosses the cap is
     *     written.
     * @throws IOException on a read or write error, or once [abort] was called (checked before every chunk).
     */
    fun copyCapped(input: InputStream, output: OutputStream, maxBytes: Long): Long {
        val buffer = ByteArray(CHUNK_BYTES)
        var total = 0L
        while (true) {
            if (aborted) {
                throw IOException("document transfers were aborted")
            }
            val n = input.read(buffer)
            if (n < 0) {
                return total
            }
            if (n > maxBytes - total) {
                return -1
            }
            output.write(buffer, 0, n)
            total += n
        }
    }
}
