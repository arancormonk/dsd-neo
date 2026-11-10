// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import java.io.File
import java.io.IOException
import java.net.StandardProtocolFamily
import java.net.UnixDomainSocketAddress
import java.nio.channels.ServerSocketChannel
import java.nio.file.AccessDeniedException
import java.nio.file.DirectoryIteratorException
import java.nio.file.DirectoryStream
import java.nio.file.Files
import java.nio.file.NoSuchFileException
import java.nio.file.Path
import java.nio.file.attribute.BasicFileAttributes
import java.nio.file.attribute.FileTime
import java.nio.file.attribute.PosixFilePermissions
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

private const val WAIT_MS = 10_000L

private val scratch: Path = Files.createTempDirectory("dsd-file-listing").toRealPath()
private var serial = 0

/** A new empty folder under the scratch folder, canonical, as a root the browser lists. */
private fun folder(name: String = "root"): Path = Files.createDirectories(scratch.resolve("${name}${++serial}"))

private fun touch(dir: Path, vararg names: String) {
    for (name in names) Files.write(dir.resolve(name), byteArrayOf(1))
}

/** A symbolic link, or null (with the reason printed) where this host cannot make one. */
private fun symlink(link: Path, target: Path): Path? = try {
    Files.createSymbolicLink(link, target)
} catch (e: UnsupportedOperationException) {
    println("SKIP: no symbolic links here (${e.message})")
    null
} catch (e: IOException) {
    println("SKIP: no symbolic links here (${e.message})")
    null
}

/** The attributes of a regular file that exists nowhere, for listings of folders too large to make on disk. */
private class FakeFileAttributes : BasicFileAttributes {
    override fun lastModifiedTime(): FileTime = FileTime.fromMillis(1_700_000_000_000)
    override fun lastAccessTime(): FileTime = lastModifiedTime()
    override fun creationTime(): FileTime = lastModifiedTime()
    override fun isRegularFile() = true
    override fun isDirectory() = false
    override fun isSymbolicLink() = false
    override fun isOther() = false
    override fun size() = 7L
    override fun fileKey(): Any? = null
}

/**
 * The real filesystem with failures and stalls put in where a test asks: an open that throws, an iteration that fails
 * part way, a folder of [synthetic] made-up files, and reads of attributes that wait at [gate] until the test opens it.
 */
private class GatedFs : BrowserFs {
    private val real = NioFs()
    @Volatile var openFailure: IOException? = null
    @Volatile var failIterationAfter = -1
    @Volatile var synthetic = -1
    @Volatile var gate: CountDownLatch? = null

    /** One permit per read of attributes that reached the gate. */
    val arrived = Semaphore(0)

    override fun open(dir: Path): DirectoryStream<Path> {
        openFailure?.let { throw it }
        if (synthetic >= 0) return listOf(dir, synthetic) { index -> dir.resolve("f%06d.csv".format(index)) }
        if (failIterationAfter >= 0) {
            val names = real.open(dir).use { stream -> stream.toList() }
            return listOf(dir, names.size) { index ->
                if (index >= failIterationAfter) throw DirectoryIteratorException(IOException("the drive went away"))
                names[index]
            }
        }
        return real.open(dir)
    }

    override fun attributes(path: Path): BasicFileAttributes {
        gate?.let { latch ->
            arrived.release()
            check(latch.await(WAIT_MS, TimeUnit.MILLISECONDS)) { "a gated read was never released" }
        }
        return if (synthetic >= 0) FakeFileAttributes() else real.attributes(path)
    }

    override fun realPath(path: Path): Path = real.realPath(path)

    private fun listOf(dir: Path, count: Int, item: (Int) -> Path) = object : DirectoryStream<Path> {
        override fun iterator(): MutableIterator<Path> = object : MutableIterator<Path> {
            var next = 0
            override fun hasNext() = next < count
            override fun next(): Path = item(next++)
            override fun remove() = throw UnsupportedOperationException()
        }

        override fun close() = Unit
    }
}

