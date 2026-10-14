// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.ContentResolver
import android.content.res.AssetFileDescriptor
import android.database.Cursor
import android.net.Uri
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.system.ErrnoException
import android.system.Os
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.nio.file.Files
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

/** A provider's answer to the size lookup: one row, one column, maybe null. */
private class SizeCursor(private val size: Long?) : Cursor {
    override fun getColumnIndex(columnName: String) = if (columnName == OpenableColumns.SIZE) 0 else -1
    override fun moveToFirst() = true
    override fun isNull(columnIndex: Int) = size == null
    override fun getLong(columnIndex: Int) = size ?: 0L
    override fun close() {}
}

/** Plays the document providers: each document is a real file, with the quirks a test gives its provider. */
private class FakeResolver : ContentResolver() {
    class Document(val file: File) {
        var declaredSize: Long? = null
        var sizeLookupFails = false
        var refusesWt = false
        var ignoresT = false
        var refusesWrites = false
        var pipe = false
        var holdsQuery = false
        var holdsOpen = false
        var deleteFails = false
    }

    val documents = ConcurrentHashMap<String, Document>()
    val calls: MutableList<String> = Collections.synchronizedList(mutableListOf())
    val deleted: MutableList<String> = Collections.synchronizedList(mutableListOf())
    val descriptors = ConcurrentHashMap<String, ParcelFileDescriptor>()

    /** Released once by each provider call that starts holding. */
    val holding = Semaphore(0)

    @Volatile
    var heldTooLong = false

    fun callsFor(uri: String): List<String> = synchronized(calls) { calls.filter { it.endsWith(" $uri") } }

    private fun document(uri: Uri) = documents[uri.toString()] ?: throw FileNotFoundException("no document $uri")

    /** Holds the call until its signal is cancelled, as a provider fetching from the network does, then throws. */
    private fun hold(signal: CancellationSignal?) {
        holding.release()
        var waits = 0
        while (signal?.cancelled != true) {
            if (++waits > 2_500) {
                heldTooLong = true
                throw IOException("a held provider call was never cancelled")
            }
            Thread.sleep(2)
        }
        signal.throwIfCanceled()
    }

    override fun query(
        uri: Uri,
        projection: Array<String>?,
        selection: String?,
        selectionArgs: Array<String>?,
        sortOrder: String?,
        cancellationSignal: CancellationSignal?,
    ): Cursor? {
        calls += "query $uri"
        cancellationSignal?.throwIfCanceled()
        val document = document(uri)
        if (document.holdsQuery) hold(cancellationSignal)
        if (document.sizeLookupFails) throw IllegalArgumentException("no size column here")
        return SizeCursor(document.declaredSize)
    }

    override fun openAssetFileDescriptor(uri: Uri, mode: String, cancellationSignal: CancellationSignal?): AssetFileDescriptor? {
        calls += "open $mode $uri"
        cancellationSignal?.throwIfCanceled()
        val document = document(uri)
        if (document.holdsOpen) hold(cancellationSignal)
        return AssetFileDescriptor(ParcelFileDescriptor(document.file, mode), 0, AssetFileDescriptor.UNKNOWN_LENGTH)
    }

    override fun openFileDescriptor(uri: Uri, mode: String, cancellationSignal: CancellationSignal?): ParcelFileDescriptor? {
        calls += "open $mode $uri"
        cancellationSignal?.throwIfCanceled()
        val document = document(uri)
        if (document.holdsOpen) hold(cancellationSignal)
        if (document.refusesWrites || (mode == "wt" && document.refusesWt)) {
            throw IllegalArgumentException("unsupported mode $mode")
        }
        val truncate = mode == "wt" && !document.ignoresT
        return ParcelFileDescriptor(document.file, mode, truncate, document.pipe).also { descriptors[uri.toString()] = it }
    }

    override fun deleteDocument(uri: Uri): Boolean {
        calls += "delete $uri"
        if (document(uri).deleteFails) throw IllegalStateException("the provider refused the delete")
        deleted += uri.toString()
        return true
    }
}

private val scratch: File = Files.createTempDirectory("dsd-document-copies").toFile()
private var serial = 0

private fun bytes(size: Int, seed: Int = 0) = ByteArray(size) { (it + seed).toByte() }

/** A fresh document holding [content], registered with [resolver] under a content URI. */
private fun FakeResolver.add(content: ByteArray, configure: FakeResolver.Document.() -> Unit = {}): String {
    val name = "doc${++serial}"
    val file = File(scratch, "$name.provider").apply { writeBytes(content) }
    val uri = "content://com.example.documents/document/$name"
    documents[uri] = FakeResolver.Document(file).apply(configure)
    return uri
}

