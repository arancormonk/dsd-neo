// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.ContentResolver
import android.database.Cursor
import android.net.Uri
import android.os.CancellationSignal
import android.os.OperationCanceledException
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream

/**
 * The document copies of the imports (#832) and of the backup's restore (#831), the probes of documents opened from
 * other apps (#831), and the backup's save (#580): the Android half that DecoderHostAndroid calls over JNI.
 *
 * The copies run on the Qt import worker thread and the save on the backup's worker, never on a main thread. The Qt
 * side calls them straight from that thread and must not wait on either main thread, which teardown may be blocking.
 * They use the application's ContentResolver, because the Activity may be finishing.
 *
 * Each transfer registers a CancellationSignal and passes it to every provider call it makes. [abort], at teardown,
 * cancels those calls (a size lookup, or an open a cloud provider holds while it downloads), stops the byte copy at
 * its next chunk, and refuses every later transfer. An import copy can also be cancelled on its own, by its request id
 * ([cancelImport]).
 *
 * A write into a document is a [DocumentWrite] ([beginWrite]): the backup's save and the call recording exports
 * (#708, RecordingExport) settle, close, report and delete a document the same way.
 */
object DocumentCopies {
    private const val TAG = "dsd-neo"

    /** The signals of the transfers in flight; also the lock that orders a transfer's start against [abort]. */
    private val inFlight = HashSet<CancellationSignal>()

    /** The import requests by id, until [forgetImport]; under [inFlight]'s lock. */
    private val imports = HashMap<Long, ImportRequest>()

    /** Opens the local file a copy writes. The tests swap it to fill the disk. */
    @Volatile
    internal var localOutput: (File) -> OutputStream = { FileOutputStream(it) }

    /**
     * One import request: whether it was cancelled, and its latest copy. A CSV and its companion files are copied one
     * after another under the same request, so a cancel stops the copy running and refuses every later one. Its
     * fields are guarded by [inFlight].
     */
    private class ImportRequest {
        var cancelled = false
        var copy: ImportCopy? = null
    }

    /** One copy of [owner]: its signal, and what it found and did ([importProgress], [importName], [importNoSpace]). */
    private class ImportCopy(val owner: ImportRequest) : DocumentTransfer.Steps {
        val signal = CancellationSignal()

        /** Guarded by [inFlight]. */
        var running = true

        @Volatile
        var name: String? = null

        @Volatile
        var declared = -1L

        @Volatile
        var bytes = 0L

        @Volatile
        var noSpace = false

        override fun check() = signal.throwIfCanceled()

        override fun copied(count: Int) {
            bytes += count // Only the copying thread writes it.
        }
    }

    /**
     * Cancel every transfer in flight and refuse every later one, for the rest of the process.
     *
     * A transfer starting concurrently either registers before the flag is raised, and is cancelled here, or sees the
     * flag and never starts: both happen under [inFlight]'s lock.
     */
    @JvmStatic
    fun abort() {
        val pending = synchronized(inFlight) {
            DocumentTransfer.abort()
            inFlight.toList()
        }
        pending.forEach(CancellationSignal::cancel)
    }

    /** How many transfers hold a signal; the tests check that none leaks. */
    internal fun transfersInFlight(): Int = synchronized(inFlight) { inFlight.size }

    /** Count [signal] in flight, so [abort] cancels it; false once aborted. */
    private fun register(signal: CancellationSignal): Boolean = synchronized(inFlight) {
        !DocumentTransfer.isAborted && inFlight.add(signal)
    }

    private fun end(signal: CancellationSignal) {
        synchronized(inFlight) { inFlight.remove(signal) }
    }

