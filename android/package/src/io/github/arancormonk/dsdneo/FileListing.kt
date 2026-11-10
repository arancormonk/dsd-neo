// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import java.io.IOException
import java.nio.file.DirectoryIteratorException
import java.nio.file.DirectoryStream
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.NoSuchFileException
import java.nio.file.NotDirectoryException
import java.nio.file.Path
import java.nio.file.attribute.BasicFileAttributes
import java.util.ArrayDeque
import java.util.IdentityHashMap
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** The three filesystem calls a listing makes, so the JVM tests can fail or block each one. */
internal interface BrowserFs {
    /** The folder's entries; throws on open, and wraps a failure part way in [DirectoryIteratorException]. */
    fun open(dir: Path): DirectoryStream<Path>

    /** What [path] itself is, never what a link there points at. */
    fun attributes(path: Path): BasicFileAttributes

    /** [path] with every link and `..` resolved. */
    fun realPath(path: Path): Path
}

internal class NioFs : BrowserFs {
    override fun open(dir: Path): DirectoryStream<Path> = Files.newDirectoryStream(dir)

    override fun attributes(path: Path): BasicFileAttributes =
        Files.readAttributes(path, BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS)

    override fun realPath(path: Path): Path = path.toRealPath()
}

/**
 * One answer for the host: [kind] is `roots`, `listing` (a listing or a page) or `handoff`, and [fields] are the keys
 * of that kind's JSON record besides `id` and `kind`. A non-ok listing carries its `status` only.
 */
internal data class BrowserRecord(val id: Long, val kind: String, val fields: Map<String, Any?>)

/**
 * One row of a listing. A folder has no [reference]; a file's is its `file:` URI, percent-encoded once, and [name] is
 * never encoded. Saved by DSD-neo rows have no [path] and a `content:` reference. [modifiedMs] is 0 where the
 * filesystem keeps no time (a FAT root).
 */
internal data class ListEntry(
    val name: String,
    val path: String,
    val reference: String,
    val dir: Boolean,
    val size: Long,
    val modifiedMs: Long,
)

/**
 * The file browser's folder reads (#830), free of Android so the JVM tests run it against [BrowserFs].
 *
 * Every listing, hand-off check and [submit]ted job runs on a daemon thread of its own, at most [maxWorkers] alive at
 * once. Nothing queues: a request beyond that is answered `busy` at once, and a thread stuck in a dead drive keeps its
 * place until it returns, so a stuck drive can never tie up more than [maxWorkers] threads. Each running request has a
 * watchdog: after [watchdogMs] it is answered `timedOut` and its thread is abandoned.
 *
 * Each request is answered exactly once, by one record that [poll] hands out. A cancel, a closed session, a newer
 * listing in the same session (it supersedes the older one) or the watchdog answers first, and the worker's own result
 * is then dropped by its id. A raised cancel flag also makes the worker stop at its next entry.
 *
 * A listing reads the whole folder (up to [entryCap] directories and regular files; one more ends it as `tooMany`),
 * drops everything else, then dot names and the kind's filter unless Show all is on, sorts folders first, then
 * case-insensitive natural order, and keeps the result as the session's one snapshot, answering its first page of
 * [pageSize]. [listMore] pages a snapshot from memory. A snapshot is released by the session's next listing, its close,
 * or [snapshotIdleMs] without a page, on [nowMs].
 *
 * Containment is decided on real paths: a folder or file is inside a root when its real path is the root's real path
 * or below it by whole path components, so `/sdcard` (a link to `/storage/emulated/0`) still owns its folders, and
 * `inbox2` is not inside `inbox`. Paths and references keep the root's own spelling.
 */
