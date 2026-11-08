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
import android.system.OsConstants
import java.io.File
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import java.nio.file.Files
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

/** A provider's answer to the lookup: one row of the columns asked for, each maybe null. */
private class DocumentCursor(private val projection: List<String>, private val name: String?, private val size: Long?) :
    Cursor {
    override fun getColumnIndex(columnName: String) = projection.indexOf(columnName)
    override fun moveToFirst() = true
    override fun isNull(columnIndex: Int) = value(columnIndex) == null
    override fun getLong(columnIndex: Int) = value(columnIndex) as? Long ?: 0L
    override fun getString(columnIndex: Int) = value(columnIndex)?.toString()
    override fun close() {}

    private fun value(columnIndex: Int): Any? = when (projection.getOrNull(columnIndex)) {
        OpenableColumns.DISPLAY_NAME -> name
        OpenableColumns.SIZE -> size
        else -> null
    }
}

/** Stops a document's read at its second chunk until the test lets it go on. */
private class Pause {
    val reached = Semaphore(0)
    val proceed = Semaphore(0)
}

/** Plays the document providers: each document is a real file, with the quirks a test gives its provider. */
private class FakeResolver : ContentResolver() {
    class Document(val file: File) {
        var declaredSize: Long? = null
        var displayName: String? = null
        var sizeLookupFails = false
        var queryAnswersNull = false
        var pause: Pause? = null
        var refusesWt = false
        var ignoresT = false
        var refusesWrites = false
        var writesFail = false
        var pipe = false
        var holdsQuery = false
        var holdsOpen = false
        var deleteFails = false
    }

    val documents = ConcurrentHashMap<String, Document>()
    val calls: MutableList<String> = Collections.synchronizedList(mutableListOf())
    val projections: MutableList<List<String>> = Collections.synchronizedList(mutableListOf())
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
        projections += projection.orEmpty().toList()
        cancellationSignal?.throwIfCanceled()
        val document = document(uri)
        if (document.holdsQuery) hold(cancellationSignal)
        if (document.sizeLookupFails) throw IllegalArgumentException("no size column here")
        if (document.queryAnswersNull) return null
        return DocumentCursor(projection.orEmpty().toList(), document.displayName, document.declaredSize)
    }

    override fun openAssetFileDescriptor(uri: Uri, mode: String, cancellationSignal: CancellationSignal?): AssetFileDescriptor? {
        calls += "open $mode $uri"
        cancellationSignal?.throwIfCanceled()
        val document = document(uri)
        if (document.holdsOpen) hold(cancellationSignal)
        val descriptor = ParcelFileDescriptor(document.file, mode)
        val pause = document.pause ?: return AssetFileDescriptor(descriptor, 0, AssetFileDescriptor.UNKNOWN_LENGTH)
        return object : AssetFileDescriptor(descriptor, 0, AssetFileDescriptor.UNKNOWN_LENGTH) {
            override fun createInputStream(): FileInputStream = PausingInput(descriptor, pause)
        }
    }

    /** The document's stream, which waits on [pause] before its second read, as a slow download does. */
    private class PausingInput(owner: ParcelFileDescriptor, private val pause: Pause) :
        ParcelFileDescriptor.AutoCloseInputStream(owner) {
        private var reads = 0

        override fun read(b: ByteArray): Int = read(b, 0, b.size)

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (++reads == 2) {
                pause.reached.release()
                if (!pause.proceed.tryAcquire(5, TimeUnit.SECONDS)) throw IOException("a paused read was never let go")
            }
            return super.read(b, off, len)
        }
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
        // A descriptor opened read-only makes every write fail, as a provider's broken pipe or full disk does.
        return ParcelFileDescriptor(document.file, if (document.writesFail) "r" else mode, truncate, document.pipe)
            .also { descriptors[uri.toString()] = it }
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
        DocumentTransfer.LOCAL_FAILED) { "a destination directory that does not exist is this device's failure" }
    // The document reads whole, but the copy cannot take the destination's place: a directory holding a file.
    val occupied = File(directory, "occupied.in").apply { mkdirs() }
    File(occupied, "keep").writeBytes(bytes(1))
    check(DocumentCopies.copyDocumentToFile(resolver, uri, occupied.path, 100) == DocumentTransfer.LOCAL_FAILED) {
        "a copy that cannot be renamed into place is this device's failure"
    }
    check(directory.list()!!.toList() == listOf("occupied.in")) { "a failed rename leaves no temp file" }
    occupied.deleteRecursively()
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
    val descriptor = resolver.descriptors.getValue(uri)
    check(descriptor.closed && descriptor.closedWithError == null) { "a good save closes the document normally" }
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
        check(resolver.descriptors.getValue(uri).closedWithError != null) {
            "a save that failed once open closes the document with an error"
        }
        val (failing, failingUri, failingSaved) = save(old = 0, bytes(10)) { deleteFails = true }
        check(!failingSaved && "delete $failingUri" in failing.calls) { "a delete the provider refuses is only logged" }
    } finally {
        Os.fsyncHook = {}
    }
}