    /**
     * Copy the document [uriText] names to [destPath] for import [request], at most [maxBytes] of it: a
     * DocumentTransfer result.
     *
     * The copy lands in a private temp file beside [destPath] and is renamed over it only once complete, so no
     * failure, the cap included, leaves a partial file there. One lookup asks the provider for the document's name and
     * size, and both are published ([importName], [importProgress]) before the open; a lookup that fails or answers
     * nothing does not stop the copy. A declared size above the cap is refused before the open, which spares a cloud
     * provider downloading a document that cannot fit; the cap still counts the bytes actually read. While the copy
     * runs, [importProgress] counts its bytes. A cancelled provider call (OperationCanceledException) is a failure like
     * any other. A failure on this device's side (the temp file, a write to it, the rename) is LOCAL_FAILED, not
     * FAILED; a copy onto a full disk is LOCAL_FAILED, and [importNoSpace] then says so.
     *
     * [cancelImport] of [request] stops this copy, in the provider call it waits in or at its next chunk, and refuses
     * every later copy for [request] until [forgetImport]: those answer CANCELLED. An [abort] answers FAILED. Only
     * COPIED leaves a file at [destPath].
     */
    @JvmStatic
    fun copyImportDocument(
        resolver: ContentResolver,
        request: Long,
        uriText: String,
        destPath: String,
        maxBytes: Long,
    ): Int {
        if (!copyable(destPath, maxBytes)) {
            return DocumentTransfer.FAILED
        }
        return runImport(request) { copy -> copyToFile(resolver, uriText, destPath, maxBytes, copy) }
    }

    /**
     * Read the start of the document [uriText] names for import [request] (#831), so a document opened from another
     * app can be told apart (a backup, or a CSV) before it is imported: a record `code;total;header;name` that
     * android/document_probe_record.h reads.
     *
     * code is a DocumentTransfer result: COPIED, FAILED or CANCELLED. header is the document's first
     * [PROBE_HEADER_BYTES] bytes as lowercase hex, fewer only when the document is that short. Only a header that
     * starts with the backup magic is read on, discarding, to count the document's length (total), which stops at
     * [maxCount] + 1; any other leaves total -1 and is never read past its header. The provider's declared size is
     * never asked for: only the bytes read count. One query asks for the document's name (DISPLAY_NAME), which ends
     * the record verbatim, empty when there is none; a lookup that fails or answers nothing still reads. Nothing is
     * written. Every stream is closed before it returns.
     *
     * The probe is an import copy of [request] in every other way: [cancelImport] stops it, in the provider call it
     * waits in or at its next read, and refuses it until [forgetImport] (CANCELLED); [abort] fails it (FAILED); a
     * probe while a copy of the request runs is FAILED; [importName] and [importProgress] see it.
     */
    @JvmStatic
    fun probeImportDocument(resolver: ContentResolver, request: Long, uriText: String, maxCount: Long): String {
        val probe = Probe()
        val code = if (maxCount < 0) {
            DocumentTransfer.FAILED
        } else {
            runImport(request) { import -> readProbe(resolver, uriText, maxCount, import, probe) }
        }
        val counted = code == DocumentTransfer.COPIED
        val header = if (counted) probe.header else ByteArray(0)
        return "$code;${if (counted) probe.total else -1L};${hex(header)};${probe.name.orEmpty()}"
    }

    /**
     * Run [transfer], a copy or probe of import [request], registered as its latest copy and in flight: the request's
     * result once [transfer] returns, CANCELLED for any failure after a cancel, and a refusal without running it once
     * aborted (FAILED), cancelled (CANCELLED), or while another copy of the request runs (FAILED).
     */
    private fun runImport(request: Long, transfer: (ImportCopy) -> Int): Int {
        val copy = synchronized(inFlight) {
            if (DocumentTransfer.isAborted) {
                return DocumentTransfer.FAILED
            }
            val owner = imports.getOrPut(request, ::ImportRequest)
            if (owner.cancelled) {
                return DocumentTransfer.CANCELLED
            }
            if (owner.copy?.running == true) {
                return DocumentTransfer.FAILED // A request's copies run one after another.
            }
            ImportCopy(owner).also {
                inFlight.add(it.signal)
                owner.copy = it
            }
        }
        val result = try {
            transfer(copy)
        } finally {
            synchronized(inFlight) {
                inFlight.remove(copy.signal)
                copy.running = false
            }
        }
        val cancelled = synchronized(inFlight) { copy.owner.cancelled }
        return if (cancelled && result != DocumentTransfer.COPIED) DocumentTransfer.CANCELLED else result
    }

