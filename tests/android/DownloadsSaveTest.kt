// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.ContentResolver
import android.content.ContentValues
import android.content.Intent
import android.content.UriPermission
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import java.io.File
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.IOException
import java.nio.file.Files
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

private const val NAME = MediaStore.MediaColumns.DISPLAY_NAME
private const val PENDING = MediaStore.MediaColumns.IS_PENDING
private const val EXPIRES = MediaStore.MediaColumns.DATE_EXPIRES

/** A provider's answer to the name lookup: one row, one text column, maybe null. */
private class NameCursor(private val name: String?) : Cursor {
    override fun getColumnIndex(columnName: String) = if (columnName == NAME) 0 else -1
    override fun moveToFirst() = true
    override fun isNull(columnIndex: Int) = name == null
    override fun getLong(columnIndex: Int) = 0L
    override fun getString(columnIndex: Int) = name
    override fun close() {}
}

/**
 * Plays MediaStore's Download collection: each row is a real file. Every call is kept; the update's answer, the name
 * read back, an open or write that fails, and an update held until the test lets it go are the test's to set.
 */
private class MediaRows : ContentResolver() {
    private val serial = AtomicInteger()
    val files = ConcurrentHashMap<String, File>()
    val inserted = ConcurrentHashMap<String, Map<String, Any?>>()
    val updates = ConcurrentHashMap<String, Map<String, Any?>>()
    val calls: MutableList<String> = Collections.synchronizedList(mutableListOf())

    @Volatile var insertAnswer: (() -> Uri?)? = null
    @Volatile var updateAnswer = 1
    @Volatile var name: String? = "published.bin"
    @Volatile var opensFail = false
    @Volatile var writesFail = false
    @Volatile var updating: CountDownLatch? = null
    @Volatile var releaseUpdate: CountDownLatch? = null

    // Persisted grants: the ones listed, the release that fails, and each release made.
    @Volatile var grants: () -> List<UriPermission> = { emptyList() }
    @Volatile var refusedGrant: Uri? = null
    val released: MutableList<Pair<String, Int>> = Collections.synchronizedList(mutableListOf())

    fun callsFor(uri: String): List<String> = synchronized(calls) { calls.filter { it.endsWith(" $uri") } }

    override fun insert(url: Uri, values: ContentValues?): Uri? {
        insertAnswer?.let { return it() }
        check(url == MediaStore.Downloads.EXTERNAL_CONTENT_URI) { "a row goes in the Download collection" }
        val uri = "$url/${serial.incrementAndGet()}"
        files[uri] = File(scratch, "row${serial.get()}").apply { writeBytes(ByteArray(0)) }
        inserted[uri] = values!!.values.toMap()
        calls += "insert $uri"
        return Uri.parse(uri)
    }

    override fun openFileDescriptor(
        uri: Uri,
        mode: String,
        cancellationSignal: CancellationSignal?,
    ): ParcelFileDescriptor? {
        calls += "open $mode $uri"
        cancellationSignal?.throwIfCanceled()
        if (opensFail) throw FileNotFoundException("the row went")
        val file = files[uri.toString()] ?: throw FileNotFoundException("no row $uri")
        return ParcelFileDescriptor(file, if (writesFail) "r" else mode, mode == "wt")
    }

    override fun update(uri: Uri, values: ContentValues?, where: String?, selectionArgs: Array<String>?): Int {
        calls += "update $uri"
        updates[uri.toString()] = values!!.values.toMap()
        updating?.countDown()
        releaseUpdate?.await(10, TimeUnit.SECONDS)
        return updateAnswer
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
        check(projection?.toList() == listOf(NAME)) { "only the name is read back" }
        return NameCursor(name)
    }

    override fun delete(url: Uri, where: String?, selectionArgs: Array<String>?): Int {
        calls += "delete $url"
        return if (files.remove(url.toString())?.delete() == true) 1 else 0
    }

    override fun deleteDocument(uri: Uri): Boolean {
        calls += "deleteDocument $uri"
        return true
    }

    override val persistedUriPermissions: List<UriPermission> get() = grants()

