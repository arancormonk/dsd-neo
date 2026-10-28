// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.ContentResolver
import android.os.CancellationSignal
import android.os.OperationCanceledException
import android.util.Log
import java.io.File
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.io.FilterOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Call recording export and save (#708, D9): the Android half that DecoderHostAndroid calls over JNI.
 *
 * [transfer] runs on the host's transfer worker thread, never on a main thread, and writes straight into the document
 * the user picked through [DocumentCopies.beginWrite]: no temp copy, and the same truncation, sync, pipe-safe failure
 * and delete as the backup's save. An export is one ZIP; a save copies one recording as it is.
 *
 * Each transfer has its own CancellationSignal: [cancel] stops that transfer only (the user's Cancel), at its next
 * chunk or in the provider call it waits in, and the next transfer runs as usual. DocumentCopies.abort(), at
 * teardown, cancels it too and refuses every later one.
 */
object RecordingExport {
    private const val TAG = "dsd-neo"

    /** [transfer] results. Must match DecoderHost::RecordingTransferResult. */
    const val DONE = 0
    const val FAILED = 1
    const val CANCELLED = 2
    const val MISSING = 3

    /**
     * Entries are DEFLATED at level 0: stored blocks that still stream with data descriptors (STORED would need each
     * entry's CRC and size before its bytes). Measured on the fixture recordings (#708), BEST_SPEED shrinks 8 kHz
     * digital voice by 13% and 48 kHz NFM by 23% at about sixty times the CPU time, which costs more than it saves
     * unless the document takes under about 3 MB/s.
     */
    private const val LEVEL = Deflater.NO_COMPRESSION

    /** One transfer: its id (DecoderHostAndroid's, rising), signal and the source bytes it has copied. */
    private class Transfer(val id: Long, val signal: CancellationSignal) {
        @Volatile
        var bytes = 0L
    }

    private val lock = Any()
    private var current: Transfer? = null

    /** Every transfer up to this id was cancelled; one cancelled before it began never starts. */
    private var cancelledThrough = 0L

    /** Cancel transfer [id], running or still to begin. */
    @JvmStatic
    fun cancel(id: Long) {
        val signal = synchronized(lock) {
            cancelledThrough = maxOf(cancelledThrough, id)
            current?.takeIf { it.id == id }?.signal
        }
        signal?.cancel()
    }

    /** The source bytes transfer [id] has written so far, or -1 when it is not running. */
    @JvmStatic
    fun progress(id: Long): Long = synchronized(lock) { current?.takeIf { it.id == id } }?.bytes ?: -1L

    /**
     * Write [paths] into the document [uriText] names: one ZIP whose entries are named [names] when [zip], else the
     * one file as it is.
     *
     * @return the result code, then the paths found gone (a ZIP leaves them out; all gone is [MISSING]), as text.
     */
    @JvmStatic
    fun transfer(
        resolver: ContentResolver,
        id: Long,
        paths: Array<String>,
        names: Array<String>,
        uriText: String,
        zip: Boolean,
    ): Array<String> {
        val missing = ArrayList<String>()
        val transfer = synchronized(lock) {
            if (id <= cancelledThrough) null else Transfer(id, CancellationSignal()).also { current = it }
        } ?: return arrayOf(CANCELLED.toString())
        val result = try {
            write(resolver, transfer, paths, names, uriText, zip, missing)
        } finally {
            synchronized(lock) {
                if (current === transfer) current = null
            }
        }
        return arrayOf(result.toString(), *missing.toTypedArray())
    }

    private fun write(
        resolver: ContentResolver,
        transfer: Transfer,
        paths: Array<String>,
        names: Array<String>,
        uriText: String,
        zip: Boolean,
        missing: MutableList<String>,
    ): Int {
        if (paths.isEmpty() || (if (zip) names.size != paths.size else paths.size != 1)) {
            return FAILED
        }
        val steps = object : Steps {
            override fun check() {
                if (DocumentTransfer.isAborted) throw IOException("document transfers were aborted")
                transfer.signal.throwIfCanceled()
            }

            override fun copied(bytes: Int) {
                transfer.bytes += bytes
            }
        }
        val write = try {
            DocumentCopies.beginWrite(resolver, uriText, transfer.signal) ?: return FAILED
        } catch (e: Exception) {
            Log.w(TAG, "could not open the document for call recordings", e)
            return if (transfer.signal.isCanceled) CANCELLED else FAILED
        }
        return try {
            val counted = CountingOutputStream(write.output)
            val written = if (zip) {
                writeZip(counted, paths.toList(), names.toList(), steps, ::openRecording, missing)
            } else {
                writeOne(counted, paths[0], steps, missing)
            }
            if (written == 0) {
                // Every recording went while the transfer ran: nothing to keep.
                write.fail(FileNotFoundException("every recording is gone"))
                return MISSING
            }
            write.finish(counted.count)
            DONE
        } catch (e: Exception) {
            Log.w(TAG, "could not write call recordings into a document", e)
            write.fail(e)
            if (transfer.signal.isCanceled) CANCELLED else FAILED
        }
    }

    /** The checks a transfer makes before every chunk, and the progress it reports after each. */
    internal interface Steps {
        /** Throw to stop the transfer. */
        fun check()

        fun copied(bytes: Int)
    }

    /** A recording opened to read, or null when it is gone. */
    internal fun openRecording(path: String): InputStream? = try {
        FileInputStream(path)
    } catch (e: FileNotFoundException) {
        if (File(path).exists()) throw e else null
    }

    /**
     * Write [paths] into [output] as a ZIP, each entry named by [names] (a repeated name gains " (2)", " (3)", ...
     * before its extension), and finish the ZIP; [output] is flushed, never closed. A recording [open] finds gone is left
     * out and added to [missing].
     *
     * @return how many entries were written.
     */
    internal fun writeZip(
        output: OutputStream,
        paths: List<String>,
        names: List<String>,
        steps: Steps,
        open: (String) -> InputStream?,
        missing: MutableList<String>,
    ): Int {
        val used = HashSet<String>()
        var entries = 0
        var failed = false
        // Closing the ZIP finishes it and releases its Deflater. The shield keeps that close from closing the
        // document's descriptor, and once the ZIP failed it drops what a close would still write: that document is
        // about to fail.
        val shield = object : FilterOutputStream(output.buffered(DocumentTransfer.CHUNK_BYTES)) {
            override fun write(b: Int) {
                if (!failed) out.write(b)
            }

            override fun write(b: ByteArray, off: Int, len: Int) {
                if (!failed) out.write(b, off, len)
            }

            override fun flush() {
                if (!failed) out.flush()
            }

            override fun close() = flush()
        }
        val zip = ZipOutputStream(shield)
        try {
            zip.setMethod(ZipOutputStream.DEFLATED)
            zip.setLevel(LEVEL)
            for ((index, path) in paths.withIndex()) {
                steps.check()
                val input = open(path)
                if (input == null) {
                    missing += path
                    continue
                }
                input.use {
                    val entry = ZipEntry(uniqueName(names[index], used))
                    val modified = File(path).lastModified()
                    if (modified > 0) entry.time = modified
                    zip.putNextEntry(entry)
                    copy(it, zip, steps)
                    zip.closeEntry()
                }
                entries++
            }
            zip.close()
        } catch (e: Throwable) {
            failed = true
            try {
                zip.close()
            } catch (_: IOException) {
                // Already failing; the close only releases the Deflater.
            }
            throw e
        }
        return entries
    }

    /** Copy the one recording at [path] into [output] as it is: 1 when it was written, 0 when it is gone. */
    private fun writeOne(output: OutputStream, path: String, steps: Steps, missing: MutableList<String>): Int {
        steps.check()
        val input = openRecording(path)
        if (input == null) {
            missing += path
            return 0
        }
        input.use { copy(it, output, steps) }
        output.flush()
        return 1
    }

    private fun copy(input: InputStream, output: OutputStream, steps: Steps) {
        val buffer = ByteArray(DocumentTransfer.CHUNK_BYTES)
        while (true) {
            steps.check()
            val n = input.read(buffer)
            if (n < 0) {
                return
            }
            output.write(buffer, 0, n)
            steps.copied(n)
        }
    }

    private fun uniqueName(name: String, used: MutableSet<String>): String {
        if (used.add(name)) {
            return name
        }
        val dot = name.lastIndexOf('.').takeIf { it > name.lastIndexOf('/') } ?: name.length
        var copy = 2
        while (true) {
            val candidate = name.substring(0, dot) + " (${copy++})" + name.substring(dot)
            if (used.add(candidate)) {
                return candidate
            }
        }
    }

    /** Counts what reaches the document, the length a regular file is cut back to. */
    private class CountingOutputStream(private val target: OutputStream) : OutputStream() {
        var count = 0L
            private set

        override fun write(b: Int) {
            target.write(b)
            count++
        }

        override fun write(b: ByteArray, off: Int, len: Int) {
            target.write(b, off, len)
            count += len
        }

        override fun flush() = target.flush()
    }
}