    /** What a probe read: the header, the counted length (-1 when not counted) and the provider's name. */
    private class Probe {
        var header = ByteArray(0)
        var total = -1L
        var name: String? = null
    }

    /**
     * The read [probeImportDocument] describes, under [import]'s signal, into [probe]: a DocumentTransfer result. A
     * cancelled provider call (OperationCanceledException) is a failure like any other.
     */
    private fun readProbe(
        resolver: ContentResolver,
        uriText: String,
        maxCount: Long,
        import: ImportCopy,
        probe: Probe,
    ): Int =
        try {
            val uri = Uri.parse(uriText)
            probe.name = lookUp(resolver, uri, import.signal, NAME_ONLY).first
            import.name = probe.name
            val document = resolver.openAssetFileDescriptor(uri, "r", import.signal)
            if (document == null) {
                DocumentTransfer.FAILED
            } else {
                document.use { opened ->
                    opened.createInputStream().use { input ->
                        probe.header = DocumentTransfer.readUpTo(input, PROBE_HEADER_BYTES, import)
                        if (startsWithBackupMagic(probe.header)) {
                            val read = probe.header.size.toLong()
                            probe.total = DocumentTransfer.countCapped(input, read, maxCount, import)
                        }
                    }
                }
                DocumentTransfer.COPIED
            }
        } catch (e: OperationCanceledException) {
            Log.i(TAG, "a document probe was cancelled")
            DocumentTransfer.FAILED
        } catch (e: Exception) {
            Log.w(TAG, "failed to read the start of a document", e)
            DocumentTransfer.FAILED
        }

    /** Whether [header] starts with [BACKUP_MAGIC]. */
    private fun startsWithBackupMagic(header: ByteArray): Boolean =
        header.size >= BACKUP_MAGIC.size && BACKUP_MAGIC.indices.all { header[it] == BACKUP_MAGIC[it] }

    /** [bytes] as lowercase hex, two digits each. */
    private fun hex(bytes: ByteArray): String = buildString(bytes.size * 2) {
        for (byte in bytes) {
            val value = byte.toInt() and 0xff
            append(HEX_DIGITS[value shr 4])
            append(HEX_DIGITS[value and 0x0f])
        }
    }

    private const val HEX_DIGITS = "0123456789abcdef"

    /** How much a probe reads as its header: a backup's whole header (DSD_BACKUP_HEADER_BYTES, kProbeHeaderBytes). */
    private const val PROBE_HEADER_BYTES = 64

    /*
     * The backup magic, only to tell whether a probe reads on to count. A copy of DSD_BACKUP_MAGIC in
     * src/crypto/backup_seal.c, which stays the only judge of a backup: the C++ side classifies the header this
     * returns. The document_transfer suite checks that the two are the same bytes.
     */
    private const val BACKUP_MAGIC_TEXT = "DSDNEOBK"
    private val BACKUP_MAGIC = BACKUP_MAGIC_TEXT.toByteArray(Charsets.US_ASCII)

    /** Cancel import [request]: the copy it runs, if any, and every later one, until [forgetImport]. */
    @JvmStatic
    fun cancelImport(request: Long) {
        val signal = synchronized(inFlight) {
            val owner = imports.getOrPut(request, ::ImportRequest)
            owner.cancelled = true
            owner.copy?.takeIf { it.running }?.signal
        }
        signal?.cancel()
    }

    /**
     * Import [request]'s latest copy: bytes copied while it runs (-1 once it has ended), then the declared size (-1 when
     * unknown); -1, -1 when there is none. The size outlives the copy, so a refused one can still say how large its
     * document is.
     */
    @JvmStatic
    fun importProgress(request: Long): LongArray = synchronized(inFlight) {
        imports[request]?.copy?.let { longArrayOf(if (it.running) it.bytes else -1L, it.declared) }
    } ?: longArrayOf(-1L, -1L)