    override fun releasePersistableUriPermission(uri: Uri, modeFlags: Int) {
        if (uri == refusedGrant) throw SecurityException("no grant held for $uri")
        released += uri.toString() to modeFlags
    }
}

private val scratch: File = Files.createTempDirectory("dsd-downloads-save").toFile()
private val rows = MediaRows()
private var nextId = 1_000L

/** A FIFO in the scratch folder, or null where this host has no mkfifo (Windows). */
private fun fifo(): File? {
    if (File.separatorChar != '/') {
        return null
    }
    val fifo = File(scratch, "feed${nextId}.wav")
    return try {
        if (ProcessBuilder("mkfifo", fifo.path).start().waitFor() == 0) fifo else null
    } catch (_: IOException) {
        null
    }
}

private fun sourceFile(size: Int): File =
    File.createTempFile("source", ".bin", scratch).apply { writeBytes(ByteArray(size) { it.toByte() }) }

private fun recording(size: Int): Array<String> = arrayOf(sourceFile(size).path)

/** A pending row as the picker's insert makes it. */
private fun row(name: String = "backup.dsdbak"): String =
    DownloadsSave.insert(rows, name, "application/octet-stream").also { check(it.isNotEmpty()) }

/** Wait until the worker ran everything queued before now. */
private fun drainWorker() {
    DownloadsSave.worker.submit {}.get(5, TimeUnit.SECONDS)
}

private fun deleted(uri: String) = "delete $uri" in rows.callsFor(uri)

/** Published, and nothing else asked of MediaStore after the insert and the write's open. */
private fun publishedOnly(uri: String) =
    rows.callsFor(uri).filter { !it.startsWith("insert") && !it.startsWith("open") } ==
        listOf("update $uri", "query $uri")

/** The worker held on a task of the test's own until the returned latch is released. */
private fun holdWorker(): CountDownLatch {
    val release = CountDownLatch(1)
    val running = CountDownLatch(1)
    DownloadsSave.worker.execute {
        running.countDown()
        release.await(10, TimeUnit.SECONDS)
    }
    check(running.await(5, TimeUnit.SECONDS)) { "the worker is held" }
    return release
}

private fun insertValues() {
    Build.VERSION.SDK_INT = 30
    val uri = row("calls.zip")
    check(rows.inserted.getValue(uri) == mapOf(
        NAME to "calls.zip",
        MediaStore.MediaColumns.MIME_TYPE to "application/octet-stream",
        MediaStore.MediaColumns.RELATIVE_PATH to "Download/DSD-neo",
        PENDING to 1,
    )) { "a pending row in Download/DSD-neo; Android 11+ sets its own expiry: ${rows.inserted.getValue(uri)}" }
    check(DownloadsSave.isPending(uri))

    Build.VERSION.SDK_INT = 29
    DownloadsSave.nowSeconds = { 1_800_000_000L }
    val android10 = row()
    check(rows.inserted.getValue(android10)[EXPIRES] == 1_800_000_000L + 7 * 24 * 60 * 60) { "Android 10: 7 days" }
    Build.VERSION.SDK_INT = 30

    rows.insertAnswer = { null }
    check(DownloadsSave.insert(rows, "x.wav", "audio/x-wav").isEmpty()) { "no row: empty" }
    rows.insertAnswer = { throw IllegalArgumentException("Primary directory not allowed") }
    check(DownloadsSave.insert(rows, "x.wav", "audio/x-wav").isEmpty()) { "a refused insert: empty" }
    rows.insertAnswer = null
    DownloadsSave.discard(uri)
    DownloadsSave.discard(android10)
    drainWorker()
    check(DownloadsSave.pendingCount() == 0 && deleted(uri) && deleted(android10))
}