private fun aFailedWriteIntoAPipeTellsTheProvider() {
    val (resolver, uri, saved) = save(old = 0, bytes(3 * DocumentTransfer.CHUNK_BYTES)) { pipe = true; writesFail = true }
    check(!saved) { "a save whose writes fail fails" }
    val descriptor = resolver.descriptors.getValue(uri)
    check(descriptor.closed) { "the failed document is closed" }
    check(!descriptor.closedWithError.isNullOrEmpty()) {
        "a provider reading a pipe learns the save failed, rather than taking a short document for a whole one"
    }
    check(resolver.deleted == listOf(uri)) { "the partial document is deleted too" }
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

/** Import request ids rise, as the import worker's do. */
private var lastRequest = 1_000L

private fun newRequest() = ++lastRequest

private fun copyImport(resolver: FakeResolver, request: Long, uri: String, dest: File, cap: Long) =
    DocumentCopies.copyImportDocument(resolver, request, uri, dest.path, cap)

private fun progressOf(request: Long) = DocumentCopies.importProgress(request).toList()

private fun checkNotRunning(request: Long, what: String, declared: Long = -1L) {
    check(progressOf(request) == listOf(-1L, declared)) {
        "$what reports no progress, only its declared size: ${progressOf(request)}"
    }
}

/** One import copy on its own thread, as the import worker runs it. */
private class ImportCopy(resolver: FakeResolver, request: Long, uri: String, dest: File, cap: Long) {
    @Volatile
    private var result: Int? = null
    private val thread = Thread { result = copyImport(resolver, request, uri, dest, cap) }.apply { start() }

    fun finish(): Int? {
        thread.join(5_000)
        check(!thread.isAlive) { "an import copy returns" }
        return result
    }
}

private fun anImportCopiesWholeAndKeepsItsName() {
    val resolver = FakeResolver()
    val content = bytes(3 * DocumentTransfer.CHUNK_BYTES + 11)
    val uri = resolver.add(content) { declaredSize = content.size.toLong(); displayName = "Talkgroups.csv" }
    val directory = freshDirectory()
    val dest = File(directory, "talkgroups.csv")
    val request = newRequest()
    check(copyImport(resolver, request, uri, dest, content.size.toLong()) == DocumentTransfer.COPIED) {
        "an import exactly at its cap copies"
    }
    check(dest.readBytes().contentEquals(content)) { "the import holds the document's bytes" }
    check(directory.list()!!.toList() == listOf("talkgroups.csv")) { "no temp file is left beside the import" }
    check(resolver.calls == listOf("query $uri", "open r $uri")) { "the lookup comes before the open: ${resolver.calls}" }
    check(resolver.projections == listOf(listOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE))) {
        "one query asks for the name and the size: ${resolver.projections}"
    }
    checkNotRunning(request, "a finished import", content.size.toLong())
    check(DocumentCopies.importName(request) == "Talkgroups.csv") { "the request keeps its document's name" }
    check(!DocumentCopies.importNoSpace(request)) { "a good import reports no full disk" }
    DocumentCopies.forgetImport(request)
    check(DocumentCopies.importName(request) == null) { "a forgotten request keeps nothing" }
    checkNotRunning(request, "a forgotten request")
    check(DocumentCopies.importName(newRequest()) == null) { "an unknown request has no name" }
    checkNotRunning(newRequest(), "an unknown request")
    checkNoTransferInFlight("an import")
}