    /** The name the provider gave the document of import [request]'s latest copy, or null. */
    @JvmStatic
    fun importName(request: Long): String? = synchronized(inFlight) { imports[request]?.copy?.name }

    /** Whether import [request]'s latest copy failed for want of space on this device. */
    @JvmStatic
    fun importNoSpace(request: Long): Boolean = synchronized(inFlight) { imports[request]?.copy?.noSpace == true }

    /** Drop import [request] and its cancel, once it has ended. */
    @JvmStatic
    fun forgetImport(request: Long) {
        synchronized(inFlight) { imports.remove(request) }
    }

    private fun copyable(destPath: String, maxBytes: Long): Boolean =
        destPath.isNotEmpty() && maxBytes >= 0 && File(destPath).parentFile != null

    /**
     * The copy [copyImportDocument] describes, under [import]'s signal: it reports what it finds and copies, stops on
     * its own cancel (once more before the rename), and records a full disk.
     */
    private fun copyToFile(
        resolver: ContentResolver,
        uriText: String,
        destPath: String,
        maxBytes: Long,
        import: ImportCopy,
    ): Int {
        val directory = File(destPath).parentFile ?: return DocumentTransfer.FAILED
        val signal = import.signal
        var staging: File? = null
        return try {
            val uri = Uri.parse(uriText)
            val (name, declared) = lookUp(resolver, uri, signal, NAME_AND_SIZE)
            import.name = name
            import.declared = declared ?: -1L
            if (declared != null && declared > maxBytes) {
                return DocumentTransfer.TOO_LARGE
            }
            val temp = localTempFile(directory)
            staging = temp
            ownerOnly(temp)
            val document = resolver.openAssetFileDescriptor(uri, "r", signal) ?: return DocumentTransfer.FAILED
            val copied = document.use { opened ->
                opened.createInputStream().use { input ->
                    DocumentTransfer.LocalOutput(localStream(temp)).use { output ->
                        DocumentTransfer.copyCapped(input, output, maxBytes, import)
                    }
                }
            }
            if (copied < 0) {
                return DocumentTransfer.TOO_LARGE
            }
            import.check()
            if (temp.renameTo(File(destPath))) DocumentTransfer.COPIED else DocumentTransfer.LOCAL_FAILED
        } catch (e: DocumentTransfer.LocalFailure) {
            Log.w(TAG, "could not write the copy of a document", e)
            import.noSpace = outOfSpace(e)
            DocumentTransfer.LOCAL_FAILED
        } catch (e: OperationCanceledException) {
            Log.i(TAG, "a document copy was cancelled")
            DocumentTransfer.FAILED
        } catch (e: Exception) {
            Log.w(TAG, "failed to copy a document", e)
            DocumentTransfer.FAILED
        } finally {
            staging?.delete() // A no-op once the rename has landed.
        }
    }

    /**
     * Write the file at [sourcePath] into the document [uriText] names; false on any failure.
     *
     * The document is replaced and settled as [DocumentWrite] describes. A missing source never opens the document.
     * A Download-folder row ([DownloadsSave]) is published once written, and deleted at every other exit; a row that
     * could not be published fails the save, and so does one a discard took before this save began (never written).
     */
    @JvmStatic
    fun copyFileToDocument(resolver: ContentResolver, sourcePath: String, uriText: String): Boolean {
        // A Download-folder row a discard already took is never written.
        val writable = DownloadsSave.take(uriText)
        val signal = CancellationSignal()
        var written = false
        try {
            written = writable && writeFile(resolver, sourcePath, uriText, signal)
        } finally {
            written = DownloadsSave.settle(resolver, uriText, written, signal) && written
        }
        return written
    }

    private fun writeFile(
        resolver: ContentResolver,
        sourcePath: String,
        uriText: String,
        signal: CancellationSignal,
    ): Boolean = try {
        File(sourcePath).inputStream().use { input ->
            val write = beginWrite(resolver, uriText, signal) ?: return false
            write.complete { output -> DocumentTransfer.copyCapped(input, output, Long.MAX_VALUE) }
        }
        true
    } catch (e: Exception) {
        Log.w(TAG, "failed to write a document", e)
        false
    }

