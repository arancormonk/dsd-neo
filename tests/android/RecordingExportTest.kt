// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.ContentResolver
import android.net.Uri
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit
import java.util.zip.ZipEntry
import java.util.zip.ZipFile

/** Plays the document providers an export writes into: each document is a real file, with a test's quirks. */
private class Provider : ContentResolver() {
    class Document(val file: File) {
        var ignoresT = false
        var writesFail = false
        var pipe = false
        var holdsOpen = false
    }

    val documents = ConcurrentHashMap<String, Document>()
    val calls: MutableList<String> = Collections.synchronizedList(mutableListOf())
    val deleted: MutableList<String> = Collections.synchronizedList(mutableListOf())
    val descriptors = ConcurrentHashMap<String, ParcelFileDescriptor>()
    val holding = Semaphore(0)

    override fun openFileDescriptor(uri: Uri, mode: String, cancellationSignal: CancellationSignal?): ParcelFileDescriptor? {
        calls += "open $mode $uri"
        cancellationSignal?.throwIfCanceled()
        val document = documents[uri.toString()] ?: throw FileNotFoundException("no document $uri")
        if (document.holdsOpen) {
            holding.release()
            var waits = 0
            while (cancellationSignal?.isCanceled != true && ++waits < 2_500) Thread.sleep(2)
            cancellationSignal?.throwIfCanceled()
        }
        return ParcelFileDescriptor(document.file, if (document.writesFail) "r" else mode,
            mode == "wt" && !document.ignoresT, document.pipe).also { descriptors[uri.toString()] = it }
    }

    override fun deleteDocument(uri: Uri): Boolean {
        calls += "delete $uri"
        deleted += uri.toString()
        return true
    }
}

private val scratch: File = Files.createTempDirectory("dsd-recording-export").toFile()
private var serial = 0
private var nextId = 0L

private fun bytes(size: Int, seed: Int = 0) = ByteArray(size) { (it * 7 + seed).toByte() }

private fun Provider.add(old: ByteArray = ByteArray(0), configure: Provider.Document.() -> Unit = {}): String {
    val name = "zip${++serial}"
    val uri = "content://com.example.documents/document/$name"
    documents[uri] = Provider.Document(File(scratch, "$name.provider").apply { writeBytes(old) }).apply(configure)
    return uri
}

private fun Provider.file(uri: String): File = documents.getValue(uri).file

/** Recordings on disk, as the store names them. */
private fun recordings(vararg sizes: Int): Pair<Array<String>, Array<String>> {
    val folder = File(scratch, "Talkgroups (abcd1234)").apply { mkdirs() }
    val files = sizes.map { size -> File(folder, "call${++serial}.wav").apply { writeBytes(bytes(size, serial)) } }
    return files.map { it.path }.toTypedArray() to files.map { "${folder.name}/${it.name}" }.toTypedArray()
}

private fun result(answer: Array<String>) = answer[0].toInt()

/** A FIFO in the scratch folder, or null where this host has no mkfifo (Windows). */
private fun fifo(): File? {
    if (File.separatorChar != '/') {
        return null
    }
    val fifo = File(scratch, "feed${++serial}.wav")
    return try {
        if (ProcessBuilder("mkfifo", fifo.path).start().waitFor() == 0) fifo else null
    } catch (_: IOException) {
        null
    }
}

private fun checkZipHolds(document: File, paths: Array<String>, names: Array<String>, what: String) {
    ZipFile(document).use { zip ->
        val entries = zip.entries().toList()
        check(entries.map { it.name } == names.toList()) { "$what: the entries are named in order: ${entries.map { it.name }}" }
        for ((index, entry) in entries.withIndex()) {
            check(entry.method == ZipEntry.DEFLATED) { "$what: entries are deflated (level 0), so they stream" }
            val content = zip.getInputStream(entry).use(InputStream::readBytes)
            check(content.contentEquals(File(paths[index]).readBytes())) { "$what: entry $index holds its recording" }
        }
    }
}