/** A listing under test, its fake clock and every record it has given so far. */
private class Harness(
    val fs: BrowserFs = NioFs(),
    maxWorkers: Int = 3,
    watchdogMs: Long = 15_000,
    snapshotIdleMs: Long = 120_000,
) {
    var now = 0L
    val listing = FileListing(fs, maxWorkers, watchdogMs, 500, 50_000, snapshotIdleMs) { now }
    private val seen = mutableListOf<BrowserRecord>()
    private var nextId = 100L

    fun id() = ++nextId

    fun drain(): List<BrowserRecord> {
        while (true) seen.add(listing.poll() ?: return seen.toList())
    }

    /** The record for [id], waiting for it; exactly one may ever arrive. */
    fun await(id: Long): BrowserRecord {
        val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(WAIT_MS)
        while (System.nanoTime() < deadline) {
            val found = drain().filter { it.id == id }
            check(found.size <= 1) { "request $id has one answer, not ${found.size}" }
            if (found.isNotEmpty()) return found[0]
            Thread.sleep(2)
        }
        error("request $id was never answered")
    }

    fun list(
        root: Path,
        dir: Path = root,
        kind: String = "csv",
        showAll: Boolean = false,
        session: Long = 1,
        generationOk: Boolean = true,
    ): BrowserRecord {
        val id = id()
        listing.listFolder(id, session, root, dir, kind, showAll, generationOk)
        return await(id)
    }

    fun more(snapshot: Long, offset: Int, session: Long = 1): BrowserRecord {
        val id = id()
        listing.listMore(id, session, snapshot, offset)
        return await(id)
    }

    fun checkFile(root: Path, file: Path, session: Long = 1, generationOk: Boolean = true): BrowserRecord {
        val id = id()
        listing.checkFile(id, session, root, file, generationOk)
        return await(id)
    }
}

private val BrowserRecord.status get() = fields["status"]

@Suppress("UNCHECKED_CAST")
private val BrowserRecord.entries get() = fields["entries"] as List<ListEntry>

private val BrowserRecord.names get() = entries.map { it.name }

private fun BrowserRecord.expect(status: String, what: String): BrowserRecord {
    check(kind == "listing") { "$what: a listing record, not $kind" }
    check(this.status == status) { "$what: $status, not ${this.status} ($fields)" }
    if (status != "ok") check(fields.keys == setOf("status")) { "$what: a $status record carries its status only: $fields" }
    return this
}

private fun BrowserRecord.listed(names: List<String>, what: String): BrowserRecord {
    expect("ok", what)
    check(this.names == names) { "$what: lists $names, not ${this.names}" }
    return this
}

private fun BrowserRecord.handoff(reason: String, what: String) {
    check(kind == "handoff") { "$what: a handoff record, not $kind" }
    check(fields == mapOf("ok" to false, "reference" to "", "name" to "", "reason" to reason)) {
        "$what: refused as $reason: $fields"
    }
}

private fun rootItselfLists() {
    val root = folder()
    touch(root, "a.csv")
    val record = Harness().list(root).listed(listOf("a.csv"), "the root itself")
    check(record.fields.keys == setOf("status", "snapshot", "offset", "entries", "more", "hiddenByFilter", "hiddenByName")) {
        "a listing carries the record's fields: ${record.fields.keys}"
    }
    check(record.fields["offset"] == 0 && record.fields["more"] == false && record.fields["hiddenByFilter"] == 0 &&
        record.fields["hiddenByName"] == 0) {
        "one short page from offset 0: ${record.fields}"
    }
}

private fun siblingPrefixIsOutside() {
    val parent = folder("t")
    val inbox = Files.createDirectory(parent.resolve("inbox"))
    val inbox2 = Files.createDirectory(parent.resolve("inbox2"))
    touch(inbox2, "a.csv")
    Harness().list(inbox, inbox2).expect("outside", "a sibling whose name starts with the root's")
}

private fun dotDotEscapeIsOutside() {
    val root = folder()
    Files.createDirectory(root.resolve("sub"))
    val harness = Harness()
    harness.list(root, root.resolve("..")).expect("outside", "the root's parent through ..")
    harness.list(root, root.resolve("sub/../..")).expect("outside", "the root's parent through sub/../..")
    harness.list(root, root.resolve("sub/..")).expect("ok", "the root again through sub/..")
}

private fun symlinkedFolderOutsideIsOutside() {
    val root = folder()
    val elsewhere = folder("elsewhere")
    touch(elsewhere, "a.csv")
    val link = symlink(root.resolve("link"), elsewhere) ?: return
    Harness().list(root, link).expect("outside", "a link inside the root to a folder outside it")
}