    /**
     * Open the document [uriText] names to replace its content, under [signal]: null once [abort] was called.
     *
     * The document is opened in mode "wt", or in "w" where a provider refuses "wt". [signal] is passed to the open and
     * counted in flight until the write's [DocumentWrite.finish] or [DocumentWrite.fail], so [abort] cancels it too; a
     * caller that can cancel its own write (an export) passes its own. An open that fails, a cancelled one included,
     * throws and leaves the document as it was: it may be an older backup the user picked to replace.
     */
    fun beginWrite(
        resolver: ContentResolver,
        uriText: String,
        signal: CancellationSignal = CancellationSignal(),
    ): DocumentWrite? {
        val uri = Uri.parse(uriText)
        if (!register(signal)) {
            return null
        }
        try {
            return DocumentWrite(resolver, uri, signal, openForReplace(resolver, uri, signal))
        } catch (e: Throwable) {
            end(signal)
            throw e
        }
    }

    /**
     * A document [beginWrite] opened to replace: write [output], then [finish] or [fail] it, once.
     *
     * After every write, a file is cut back to the written length (some providers accept "wt" but ignore the t, and
     * "w" never truncates) and synced before it is closed. A sync that fails fails the write. A write that fails
     * closes the document with closeWithError(), so a provider reading the other end of a pipe or socket sees the
     * failure instead of a short document that looks complete, and deletes it, best effort, so a partial document
     * never looks like a good one. After an abort nothing is deleted, because deleteDocument() is one more provider
     * call, takes no signal, and could hold teardown. The descriptor is closed exactly once, here, so no stream owns
     * it.
     */
    class DocumentWrite internal constructor(
        private val resolver: ContentResolver,
        private val uri: Uri,
        private val signal: CancellationSignal,
        private val document: ParcelFileDescriptor,
    ) {
        /** The document's bytes, unbuffered. Never closed: closing it would close the document's descriptor. */
        val output: OutputStream = FileOutputStream(document.fileDescriptor)

        private var ended = false

        /**
         * Run [write] on [output], which answers the bytes it wrote, then [finish]. Any failure, an Error included, is
         * [fail]ed (so the descriptor closes and the signal leaves the transfers in flight) and rethrown.
         */
        fun complete(write: (OutputStream) -> Long) {
            val length = try {
                write(output)
            } catch (e: Throwable) {
                fail(e)
                throw e
            }
            finish(length)
        }

        /** Settle and close the document, [length] bytes long; a failure fails it ([fail]) and is rethrown. */
        fun finish(length: Long) {
            try {
                output.flush()
                settle(document, length)
                document.close()
            } catch (e: Throwable) {
                fail(e)
                throw e
            }
            ended = true
            end(signal)
        }

        /**
         * Close the document with an error and delete it, best effort; a no-op once it ended. A Download-folder row is
         * left to [DownloadsSave.settle], which deletes it through MediaStore: deleteDocument() cannot.
         */
        fun fail(cause: Throwable) {
            if (ended) {
                return
            }
            ended = true
            try {
                document.closeWithError("the document could not be written: ${cause.javaClass.simpleName}")
            } catch (closing: IOException) {
                Log.w(TAG, "could not report a failed write to the document provider", closing)
            }
            if (!DocumentTransfer.isAborted && !DownloadsSave.isPending(uri.toString())) {
                deletePartialDocument(resolver, uri)
            }
            end(signal)
        }
    }