private fun anExportWritesAZipIntoARegularFile() {
    val provider = Provider()
    val (paths, names) = recordings(3 * DocumentTransfer.CHUNK_BYTES + 5, 44, 1000)
    // An old, longer document behind a provider that ignores the t of "wt": the export cuts its tail off.
    val uri = provider.add(bytes(3 * DocumentTransfer.CHUNK_BYTES + 4000)) { ignoresT = true }
    val synced = Os.synced.size
    val id = ++nextId
    val answer = RecordingExport.transfer(provider, id, paths, names, uri, true)
    check(answer.toList() == listOf(RecordingExport.DONE.toString())) { "an export of present files is done: ${answer.toList()}" }
    checkZipHolds(provider.file(uri), paths, names, "a regular file")
    check(Os.synced.size == synced + 1) { "the regular file is synced" }
    val descriptor = provider.descriptors.getValue(uri)
    check(descriptor.closed && descriptor.closedWithError == null && provider.deleted.isEmpty()) {
        "a whole export is closed normally and kept"
    }
    check(RecordingExport.progress(id) == -1L) { "a finished export reports no progress" }
}

private fun anExportIntoAPipeIsNeitherTrimmedNorSynced() {
    val provider = Provider()
    val (paths, names) = recordings(100, 2 * DocumentTransfer.CHUNK_BYTES)
    val uri = provider.add { pipe = true }
    val synced = Os.synced.size
    val answer = RecordingExport.transfer(provider, ++nextId, paths, names, uri, true)
    check(result(answer) == RecordingExport.DONE) { "an export into a pipe is done" }
    check(Os.synced.size == synced) { "a pipe is not synced" }
    checkZipHolds(provider.file(uri), paths, names, "a pipe")
}

private fun aFailedWriteIntoAPipeTellsTheProviderAndDeletes() {
    val provider = Provider()
    val (paths, names) = recordings(2 * DocumentTransfer.CHUNK_BYTES)
    val uri = provider.add { pipe = true; writesFail = true }
    val answer = RecordingExport.transfer(provider, ++nextId, paths, names, uri, true)
    check(result(answer) == RecordingExport.FAILED) { "an export whose writes fail fails" }
    check(!provider.descriptors.getValue(uri).closedWithError.isNullOrEmpty()) {
        "the provider reading the pipe learns the export failed"
    }
    check(provider.deleted == listOf(uri)) { "the partial export is deleted" }
}

private fun aFailedCloseFailsTheExportAndDeletesIt() {
    val provider = Provider()
    val (paths, names) = recordings(500)
    val uri = provider.add()
    Os.fsyncHook = { throw ErrnoException("fsync", 5) }
    try {
        val answer = RecordingExport.transfer(provider, ++nextId, paths, names, uri, true)
        check(result(answer) == RecordingExport.FAILED) { "an export whose sync fails at the close fails" }
    } finally {
        Os.fsyncHook = {}
    }
    check(provider.descriptors.getValue(uri).closedWithError != null && provider.deleted == listOf(uri)) {
        "an export that failed at its close is closed with an error and deleted"
    }
}

/** A sink that takes [limit] bytes, then fails like a full disk or a broken pipe; it counts what it got. */
private class ShortSink(private val limit: Long) : OutputStream() {
    var taken = 0L
    var closed = false

    override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)

    override fun write(b: ByteArray, off: Int, len: Int) {
        if (taken + len > limit) throw IOException("No space left on device")
        taken += len
    }

    override fun close() {
        closed = true
    }
}

private val noSteps = object : RecordingExport.Steps {
    override fun check() {}

    override fun copied(bytes: Int) {}
}