/** A fresh, empty local directory for a copy's destination. */
private fun freshDirectory(): File = File(scratch, "dest${++serial}").apply { mkdirs() }

private fun sourceFile(content: ByteArray): File = File(scratch, "source${++serial}.bin").apply { writeBytes(content) }

private fun checkNoTransferInFlight(what: String) {
    check(DocumentCopies.transfersInFlight() == 0) { "$what leaves no transfer registered" }
}

private fun aDocumentCopiesWhole() {
    val resolver = FakeResolver()
    val content = bytes(3 * DocumentTransfer.CHUNK_BYTES + 11)
    val uri = resolver.add(content) { declaredSize = content.size.toLong() }
    val directory = freshDirectory()
    val dest = File(directory, "copy.in")
    check(DocumentCopies.copyDocumentToFile(resolver, uri, dest.path, 1L shl 20) == DocumentTransfer.COPIED) {
        "a document within the cap copies"
    }
    check(dest.readBytes().contentEquals(content)) { "the copy holds the document's bytes" }
    check(directory.list()!!.toList() == listOf("copy.in")) { "no temp file is left beside the copy" }
    check(resolver.calls == listOf("query $uri", "open r $uri")) { "the size is asked before the open: ${resolver.calls}" }
    checkNoTransferInFlight("a copy")
}

private fun aDeclaredSizeOverTheCapIsRefusedBeforeOpening() {
    val resolver = FakeResolver()
    val uri = resolver.add(bytes(10)) { declaredSize = 2_000 }
    val directory = freshDirectory()
    val dest = File(directory, "copy.in")
    check(DocumentCopies.copyDocumentToFile(resolver, uri, dest.path, 1_000) == DocumentTransfer.TOO_LARGE) {
        "a declared size over the cap is too large"
    }
    check(resolver.calls == listOf("query $uri")) { "a document declared too large is never opened" }
    check(directory.list()!!.isEmpty()) { "a refused copy leaves nothing" }
    checkNoTransferInFlight("a refused copy")
}

private fun anUnknownOrFailedSizeFallsBackToTheCountedCap() {
    val resolver = FakeResolver()
    val cap = DocumentTransfer.CHUNK_BYTES + 1
    val over = resolver.add(bytes(cap + 1))
    val directory = freshDirectory()
    check(DocumentCopies.copyDocumentToFile(resolver, over, File(directory, "a.in").path, cap.toLong()) ==
        DocumentTransfer.TOO_LARGE) { "with no declared size, one byte over the cap is still too large" }
    check(directory.list()!!.isEmpty()) { "a copy over the cap leaves nothing" }
    val fits = resolver.add(bytes(cap)) { sizeLookupFails = true }
    val dest = File(directory, "b.in")
    check(DocumentCopies.copyDocumentToFile(resolver, fits, dest.path, cap.toLong()) == DocumentTransfer.COPIED) {
        "a failed size lookup falls back to the counted cap"
    }
    check(dest.readBytes().contentEquals(bytes(cap))) { "the copy exactly at the cap is whole" }
    // A lie in the other direction: the provider says small, the bytes say large.
    val liar = resolver.add(bytes(cap + 1)) { declaredSize = 10 }
    check(DocumentCopies.copyDocumentToFile(resolver, liar, File(directory, "c.in").path, cap.toLong()) ==
        DocumentTransfer.TOO_LARGE) { "the cap counts the bytes read, not the declared size" }
    checkNoTransferInFlight("capped copies")
}

private fun aCopyThatCannotProceedFailsAndLeavesNothing() {
    val resolver = FakeResolver()
    val directory = freshDirectory()
    val missing = "content://com.example.documents/document/gone"
    check(DocumentCopies.copyDocumentToFile(resolver, missing, File(directory, "a.in").path, 100) ==
        DocumentTransfer.FAILED) { "a document the provider cannot open fails" }
    val uri = resolver.add(bytes(10))
    check(DocumentCopies.copyDocumentToFile(resolver, uri, File(scratch, "nowhere/a.in").path, 100) ==
        DocumentTransfer.FAILED) { "a destination directory that does not exist fails" }
    val before = resolver.calls.size
    check(DocumentCopies.copyDocumentToFile(resolver, uri, "", 100) == DocumentTransfer.FAILED) { "an empty path fails" }
    check(DocumentCopies.copyDocumentToFile(resolver, uri, File(directory, "b.in").path, -1) ==
        DocumentTransfer.FAILED) { "a negative cap fails" }
    check(resolver.calls.size == before) { "a copy refused for its arguments asks the provider nothing" }
    check(directory.list()!!.isEmpty()) { "failed copies leave nothing" }
    checkNoTransferInFlight("failed copies")
}