/** /sdcard is a link to /storage/emulated/0: a root given through a link still lists, and owns, its own folders. */
private fun symlinkedRootListsItsOwnFolders() {
    val real = folder("real")
    Files.createDirectory(real.resolve("sub"))
    touch(real.resolve("sub"), "x.csv")
    val root = symlink(scratch.resolve("link${++serial}"), real) ?: return
    val harness = Harness()
    val top = harness.list(root).listed(listOf("sub"), "a root given through a link")
    // The browser builds its crumbs and Back from these paths, so they start with the root as it was given.
    check(top.entries[0].path == root.resolve("sub").toString()) { "folders keep the root's spelling: ${top.entries[0].path}" }
    val sub = harness.list(root, root.resolve("sub")).listed(listOf("x.csv"), "a folder under a root given through a link")
    val entry = sub.entries[0]
    check(entry.path == root.resolve("sub/x.csv").toString()) { "entries keep the root's spelling: ${entry.path}" }
    check(entry.reference == "file://$root/sub/x.csv") { "references keep the root's spelling: ${entry.reference}" }
    val handoff = harness.checkFile(root, root.resolve("sub/x.csv"))
    check(handoff.fields["ok"] == true) { "a file under a root given through a link hands off: ${handoff.fields}" }
    harness.list(real, root.resolve("sub")).listed(listOf("x.csv"), "a folder given through a link to the root")
}

private fun specialFilesNeverListed() {
    val root = folder()
    val outside = folder("outside")
    touch(root, "real.csv")
    touch(outside, "far.csv")
    symlink(root.resolve("near.csv"), root.resolve("real.csv"))
    symlink(root.resolve("far.csv"), outside.resolve("far.csv"))
    symlink(root.resolve("dirlink"), outside)
    val fifo = root.resolve("feed.csv")
    val made = try {
        File.separatorChar == '/' && ProcessBuilder("mkfifo", fifo.toString()).start().waitFor() == 0
    } catch (_: IOException) {
        false
    }
    if (!made) println("SKIP: no mkfifo here, so no FIFO in the listing")
    try {
        ServerSocketChannel.open(StandardProtocolFamily.UNIX).use { socket ->
            socket.bind(UnixDomainSocketAddress.of(root.resolve("sock.csv")))
            Harness().list(root, showAll = true).listed(listOf("real.csv"), "links, FIFOs and sockets")
        }
    } catch (e: UnsupportedOperationException) {
        println("SKIP: no Unix domain sockets here (${e.message})")
        Harness().list(root, showAll = true).listed(listOf("real.csv"), "links and FIFOs")
    } catch (e: IOException) {
        println("SKIP: no Unix domain sockets here (${e.message})")
        Harness().list(root, showAll = true).listed(listOf("real.csv"), "links and FIFOs")
    }
}

private fun unreadableIsNotEmpty() {
    val root = folder()
    val locked = Files.createDirectory(root.resolve("locked"))
    touch(locked, "a.csv")
    var lockedDown = false
    try {
        Files.setPosixFilePermissions(locked, PosixFilePermissions.fromString("---------"))
        lockedDown = true
        val stillReadable = try {
            Files.newDirectoryStream(locked).use { true }
        } catch (_: IOException) {
            false
        }
        if (stillReadable) {
            println("SKIP: this runner reads a folder with mode 000 (root?)")
        } else {
            Harness().list(root, locked).expect("unreadable", "a folder with mode 000")
        }
    } catch (e: UnsupportedOperationException) {
        println("SKIP: no POSIX permissions here (${e.message})")
    } finally {
        if (lockedDown) Files.setPosixFilePermissions(locked, PosixFilePermissions.fromString("rwx------"))
    }

    val denied = GatedFs().apply { openFailure = AccessDeniedException(root.toString()) }
    Harness(denied).list(root).expect("unreadable", "an open refused with AccessDeniedException")
    val gone = GatedFs().apply { openFailure = NoSuchFileException(root.toString()) }
    Harness(gone).list(root).expect("gone", "an open that finds no folder")

    touch(root, "b.csv", "c.csv", "d.csv")
    val failing = GatedFs().apply { failIterationAfter = 2 }
    Harness(failing).list(root).expect("unreadable", "a folder whose reading fails part way")

    val empty = folder()
    val record = Harness().list(empty).listed(emptyList(), "an empty folder")
    check(record.fields["more"] == false && record.fields["hiddenByFilter"] == 0) {
        "an empty folder has nothing more and nothing hidden: ${record.fields}"
    }
}