/** DocumentCopies.copyFileToDocument() publishes a row it wrote, deletes it at every other exit, and never leaks one. */
private fun backupSaveSettles() {
    val bytes = 3 * DocumentTransfer.CHUNK_BYTES + 5
    rows.name = "backup (1).dsdbak"
    val saved = row()
    check(DocumentCopies.copyFileToDocument(rows, sourceFile(bytes).path, saved)) { "a written row is saved" }
    check(rows.files.getValue(saved).length() == bytes.toLong() && publishedOnly(saved)) { "${rows.callsFor(saved)}" }
    check(rows.updates.getValue(saved) == mapOf(PENDING to 0)) { "published: IS_PENDING cleared" }
    check(DownloadsSave.publishedName(saved) == "backup (1).dsdbak") { "the name MediaStore gave it is kept" }

    Build.VERSION.SDK_INT = 29
    val android10 = row()
    check(DocumentCopies.copyFileToDocument(rows, sourceFile(10).path, android10))
    check(rows.updates.getValue(android10) == mapOf(PENDING to 0, EXPIRES to null)) { "Android 10: expiry cleared" }
    Build.VERSION.SDK_INT = 30

    // The exits that write nothing: a missing source (never opened), an open that fails, a write that fails. Each
    // deletes the row through MediaStore, never deleteDocument(), which cannot.
    val missing = row()
    check(!DocumentCopies.copyFileToDocument(rows, File(scratch, "gone").path, missing))
    check(rows.callsFor(missing) == listOf("insert $missing", "delete $missing")) {
        "missing source: ${rows.callsFor(missing)}"
    }
    rows.opensFail = true
    val unopened = row()
    check(!DocumentCopies.copyFileToDocument(rows, sourceFile(10).path, unopened) && deleted(unopened))
    rows.opensFail = false
    rows.writesFail = true
    val unwritten = row()
    check(!DocumentCopies.copyFileToDocument(rows, sourceFile(10).path, unwritten) && deleted(unwritten))
    check(rows.callsFor(unwritten).none { it.startsWith("deleteDocument") || it.startsWith("update") })
    rows.writesFail = false

    // Written, but not published: an update that changes no row, or a name that cannot be read back.
    for ((updated, name) in listOf(0 to "x", 2 to "x", 1 to "", 1 to null)) {
        rows.updateAnswer = updated
        rows.name = name
        val unpublished = row()
        check(!DocumentCopies.copyFileToDocument(rows, sourceFile(10).path, unpublished)) {
            "update $updated, name $name"
        }
        check(deleted(unpublished) && DownloadsSave.publishedName(unpublished).isEmpty())
    }
    rows.updateAnswer = 1
    rows.name = "published.bin"

    // An interrupt once the publication began counts as a late cancel: the save stands, and the interrupt stays raised.
    val interrupted = row()
    rows.updating = CountDownLatch(1)
    rows.releaseUpdate = CountDownLatch(1)
    var stood = false
    var stillInterrupted = false
    val saving = Thread {
        stood = DocumentCopies.copyFileToDocument(rows, sourceFile(10).path, interrupted)
        stillInterrupted = Thread.currentThread().isInterrupted
    }
    saving.start()
    check(rows.updating!!.await(5, TimeUnit.SECONDS)) { "the update began" }
    saving.interrupt()
    saving.join(3 * 100L)
    check(saving.isAlive) { "an interrupt after the boundary waits for the publication" }
    rows.releaseUpdate!!.countDown()
    saving.join(5_000)
    check(!saving.isAlive && stood && stillInterrupted && publishedOnly(interrupted)) { "published, interrupt kept" }
    rows.updating = null
    rows.releaseUpdate = null

    // A document that is no pending row is the provider's as before: no MediaStore call.
    val document = "content://com.example.documents/document/saf"
    rows.files[document] = File(scratch, "saf").apply { writeBytes(ByteArray(0)) }
    check(DocumentCopies.copyFileToDocument(rows, sourceFile(10).path, document))
    check(rows.callsFor(document) == listOf("open wt $document")) { "${rows.callsFor(document)}" }
    check(DownloadsSave.pendingCount() == 0 && DocumentCopies.transfersInFlight() == 0)
}

private fun transfer(uri: String, paths: Array<String> = recording(100), id: Long = ++nextId) =
    RecordingExport.transfer(rows, id, paths, arrayOf("a.wav"), uri, true)[0].toInt()

