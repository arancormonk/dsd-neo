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
import java.io.FileDescriptor
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.io.FilterInputStream
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

    /** A recording is not the file selected any more: the transfer keeps nothing, and the user exports again. */
    const val CHANGED = 6

    /** Thrown by [openSelected] at a recording that is not the file selected: the whole transfer is abandoned. */
    internal class RecordingChangedException(val path: String) : IOException("not the recording selected: $path")

    /** The store read storage again since the selection ([selectionCurrent]): the whole transfer is abandoned. */
    private class SelectionStaleException : IOException("the recordings were read again since they were selected")

    /** A recording as the store selected it: its size and modification time. */
    internal data class Selected(val size: Long, val seconds: Long, val nanos: Long)

    /**
     * Entries are DEFLATED at level 0: stored blocks that still stream with data descriptors (STORED would need each
     * entry's CRC and size before its bytes). Measured on the fixture recordings (#708), BEST_SPEED shrinks 8 kHz
     * digital voice by 13% and 48 kHz NFM by 23% at about sixty times the CPU time. It pays only for a document that
     * takes bytes slower than about 13% of the deflate rate: about 9 MB/s at the measuring desktop's 66 MB/s, less on
     * a phone, while documents are written to local storage far faster.
     */
    private const val LEVEL = Deflater.NO_COMPRESSION

    /**
     * How a transfer opens each recording: [openSelected]. A test swaps it to feed a FIFO, which the check refuses, to
     * hold an export between chunks.
     */
    @Volatile
    internal var inputOpener: (String, Selected) -> InputStream? = ::openSelected

    /**
     * Whether transfer [id]'s selection still stands: the store has not started reading storage again since it was
     * selected (DecoderHostAndroid's token). Asked before the first recording is read and before the document is kept.
     * A test swaps it.
     */
    @Volatile
    internal var selectionCurrent: (Long) -> Boolean = { DsdNative.nativeRecordingSelectionCurrent(it) }

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
     * one file as it is. [selected] holds three numbers per path, its size and its modification time's seconds and
     * nanoseconds as the store selected it: right before each file is read it must still be that regular file, never
     * a link; one gone is left out, and any other difference abandons the whole transfer ([CHANGED]), writing nothing.
     *
     * A Download-folder row ([DownloadsSave]) is published once written, and deleted at every other exit. Its
     * publication still answers [cancel] until it begins; a row that could not be published fails the transfer, and so
     * does one a discard took before this transfer began (never written).
     *
     * @return the result code, then the paths found gone (a ZIP leaves them out; all gone is [MISSING]) and, for
     * [CHANGED], the one found changed, as text: the store looks at each again. A selection that stopped standing
     * ([selectionCurrent]) is [CHANGED] with none named.
     */
    @JvmStatic
    fun transfer(
        resolver: ContentResolver,
        id: Long,
        paths: Array<String>,
        names: Array<String>,
        selected: LongArray,
        uriText: String,
        zip: Boolean,
    ): Array<String> {
        if (!DownloadsSave.take(uriText)) {
            return arrayOf(FAILED.toString()) // a Download-folder row a discard already took
        }
        val missing = ArrayList<String>()
        val transfer = synchronized(lock) {
            if (id <= cancelledThrough) null else Transfer(id, CancellationSignal()).also { current = it }
        }
        if (transfer == null) {
            // Already cancelled: the row's delete is queued and nothing waits for it, since no Cancel can reach a
            // transfer that never became the current one.
            DownloadsSave.settle(resolver, uriText, false, CancellationSignal().apply { cancel() })
            return arrayOf(CANCELLED.toString())
        }
        var result = FAILED
        try {
            try {
                result = write(resolver, transfer, paths, names, selected, uriText, zip, missing)
            } finally {
                // Still the current transfer, so a cancel reaches the publication.
                result = settled(resolver, transfer, uriText, result)
            }
        } finally {
            synchronized(lock) {
                if (current === transfer) current = null
            }
        }
        return arrayOf(result.toString(), *missing.toTypedArray())
    }

    /**
     * [result] once the document was settled: a written row that does not stand is cancelled or failed, or changed when
     * the selection stopped standing before its publication (asked on DownloadsSave's worker right before it).
     */
    private fun settled(resolver: ContentResolver, transfer: Transfer, uriText: String, result: Int): Int {
        val stands = DownloadsSave.settle(resolver, uriText, result == DONE, transfer.signal) {
            selectionCurrent(transfer.id)
        }
        return when {
            result != DONE || stands -> result
            transfer.signal.isCanceled -> CANCELLED
            !selectionCurrent(transfer.id) -> CHANGED
            else -> FAILED
        }
    }

    private fun write(
        resolver: ContentResolver,
        transfer: Transfer,
        paths: Array<String>,
        names: Array<String>,
        selected: LongArray,
        uriText: String,
        zip: Boolean,
        missing: MutableList<String>,
    ): Int {
        if (paths.isEmpty() || (if (zip) names.size != paths.size else paths.size != 1) ||
            selected.size != paths.size * 3
        ) {
            return FAILED
        }
        val files = HashMap<String, Selected>()
        for ((index, path) in paths.withIndex()) {
            files[path] = Selected(selected[index * 3], selected[index * 3 + 1], selected[index * 3 + 2])
        }
        val open = { path: String -> inputOpener(path, files.getValue(path)) }
        val steps = object : DocumentTransfer.Steps {
            override fun check() {
                if (DocumentTransfer.isAborted) throw IOException("document transfers were aborted")
                transfer.signal.throwIfCanceled()
            }

            override fun copied(count: Int) {
                transfer.bytes += count
            }
        }
        // Before the document is opened for replacement: a transfer already known stale never truncates, or deletes, a
        // document the user picked. A pending Download-folder row made for it goes as for any other end (settled()).
        if (!selectionCurrent(transfer.id)) {
            Log.w(TAG, "call recordings were read again since they were selected; the document is left as it was")
            return CHANGED
        }
        val write = try {
            DocumentCopies.beginWrite(resolver, uriText, transfer.signal) ?: return FAILED
        } catch (e: Exception) {
            Log.w(TAG, "could not open the document for call recordings", e)
            return if (transfer.signal.isCanceled) CANCELLED else FAILED
        }
        return try {
            // Again once the provider's open (which may wait) is done, before the first recording is read.
            if (!selectionCurrent(transfer.id)) throw SelectionStaleException()
            val counted = CountingOutputStream(write.output)
            val written = if (zip) {
                writeZip(counted, paths.toList(), names.toList(), steps, open, missing)
            } else {
                writeOne(counted, paths[0], steps, open, missing)
            }
            if (written == 0) {
                // Every recording went while the transfer ran: nothing to keep.
                write.fail(FileNotFoundException("every recording is gone"))
                return MISSING
            }
            // Read again while it was written: the store may have given a path it named to another recording. Asked
            // again once the document is synced (that can wait), before it is closed as good.
            if (!selectionCurrent(transfer.id)) throw SelectionStaleException()
            write.finish(counted.count) {
                if (!selectionCurrent(transfer.id)) throw SelectionStaleException()
            }
            DONE
        } catch (e: SelectionStaleException) {
            Log.w(TAG, "call recordings were read again since they were selected; nothing is kept", e)
            write.fail(e)
            CHANGED
        } catch (e: RecordingChangedException) {
            Log.w(TAG, "a call recording changed since it was selected; nothing is kept", e)
            write.fail(e)
            missing += e.path // the store looks at it again, so the next export takes what is there now
            CHANGED
        } catch (e: Exception) {
            Log.w(TAG, "could not write call recordings into a document", e)
            write.fail(e)
            if (transfer.signal.isCanceled) CANCELLED else FAILED
        } catch (e: Throwable) {
            // An Error still closes the document and leaves the transfers in flight, then goes on up.
            write.fail(e)
            throw e
        }
    }

    /**
     * The recording at [path] opened once to read and checked to be the file [selected] names
     * ([RecordingDescriptor.open]): what is read is what was checked. Null when it is gone;
     * [RecordingChangedException] otherwise, a link and a name that cannot be read included.
     */
    internal fun openSelected(path: String, selected: Selected): InputStream? =
        when (val opened = RecordingDescriptor.open(path, selected.size, selected.seconds, selected.nanos)) {
            is RecordingDescriptor.Result.Opened -> DescriptorInputStream(opened.fd)
            RecordingDescriptor.Result.Gone -> null
            RecordingDescriptor.Result.Changed -> throw RecordingChangedException(path)
        }

    /** Reads [fd] and closes it with the stream: a FileInputStream made on a descriptor does not own it. */
    private class DescriptorInputStream(private val fd: FileDescriptor) : FilterInputStream(FileInputStream(fd)) {
        override fun close() {
            try {
                super.close()
            } finally {
                RecordingDescriptor.close(fd)
            }
        }
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
        steps: DocumentTransfer.Steps,
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
            } catch (closing: Throwable) {
                // Already failing; the close only releases the Deflater. The first failure is the one reported.
                e.addSuppressed(closing)
            }
            throw e
        }
        return entries
    }

    /** Copy the one recording at [path] into [output] as it is: 1 when it was written, 0 when it is gone. */
    private fun writeOne(
        output: OutputStream,
        path: String,
        steps: DocumentTransfer.Steps,
        open: (String) -> InputStream?,
        missing: MutableList<String>,
    ): Int {
        steps.check()
        val input = open(path)
        if (input == null) {
            missing += path
            return 0
        }
        input.use { copy(it, output, steps) }
        output.flush()
        return 1
    }

    private fun copy(input: InputStream, output: OutputStream, steps: DocumentTransfer.Steps) {
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