private fun goneWhenRootVanishes() {
    val root = folder()
    val sub = Files.createDirectory(root.resolve("sub"))
    Files.delete(sub)
    Files.delete(root)
    val harness = Harness()
    harness.list(root).expect("gone", "a root that was deleted")
    harness.list(root, sub).expect("gone", "a folder under a root that was deleted")
    val alive = folder()
    harness.list(alive, alive.resolve("missing")).expect("gone", "a folder that is not there")
    touch(alive, "file.csv")
    harness.list(alive, alive.resolve("file.csv")).expect("gone", "a file named as a folder")
}

private fun staleGeneration() {
    val root = folder()
    touch(root, "a.csv")
    val harness = Harness()
    harness.list(root, generationOk = false).expect("stale", "a listing for a root generation that moved on")
    harness.checkFile(root, root.resolve("a.csv"), generationOk = false).handoff("stale", "a hand-off from an old generation")
}

private fun dotFilesNeedShowAll() {
    val root = folder()
    Files.createDirectory(root.resolve(".config"))
    Files.createDirectory(root.resolve("Lists"))
    touch(root, ".hidden.csv", "a.csv")
    val harness = Harness()
    val plain = harness.list(root).listed(listOf("Lists", "a.csv"), "dot names without Show all")
    check(plain.fields["hiddenByFilter"] == 0) { "dot names are not counted as filtered: ${plain.fields}" }
    harness.list(root, showAll = true).listed(listOf(".config", "Lists", ".hidden.csv", "a.csv"), "Show all")
}

private fun filterBeforePage() {
    val root = folder()
    for (n in 0 until 700) touch(root, "a%03d.dat".format(n))
    touch(root, "zzz.csv")
    val record = Harness().list(root).listed(listOf("zzz.csv"), "700 other files before a CSV")
    check(record.fields["hiddenByFilter"] == 700 && record.fields["more"] == false) {
        "the filter runs before the page: ${record.fields}"
    }
}

private fun filters() {
    val root = folder()
    Files.createDirectory(root.resolve("sub.dat"))
    touch(root, "A.CSV", "b.Txt", "c.dat", "x.DSDBAK", "y.dsdbak.old", "c.WAV", "capture.bin", "a.binary", "CAPTURE.BIN",
        "s.sym", "r.raw2")
    val harness = Harness()
    val csv = harness.list(root, kind = "csv").listed(listOf("sub.dat", "A.CSV", "b.Txt"), "CSV imports")
    check(csv.fields["hiddenByFilter"] == 9) { "nine files hidden from CSV imports: ${csv.fields}" }
    harness.list(root, kind = "backup").listed(listOf("sub.dat", "x.DSDBAK"), "backups")
    harness.list(root, kind = "replay")
        .listed(listOf("sub.dat", "a.binary", "c.WAV", "capture.bin", "r.raw2", "s.sym"), "replay files")
    harness.list(root, kind = "csv", showAll = true).expect("ok", "Show all").also {
        check(it.entries.size == 12 && it.fields["hiddenByFilter"] == 0) { "Show all lists every file: ${it.fields}" }
    }
    // The symbol-capture rule reads the whole path, as the engine does: a folder's ".bin" counts.
    val capture = Files.createDirectory(root.resolve("caps.bin"))
    touch(capture, "plain")
    harness.list(root, capture, kind = "replay").listed(listOf("plain"), "a file under a folder named *.bin")
}

private fun naturalOrderFoldersFirst() {
    val root = folder()
    Files.createDirectory(root.resolve("lists2"))
    Files.createDirectory(root.resolve("Lists"))
    touch(root, "b10.csv", "B2.csv", "a.csv", "a02.csv", "a1.csv")
    Harness().list(root).listed(listOf("Lists", "lists2", "a.csv", "a1.csv", "a02.csv", "B2.csv", "b10.csv"),
        "folders first, then case-insensitive natural order")
}