private fun anImportOverItsCapIsTooLargeAndLeavesNothing() {
    val resolver = FakeResolver()
    val cap = DocumentTransfer.CHUNK_BYTES + 1L
    val directory = freshDirectory()
    val declaredOver = resolver.add(bytes(10)) { declaredSize = cap + 1; displayName = "big.csv" }
    val request = newRequest()
    check(copyImport(resolver, request, declaredOver, File(directory, "a.csv"), cap) == DocumentTransfer.TOO_LARGE) {
        "a declared size over the cap is too large"
    }
    check(resolver.callsFor(declaredOver) == listOf("query $declaredOver")) { "an import declared too large is never opened" }
    check(DocumentCopies.importName(request) == "big.csv") { "a refused import still names its document" }
    checkNotRunning(request, "a refused import", cap + 1)
    // A lie in the other direction: the provider says small, the bytes say large.
    val liar = resolver.add(bytes(cap.toInt() + 1)) { declaredSize = 10 }
    check(copyImport(resolver, request, liar, File(directory, "b.csv"), cap) == DocumentTransfer.TOO_LARGE) {
        "the cap counts the bytes read, not the declared size"
    }
    val unknown = resolver.add(bytes(cap.toInt() + 1))
    check(copyImport(resolver, request, unknown, File(directory, "c.csv"), cap) == DocumentTransfer.TOO_LARGE) {
        "with no declared size, one byte over the cap is too large"
    }
    check(DocumentCopies.importName(request) == null) { "each copy names its own document, or none" }
    check(directory.list()!!.isEmpty()) { "imports over the cap leave nothing" }
    val before = resolver.calls.size
    check(copyImport(resolver, request, unknown, File(""), cap) == DocumentTransfer.FAILED) { "an empty path fails" }
    check(copyImport(resolver, request, unknown, File(directory, "d.csv"), -1) == DocumentTransfer.FAILED) {
        "a negative cap fails"
    }
    check(resolver.calls.size == before) { "an import refused for its arguments asks the provider nothing" }
    DocumentCopies.forgetImport(request)
    checkNoTransferInFlight("imports over the cap")
}

private fun aFailedOrEmptyLookupStillCopies() {
    val resolver = FakeResolver()
    val directory = freshDirectory()
    for ((what, configure) in listOf<Pair<String, FakeResolver.Document.() -> Unit>>(
        "a failed lookup" to { sizeLookupFails = true },
        "a lookup that answers null" to { queryAnswersNull = true },
        "a lookup with no name or size" to {},
    )) {
        val uri = resolver.add(bytes(100), configure)
        val dest = File(directory, "$serial.csv")
        val request = newRequest()
        check(copyImport(resolver, request, uri, dest, 100) == DocumentTransfer.COPIED) { "$what still copies" }
        check(resolver.callsFor(uri) == listOf("query $uri", "open r $uri")) { "$what is asked once, then opened" }
        check(dest.readBytes().contentEquals(bytes(100))) { "$what copies the whole document" }
        check(DocumentCopies.importName(request) == null) { "$what names nothing" }
        DocumentCopies.forgetImport(request)
    }
    checkNoTransferInFlight("imports without a lookup")
}