private fun aShortWriteFailsTheZipWithoutClosingTheSink() {
    val (paths, names) = recordings(4 * DocumentTransfer.CHUNK_BYTES, 300)
    for (limit in listOf(0L, 10L, DocumentTransfer.CHUNK_BYTES + 1L, 4L * DocumentTransfer.CHUNK_BYTES + 200)) {
        val sink = ShortSink(limit)
        val missing = mutableListOf<String>()
        val failed = try {
            RecordingExport.writeZip(sink, paths.toList(), names.toList(), noSteps, RecordingExport::openRecording, missing)
            false
        } catch (_: IOException) {
            true
        }
        check(failed) { "a sink that stops taking bytes after $limit fails the ZIP" }
        check(!sink.closed) { "a failed ZIP never closes the document's stream (it owns the descriptor)" }
        check(sink.taken <= limit) { "nothing past the failure reaches the sink" }
    }
    // The last bytes fail: the central directory, written as the ZIP closes.
    val whole = ShortSink(Long.MAX_VALUE)
    RecordingExport.writeZip(whole, paths.toList(), names.toList(), noSteps, RecordingExport::openRecording, mutableListOf())
    val atTheEnd = ShortSink(whole.taken - 1)
    val failed = try {
        RecordingExport.writeZip(atTheEnd, paths.toList(), names.toList(), noSteps, RecordingExport::openRecording, mutableListOf())
        false
    } catch (_: IOException) {
        true
    }
    check(failed && !whole.closed && !atTheEnd.closed) { "a close that cannot write the central directory fails the ZIP" }
}

private fun cancellingOneExportLeavesTheNextAlone() {
    val provider = Provider()
    val (paths, names) = recordings(1000)

    // Cancelled before it began: it never asks the provider.
    val early = provider.add()
    val earlyId = ++nextId
    RecordingExport.cancel(earlyId)
    check(result(RecordingExport.transfer(provider, earlyId, paths, names, early, true)) == RecordingExport.CANCELLED) {
        "an export cancelled before it began is cancelled"
    }
    check(provider.calls.isEmpty()) { "an export cancelled before it began asks the provider nothing" }

    // Cancelled while the provider holds its open: the document was never opened, so it is left as it was.
    val held = provider.add { holdsOpen = true }
    val heldId = ++nextId
    var heldAnswer = emptyArray<String>()
    val waiting = Thread { heldAnswer = RecordingExport.transfer(provider, heldId, paths, names, held, true) }
    waiting.start()
    check(provider.holding.tryAcquire(5, TimeUnit.SECONDS)) { "the provider holds the open" }
    RecordingExport.cancel(heldId)
    waiting.join(5_000)
    check(!waiting.isAlive && result(heldAnswer) == RecordingExport.CANCELLED) { "a cancelled open cancels the export" }
    check(provider.calls.filter { it.endsWith(held) } == listOf("open wt $held")) { "a cancelled open is not retried in w" }
    check(held !in provider.deleted) { "a document never opened is not deleted" }

    // Cancelled between chunks: the recording is a FIFO the test feeds, so the export waits mid-entry. Only where the
    // host makes FIFOs (Linux, macOS; Android's own storage does too); Windows skips this case.
    val fifo = fifo()
    if (fifo == null) {
        println("SKIP: no mkfifo here; the cancel between chunks did not run")
    } else {
        val midway = provider.add()
        val midwayId = ++nextId
        var midwayAnswer = emptyArray<String>()
        val copying = Thread {
            midwayAnswer = RecordingExport.transfer(provider, midwayId, arrayOf(fifo.path), arrayOf("feed.wav"), midway, true)
        }
        copying.start()
        FileOutputStream(fifo).use { feed ->
            feed.write(bytes(1000))
            feed.flush()
            var waits = 0
            while (RecordingExport.progress(midwayId) < 1000 && ++waits < 2_500) Thread.sleep(2)
            check(RecordingExport.progress(midwayId) == 1000L) { "the export reports the bytes it copied" }
            RecordingExport.cancel(midwayId)
            feed.write(bytes(10))
        }
        copying.join(5_000)
        check(!copying.isAlive && result(midwayAnswer) == RecordingExport.CANCELLED) { "a cancel between chunks cancels" }
        check(provider.descriptors.getValue(midway).closedWithError != null && midway in provider.deleted) {
            "an export cancelled once open is closed with an error and deleted"
        }
    }

    // The next export is its own: no cancel carries over, unlike the teardown abort.
    val next = provider.add()
    val nextAnswer = RecordingExport.transfer(provider, ++nextId, paths, names, next, true)
    check(result(nextAnswer) == RecordingExport.DONE) { "the export after a cancelled one is done" }
    checkZipHolds(provider.file(next), paths, names, "the export after a cancel")
    check(DocumentCopies.transfersInFlight() == 0) { "no transfer stays registered" }
}