/** RecordingExport.transfer() publishes a row it wrote, and deletes it at every other exit. */
private fun recordingTransferSettles() {
    val done = row("calls.zip")
    check(transfer(done) == RecordingExport.DONE && publishedOnly(done)) { "an export is saved" }

    val early = row()
    val earlyId = ++nextId
    RecordingExport.cancel(earlyId)
    check(transfer(early, id = earlyId) == RecordingExport.CANCELLED) { "cancelled before it began" }
    drainWorker()
    check(rows.callsFor(early) == listOf("insert $early", "delete $early")) { "never opened, deleted" }

    // Cancelled before it began while the worker is held in another call: no Cancel can reach it any more, so it never
    // waits for the worker; its row goes once the worker is free.
    val stuck = row()
    val stuckId = ++nextId
    RecordingExport.cancel(stuckId)
    val freed = holdWorker()
    var stuckAnswer = -1
    val cancelling = Thread { stuckAnswer = transfer(stuck, id = stuckId) }
    cancelling.start()
    cancelling.join(5_000)
    val returned = !cancelling.isAlive
    freed.countDown()
    cancelling.join(5_000)
    check(returned && stuckAnswer == RecordingExport.CANCELLED) { "a pre-cancelled export never waits on the worker" }
    drainWorker()
    check(rows.callsFor(stuck) == listOf("insert $stuck", "delete $stuck")) { "${rows.callsFor(stuck)}" }

    val gone = row()
    check(transfer(gone, arrayOf(File(scratch, "gone.wav").path)) == RecordingExport.MISSING && deleted(gone))
    val refused = row()
    check(transfer(refused, emptyArray()) == RecordingExport.FAILED && deleted(refused)) { "a refused transfer" }
    rows.writesFail = true
    val unwritten = row()
    check(transfer(unwritten) == RecordingExport.FAILED && deleted(unwritten))
    check(rows.callsFor(unwritten).none { it.startsWith("deleteDocument") }) { "fail() leaves the row to settle" }
    rows.writesFail = false
    rows.updateAnswer = 0
    val unpublished = row()
    check(transfer(unpublished) == RecordingExport.FAILED && deleted(unpublished)) { "a row not published fails" }
    rows.updateAnswer = 1

    // Cancelled while its publication waits behind another row's call: the row is deleted, never published.
    val queued = row()
    val queuedId = ++nextId
    val release = holdWorker()
    var answer = -1
    val exporting = Thread { answer = transfer(queued, id = queuedId) }
    exporting.start()
    var waits = 0
    while (DownloadsSave.isPending(queued) && ++waits < 2_500) Thread.sleep(2)
    check(!DownloadsSave.isPending(queued)) { "the export settles once written" }
    RecordingExport.cancel(queuedId)
    exporting.join(5_000)
    check(!exporting.isAlive && answer == RecordingExport.CANCELLED) { "a cancel before the publication is accepted" }
    release.countDown()
    drainWorker()
    check(deleted(queued) && rows.callsFor(queued).none { it.startsWith("update") }) { "${rows.callsFor(queued)}" }

    // Cancelled once the update began: too late, the file is saved.
    val late = row()
    val lateId = ++nextId
    rows.updating = CountDownLatch(1)
    rows.releaseUpdate = CountDownLatch(1)
    val publishing = Thread { answer = transfer(late, id = lateId) }
    publishing.start()
    check(rows.updating!!.await(5, TimeUnit.SECONDS)) { "the update began" }
    RecordingExport.cancel(lateId)
    // An end that must not come, so no condition to wait for: the bound outlasts two of settle()'s 100 ms checks.
    publishing.join(3 * 100L)
    check(publishing.isAlive) { "a cancel after the boundary waits for the publication" }
    rows.releaseUpdate!!.countDown()
    publishing.join(5_000)
    check(!publishing.isAlive && answer == RecordingExport.DONE && publishedOnly(late)) { "published despite the cancel" }
    rows.updating = null
    rows.releaseUpdate = null

    // Cancelled during the copy itself (a FIFO the test feeds holds the export mid-entry): deleted, Cancelled.
    val fifo = fifo()
    if (fifo == null) {
        println("SKIP: no mkfifo here; the cancel during the copy did not run")
    } else {
        val midway = row()
        val midwayId = ++nextId
        val copying = Thread { answer = transfer(midway, arrayOf(fifo.path), midwayId) }
        copying.start()
        FileOutputStream(fifo).use { feed ->
            feed.write(ByteArray(1000))
            feed.flush()
            var copied = 0
            while (RecordingExport.progress(midwayId) < 1000 && ++copied < 2_500) Thread.sleep(2)
            check(RecordingExport.progress(midwayId) == 1000L) { "the export is mid-copy" }
            RecordingExport.cancel(midwayId)
            feed.write(ByteArray(10))
        }
        copying.join(5_000)
        check(!copying.isAlive && answer == RecordingExport.CANCELLED) { "a cancel during the copy cancels" }
        // A cancelled export does not wait for its row's delete.
        drainWorker()
        check(deleted(midway) && rows.callsFor(midway).none { it.startsWith("update") }) { "${rows.callsFor(midway)}" }
    }
    check(DownloadsSave.pendingCount() == 0 && DocumentCopies.transfersInFlight() == 0)
}