private fun pagesOf500() {
    val root = folder()
    for (n in 0 until 1201) touch(root, "f$n.csv")
    val harness = Harness()
    val first = harness.list(root).expect("ok", "a folder of 1201 files")
    check(first.entries.size == 500 && first.fields["more"] == true && first.fields["offset"] == 0) {
        "the first page holds 500 and says there is more: ${first.entries.size} ${first.fields["more"]}"
    }
    check(first.entries.first().name == "f0.csv" && first.entries.last().name == "f499.csv") { "page 1 is in order" }
    val snapshot = first.fields["snapshot"] as Long
    val second = harness.more(snapshot, 500).expect("ok", "page 2")
    check(second.entries.size == 500 && second.fields["more"] == true && second.fields["offset"] == 500) {
        "the second page: ${second.entries.size} at ${second.fields["offset"]}"
    }
    check(second.entries.first().name == "f500.csv" && second.fields["snapshot"] == snapshot) { "page 2 continues" }
    val third = harness.more(snapshot, 1000).expect("ok", "page 3")
    check(third.entries.size == 201 && third.fields["more"] == false && third.fields["offset"] == 1000) {
        "the last page: ${third.entries.size} more=${third.fields["more"]}"
    }
    check(third.entries.last().name == "f1200.csv") { "the last page ends the folder" }
    harness.more(snapshot, 1202).expect("failed", "a page past the end")
}

private fun tooMany() {
    val root = folder()
    val fs = GatedFs().apply { synthetic = 50_001 }
    val harness = Harness(fs)
    harness.list(root).expect("tooMany", "50,001 entries")
    fs.synthetic = 50_000
    val record = harness.list(root).expect("ok", "50,000 entries")
    check(record.entries.size == 500 && record.fields["more"] == true) { "50,000 entries page as usual" }
}

private fun snapshotExpiry() {
    val root = folder()
    val fs = GatedFs().apply { synthetic = 600 }
    val harness = Harness(fs, snapshotIdleMs = 1_000)
    val idle = harness.list(root).fields["snapshot"] as Long
    harness.now += 1_000
    harness.more(idle, 500).expect("ok", "a page at exactly the idle limit")
    harness.now += 1_001
    harness.more(idle, 500).expect("expired", "a page after the snapshot sat idle too long")

    val old = harness.list(root).fields["snapshot"] as Long
    val current = harness.list(root).fields["snapshot"] as Long
    check(old != current) { "each listing has its own snapshot" }
    harness.more(old, 500).expect("expired", "a page of a superseded snapshot")
    harness.more(current, 500).expect("ok", "a stray page of the old snapshot leaves the current one alone")

    val other = harness.list(root, session = 2).fields["snapshot"] as Long
    harness.more(other, 500, session = 3).expect("expired", "a page of another session's snapshot")
    val closing = harness.list(root, session = 2).fields["snapshot"] as Long
    check(harness.listing.holdsSnapshot(2)) { "a listed session holds its snapshot" }
    harness.listing.closeSession(2)
    check(!harness.listing.holdsSnapshot(2)) { "closing a session releases its snapshot" }
    harness.more(closing, 500, session = 2).expect("cancelled", "a page for a closed session")
    harness.more(closing, 500, session = 4).expect("expired", "a page of a closed session's snapshot from another")

    // Closed sessions are remembered up to a bound: the oldest is forgotten first.
    val bounded = Harness()
    val sessions = 1000L..(1000L + FileListing.CLOSED_SESSIONS_KEPT)
    for (session in sessions) bounded.listing.closeSession(session)
    bounded.checkFile(root, root, session = sessions.last).handoff("cancelled", "the latest closed session")
    bounded.checkFile(root, root, session = sessions.first + 1).handoff("cancelled", "the oldest remembered session")
    bounded.checkFile(root, root, session = sessions.first).handoff("notFile", "a session past the bound")
}