private fun recordingsGoneAreLeftOutAndReported() {
    val provider = Provider()
    val (paths, names) = recordings(100, 200, 300)
    File(paths[1]).delete()
    val uri = provider.add()
    val answer = RecordingExport.transfer(provider, ++nextId, paths, names, uri, true)
    check(answer.toList() == listOf(RecordingExport.DONE.toString(), paths[1])) {
        "a recording gone is reported, and the rest exported: ${answer.toList()}"
    }
    checkZipHolds(provider.file(uri), arrayOf(paths[0], paths[2]), arrayOf(names[0], names[2]), "with one gone")

    val allGone = provider.add()
    File(paths[0]).delete()
    File(paths[2]).delete()
    val none = RecordingExport.transfer(provider, ++nextId, arrayOf(paths[0], paths[2]), arrayOf(names[0], names[2]), allGone, true)
    check(none.toList() == listOf(RecordingExport.MISSING.toString(), paths[0], paths[2])) { "all gone is missing" }
    check(allGone in provider.deleted) { "an export with nothing in it is not kept" }
}

private fun aSaveCopiesOneRecordingAsItIs() {
    val provider = Provider()
    val (paths, _) = recordings(2 * DocumentTransfer.CHUNK_BYTES + 9)
    val uri = provider.add(bytes(5 * DocumentTransfer.CHUNK_BYTES)) { ignoresT = true }
    val answer = RecordingExport.transfer(provider, ++nextId, paths, emptyArray(), uri, false)
    check(answer.toList() == listOf(RecordingExport.DONE.toString())) { "a save is done" }
    check(provider.file(uri).readBytes().contentEquals(File(paths[0]).readBytes())) { "a save is the recording as it is" }

    val gone = provider.add()
    File(paths[0]).delete()
    val missing = RecordingExport.transfer(provider, ++nextId, paths, emptyArray(), gone, false)
    check(missing.toList() == listOf(RecordingExport.MISSING.toString(), paths[0])) { "a save of a recording gone is missing" }
    val refused = RecordingExport.transfer(provider, ++nextId, arrayOf(), arrayOf(), provider.add(), false)
    check(result(refused) == RecordingExport.FAILED) { "a save of nothing fails" }
}

private fun repeatedNamesStayApart() {
    val provider = Provider()
    val (paths, _) = recordings(10, 20, 30)
    val uri = provider.add()
    val names = arrayOf("Explore/a.wav", "Explore/a.wav", "Explore/a.wav")
    check(result(RecordingExport.transfer(provider, ++nextId, paths, names, uri, true)) == RecordingExport.DONE) {
        "an export with repeated names is done"
    }
    checkZipHolds(provider.file(uri), paths, arrayOf("Explore/a.wav", "Explore/a (2).wav", "Explore/a (3).wav"), "repeated names")
}

/** Keeps only the last bytes it is given: enough for a ZIP's end records, whatever the ZIP's size. */
private class TailSink : OutputStream() {
    private val tail = ByteArray(4096)
    var total = 0L

    override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)

    override fun write(b: ByteArray, off: Int, len: Int) {
        for (i in 0 until len) tail[((total + i) % tail.size).toInt()] = b[off + i]
        total += len
    }

    fun last(count: Int): ByteBuffer {
        val out = ByteArray(count)
        for (i in 0 until count) out[i] = tail[((total - count + i) % tail.size).toInt()]
        return ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
    }
}

