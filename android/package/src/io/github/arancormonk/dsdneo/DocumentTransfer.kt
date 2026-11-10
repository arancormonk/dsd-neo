// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * The stream half of the document copies (the backup's, #580, and the imports', #832) and of the probes (#831), free of
 * Android so the JVM tests run it.
 *
 * [DocumentCopies] opens the document and the local file; this moves the bytes. The size cap is counted on what was
 * actually read, because a provider's declared size is only a hint, and every chunk first checks the process-wide
 * abort that teardown raises, then the copy's own [Steps].
 */
object DocumentTransfer {
    /** DocumentCopies.copyImportDocument() and probeImportDocument() results. Must match DecoderHost::DocumentCopy. */
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

    /** The steps of a copy with no stop of its own and nobody counting: a save's. */
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
     * BackupModel's destructor and the host's closeRecordingWork()), which also cancels the provider calls in flight. A
     * read or write on a descriptor the provider holds blocked is not interrupted: the copy stops once that call
     * returns.
     */
    fun abort() {
        aborted = true
    }

    /**
     * Read [input] until it has given [max] bytes or ended, however short each read is, telling [steps] of each read:
     * the bytes read. A probe's header (DocumentCopies.probeImportDocument()).
     *
     * @throws IOException on a read error, or once [abort] was called (checked before every read); whatever
     *     [Steps.check] throws.
     */
    fun readUpTo(input: InputStream, max: Int, steps: Steps = NoSteps): ByteArray {
        val buffer = ByteArray(max)
        var filled = 0
        while (filled < max) {
            checkStop(steps)
            val n = input.read(buffer, filled, max - filled)
            if (n < 0) {
                break
            }
            filled += n
            steps.copied(n)
        }
        return buffer.copyOf(filled)
    }

    /**
     * Read the rest of [input], discarding it, and count it on from the [counted] bytes already read, telling [steps]
     * of each chunk: the whole length, or [maxCount] + 1 once there is more than [maxCount] (no read goes past that).
     *
     * @throws IOException on a read error, or once [abort] was called (checked before every chunk); whatever
     *     [Steps.check] throws.
     */
    fun countCapped(input: InputStream, counted: Long, maxCount: Long, steps: Steps = NoSteps): Long {
        val buffer = ByteArray(CHUNK_BYTES)
        var total = counted
        while (total <= maxCount) {
            checkStop(steps)
            val room = maxCount - total
            val n = input.read(buffer, 0, if (room >= CHUNK_BYTES) CHUNK_BYTES else (room + 1).toInt())
            if (n < 0) {
                return total
            }
            total += n
            steps.copied(n)
        }
        return total
    }

    /** Stop a transfer once [abort] was called, or as [steps] says. */
    private fun checkStop(steps: Steps) {
        if (aborted) {
            throw IOException("document transfers were aborted")
        }
        steps.check()
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
            checkStop(steps)
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