/** A row is published or deleted, never both: whichever claims it first acts, the other does nothing. */
private fun claimActsOnce() {
    val discarded = row()
    DownloadsSave.discard(discarded)
    drainWorker()
    check(!DownloadsSave.settle(rows, discarded, true, null)) { "a settle after a discard fails and publishes nothing" }
    check(rows.callsFor(discarded) == listOf("insert $discarded", "delete $discarded")) { "${rows.callsFor(discarded)}" }

    val settled = row()
    check(DownloadsSave.settle(rows, settled, true, null))
    DownloadsSave.discard(settled)
    drainWorker()
    check(publishedOnly(settled)) { "a published row is not discarded: ${rows.callsFor(settled)}" }

    // A row a write took is the write's: a discard (a refused second request for it) leaves it to the write's settle.
    val taken = row()
    DownloadsSave.take(taken)
    DownloadsSave.discard(taken)
    drainWorker()
    check(DownloadsSave.isPending(taken) && !deleted(taken)) { "a discard never takes a row being written" }
    check(DownloadsSave.settle(rows, taken, true, null) && publishedOnly(taken)) { "the write still publishes it" }

    // A discard that comes before the write took its row (a refused duplicate request, while the accepted one is still
    // on its way) wins: the write fails rather than report a file the discard is deleting.
    val raced = row()
    val racedExport = row()
    val racing = holdWorker()
    DownloadsSave.discard(raced)
    DownloadsSave.discard(racedExport)
    check(!DocumentCopies.copyFileToDocument(rows, sourceFile(10).path, raced)) { "a save into a discarded row fails" }
    check(transfer(racedExport) == RecordingExport.FAILED) { "an export into a discarded row fails" }
    racing.countDown()
    drainWorker()
    for (uri in listOf(raced, racedExport)) {
        check(rows.callsFor(uri) == listOf("insert $uri", "delete $uri")) { "never written: ${rows.callsFor(uri)}" }
    }
    // The last 16 discards are remembered: 15 more forget the first of the two, never the second.
    repeat(15) { DownloadsSave.discard(row()) }
    drainWorker()
    check(DownloadsSave.settle(rows, raced, true, null)) { "the 17th discard forgets the first" }
    check(!DownloadsSave.settle(rows, racedExport, true, null)) { "the 16 last discards are remembered" }

    // A discard never waits, even for a worker held in another call; the row goes once it is free.
    val held = row()
    val release = holdWorker()
    val started = System.nanoTime()
    DownloadsSave.discard(held)
    DownloadsSave.discard("content://media/external/downloads/unknown")
    check(System.nanoTime() - started < TimeUnit.SECONDS.toNanos(1)) { "a discard returns at once" }
    check(!deleted(held))
    release.countDown()
    drainWorker()
    check(deleted(held) && rows.calls.none { it.endsWith("/unknown") }) { "only a pending row is deleted" }
}