private fun entryShape() {
    val root = folder()
    val sub = Files.createDirectory(root.resolve("Lists"))
    val file = root.resolve("dmr.csv")
    Files.write(file, ByteArray(4321))
    Files.setLastModifiedTime(file, FileTime.fromMillis(1_700_000_123_000))
    touch(root, "fat.csv")
    Files.setLastModifiedTime(root.resolve("fat.csv"), FileTime.fromMillis(0))
    val entries = Harness().list(root).listed(listOf("Lists", "dmr.csv", "fat.csv"), "entry shapes").entries
    check(entries[0] == ListEntry("Lists", sub.toString(), "", true, entries[0].size, entries[0].modifiedMs)) {
        "a folder has its path, no reference and dir: ${entries[0]}"
    }
    check(entries[1] == ListEntry("dmr.csv", file.toString(), file.toUri().toASCIIString(), false, 4321, 1_700_000_123_000)) {
        "a file has its size and modified time from its attributes: ${entries[1]}"
    }
    check(entries[2].modifiedMs == 0L) { "a file with no modified time (a FAT root) lists with 0: ${entries[2]}" }
}

private fun handoffReference() {
    val root = folder()
    val dir = Files.createDirectory(root.resolve("in"))
    touch(dir, "a b#1%.csv", "map%20v2.csv")
    touch(root.parent, "x${serial}.csv")
    val harness = Harness()
    val ok = harness.checkFile(root, dir.resolve("a b#1%.csv"))
    check(ok.kind == "handoff" && ok.fields == mapOf("ok" to true, "reference" to "file://$dir/a%20b%231%25.csv",
        "name" to "a b#1%.csv", "reason" to "")) { "a regular file hands off encoded once: ${ok.fields}" }
    val listed = harness.list(root, dir).entries.first { it.name == "a b#1%.csv" }
    check(listed.reference == ok.fields["reference"]) { "the listing gives the same reference: ${listed.reference}" }
    val encoded = harness.checkFile(root, dir.resolve("map%20v2.csv"))
    check(encoded.fields["reference"] == "file://$dir/map%2520v2.csv" && encoded.fields["name"] == "map%20v2.csv") {
        "a name that looks encoded is encoded again and never decoded: ${encoded.fields}"
    }
    symlink(dir.resolve("link.csv"), dir.resolve("a b#1%.csv"))?.let {
        harness.checkFile(root, it).handoff("notFile", "a link")
    }
    harness.checkFile(root, dir).handoff("notFile", "a folder")
    harness.checkFile(root, root.resolve("../x${serial}.csv")).handoff("outside", "a file outside the root through ..")
    harness.checkFile(root, dir.resolve("deleted.csv")).handoff("gone", "a file that is not there")
    harness.listing.closeSession(9)
    harness.checkFile(root, dir.resolve("a b#1%.csv"), session = 9).handoff("cancelled", "a hand-off for a closed session")
}

/**
 * Names that are not valid UTF-8 (ext4 takes any bytes, through adb push for one) read with U+FFFD in place of the bad
 * bytes, so their text names another file or none on the host's side: they are never listed, only counted, and two
 * of them that read the same never make two rows with one path. A hand-off of one is refused.
 */
private fun invalidUtf8NamesAreCountedNotListed() {
    val root = folder()
    touch(root, "good.csv")
    val made = try {
        File.separatorChar == '/' &&
            ProcessBuilder("sh", "-c", "touch \"$(printf 'bad\\377.csv')\" \"$(printf 'bad\\376.csv')\" " +
                "\"$(printf '.dot\\377.csv')\" \"$(printf 'bad\\377.wav')\" && mkdir \"$(printf 'dir\\377')\"")
                .directory(root.toFile()).start().waitFor() == 0
    } catch (_: IOException) {
        false
    }
    val onDisk = if (made) Files.list(root).use { it.count() } else 0L
    if (onDisk != 6L) {
        println("SKIP: this filesystem keeps no name that is not valid UTF-8")
        return
    }
    val harness = Harness()
    val csv = harness.list(root).listed(listOf("good.csv"), "only the name that reads as text")
    check(csv.fields["hiddenByName"] == 3 && csv.fields["hiddenByFilter"] == 0) {
        "the folder and the two CSV names count; the dot name and the .wav would not have shown: ${csv.fields}"
    }
    val all = harness.list(root, showAll = true).listed(listOf("good.csv"), "Show all lists them no more")
    check(all.fields["hiddenByName"] == 5) { "with Show all, every one of them counts: ${all.fields}" }
    val bad = Files.list(root).use { paths -> paths.filter { it.fileName.toString() == "bad\uFFFD.csv" }.toList() }
    check(bad.size == 2) { "two names read the same: ${bad.map { it.fileName }}" }
    for (file in bad) harness.checkFile(root, file).handoff("notFile", "a name that is not valid UTF-8")
    val good = harness.checkFile(root, root.resolve("good.csv"))
    check(good.fields["ok"] == true) { "the name beside them still hands off: ${good.fields}" }
}