/** Saves [source] over a document of [old] bytes whose provider [configure] describes; returns it and the result. */
private fun save(old: Int, source: ByteArray, configure: FakeResolver.Document.() -> Unit): Triple<FakeResolver, String, Boolean> {
    val resolver = FakeResolver()
    val uri = resolver.add(bytes(old, seed = 99), configure)
    val saved = DocumentCopies.copyFileToDocument(resolver, sourceFile(source).path, uri)
    checkNoTransferInFlight("a save")
    return Triple(resolver, uri, saved)
}

private fun aSaveReplacesTheDocumentAndSyncsIt() {
    val source = bytes(DocumentTransfer.CHUNK_BYTES + 3)
    val synced = Os.synced.size
    val (resolver, uri, saved) = save(old = 2 * DocumentTransfer.CHUNK_BYTES, source) {}
    check(saved) { "a save to a provider that takes wt succeeds" }
    check(resolver.documents.getValue(uri).file.readBytes().contentEquals(source)) { "the document holds the save" }
    check(resolver.calls == listOf("open wt $uri")) { "wt is asked first: ${resolver.calls}" }
    check(Os.synced.size == synced + 1) { "a saved file is synced before it is closed" }
    check(resolver.deleted.isEmpty()) { "a good save is not deleted" }
}

private fun aProviderThatRefusesWtIsWrittenWithWAndTrimmed() {
    val source = bytes(10)
    val (resolver, uri, saved) = save(old = 500, source) { refusesWt = true }
    check(saved) { "a provider that refuses wt is written with w" }
    check(resolver.calls == listOf("open wt $uri", "open w $uri")) { "w only after wt is refused: ${resolver.calls}" }
    check(resolver.documents.getValue(uri).file.readBytes().contentEquals(source)) {
        "the old tail past a save in w mode is cut off"
    }
}

private fun aProviderThatIgnoresTheTIsTrimmedToo() {
    val source = bytes(10)
    val (resolver, uri, saved) = save(old = 500, source) { ignoresT = true }
    check(saved && resolver.calls == listOf("open wt $uri")) { "a provider that takes wt is not asked for w" }
    check(resolver.documents.getValue(uri).file.readBytes().contentEquals(source)) {
        "the old tail is cut off even when wt did not truncate"
    }
}

private fun aPipeIsNeitherTrimmedNorSynced() {
    val source = bytes(10)
    val synced = Os.synced.size
    // ignoresT keeps the stub's old bytes, so the check below sees that nothing cut them.
    val (resolver, uri, saved) = save(old = 500, source) { pipe = true; ignoresT = true }
    check(saved) { "a save into a pipe succeeds" }
    val written = resolver.documents.getValue(uri).file.readBytes()
    // Only the stub keeps an old tail behind a pipe; a real one has none to cut, and no disk to sync.
    check(written.size == 500 && written.copyOf(10).contentEquals(source)) { "a pipe is not truncated" }
    check(Os.synced.size == synced) { "a pipe is not synced" }
}

private fun aFailedSyncFailsTheSaveAndDeletesTheDocument() {
    Os.fsyncHook = { throw ErrnoException("fsync", 5) }
    try {
        val (resolver, uri, saved) = save(old = 0, bytes(10)) {}
        check(!saved) { "a save whose sync fails fails" }
        check(resolver.deleted == listOf(uri)) { "a save that failed once open is deleted" }
        val (failing, failingUri, failingSaved) = save(old = 0, bytes(10)) { deleteFails = true }
        check(!failingSaved && "delete $failingUri" in failing.calls) { "a delete the provider refuses is only logged" }
    } finally {
        Os.fsyncHook = {}
    }
}

private fun aSaveThatNeverOpensLeavesTheDocumentAlone() {
    val (resolver, uri, saved) = save(old = 50, bytes(10)) { refusesWrites = true }
    check(!saved) { "a document the provider will not open for writing fails the save" }
    check(resolver.calls == listOf("open wt $uri", "open w $uri")) { "both modes are tried: ${resolver.calls}" }
    check(resolver.deleted.isEmpty()) { "a document never opened is not deleted: it may be an older backup" }
    check(resolver.documents.getValue(uri).file.readBytes().contentEquals(bytes(50, seed = 99))) {
        "a document never opened keeps its content"
    }
    val missingSource = FakeResolver()
    val target = missingSource.add(bytes(50))
    check(!DocumentCopies.copyFileToDocument(missingSource, File(scratch, "no-such-source").path, target)) {
        "a missing source fails the save"
    }
    check(missingSource.calls.isEmpty()) { "a missing source never opens the document" }
    checkNoTransferInFlight("failed saves")
}