    private val NAME_AND_SIZE = arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)

    /** A probe's lookup: the name alone, since a declared size is only a hint and the probe counts the bytes. */
    private val NAME_ONLY = arrayOf(OpenableColumns.DISPLAY_NAME)

    /**
     * The name and size the provider reports for [uri], in one query of [columns]; each null when it reports none, or
     * was not asked. A lookup that fails reports neither; a cancelled one propagates.
     */
    private fun lookUp(
        resolver: ContentResolver,
        uri: Uri,
        signal: CancellationSignal,
        columns: Array<String>,
    ): Pair<String?, Long?> = try {
        resolver.query(uri, columns, null, null, null, signal)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val name = cursor.column(OpenableColumns.DISPLAY_NAME)?.let(cursor::getString)
                Pair(name?.takeIf(String::isNotBlank), cursor.column(OpenableColumns.SIZE)?.let(cursor::getLong))
            } else {
                null
            }
        } ?: Pair(null, null)
    } catch (e: OperationCanceledException) {
        throw e
    } catch (e: Exception) {
        Log.w(TAG, "lookup failed for a document", e)
        Pair(null, null)
    }

    /** The index of [name] in the cursor's row, or null when it was not asked or holds null. */
    private fun Cursor.column(name: String): Int? = getColumnIndex(name).takeIf { it >= 0 && !isNull(it) }

    /**
     * Whether [failure] is this device running out of room: ENOSPC anywhere in its causes, as an errno or, where
     * nothing carries the errno, in a message.
     */
    private fun outOfSpace(failure: Throwable): Boolean = generateSequence(failure) { it.cause }.take(MAX_CAUSES).any {
        (it is ErrnoException && it.errno == OsConstants.ENOSPC) ||
            it.message.orEmpty().let { message -> "ENOSPC" in message || "No space left on device" in message }
    }

    /** How deep [outOfSpace] looks: far past any real chain, and finite on a cyclic one. */
    private const val MAX_CAUSES = 16

    /** [uri] opened to replace its content: "wt" where the provider takes it, else "w". A cancelled open propagates. */
    private fun openForReplace(resolver: ContentResolver, uri: Uri, signal: CancellationSignal): ParcelFileDescriptor {
        try {
            resolver.openFileDescriptor(uri, "wt", signal)?.let { return it }
        } catch (e: OperationCanceledException) {
            throw e
        } catch (e: Exception) {
            // Some providers refuse "wt" (FileNotFoundException, IllegalArgumentException or
            // UnsupportedOperationException, by provider); "w" is the mode every writable document takes.
            Log.i(TAG, "document provider refused mode wt; writing with w", e)
        }
        return resolver.openFileDescriptor(uri, "w", signal) ?: throw IOException("the document provider gave no descriptor")
    }

    /**
     * Cut a saved file back to [length], a no-op when it is no longer, and flush it to storage.
     *
     * A descriptor that is no file (a provider's pipe; statSize -1) has neither an old tail nor a disk behind it.
     */
    private fun settle(document: ParcelFileDescriptor, length: Long) {
        val size = document.statSize
        if (size < 0) {
            return
        }
        if (size > length) {
            Os.ftruncate(document.fileDescriptor, length)
        }
        Os.fsync(document.fileDescriptor)
    }

    private fun deletePartialDocument(resolver: ContentResolver, uri: Uri) {
        try {
            if (!DocumentsContract.deleteDocument(resolver, uri)) {
                Log.w(TAG, "could not delete a partly written document")
            }
        } catch (e: Exception) {
            Log.w(TAG, "could not delete a partly written document", e)
        }
    }

    /** A temp file in [directory] for a copy; a failure is this device's ([DocumentTransfer.LocalFailure]). */
    private fun localTempFile(directory: File): File = try {
        File.createTempFile(".document", ".tmp", directory)
    } catch (e: IOException) {
        throw DocumentTransfer.LocalFailure(e)
    }

    /** [file] opened for writing; a failure is this device's ([DocumentTransfer.LocalFailure]). */
    private fun localStream(file: File): OutputStream = try {
        localOutput(file)
    } catch (e: IOException) {
        throw DocumentTransfer.LocalFailure(e)
    }

    /** Readable and writable by the owner only, best effort: app-internal storage already isolates the file. */
    private fun ownerOnly(file: File) {
        file.setReadable(false, false)
        file.setWritable(false, false)
        file.setReadable(true, true)
        file.setWritable(true, true)
    }
}