private fun moreThan65535EntriesWriteZip64() {
    val count = 70_000
    val paths = (0 until count).map { "virtual/$it.wav" }
    val names = (0 until count).map { "Explore/%05d.wav".format(it) }
    val sink = TailSink()
    val written = RecordingExport.writeZip(sink, paths, names, noSteps, { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }, mutableListOf())
    check(written == count) { "every entry is written" }
    // The end: ZIP64 end record (56 bytes), its locator (20), then the classic end record (22, no comment).
    val end = sink.last(22)
    check(end.getInt(0) == 0x06054b50) { "the ZIP ends with its end record" }
    check(end.getShort(10).toInt() and 0xffff == 0xffff) { "the classic record's entry count is saturated" }
    val zip64 = sink.last(22 + 20 + 56)
    check(zip64.getInt(0) == 0x06064b50) { "a ZIP64 end record precedes it" }
    check(zip64.getLong(32) == count.toLong()) { "the ZIP64 end record counts every entry: ${zip64.getLong(32)}" }
    check(zip64.getInt(56) == 0x07064b50) { "with its locator" }
}

/** An Error (not an Exception) still fails the document: its descriptor closes and its signal leaves the transfers. */
private fun anErrorStillFailsTheDocument() {
    val provider = Provider()
    val uri = provider.add()
    val injected = AssertionError("injected")
    val write = DocumentCopies.beginWrite(provider, uri) ?: error("the document opens")
    val thrown = try {
        write.complete { throw injected }
        null
    } catch (e: Throwable) {
        e
    }
    check(thrown === injected) { "the Error goes on up as it was: $thrown" }
    check(provider.descriptors.getValue(uri).closedWithError != null && provider.deleted == listOf(uri)) {
        "the document is closed with an error and deleted"
    }
    check(DocumentCopies.transfersInFlight() == 0) { "and its signal is no longer in flight" }

    // In the middle of a ZIP: the same Error comes out, and the document's stream is never closed.
    val (paths, names) = recordings(100, 100)
    val sink = ShortSink(Long.MAX_VALUE)
    val zipped = try {
        RecordingExport.writeZip(sink, paths.toList(), names.toList(), noSteps, { path ->
            if (path == paths[1]) throw injected else RecordingExport.openRecording(path)
        }, mutableListOf())
        null
    } catch (e: Throwable) {
        e
    }
    check(zipped === injected && !sink.closed) { "an Error mid-ZIP comes out unchanged, the stream left open: $zipped" }
}

/** Last: the teardown abort is sticky for the rest of the process. */
private fun anAbortStopsExportsForGood() {
    val provider = Provider()
    val (paths, names) = recordings(100)
    DocumentCopies.abort()
    val uri = provider.add()
    check(result(RecordingExport.transfer(provider, ++nextId, paths, names, uri, true)) == RecordingExport.FAILED) {
        "an export after the teardown abort fails"
    }
    check(provider.calls.isEmpty()) { "an export after the abort asks the provider nothing" }
}

fun main() {
    try {
        anExportWritesAZipIntoARegularFile()
        anExportIntoAPipeIsNeitherTrimmedNorSynced()
        aFailedWriteIntoAPipeTellsTheProviderAndDeletes()
        aFailedCloseFailsTheExportAndDeletesIt()
        aShortWriteFailsTheZipWithoutClosingTheSink()
        cancellingOneExportLeavesTheNextAlone()
        recordingsGoneAreLeftOutAndReported()
        aSaveCopiesOneRecordingAsItIs()
        repeatedNamesStayApart()
        moreThan65535EntriesWriteZip64()
        anErrorStillFailsTheDocument()
        anAbortStopsExportsForGood()
    } finally {
        scratch.deleteRecursively()
    }
    println("PASS: call recording exports stream a ZIP into the document, fail, cancel and report gone files as specified")
}