private fun cancelWhileBlocked() {
    val root = folder()
    touch(root, "a.csv")
    val gate = CountDownLatch(1)
    val fs = GatedFs().apply { this.gate = gate }
    val harness = Harness(fs, maxWorkers = 1)
    val id = harness.id()
    harness.listing.listFolder(id, 1, root, root, "csv", false, true)
    check(fs.arrived.tryAcquire(WAIT_MS, TimeUnit.MILLISECONDS)) { "the listing reached the blocked read" }
    harness.listing.cancel(id)
    harness.await(id).expect("cancelled", "a listing cancelled while its read is blocked")
    fs.gate = null
    gate.countDown()
    awaitIdle(harness, root, 1)
    check(harness.drain().count { it.id == id } == 1) { "the cancelled listing's late result is dropped" }
}

/**
 * Lists [root] in [workers] sessions at once until none is busy: capacity comes back only once every blocked worker has
 * returned, so after this no worker of an earlier request is still running.
 */
private fun awaitIdle(harness: Harness, root: Path, workers: Int) {
    val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(WAIT_MS)
    while (System.nanoTime() < deadline) {
        val ids = (1..workers).map { n ->
            harness.id().also { harness.listing.listFolder(it, 50L + n, root, root, "csv", false, true) }
        }
        val records = ids.map { harness.await(it) }
        if (records.none { it.status == "busy" }) {
            records.forEach { it.expect("ok", "a listing once the blocked workers returned") }
            return
        }
        Thread.sleep(5)
    }
    error("capacity never came back")
}

private fun saturationAndWatchdog() {
    val root = folder()
    touch(root, "a.csv")
    val gate = CountDownLatch(1)
    val fs = GatedFs().apply { this.gate = gate }
    val harness = Harness(fs, watchdogMs = 50)
    val blocked = (1L..3L).map { session ->
        harness.id().also { harness.listing.listFolder(it, session, root, root, "csv", false, true) }
    }
    harness.list(root, session = 4).expect("busy", "a fourth listing while three reads are blocked")
    for (id in blocked) harness.await(id).expect("timedOut", "a blocked listing at its watchdog")
    check(fs.arrived.tryAcquire(3, WAIT_MS, TimeUnit.MILLISECONDS)) { "all three reads are blocked" }
    harness.list(root, session = 5).expect("busy", "a fifth listing while the abandoned reads are still blocked")
    val handoff = harness.checkFile(root, root.resolve("a.csv"), session = 6)
    handoff.handoff("busy", "a hand-off while the abandoned reads are still blocked")
    val roots = harness.id()
    harness.listing.submit(roots, 7, "roots") { mapOf("status" to "ok") }
    check(harness.await(roots).fields == mapOf("status" to "busy", "generation" to 0L, "roots" to emptyList<Any>())) {
        "a roots request while saturated is busy"
    }
    fs.gate = null
    gate.countDown()
    awaitIdle(harness, root, 3)
    check(blocked.all { id -> harness.drain().count { it.id == id } == 1 }) { "abandoned reads never answer again" }
}