internal class FileListing(
    private val fs: BrowserFs = NioFs(),
    private val maxWorkers: Int = 3,
    private val watchdogMs: Long = 15_000,
    private val pageSize: Int = 500,
    /** The most directories and regular files a listing takes; one more is `tooMany`. */
    val entryCap: Int = 50_000,
    private val snapshotIdleMs: Long = 120_000,
    private val nowMs: () -> Long,
) {
    private class Request(val id: Long, val session: Long, val kind: String) {
        val cancelled = AtomicBoolean()
        var watchdog: ScheduledFuture<*>? = null
    }

    private class Snapshot(
        val id: Long,
        val entries: List<ListEntry>,
        val hiddenByFilter: Int,
        val hiddenByName: Int,
        var usedMs: Long,
    ) {
        var sweep: ScheduledFuture<*>? = null
    }

    /** What a worker ends with: a record's fields, or the folder it read, kept as a snapshot when it is answered. */
    private sealed interface Outcome

    private class Answer(val fields: Map<String, Any?>) : Outcome

    /** [hiddenByName]: the items left out because their names are not text the host can carry (see [collect]). */
    private class Listed(val entries: List<ListEntry>, val hiddenByFilter: Int, val hiddenByName: Int = 0) : Outcome

    /** A folder as read: its listable entries with their paths, and those whose names cannot be listed. */
    private class Read(val entries: Map<ListEntry, Path>, val unnamed: List<ListEntry>)

    // Everything below is guarded by this object's monitor.
    private val requests = HashMap<Long, Request>()
    private val records = ArrayDeque<BrowserRecord>()
    private val snapshots = HashMap<Long, Snapshot>() // By session.

    // The host never reuses a session id; a request naming a closed one is answered `cancelled`. The host refuses
    // those itself, so this only guards against its mistakes: the [CLOSED_SESSIONS_KEPT] most recent are enough, and
    // the set stays small however many times the browser opens.
    private val closedSessions = LinkedHashSet<Long>()
    private var liveWorkers = 0
    private var nextSnapshot = 0L

    /**
     * Told the id of each request its watchdog answered `timedOut`, outside the lock, so the owner of a call that does
     * not poll the cancel flag (a provider query's CancellationSignal) can end it and free its place sooner.
     */
    @Volatile
    var onTimedOut: (id: Long) -> Unit = {}

    /** Watchdogs and snapshot expiry, on one daemon thread. */
    private val timer = ScheduledThreadPoolExecutor(1) { task ->
        Thread(task, "dsd-file-browser-timer").apply { isDaemon = true }
    }.apply { removeOnCancelPolicy = true }

    /** List folder [dir] under [root] for a [kind] pick; [generationOk] is false when the roots moved on since. */
    fun listFolder(
        id: Long,
        session: Long,
        root: Path,
        dir: Path,
        kind: String,
        showAll: Boolean,
        generationOk: Boolean,
    ) {
        if (!beginFolderRequest(id, session)) return
        if (!generationOk) return answer(id, LISTING, failureFields(LISTING, STALE))
        start(id, session, LISTING) { cancelled -> readFolder(root, dir, kind, showAll, cancelled) }
    }

    /**
     * List the rows [produce] returns (Saved by DSD-neo), filtered and sorted as a folder is. A null from it is
     * `unreadable`; more than [entryCap] rows is `tooMany`. [generationOk] false is `stale`, and [produce] never runs.
     */
    fun listEntries(
        id: Long,
        session: Long,
        kind: String,
        showAll: Boolean,
        generationOk: Boolean = true,
        produce: (cancelled: () -> Boolean) -> List<ListEntry>?,
    ) {
        if (!beginFolderRequest(id, session)) return
        if (!generationOk) return answer(id, LISTING, failureFields(LISTING, STALE))
        start(id, session, LISTING) { cancelled ->
            val entries = produce(cancelled)
            when {
                entries == null -> failure(LISTING, UNREADABLE)
                entries.size > entryCap -> failure(LISTING, TOO_MANY)
                else -> shape(entries, kind, showAll)
            }
        }
    }

    /**
     * The page of the session's [snapshot] that starts at [offset], from memory; `expired` once it is released. A page
     * naming another snapshot (a stray old "Show more") is `expired` and leaves the current one alone.
     */
    @Synchronized
    fun listMore(id: Long, session: Long, snapshot: Long, offset: Int) {
        if (!admit(id, session, LISTING)) return
        supersedeFolderRequests(session)
        val current = snapshots[session]
        val fields = when {
            current == null || current.id != snapshot -> failureFields(LISTING, EXPIRED)
            expired(current) -> {
                releaseSnapshot(session)
                failureFields(LISTING, EXPIRED)
            }
            offset < 0 || offset > current.entries.size -> failureFields(LISTING, FAILED)
            else -> {
                current.usedMs = nowMs()
                page(current, offset)
            }
        }
        answer(id, LISTING, fields)
    }

    /** Whether [file] can be handed off: a regular file (not a link) inside [root], answered with its reference. */
    fun checkFile(id: Long, session: Long, root: Path, file: Path, generationOk: Boolean) {
        if (!admit(id, session, HANDOFF)) return
        if (!generationOk) return answer(id, HANDOFF, failureFields(HANDOFF, STALE))
        start(id, session, HANDOFF) { checkHandoff(root, file) }
    }

    /** Answer hand-off [id] `stale` now, with no worker: the roots moved on since its root was listed. */
    fun staleHandoff(id: Long, session: Long) {
        if (!admit(id, session, HANDOFF)) return
        answer(id, HANDOFF, failureFields(HANDOFF, STALE))
    }

    /** Run [work] (the roots, a Saved hand-off) as a request of [kind]; it answers that record's fields. */
    fun submit(id: Long, session: Long, kind: String, work: (cancelled: () -> Boolean) -> Map<String, Any?>) {
        if (!admit(id, session, kind)) return
        start(id, session, kind) { cancelled -> Answer(work(cancelled)) }
    }

    /** Answer request [id] `cancelled` now, if it is still running; its worker's result is dropped. */
    @Synchronized
    fun cancel(id: Long) {
        requests[id]?.let { finish(it, failure(it.kind, CANCELLED)) }
    }

    /** Answer every running request of [session] `cancelled`, release its snapshot, and refuse its later requests. */
    @Synchronized
    fun closeSession(session: Long) {
        closedSessions.add(session)
        if (closedSessions.size > CLOSED_SESSIONS_KEPT) closedSessions.remove(closedSessions.first())
        requests.values.filter { it.session == session }.forEach { finish(it, failure(it.kind, CANCELLED)) }
        releaseSnapshot(session)
    }

    /** Whether [session] still holds a snapshot; for the tests. */
    @Synchronized
    internal fun holdsSnapshot(session: Long): Boolean = session in snapshots

    /** The oldest answer not yet taken, or null. */
    @Synchronized
    fun poll(): BrowserRecord? = records.pollFirst()

    @Synchronized
    private fun answer(id: Long, kind: String, fields: Map<String, Any?>) {
        records.addLast(BrowserRecord(id, kind, fields))
    }

    /** False, with the request answered `cancelled`, when [session] is closed. */
    @Synchronized
    private fun admit(id: Long, session: Long, kind: String): Boolean {
        if (session !in closedSessions) return true
        answer(id, kind, failureFields(kind, CANCELLED))
        return false
    }

    /** A new listing in [session]: the one running there and the session's snapshot give way to it. */
    @Synchronized
    private fun beginFolderRequest(id: Long, session: Long): Boolean {
        if (!admit(id, session, LISTING)) return false
        supersedeFolderRequests(session)
        releaseSnapshot(session)
        return true
    }

    @Synchronized
    private fun supersedeFolderRequests(session: Long) {
        requests.values.filter { it.session == session && it.kind == LISTING }
            .forEach { finish(it, failure(LISTING, CANCELLED)) }
    }

    private fun start(id: Long, session: Long, kind: String, work: (cancelled: () -> Boolean) -> Outcome) {
        val request = Request(id, session, kind)
        synchronized(this) {
            if (liveWorkers >= maxWorkers) return answer(id, kind, failureFields(kind, BUSY))
            liveWorkers++
            requests[id] = request
            val timeout = Runnable { if (finish(request, failure(kind, TIMED_OUT))) onTimedOut(id) }
            request.watchdog = timer.schedule(timeout, watchdogMs, TimeUnit.MILLISECONDS)
        }
        val worker = Thread({ runWorker(request, work) }, "dsd-file-browser-$id").apply { isDaemon = true }
        try {
            worker.start()
        } catch (_: OutOfMemoryError) {
            // No thread could be made for it.
            synchronized(this) { liveWorkers-- }
            finish(request, failure(kind, FAILED))
        }
    }

    private fun runWorker(request: Request, work: (cancelled: () -> Boolean) -> Outcome) {
        var outcome = failure(request.kind, FAILED)
        try {
            outcome = work(request.cancelled::get)
        } catch (_: Exception) {
            // Answered `failed`.
        } finally {
            // In one step: a caller who has the answer can already start another request, and once a place is free
            // again the result of the request that held it has been answered or dropped.
            synchronized(this) {
                liveWorkers--
                finish(request, outcome)
            }
        }
    }

    /**
     * Answer [request] with [outcome] unless something answered it first (false then); a listing becomes its session's
     * snapshot.
     */
    @Synchronized
    private fun finish(request: Request, outcome: Outcome): Boolean {
        if (requests[request.id] !== request) return false
        requests.remove(request.id)
        request.cancelled.set(true)
        request.watchdog?.cancel(false)
        val fields = when (outcome) {
            is Answer -> outcome.fields
            is Listed -> page(keepSnapshot(request.session, outcome), 0)
        }
        answer(request.id, request.kind, fields)
        return true
    }

    @Synchronized
    private fun keepSnapshot(session: Long, listed: Listed): Snapshot {
        releaseSnapshot(session)
        val snapshot = Snapshot(++nextSnapshot, listed.entries, listed.hiddenByFilter, listed.hiddenByName, nowMs())
        snapshots[session] = snapshot
        scheduleSweep(session, snapshot, snapshotIdleMs + 1)
        return snapshot
    }

    @Synchronized
    private fun releaseSnapshot(session: Long) {
        snapshots.remove(session)?.sweep?.cancel(false)
    }

    private fun expired(snapshot: Snapshot) = nowMs() - snapshot.usedMs > snapshotIdleMs

    private fun scheduleSweep(session: Long, snapshot: Snapshot, delayMs: Long) {
        snapshot.sweep = timer.schedule(Runnable { sweep(session, snapshot) }, delayMs, TimeUnit.MILLISECONDS)
    }

    /** Release an idle snapshot, so a browser left open does not hold a large folder; a used one waits again. */
    @Synchronized
    private fun sweep(session: Long, snapshot: Snapshot) {
        if (snapshots[session] !== snapshot) return
        val idleMs = nowMs() - snapshot.usedMs
        if (idleMs > snapshotIdleMs) {
            snapshots.remove(session)
        } else {
            scheduleSweep(session, snapshot, snapshotIdleMs - idleMs + 1)
        }
    }

    private fun page(snapshot: Snapshot, offset: Int): Map<String, Any?> {
        val end = minOf(snapshot.entries.size, offset + pageSize)
        return mapOf(
            STATUS to OK,
            "snapshot" to snapshot.id,
            "offset" to offset,
            "entries" to snapshot.entries.subList(offset, end).toList(),
            "more" to (end < snapshot.entries.size),
            "hiddenByFilter" to snapshot.hiddenByFilter,
            "hiddenByName" to snapshot.hiddenByName,
        )
    }

    private fun readFolder(root: Path, dir: Path, kind: String, showAll: Boolean, cancelled: () -> Boolean): Outcome {
        val real: Path
        val shown: Path
        try {
            val rootReal = fs.realPath(root)
            real = fs.realPath(dir)
            if (!inside(rootReal, real)) return failure(LISTING, OUTSIDE)
            shown = root.resolve(rootReal.relativize(real))
        } catch (e: IOException) {
            return failure(LISTING, readFailure(e))
        }
        val read: Read?
        try {
            read = fs.open(real).use { stream -> collect(stream, shown, cancelled) }
        } catch (_: DirectoryIteratorException) {
            return failure(LISTING, UNREADABLE)
        } catch (e: IOException) {
            return failure(LISTING, readFailure(e))
        }
        if (read == null) return failure(LISTING, TOO_MANY)
        val entries = read.entries
        val listed = shape(entries.keys.toList(), kind, showAll)
        // Counted as they would have been listed: past the dot-name rule and the filter.
        val hiddenByName = shape(read.unnamed, kind, showAll).entries.size
        // References only for the files that are shown: Path.toUri() looks at the file once more.
        val referenced = ArrayList<ListEntry>(listed.entries.size)
        for (entry in listed.entries) {
            if (cancelled()) return failure(LISTING, CANCELLED) // Answered already; this result is dropped.
            val reference = if (entry.dir) "" else entries.getValue(entry).toUri().toASCIIString()
            referenced.add(entry.copy(reference = reference))
        }
        return Listed(referenced, listed.hiddenByFilter, hiddenByName)
    }

    /**
     * The folders and regular files of [stream], each with its path under [shown]; null past [entryCap]. A name that is
     * not valid UTF-8 (ext4 takes any bytes, through adb push for one) reads with U+FFFD in place of its bad bytes, and
     * that text names another file or none: the host decodes a reference's escapes as UTF-8, and a folder is listed by
     * its path as text. Such an entry goes to [Read.unnamed] instead, never listed and never two rows with one path.
     */
    private fun collect(stream: DirectoryStream<Path>, shown: Path, cancelled: () -> Boolean): Read? {
        val entries = IdentityHashMap<ListEntry, Path>()
        val unnamed = ArrayList<ListEntry>()
        for (path in stream) {
            if (cancelled()) break // Answered already; this result is dropped.
            val attributes = try {
                fs.attributes(path)
            } catch (_: NoSuchFileException) {
                continue // Deleted since the folder was read.
            }
            if (!attributes.isDirectory && !attributes.isRegularFile) continue
            if (entries.size + unnamed.size == entryCap) return null
            val located = shown.resolve(path.fileName)
            val entry = ListEntry(path.fileName.toString(), located.toString(), "", attributes.isDirectory,
                attributes.size(), attributes.lastModifiedTime().toMillis())
            if (readsAsText(path.fileName)) entries[entry] = located else unnamed.add(entry)
        }
        return Read(entries, unnamed)
    }

    private fun checkHandoff(root: Path, file: Path): Outcome = try {
        if (!fs.attributes(file).isRegularFile) {
            failure(HANDOFF, NOT_FILE)
        } else {
            val rootReal = fs.realPath(root)
            val real = fs.realPath(file)
            val shown = if (inside(rootReal, real)) root.resolve(rootReal.relativize(real)) else null
            when {
                shown == null -> failure(HANDOFF, OUTSIDE)
                // Never listed (see collect); a reference made up for one would open nothing on the host's side.
                !readsAsText(shown) -> failure(HANDOFF, NOT_FILE)
                else -> Answer(mapOf("ok" to true, "reference" to shown.toUri().toASCIIString(),
                    "name" to shown.fileName.toString(), "reason" to ""))
            }
        }
    } catch (e: IOException) {
        failure(HANDOFF, if (readFailure(e) == GONE) GONE else FAILED)
    }

    internal companion object {
        const val ROOTS = "roots"
        const val LISTING = "listing"
        const val HANDOFF = "handoff"

        const val STATUS = "status"
        const val OK = "ok"
        const val UNREADABLE = "unreadable"
        const val GONE = "gone"
        const val OUTSIDE = "outside"
        const val TOO_MANY = "tooMany"
        const val STALE = "stale"
        const val EXPIRED = "expired"
        const val NOT_FILE = "notFile"
        const val BUSY = "busy"
        const val TIMED_OUT = "timedOut"
        const val CANCELLED = "cancelled"
        const val FAILED = "failed"

        /** How many closed session ids are remembered, most recent first. */
        const val CLOSED_SESSIONS_KEPT = 64

        private val SYMBOL_CAPTURE_EXTENSIONS = listOf(".bin", ".raw", ".sym")

        private val ORDER = compareBy<ListEntry> { !it.dir }.then { a, b -> NaturalOrder.compare(a.name, b.name) }

        /** A failed request's record fields, in its kind's shape. */
        fun failureFields(kind: String, status: String): Map<String, Any?> = when (kind) {
            HANDOFF -> mapOf("ok" to false, "reference" to "", "name" to "", "reason" to status)
            ROOTS -> mapOf(STATUS to status, "generation" to 0L, "roots" to emptyList<Any>())
            else -> mapOf(STATUS to status)
        }

        private fun failure(kind: String, status: String): Outcome = Answer(failureFields(kind, status))

        /** A missing folder (or a file where a folder was asked for) is gone; any other refusal is unreadable. */
        private fun readFailure(e: IOException) =
            if (e is NoSuchFileException || e is NotDirectoryException) GONE else UNREADABLE

        /** Whether [path] is the path its own text names: false when a name in it is not valid UTF-8. */
        private fun readsAsText(path: Path): Boolean = path.fileSystem.getPath(path.toString()) == path

        /** Whether [real] is [rootReal] or below it, by whole path components. */
        private fun inside(rootReal: Path, real: Path): Boolean =
            rootReal.toString().isNotEmpty() && real.toString().isNotEmpty() &&
                (real == rootReal || real.startsWith(rootReal))

        /** Dot names out unless [showAll], then files the [kind] does not take; folders first, natural order. */
        private fun shape(entries: List<ListEntry>, kind: String, showAll: Boolean): Listed {
            if (showAll) return Listed(entries.sortedWith(ORDER), 0)
            val visible = entries.filter { !it.name.startsWith(".") }
            val kept = visible.filter { it.dir || takes(kind, it) }
            return Listed(kept.sortedWith(ORDER), visible.size - kept.size)
        }

        /** Whether a [kind] pick lists this file. Suffixes ignore case, except the engine's symbol-capture rule. */
        fun takes(kind: String, entry: ListEntry): Boolean = when (kind) {
            "csv" -> entry.name.endsWith(".csv", true) || entry.name.endsWith(".txt", true)
            "backup" -> entry.name.endsWith(".dsdbak", true)
            "replay" -> entry.name.endsWith(".wav", true) || isSymbolCapture(entry.path.ifEmpty { entry.name })
            else -> true
        }

        /**
         * Whether the engine opens [path] as a symbol capture: the last "." of the whole path starts ".bin", ".raw" or
         * ".sym", case-sensitive (dsd_opts_audio_in_dev_is_symbol_capture(), Util.isSymbolCapture).
         */
        fun isSymbolCapture(path: String): Boolean {
            val dot = path.lastIndexOf('.')
            return dot >= 0 && SYMBOL_CAPTURE_EXTENSIONS.any { path.startsWith(it, dot) }
        }
    }
}