private fun aCancelledImportNeverStartsUntilItIsForgotten() {
    val resolver = FakeResolver()
    val directory = freshDirectory()
    val uri = resolver.add(bytes(10))
    val request = newRequest()
    DocumentCopies.cancelImport(request)
    check(copyImport(resolver, request, uri, File(directory, "a.csv"), 100) == DocumentTransfer.CANCELLED) {
        "an import cancelled before it starts is cancelled"
    }
    check(copyImport(resolver, request, uri, File(directory, "b.csv"), 100) == DocumentTransfer.CANCELLED) {
        "a cancelled request starts no later copy"
    }
    check(resolver.calls.isEmpty()) { "a cancelled import asks the provider nothing: ${resolver.calls}" }
    check(directory.list()!!.isEmpty()) { "a cancelled import leaves nothing" }
    checkNotRunning(request, "a cancelled import")
    DocumentCopies.forgetImport(request)
    check(copyImport(resolver, request, uri, File(directory, "c.csv"), 100) == DocumentTransfer.COPIED) {
        "a forgotten id holds no cancel"
    }
    DocumentCopies.forgetImport(request)
    checkNoTransferInFlight("cancelled imports")
}

private fun aCancelStopsTheProviderCallAnImportWaitsIn() {
    val resolver = FakeResolver()
    val directory = freshDirectory()
    val heldQuery = resolver.add(bytes(10)) { holdsQuery = true; declaredSize = 10; displayName = "query.csv" }
    val heldOpen = resolver.add(bytes(10)) { holdsOpen = true; declaredSize = 10; displayName = "open.csv" }
    val inQuery = newRequest()
    val inOpen = newRequest()
    val querying = ImportCopy(resolver, inQuery, heldQuery, File(directory, "q.csv"), 100)
    val opening = ImportCopy(resolver, inOpen, heldOpen, File(directory, "o.csv"), 100)
    check(resolver.holding.tryAcquire(2, 5, TimeUnit.SECONDS)) { "both provider calls are held" }
    check(DocumentCopies.transfersInFlight() == 2) { "every import in flight holds a signal" }
    check(progressOf(inQuery) == listOf(0L, -1L) && DocumentCopies.importName(inQuery) == null) {
        "before its lookup answers, an import knows no size or name"
    }
    check(progressOf(inOpen) == listOf(0L, 10L) && DocumentCopies.importName(inOpen) == "open.csv") {
        "the name and size are known before the open: ${progressOf(inOpen)}"
    }
    DocumentCopies.cancelImport(inQuery)
    check(querying.finish() == DocumentTransfer.CANCELLED) { "a cancel during the lookup cancels the import" }
    check(resolver.callsFor(heldQuery) == listOf("query $heldQuery")) { "an import cancelled in its lookup never opens" }
    check(DocumentCopies.transfersInFlight() == 1) { "cancelling one import leaves the other in its open" }
    DocumentCopies.cancelImport(inOpen)
    check(opening.finish() == DocumentTransfer.CANCELLED) { "a cancel during the open cancels the import" }
    check(!resolver.heldTooLong) { "every held provider call was cancelled" }
    check(directory.list()!!.isEmpty()) { "cancelled imports leave nothing" }
    checkNotRunning(inOpen, "a cancelled import", 10L)
    DocumentCopies.forgetImport(inQuery)
    DocumentCopies.forgetImport(inOpen)
    checkNoTransferInFlight("imports cancelled in a provider call")
}

