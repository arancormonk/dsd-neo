// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.ContentResolver
import android.net.Uri
import android.os.CancellationSignal
import android.os.OperationCanceledException
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.system.Os
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream

/**
 * The backup's document copies (#580): the Android half that DecoderHostAndroid calls over JNI.
 *
 * Both copies run on the Qt backup worker thread, never on a main thread. The Qt side calls them straight from that
 * thread and must not wait on either main thread, which teardown may be blocking. They use the application's
 * ContentResolver, because the Activity may be finishing.
 *
 * Each transfer registers a CancellationSignal and passes it to every provider call it makes. [abort], at teardown,
 * cancels those calls (a size lookup, or an open a cloud provider holds while it downloads), stops the byte copy at
 * its next chunk, and refuses every later transfer.
 *
 * A write into a document is a [DocumentWrite] ([beginWrite]): the backup's save and the call recording exports
 * (#708, RecordingExport) settle, close, report and delete a document the same way.
 */
object DocumentCopies {
    private const val TAG = "dsd-neo"

    /** The signals of the transfers in flight; also the lock that orders a transfer's start against [abort]. */
    private val inFlight = HashSet<CancellationSignal>()

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

    private fun begin(): CancellationSignal? = CancellationSignal().takeIf(::register)

    /** Count [signal] in flight, so [abort] cancels it; false once aborted. */
    private fun register(signal: CancellationSignal): Boolean = synchronized(inFlight) {
        !DocumentTransfer.isAborted && inFlight.add(signal)
    }

    private fun end(signal: CancellationSignal) {
        synchronized(inFlight) { inFlight.remove(signal) }
    }

    /**
     * Copy the document [uriText] names to [destPath], at most [maxBytes] of it: a DocumentTransfer result.
     *
     * The copy lands in a private temp file beside [destPath] and is renamed over it only once complete, so no
     * failure, the cap included, leaves a partial file there. A size the provider declares above the cap is refused
     * before the open, which spares a cloud provider downloading a document that cannot fit. The cap still counts the
     * bytes actually read. A cancelled provider call (OperationCanceledException) is a failure like any other. A
     * failure on this device's side (the temp file, a write to it, the rename) is LOCAL_FAILED, not FAILED.
     */
    @JvmStatic
    fun copyDocumentToFile(resolver: ContentResolver, uriText: String, destPath: String, maxBytes: Long): Int {
        val directory = File(destPath).parentFile
        if (destPath.isEmpty() || maxBytes < 0 || directory == null) {
            return DocumentTransfer.FAILED
        }
        val signal = begin() ?: return DocumentTransfer.FAILED
        var staging: File? = null
        return try {
            val uri = Uri.parse(uriText)
            val declared = declaredSize(resolver, uri, signal)
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
                        DocumentTransfer.copyCapped(input, output, maxBytes)
                    }
                }
            }
            when {
                copied < 0 -> DocumentTransfer.TOO_LARGE
                temp.renameTo(File(destPath)) -> DocumentTransfer.COPIED
                else -> DocumentTransfer.LOCAL_FAILED
            }
        } catch (e: DocumentTransfer.LocalFailure) {
            Log.w(TAG, "could not write the copy of a document", e)
            DocumentTransfer.LOCAL_FAILED
        } catch (e: Exception) {
            Log.w(TAG, "failed to copy a document", e)
            DocumentTransfer.FAILED
        } finally {
            staging?.delete() // A no-op once the rename has landed.
            end(signal)
        }
    }

    /**
     * Write the file at [sourcePath] into the document [uriText] names; false on any failure.
     *
     * The document is replaced and settled as [DocumentWrite] describes. A missing source never opens the document.
     */
    @JvmStatic
    fun copyFileToDocument(resolver: ContentResolver, sourcePath: String, uriText: String): Boolean = try {
        File(sourcePath).inputStream().use { input ->
            val write = beginWrite(resolver, uriText) ?: return false
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

        /** Close the document with an error and delete it, best effort; a no-op once it ended. */
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
            if (!DocumentTransfer.isAborted) {
                deletePartialDocument(resolver, uri)
            }
            end(signal)
        }
    }

    /** The size the provider reports for [uri], or null when it reports none. A cancelled lookup propagates. */
    private fun declaredSize(resolver: ContentResolver, uri: Uri, signal: CancellationSignal): Long? = try {
        resolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null, signal)?.use { cursor ->
            val column = cursor.getColumnIndex(OpenableColumns.SIZE)
            if (column >= 0 && cursor.moveToFirst() && !cursor.isNull(column)) cursor.getLong(column) else null
        }
    } catch (e: OperationCanceledException) {
        throw e
    } catch (e: Exception) {
        Log.w(TAG, "size lookup failed for a document", e)
        null
    }

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
    private fun localStream(file: File): FileOutputStream = try {
        FileOutputStream(file)
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