/** savedDocumentName() reads the last 16 publications; the oldest goes first. */
private fun publishedIsBounded() {
    val uris = (0 until 17).map { index ->
        rows.name = "saved $index.bin"
        row().also { check(DownloadsSave.settle(rows, it, true, CancellationSignal())) }
    }
    rows.name = "published.bin"
    check(DownloadsSave.publishedName(uris[0]).isEmpty()) { "the 17th publication evicts the first" }
    check(uris.drop(1).withIndex().all { (i, uri) -> DownloadsSave.publishedName(uri) == "saved ${i + 1}.bin" })
}

/** B7: every persisted grant goes, in the modes it holds; one that cannot be released does not stop the rest. */
private fun leftoverGrantsGo() {
    val read = Uri.parse("content://a/read")
    val write = Uri.parse("content://a/write")
    val both = Uri.parse("content://a/both")
    val refused = Uri.parse("content://a/refused")
    rows.grants = {
        listOf(UriPermission(read, true, false), UriPermission(refused, true, true), UriPermission(write, false, true),
            UriPermission(both, true, true))
    }
    rows.refusedGrant = refused
    rows.released.clear()
    DownloadsSave.start(rows)
    drainWorker()
    val readWrite = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
    check(rows.released == listOf("$read" to Intent.FLAG_GRANT_READ_URI_PERMISSION,
        "$write" to Intent.FLAG_GRANT_WRITE_URI_PERMISSION, "$both" to readWrite)) { "${rows.released}" }
    rows.grants = { throw SecurityException("no listing") }
    check(DownloadsSave.releaseGrants(rows) == 0) { "a listing that fails releases nothing and does not throw" }
    rows.grants = { emptyList() }
}

/** Last: the teardown abort is sticky. A settle held in the update returns without it; a later write deletes its row. */
private fun abortLeavesAHeldUpdate() {
    val held = row()
    rows.updating = CountDownLatch(1)
    rows.releaseUpdate = CountDownLatch(1)
    var saved = true
    val saving = Thread { saved = DocumentCopies.copyFileToDocument(rows, sourceFile(10).path, held) }
    saving.start()
    check(rows.updating!!.await(5, TimeUnit.SECONDS)) { "the update began" }
    DocumentCopies.abort()
    saving.join(2_000)
    check(!saving.isAlive && !saved) { "teardown never waits for a held update" }
    check(rows.releaseUpdate!!.count == 1L) { "the update was still held" }
    rows.releaseUpdate!!.countDown()
    drainWorker()
    // The accepted teardown limit: a publication that began finishes on the worker though the save reported failure;
    // the process is ending, and the file is whole.
    check(DownloadsSave.publishedName(held) == "published.bin") { "the held publication still completes" }
    rows.updating = null
    rows.releaseUpdate = null

    // beginWrite() refuses after the abort: the row is deleted, never opened.
    val refused = row()
    check(!DocumentCopies.copyFileToDocument(rows, sourceFile(10).path, refused))
    drainWorker()
    check(rows.callsFor(refused) == listOf("insert $refused", "delete $refused")) { "${rows.callsFor(refused)}" }
}

/** After everything else: with its worker gone, an insert still answers and a settle still deletes the row it claimed. */
private fun aStoppedWorkerStillAnswers() {
    DownloadsSave.worker.shutdownNow()
    var answered: String? = null
    DownloadsSave.insertLater("x.wav", "audio/x-wav") { answered = it }
    check(answered == "") { "an insert with no worker answers empty at once" }
    val stranded = row()
    check(!DownloadsSave.settle(rows, stranded, true, null) && deleted(stranded)) { "a claimed row is deleted here" }
}

fun main() {
    try {
        DownloadsSave.start(rows)
        insertValues()
        backupSaveSettles()
        recordingTransferSettles()
        claimActsOnce()
        publishedIsBounded()
        leftoverGrantsGo()
        abortLeavesAHeldUpdate()
        aStoppedWorkerStillAnswers()
        check(DownloadsSave.pendingCount() == 0) { "no row stays pending" }
    } finally {
        DownloadsSave.worker.shutdownNow()
        scratch.deleteRecursively()
    }
    println("PASS: Download folder rows are inserted, published or deleted at every write exit, discarded and bounded")
}