private fun aCancelMidCopyStopsAtTheNextChunk() {
    val resolver = FakeResolver()
    val directory = freshDirectory()
    val content = bytes(3 * DocumentTransfer.CHUNK_BYTES)
    val pause = Pause()
    val uri = resolver.add(content) { declaredSize = content.size.toLong(); this.pause = pause }
    val request = newRequest()
    val copy = ImportCopy(resolver, request, uri, File(directory, "a.csv"), 1L shl 20)
    check(pause.reached.tryAcquire(5, TimeUnit.SECONDS)) { "the import reads its first chunk" }
    check(progressOf(request) == listOf(DocumentTransfer.CHUNK_BYTES.toLong(), content.size.toLong())) {
        "progress counts the bytes copied so far: ${progressOf(request)}"
    }
    DocumentCopies.cancelImport(request)
    pause.proceed.release()
    check(copy.finish() == DocumentTransfer.CANCELLED) { "a cancel mid-copy cancels the import" }
    check(directory.list()!!.isEmpty()) { "an import cancelled mid-copy leaves nothing" }
    checkNotRunning(request, "an import cancelled mid-copy", content.size.toLong())
    DocumentCopies.forgetImport(request)
    checkNoTransferInFlight("an import cancelled mid-copy")
}

/** A bundle's copies share one request id: cancelling a later request never touches an earlier one. */
private fun cancellingOneImportLeavesAnotherRunning() {
    val resolver = FakeResolver()
    val directory = freshDirectory()
    val first = newRequest()
    val second = newRequest()
    val primary = resolver.add(bytes(100))
    check(copyImport(resolver, first, primary, File(directory, "primary.csv"), 1_000) == DocumentTransfer.COPIED) {
        "the first request copies its primary file"
    }
    // Between the first request's two copies, and again while its companion copies, the second is cancelled.
    DocumentCopies.cancelImport(second)
    val pause = Pause()
    val companion = resolver.add(bytes(3 * DocumentTransfer.CHUNK_BYTES)) { this.pause = pause }
    val running = ImportCopy(resolver, first, companion, File(directory, "companion.csv"), 1L shl 20)
    check(pause.reached.tryAcquire(5, TimeUnit.SECONDS)) { "the companion copy is under way" }
    DocumentCopies.cancelImport(second)
    pause.proceed.release()
    check(running.finish() == DocumentTransfer.COPIED) { "cancelling a later request leaves an earlier one running" }
    check(File(directory, "companion.csv").readBytes().contentEquals(bytes(3 * DocumentTransfer.CHUNK_BYTES))) {
        "the earlier request's companion is whole"
    }
    check(copyImport(resolver, second, primary, File(directory, "other.csv"), 1_000) == DocumentTransfer.CANCELLED) {
        "the cancelled request is refused"
    }
    DocumentCopies.cancelImport(first)
    val third = newRequest()
    check(copyImport(resolver, third, primary, File(directory, "third.csv"), 1_000) == DocumentTransfer.COPIED) {
        "cancelling an earlier request leaves a later one alone"
    }
    listOf(first, second, third).forEach(DocumentCopies::forgetImport)
    checkNoTransferInFlight("imports beside a cancelled one")
}

/** A local file that takes no bytes, failing each write with [failure]. */
private fun refusingFile(failure: () -> IOException): (File) -> OutputStream = { file ->
    object : FileOutputStream(file) {
        override fun write(b: ByteArray, off: Int, len: Int) = throw failure()
    }
}