/** Last: the abort is sticky for the rest of the process. */
private fun anAbortCancelsHeldProviderCallsAndRefusesNewOnes() {
    val resolver = FakeResolver()
    val heldQuery = resolver.add(bytes(10)) { holdsQuery = true }
    val heldRead = resolver.add(bytes(10)) { holdsOpen = true }
    val heldSave = resolver.add(bytes(10)) { holdsOpen = true }
    val settling = resolver.add(bytes(10))
    val directory = freshDirectory()
    val settlingNow = Semaphore(0)
    // A save that fails after the abort, while it settles: its document is left, since a delete could hold teardown.
    Os.fsyncHook = { fd ->
        if (fd === resolver.descriptors[settling]?.fileDescriptor) {
            settlingNow.release()
            var waits = 0
            while (!DocumentTransfer.isAborted && ++waits < 2_500) Thread.sleep(2)
            throw ErrnoException("fsync", 5)
        }
    }
    val results = ConcurrentHashMap<String, Any>()
    val threads = listOf(
        Thread { results["query"] = DocumentCopies.copyDocumentToFile(resolver, heldQuery, File(directory, "q.in").path, 100) },
        Thread { results["read"] = DocumentCopies.copyDocumentToFile(resolver, heldRead, File(directory, "r.in").path, 100) },
        Thread { results["save"] = DocumentCopies.copyFileToDocument(resolver, sourceFile(bytes(10)).path, heldSave) },
        Thread { results["settle"] = DocumentCopies.copyFileToDocument(resolver, sourceFile(bytes(10)).path, settling) },
    )
    threads.forEach(Thread::start)
    check(resolver.holding.tryAcquire(3, 5, TimeUnit.SECONDS)) { "three provider calls are held" }
    check(settlingNow.tryAcquire(5, TimeUnit.SECONDS)) { "a save is settling" }
    check(DocumentCopies.transfersInFlight() == 4) { "every transfer in flight holds a signal" }

    DocumentCopies.abort()
    threads.forEach { it.join(5_000) }
    Os.fsyncHook = {}
    check(threads.none(Thread::isAlive)) { "every transfer returns once aborted" }
    check(!resolver.heldTooLong) { "every held provider call was cancelled" }
    check(results["query"] == DocumentTransfer.FAILED) { "a cancelled size lookup fails the copy" }
    check(resolver.callsFor(heldQuery) == listOf("query $heldQuery")) {
        "a cancelled size lookup is not taken for an unknown size: ${resolver.callsFor(heldQuery)}"
    }
    check(results["read"] == DocumentTransfer.FAILED) { "a cancelled open fails the copy" }
    check(results["save"] == false) { "a cancelled open fails the save" }
    check(resolver.callsFor(heldSave) == listOf("open wt $heldSave")) {
        "a cancelled wt open does not fall back to w: ${resolver.callsFor(heldSave)}"
    }
    check(results["settle"] == false) { "a save that fails after the abort fails" }
    check(resolver.deleted.isEmpty()) { "after an abort nothing is deleted" }
    check(directory.list()!!.isEmpty()) { "aborted copies leave nothing" }
    checkNoTransferInFlight("aborted transfers")

    val before = resolver.calls.size
    check(DocumentCopies.copyDocumentToFile(resolver, settling, File(directory, "late.in").path, 100) ==
        DocumentTransfer.FAILED) { "a copy started after the abort fails" }
    check(!DocumentCopies.copyFileToDocument(resolver, sourceFile(bytes(10)).path, settling)) {
        "a save started after the abort fails"
    }
    check(resolver.calls.size == before) { "a transfer started after the abort asks the provider nothing" }
    checkNoTransferInFlight("refused transfers")
}

fun main() {
    try {
        aDocumentCopiesWhole()
        aDeclaredSizeOverTheCapIsRefusedBeforeOpening()
        anUnknownOrFailedSizeFallsBackToTheCountedCap()
        aCopyThatCannotProceedFailsAndLeavesNothing()
        aSaveReplacesTheDocumentAndSyncsIt()
        aProviderThatRefusesWtIsWrittenWithWAndTrimmed()
        aProviderThatIgnoresTheTIsTrimmedToo()
        aPipeIsNeitherTrimmedNorSynced()
        aFailedSyncFailsTheSaveAndDeletesTheDocument()
        aSaveThatNeverOpensLeavesTheDocumentAlone()
        anAbortCancelsHeldProviderCallsAndRefusesNewOnes()
    } finally {
        scratch.deleteRecursively()
    }
    println("PASS: document copies size, open, trim, sync, delete and cancel through the provider as specified")
}