/**
 * Case-insensitive natural order: runs of ASCII digits compare by value, so `b2` comes before `b10`. Names equal that
 * way fall back to plain order, so the sort is total.
 */
internal object NaturalOrder : Comparator<String> {
    override fun compare(a: String, b: String): Int {
        val folded = compareFolded(a, b)
        return if (folded != 0) folded else a.compareTo(b)
    }

    private fun compareFolded(a: String, b: String): Int {
        var i = 0
        var j = 0
        while (i < a.length && j < b.length) {
            if (isDigit(a[i]) && isDigit(b[j])) {
                val endA = runEnd(a, i)
                val endB = runEnd(b, j)
                val byValue = compareRuns(a, i, endA, b, j, endB)
                if (byValue != 0) return byValue
                i = endA
                j = endB
            } else {
                val byChar = fold(a[i]).compareTo(fold(b[j]))
                if (byChar != 0) return byChar
                i++
                j++
            }
        }
        return (a.length - i).compareTo(b.length - j)
    }

    /** Two digit runs by value: leading zeros dropped, then the longer is larger, then digit by digit. */
    private fun compareRuns(a: String, startA: Int, endA: Int, b: String, startB: Int, endB: Int): Int {
        val fromA = skipZeros(a, startA, endA)
        val fromB = skipZeros(b, startB, endB)
        val byLength = (endA - fromA).compareTo(endB - fromB)
        if (byLength != 0) return byLength
        for (k in 0 until endA - fromA) {
            val byDigit = a[fromA + k].compareTo(b[fromB + k])
            if (byDigit != 0) return byDigit
        }
        return 0
    }

    private fun isDigit(c: Char) = c in '0'..'9'

    private fun runEnd(s: String, start: Int): Int {
        var end = start
        while (end < s.length && isDigit(s[end])) end++
        return end
    }

    private fun skipZeros(s: String, start: Int, end: Int): Int {
        var from = start
        while (from < end - 1 && s[from] == '0') from++
        return from
    }

    private fun fold(c: Char) = Character.toLowerCase(Character.toUpperCase(c))
}