private fun submitAndSavedListings() {
    val harness = Harness()
    val roots = harness.id()
    harness.listing.submit(roots, 1, "roots") { mapOf("status" to "ok", "generation" to 3L, "roots" to emptyList<Any>()) }
    val record = harness.await(roots)
    check(record.kind == "roots" && record.fields["generation"] == 3L) { "a submitted request answers its work: $record" }
    val failing = harness.id()
    harness.listing.submit(failing, 1, "handoff") { throw IllegalStateException("provider crashed") }
    harness.await(failing).handoff("failed", "a hand-off whose work threw")

    fun saved(name: String, id: Int) =
        ListEntry(name, "", "content://media/external/downloads/$id", false, 10, 1_700_000_000_000)
    val saved = harness.id()
    harness.listing.listEntries(saved, 1, "csv", false) {
        listOf(saved("b10.csv", 1), saved("x.wav", 2), saved("B2.csv", 3), saved(".dot.csv", 4))
    }
    val listed = harness.await(saved).listed(listOf("B2.csv", "b10.csv"), "Saved by DSD-neo, filtered and sorted")
    check(listed.fields["hiddenByFilter"] == 1) { "Saved counts what the filter hid: ${listed.fields}" }
    val capture = harness.id()
    harness.listing.listEntries(capture, 1, "replay", false) { listOf(saved("capture.bin", 5), saved("x.csv", 6)) }
    harness.await(capture).listed(listOf("capture.bin"), "Saved replay files read the name")
    val unreadable = harness.id()
    harness.listing.listEntries(unreadable, 1, "csv", false) { null }
    harness.await(unreadable).expect("unreadable", "a Saved query with no answer")
    val empty = harness.id()
    harness.listing.listEntries(empty, 1, "csv", false) { cancelled ->
        check(!cancelled()) { "a live request is not cancelled" }
        emptyList()
    }
    harness.await(empty).listed(emptyList(), "an empty Saved folder")
}

private fun supersedeDropsTheOlderListing() {
    val root = folder()
    touch(root, "a.csv")
    val gate = CountDownLatch(1)
    val fs = GatedFs().apply { this.gate = gate }
    val harness = Harness(fs)
    val older = harness.id()
    harness.listing.listFolder(older, 1, root, root, "csv", false, true)
    check(fs.arrived.tryAcquire(WAIT_MS, TimeUnit.MILLISECONDS)) { "the older listing reached the blocked read" }
    val newer = harness.id()
    harness.listing.listEntries(newer, 1, "csv", false) { emptyList() }
    harness.await(older).expect("cancelled", "a listing superseded by a newer one in its session")
    val current = harness.await(newer).expect("ok", "the newer listing").fields["snapshot"] as Long
    fs.gate = null
    gate.countDown()
    awaitIdle(harness, root, 3)
    harness.more(current, 0).expect("ok", "the newer snapshot outlives the older listing's late result")
    check(harness.drain().count { it.id == older } == 1) { "the superseded listing answers once" }
    harness.listing.closeSession(1)
}

private fun closeSessionCancelsWhatIsLive() {
    val root = folder()
    touch(root, "a.csv")
    val gate = CountDownLatch(1)
    val fs = GatedFs().apply { this.gate = gate }
    val harness = Harness(fs)
    val listingId = harness.id()
    harness.listing.listFolder(listingId, 8, root, root, "csv", false, true)
    val handoffId = harness.id()
    harness.listing.checkFile(handoffId, 8, root, root.resolve("a.csv"), true)
    check(fs.arrived.tryAcquire(2, WAIT_MS, TimeUnit.MILLISECONDS)) { "both requests reached the blocked read" }
    harness.listing.closeSession(8)
    harness.await(listingId).expect("cancelled", "a listing of a closed session")
    harness.await(handoffId).handoff("cancelled", "a hand-off of a closed session")
    harness.list(root, session = 8).expect("cancelled", "a listing for a session already closed")
    fs.gate = null
    gate.countDown()
}

fun main() {
    rootItselfLists()
    siblingPrefixIsOutside()
    dotDotEscapeIsOutside()
    symlinkedFolderOutsideIsOutside()
    symlinkedRootListsItsOwnFolders()
    specialFilesNeverListed()
    unreadableIsNotEmpty()
    goneWhenRootVanishes()
    staleGeneration()
    dotFilesNeedShowAll()
    filterBeforePage()
    filters()
    naturalOrderFoldersFirst()
    pagesOf500()
    tooMany()
    snapshotExpiry()
    entryShape()
    handoffReference()
    invalidUtf8NamesAreCountedNotListed()
    cancelWhileBlocked()
    saturationAndWatchdog()
    submitAndSavedListings()
    supersedeDropsTheOlderListing()
    closeSessionCancelsWhatIsLive()
    // Through Path, not File: a name that is not valid UTF-8 keeps its bytes only there.
    Files.walk(scratch).use { paths -> paths.sorted(Comparator.reverseOrder()).forEach(Files::delete) }
    println("PASS: folder listings stay inside their roots, page, filter, time out and answer each request once")
}