private fun aFullDiskIsALocalFailureThatSaysSo() {
    val resolver = FakeResolver()
    val directory = freshDirectory()
    val uri = resolver.add(bytes(100))
    val writing = DocumentCopies.localOutput
    try {
        val request = newRequest()
        for ((what, failure) in listOf<Pair<String, () -> IOException>>(
            "ENOSPC" to { IOException("write failed", ErrnoException("write", OsConstants.ENOSPC)) },
            "a full disk's message" to { IOException("No space left on device") },
        )) {
            DocumentCopies.localOutput = refusingFile(failure)
            check(copyImport(resolver, request, uri, File(directory, "a.csv"), 1_000) == DocumentTransfer.LOCAL_FAILED) {
                "$what is this device's failure"
            }
            check(DocumentCopies.importNoSpace(request)) { "$what is reported as no space" }
            check(directory.list()!!.isEmpty()) { "$what leaves nothing" }
        }
        DocumentCopies.localOutput = refusingFile { IOException("write failed", ErrnoException("write", 5)) }
        check(copyImport(resolver, request, uri, File(directory, "b.csv"), 1_000) == DocumentTransfer.LOCAL_FAILED) {
            "another write failure is this device's failure"
        }
        check(!DocumentCopies.importNoSpace(request)) { "another write failure is not a full disk, nor is an earlier one" }
        DocumentCopies.localOutput = refusingFile { IOException("write failed", ErrnoException("write", OsConstants.ENOSPC)) }
        check(DocumentCopies.copyDocumentToFile(resolver, uri, File(directory, "c.in").path, 1_000) ==
            DocumentTransfer.LOCAL_FAILED) { "a backup copy onto a full disk is this device's failure, as before" }
        check(directory.list()!!.isEmpty()) { "failed local writes leave nothing" }
        DocumentCopies.forgetImport(request)
        check(!DocumentCopies.importNoSpace(request)) { "a forgotten request reports no full disk" }
    } finally {
        DocumentCopies.localOutput = writing
    }
    checkNoTransferInFlight("imports onto a full disk")
}

/** Last: the abort is sticky for the rest of the process. */
private fun anAbortCancelsHeldProviderCallsAndRefusesNewOnes() {
    val resolver = FakeResolver()
    val heldQuery = resolver.add(bytes(10)) { holdsQuery = true }
    val heldRead = resolver.add(bytes(10)) { holdsOpen = true }
    val heldSave = resolver.add(bytes(10)) { holdsOpen = true }
    val heldImport = resolver.add(bytes(10)) { holdsOpen = true }
    val importRequest = newRequest()
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
        Thread { results["import"] = copyImport(resolver, importRequest, heldImport, File(directory, "i.csv"), 100) },
    )
    threads.forEach(Thread::start)
    check(resolver.holding.tryAcquire(4, 5, TimeUnit.SECONDS)) { "four provider calls are held" }
    check(settlingNow.tryAcquire(5, TimeUnit.SECONDS)) { "a save is settling" }
    check(DocumentCopies.transfersInFlight() == 5) { "every transfer in flight holds a signal" }

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
    check(results["import"] == DocumentTransfer.FAILED) { "an aborted import fails: it was not cancelled" }
    check(resolver.deleted.isEmpty()) { "after an abort nothing is deleted" }
    check(directory.list()!!.isEmpty()) { "aborted copies leave nothing" }
    checkNoTransferInFlight("aborted transfers")

    val before = resolver.calls.size
    check(DocumentCopies.copyDocumentToFile(resolver, settling, File(directory, "late.in").path, 100) ==
        DocumentTransfer.FAILED) { "a copy started after the abort fails" }
    check(!DocumentCopies.copyFileToDocument(resolver, sourceFile(bytes(10)).path, settling)) {
        "a save started after the abort fails"
    }
    check(copyImport(resolver, newRequest(), settling, File(directory, "late.csv"), 100) == DocumentTransfer.FAILED) {
        "an import started after the abort fails"
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
        aFailedWriteIntoAPipeTellsTheProvider()
        aSaveThatNeverOpensLeavesTheDocumentAlone()
        anImportCopiesWholeAndKeepsItsName()
        anImportOverItsCapIsTooLargeAndLeavesNothing()
        aFailedOrEmptyLookupStillCopies()
        aCancelledImportNeverStartsUntilItIsForgotten()
        aCancelStopsTheProviderCallAnImportWaitsIn()
        aCancelMidCopyStopsAtTheNextChunk()
        cancellingOneImportLeavesAnotherRunning()
        aFullDiskIsALocalFailureThatSaysSo()
        anAbortCancelsHeldProviderCallsAndRefusesNewOnes()
    } finally {
        scratch.deleteRecursively()
    }
    println("PASS: document copies size, open, trim, sync, report failures, delete and cancel as specified; imports by request")
}
