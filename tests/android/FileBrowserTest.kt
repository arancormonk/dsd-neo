// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.pm.PackageManager
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.os.CancellationSignal
import android.os.Environment
import android.os.Handler
import android.os.storage.StorageManager
import android.os.storage.StorageVolume
import android.provider.Settings
import java.io.File
import java.net.URI
import java.nio.file.AccessDeniedException
import java.nio.file.DirectoryStream
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import java.nio.file.attribute.BasicFileAttributes
import java.util.concurrent.atomic.AtomicInteger

private const val PKG = "io.github.arancormonk.dsdneo"
private const val READ = "android.permission.READ_EXTERNAL_STORAGE"
private const val MANAGE = "android.permission.MANAGE_EXTERNAL_STORAGE"
private const val USB_DIR = "/mnt/media_rw/6187-25A8"
private const val DOWNLOADS = "content://media/external/downloads"
private const val WAIT_MS = 10_000L
private const val FIRST_CODE = 0x5E00
private const val LAST_CODE = 0x5E0F

private val scratch: Path = Files.createTempDirectory("dsd-file-browser").toRealPath()
private var serial = 0

private fun folder(name: String): Path = Files.createDirectories(scratch.resolve("$name${++serial}"))

private fun primary(dir: String? = folder("primary").toString(), state: String = Environment.MEDIA_MOUNTED) =
    BrowserVolume(dir, state, primary = true, removable = false, uuid = null, description = "Internal shared storage")

private fun usb(dir: String? = USB_DIR, state: String = Environment.MEDIA_MOUNTED, description: String? = "USBSTICK") =
    BrowserVolume(dir, state, primary = false, removable = true, uuid = "6187-25A8", description = description)

/** One MediaStore row as the fake resolver holds it, keyed by column name. */
private fun row(id: Long, name: String, owner: String, folder: String = "Download/DSD-neo/") = mapOf<String, Any?>(
    "_id" to id, "_display_name" to name, "_size" to 2048L, "date_modified" to 1_700_000_000L,
    "relative_path" to folder, "owner_package_name" to owner,
)

/** A query the fake resolver was asked. */
private class Query(
    val uri: String,
    val projection: List<String>,
    val selection: String,
    val args: List<String>,
    val signal: CancellationSignal,
)

/** The rows of a query, over the asked projection only. */
private class FakeCursor(private val projection: List<String>, private val rows: List<Map<String, Any?>>) : Cursor {
    private var position = -1
    var closed = false

    override fun getColumnIndex(columnName: String) = projection.indexOf(columnName)
    override fun moveToFirst(): Boolean { position = 0; return rows.isNotEmpty() }
    override fun moveToNext(): Boolean { position++; return position < rows.size }
    override fun isNull(columnIndex: Int) = rows[position][projection[columnIndex]] == null
    override fun getLong(columnIndex: Int) = rows[position][projection[columnIndex]] as Long
    override fun getString(columnIndex: Int) = rows[position][projection[columnIndex]]?.toString()
    override fun close() { closed = true }
}

/**
 * The platform as a test arranges it. Its resolver plays MediaStore: it keeps each query and answers the rows its
 * selection and arguments match (the `_id`, folder and owner predicates), so a row the predicate leaves out is never
 * seen. [holdQueries] makes a query wait for its signal's cancel.
 */
private class FakePlatform(override var apiLevel: Int = 36) : BrowserPlatform {
    override val packageName = PKG
    val volumeList = mutableListOf<BrowserVolume>()
    var filesDir: File? = folder("files").toFile()
    val declaredPermissions = mutableSetOf<String>()
    var allFiles = false
    val held = mutableSetOf<String>()
    var groups: String? = "Name:\tdsd\nGroups:\t3003 9997\n"
    val rows = mutableListOf<Map<String, Any?>>()
    val queries = mutableListOf<Query>()
    var holdQueries = false
    var onVolumes: (() -> Unit)? = null
    val volumeCalls = AtomicInteger()
    val filesDirCalls = AtomicInteger()
    val groupsCalls = AtomicInteger()

    override fun volumes(): List<BrowserVolume> {
        volumeCalls.incrementAndGet()
        return synchronized(volumeList) { volumeList.toList() }
    }

    override fun externalFilesDir(): File? {
        filesDirCalls.incrementAndGet()
        return filesDir
    }

    override fun declares(permission: String) = permission in declaredPermissions
    override fun allFilesAccess() = allFiles
    override fun holds(permission: String) = permission in held

    override fun groupsText(): String? {
        groupsCalls.incrementAndGet()
        return groups
    }

    override fun watchVolumes(onChange: () -> Unit) {
        onVolumes = onChange
    }

    override fun query(
        uri: Uri,
        projection: Array<String>,
        selection: String,
        args: Array<String>,
        signal: CancellationSignal,
    ): Cursor? {
        synchronized(queries) { queries += Query(uri.toString(), projection.toList(), selection, args.toList(), signal) }
        if (holdQueries) {
            val until = System.nanoTime() + WAIT_MS * 1_000_000
            while (!signal.isCanceled && System.nanoTime() < until) Thread.sleep(2)
            signal.throwIfCanceled()
        }
        val columns = Regex("(\\w+) = \\?").findAll(selection).map { it.groupValues[1] }.toList()
        check(columns.size == args.size) { "selection $selection takes ${columns.size} arguments, got ${args.size}" }
        val matched = rows.filter { candidate -> columns.indices.all { candidate[columns[it]].toString() == args[it] } }
        return FakeCursor(projection.toList(), matched)
    }

    fun lastQuery(): Query = synchronized(queries) { queries.last() }
}

/** NioFs, except that it counts its opens and refuses the paths under [denied], as a process without gid 1077. */
private class GateFs(private val denied: String = "/mnt/media_rw/") : BrowserFs {
    private val nio = NioFs()
    val opens = AtomicInteger()

    override fun open(dir: Path): DirectoryStream<Path> {
        opens.incrementAndGet()
        if (dir.toString().startsWith(denied)) throw AccessDeniedException(dir.toString())
        return nio.open(dir)
    }

    override fun attributes(path: Path): BasicFileAttributes = nio.attributes(path)
    override fun realPath(path: Path): Path {
        if (path.toString().startsWith(denied)) throw AccessDeniedException(path.toString())
        return nio.realPath(path)
    }
}

/** A fresh browser over [platform]; every test starts from one, so no state leaks between them. */
private fun install(platform: FakePlatform, fs: BrowserFs = GateFs(), watchdogMs: Long = WAIT_MS): FakePlatform {
    FileBrowser.install(BrowserCore(platform, FileListing(fs = fs, watchdogMs = watchdogMs, nowMs = { 0L })))
    Handler.drain()
    while (FileBrowser.pollRecord().isNotEmpty()) Unit
    return platform
}

/** A minimal JSON reader, so each record is checked as the host's parser sees it. */
private class Json(private val text: String) {
    private var at = 0

    fun read(): Any? {
        val value = value()
        space()
        check(at == text.length) { "trailing text in $text" }
        return value
    }

    private fun space() {
        while (at < text.length && text[at].isWhitespace()) at++
    }

    private fun value(): Any? {
        space()
        return when (text[at]) {
            '{' -> obj()
            '[' -> array()
            '"' -> string()
            't' -> word("true", true)
            'f' -> word("false", false)
            'n' -> word("null", null)
            else -> number()
        }
    }

    private fun obj(): Map<String, Any?> {
        val map = LinkedHashMap<String, Any?>()
        expect('{')
        space()
        if (text[at] == '}') return map.also { at++ }
        while (true) {
            space()
            val key = string()
            space()
            expect(':')
            check(key !in map) { "key $key twice in $text" }
            map[key] = value()
            space()
            if (text[at] == ',') { at++; continue }
            expect('}')
            return map
        }
    }

    private fun array(): List<Any?> {
        val list = ArrayList<Any?>()
        expect('[')
        space()
        if (text[at] == ']') return list.also { at++ }
        while (true) {
            list += value()
            space()
            if (text[at] == ',') { at++; continue }
            expect(']')
            return list
        }
    }

    private fun string(): String {
        expect('"')
        val out = StringBuilder()
        while (true) {
            val c = text[at++]
            when {
                c == '"' -> return out.toString()
                c != '\\' -> out.append(c)
                else -> when (val escape = text[at++]) {
                    'n' -> out.append('\n')
                    't' -> out.append('\t')
                    'r' -> out.append('\r')
                    'b' -> out.append('\b')
                    'f' -> out.append('\u000c')
                    'u' -> out.append(text.substring(at, at + 4).toInt(16).toChar()).also { at += 4 }
                    else -> out.append(escape)
                }
            }
        }
    }

    private fun number(): Any {
        val start = at
        while (at < text.length && (text[at].isDigit() || text[at] in "+-.eE")) at++
        val digits = text.substring(start, at)
        return digits.toLongOrNull() ?: digits.toDouble()
    }

    private fun word(word: String, value: Any?): Any? {
        check(text.startsWith(word, at)) { "bad literal at $at in $text" }
        at += word.length
        return value
    }

    private fun expect(c: Char) {
        check(at < text.length && text[at] == c) { "expected $c at $at in $text" }
        at++
    }
}

@Suppress("UNCHECKED_CAST")
private fun parse(text: String): Map<String, Any?> = Json(text).read() as Map<String, Any?>

/** The next record, waiting for a worker thread's answer. */
private fun awaitRecord(): Map<String, Any?> {
    val until = System.nanoTime() + WAIT_MS * 1_000_000
    while (System.nanoTime() < until) {
        val text = FileBrowser.pollRecord()
        if (text.isNotEmpty()) return parse(text)
        Thread.sleep(2)
    }
    error("no record within $WAIT_MS ms")
}

/** The access records the main looper settled; none means the request is still out. */
private fun accessRecords(): List<Map<String, Any?>> {
    Handler.drain()
    return generateSequence { FileBrowser.pollRecord().ifEmpty { null } }.map(::parse).toList()
}

private fun accessOutcome(id: Long): String {
    val records = accessRecords()
    check(records.size == 1) { "expected one access record, got $records" }
    val record = records.single()
    check(record == mapOf("id" to id, "kind" to "access", "outcome" to record["outcome"])) { "access record $record" }
    return record["outcome"] as String
}

private fun accessState(): Map<String, Any?> = parse(FileBrowser.accessState())

@Suppress("UNCHECKED_CAST")
private fun roots(id: Long = 900, session: Long = 1): Pair<Long, List<Map<String, Any?>>> {
    check(FileBrowser.listRoots(id, session))
    val record = awaitRecord()
    check(record["id"] == id && record["kind"] == "roots" && record["status"] == "ok") { "roots record $record" }
    return record["generation"] as Long to record["roots"] as List<Map<String, Any?>>
}

private fun states(roots: List<Map<String, Any?>>) = roots.map { "${it["key"]} ${it["state"]}" }

private fun rootsByApiAndGrant() {
    val platform = install(FakePlatform(36).apply {
        declaredPermissions += MANAGE
        volumeList += primary()
        volumeList += usb()
    })
    val (_, listed) = roots()
    check(states(listed) == listOf("inbox ready", "saved ready", "storage needsAccess", "removable:6187-25A8 needsAccess")) {
        "API 36 declared, not granted: ${states(listed)}"
    }
    val inbox = listed[0]
    val inboxPath = File(platform.filesDir, "Inbox")
    check(inbox["path"] == inboxPath.path && inboxPath.isDirectory) { "the inbox is <files>/Inbox, created: $inbox" }
    check(inbox == mapOf("key" to "inbox", "kind" to "inbox", "title" to "", "path" to inboxPath.path,
        "state" to "ready", "detail" to "")) { "inbox root $inbox" }
    check(listed[1] == mapOf("key" to "saved", "kind" to "saved", "title" to "", "path" to "", "state" to "ready",
        "detail" to "")) { "saved root ${listed[1]}" }
    check(listed[2]["kind"] == "storage" && listed[2]["path"] == platform.volumeList[0].directory &&
        listed[2]["detail"] == "mounted") { "storage root ${listed[2]}" }
    val state = accessState()
    check(state["declared"] == true && state["granted"] == false && state["route"] == "settings" &&
        state["apiLevel"] == 36L) { "API 36 access state $state" }
    check(state.keys == setOf("declared", "granted", "route", "apiLevel", "rootsGeneration")) { "access keys $state" }

    // A Play build: no All files access declared, so no broad roots and no route.
    install(FakePlatform(36).apply { volumeList += primary(); volumeList += usb() })
    check(states(roots().second) == listOf("inbox ready", "saved ready")) { "not declared lists inbox and Saved only" }
    check(accessState()["route"] == "none") { "not declared has no route" }

    // Android 10: the legacy READ permission, asked with the runtime dialog; USB stays out of reach.
    install(FakePlatform(29).apply {
        declaredPermissions += READ
        volumeList += primary()
        volumeList += usb()
    })
    check(states(roots().second) == listOf("inbox ready", "saved ready", "storage needsAccess")) {
        "API 29 READ declared, not granted"
    }
    check(accessState()["route"] == "permission" && accessState()["apiLevel"] == 29L) { "API 29 route" }
    install(FakePlatform(29).apply { declaredPermissions += MANAGE; volumeList += primary() })
    check(states(roots().second) == listOf("inbox ready", "saved ready")) { "API 29 needs READ, not MANAGE" }
    check(accessState()["route"] == "none") { "API 29 without READ has no route" }
    install(FakePlatform(29).apply { declaredPermissions += READ; held += READ; volumeList += primary() })
    check(states(roots().second) == listOf("inbox ready", "saved ready", "storage ready")) { "API 29 READ granted" }
    check(accessState()["granted"] == true) { "API 29 granted reads READ" }

    // Shared storage away: no app folder, so no inbox.
    install(FakePlatform(36).apply { filesDir = null })
    val away = roots().second
    check(away[0] == mapOf("key" to "inbox", "kind" to "inbox", "title" to "", "path" to "", "state" to "unavailable",
        "detail" to "")) { "no external files dir: ${away[0]}" }
}

private fun rootStatePrecedence() {
    fun removable(): Map<String, Any?> = roots().second.single { it["kind"] == "removable" }

    // An invisible USB volume after a grant, in a process started without gid 1077: its root does not even exist yet.
    val granted = install(FakePlatform(36).apply {
        declaredPermissions += MANAGE
        allFiles = true
        volumeList += primary()
        volumeList += usb()
    })
    check(!File(USB_DIR).exists()) { "this host has $USB_DIR" }
    val restart = removable()
    check(restart == mapOf("key" to "removable:6187-25A8", "kind" to "removable", "title" to "USBSTICK",
        "path" to USB_DIR, "state" to "needsRestart", "detail" to "mounted")) { "invisible USB, granted: $restart" }

    // `sm unmount`: no directory, unmounted.
    synchronized(granted.volumeList) { granted.volumeList[1] = usb(dir = null, state = Environment.MEDIA_UNMOUNTED) }
    val gone = removable()
    check(gone["state"] == "removed" && gone["path"] == "" && gone["detail"] == "unmounted") { "unmounted: $gone" }

    // A visible removable volume (an SD card through FUSE) is readable right after the grant: no restart.
    val visible = folder("sdcard").toString()
    synchronized(granted.volumeList) {
        granted.volumeList[1] = BrowserVolume(visible, Environment.MEDIA_MOUNTED, primary = false, removable = true,
            uuid = "8A7C-18ED", description = null)
    }
    val sd = removable()
    check(sd["state"] == "ready" && sd["key"] == "removable:8A7C-18ED" && sd["title"] == "") {
        "visible removable, granted: $sd (a null description is an empty title)"
    }
    check(roots().second.single { it["kind"] == "storage" }["state"] == "ready") { "granted storage is ready" }

    // Started with gid 1077: ready, from the volume and the process alone. The roots never look at the drive, so a
    // drive that still cannot be read says so when it is listed.
    val fs = GateFs()
    install(FakePlatform(36).apply {
        declaredPermissions += MANAGE
        allFiles = true
        groups = "Name:\tdsd\nGroups:\t1077 3003\n"
        volumeList += usb()
    }, fs)
    val (generation, withGid) = roots()
    check(withGid.single { it["kind"] == "removable" }["state"] == "ready") { "granted with gid 1077 is ready" }
    check(fs.opens.get() == 0) { "the roots made ${fs.opens.get()} filesystem calls" }
    check(FileBrowser.listFolder(1, 1, "removable:6187-25A8", generation, "", "csv", false))
    check(awaitRecord() == mapOf("id" to 1L, "kind" to "listing", "status" to "unreadable")) {
        "a ready root the process cannot read lists unreadable"
    }

    // States without a readable directory: a drive Android is checking or cannot mount is there but unavailable, with
    // or without access; every other one is removed.
    val states = install(FakePlatform(36).apply { declaredPermissions += MANAGE; allFiles = true })
    for ((state, expected) in listOf(
        Environment.MEDIA_UNMOUNTED to "removed",
        Environment.MEDIA_REMOVED to "removed",
        Environment.MEDIA_BAD_REMOVAL to "removed",
        Environment.MEDIA_EJECTING to "removed",
        Environment.MEDIA_CHECKING to "unavailable",
        Environment.MEDIA_UNMOUNTABLE to "unavailable",
        Environment.MEDIA_NOFS to "unavailable",
    )) {
        for (granted in listOf(true, false)) {
            states.allFiles = granted
            synchronized(states.volumeList) {
                states.volumeList.clear()
                states.volumeList += usb(dir = null, state = state)
            }
            val root = removable()
            check(root["state"] == expected && root["detail"] == state) { "$state (granted $granted): $root" }
        }
    }
    synchronized(states.volumeList) { states.volumeList[0] = usb(state = Environment.MEDIA_MOUNTED_READ_ONLY) }
    check(removable()["state"] == "needsAccess") { "mounted read-only, not granted" }
    states.allFiles = true
    check(removable()["state"] == "needsRestart") { "mounted read-only, granted, no gid 1077" }

    // Precedence: removed before needsAccess before anything readable.
    val notGranted = install(FakePlatform(36).apply {
        declaredPermissions += MANAGE
        volumeList += usb(dir = null, state = Environment.MEDIA_UNMOUNTED)
    })
    check(removable()["state"] == "removed") { "unmounted and not granted is removed" }
    synchronized(notGranted.volumeList) { notGranted.volumeList[0] = usb() }
    check(removable()["state"] == "needsAccess") { "invisible and not granted needs access" }

    // A volume without a UUID is keyed by its place in the list.
    install(FakePlatform(36).apply {
        declaredPermissions += MANAGE
        volumeList += primary()
        volumeList += BrowserVolume(folder("anon").toString(), Environment.MEDIA_MOUNTED, false, true, null, "SD")
    })
    check(removable()["key"] == "removable:1") { "no UUID: keyed by index" }
}

private fun rootsGenerationMoves() {
    val fs = GateFs()
    val platform = install(FakePlatform(36).apply {
        declaredPermissions += MANAGE
        volumeList += primary()
    }, fs)
    val filesCalls = platform.filesDirCalls.get()
    val first = accessState()["rootsGeneration"] as Long
    check(first > 0 && accessState()["rootsGeneration"] == first) { "an unchanged state keeps its generation" }
    check(fs.opens.get() == 0 && platform.filesDirCalls.get() == filesCalls) {
        "the access state makes no filesystem call (${fs.opens.get()} opens)"
    }
    val (listedAt, _) = roots()
    check(listedAt == first) { "the roots carry the generation they were read at" }
    check(fs.opens.get() == 0) { "the roots never look at a drive" }

    synchronized(platform.volumeList) { platform.volumeList += usb() }
    val added = accessState()["rootsGeneration"] as Long
    check(added > first) { "a volume added moves the generation" }
    synchronized(platform.volumeList) { platform.volumeList[1] = usb(dir = null, state = Environment.MEDIA_UNMOUNTED) }
    val changed = accessState()["rootsGeneration"] as Long
    check(changed > added) { "a state change moves the generation" }
    platform.allFiles = true
    val grantedAt = accessState()["rootsGeneration"] as Long
    check(grantedAt > changed) { "a grant moves the generation" }
    check(accessState()["rootsGeneration"] == grantedAt) { "and then it stays" }
    check(fs.opens.get() == 0) { "still no filesystem call" }
}

private fun gidAtStart() {
    check(hasStorageGid("Name:\tdsd\nGroups:\t1077 3003\n")) { "1077 listed" }
    check(hasStorageGid("Groups:\t3003 1077")) { "1077 last, no newline" }
    check(!hasStorageGid("Name:\tdsd\nGroups:\t3003\n")) { "1077 absent" }
    check(!hasStorageGid("Groups:\t10770 31077\n")) { "only the whole group id counts" }
    check(!hasStorageGid("Name:\tdsd\n")) { "no Groups: line" }
    check(!hasStorageGid(null)) { "unreadable status" }

    // Read once, when the browser starts: the process keeps the groups it started with.
    val platform = install(FakePlatform(36).apply { declaredPermissions += MANAGE; volumeList += primary() })
    roots()
    roots()
    accessState()
    check(platform.groupsCalls.get() == 1) { "the groups were read ${platform.groupsCalls.get()} times" }
}

@Suppress("UNCHECKED_CAST")
private fun savedQuery() {
    val platform = install(FakePlatform(36).apply {
        rows += row(42, "dmr.csv", PKG)
        rows += row(43, "other.csv", "com.android.shell")
        rows += row(44, "loose.csv", PKG, folder = "Download/")
        rows += row(45, "backup.dsdbak", PKG)
    })
    val (generation, _) = roots()
    check(FileBrowser.listFolder(1, 1, "saved", generation, "", "csv", false))
    val record = awaitRecord()
    val query = platform.lastQuery()
    check(query.uri == DOWNLOADS) { "queried ${query.uri}" }
    check(query.projection == listOf("_id", "_display_name", "_size", "date_modified")) { "projection ${query.projection}" }
    check(query.selection == "relative_path = ? AND owner_package_name = ?") { "selection ${query.selection}" }
    check(query.args == listOf("Download/DSD-neo/", PKG)) { "selection args ${query.args}" }
    check(record["status"] == "ok" && record["hiddenByFilter"] == 1L && record["hiddenByName"] == 0L) {
        "Saved listing $record"
    }
    val entries = record["entries"] as List<Map<String, Any?>>
    check(entries == listOf(mapOf("name" to "dmr.csv", "path" to "", "reference" to "$DOWNLOADS/42", "dir" to false,
        "size" to 2048L, "modifiedMs" to 1_700_000_000_000L))) { "Saved entries $entries (the other owner's row never)" }

    // Cancel, close and a newer listing each cancel the query's signal, not only the request.
    platform.holdQueries = true
    check(FileBrowser.listFolder(2, 1, "saved", generation, "", "csv", false))
    val held = awaitQuery(platform, 2)
    FileBrowser.cancel(2)
    check(awaitRecord() == mapOf("id" to 2L, "kind" to "listing", "status" to "cancelled")) { "cancelled listing" }
    check(held.signal.isCanceled) { "cancel(id) cancels the query's CancellationSignal" }

    check(FileBrowser.listFolder(3, 7, "saved", generation, "", "csv", false))
    val closing = awaitQuery(platform, 3)
    FileBrowser.closeSession(7)
    check(awaitRecord()["status"] == "cancelled" && closing.signal.isCanceled) { "closeSession cancels the query" }
    // A cancelled query ends its worker promptly, so the next two requests find a free place.
    awaitWorkerGone(2)
    awaitWorkerGone(3)

    check(FileBrowser.listFolder(4, 8, "saved", generation, "", "csv", false))
    val older = awaitQuery(platform, 4)
    platform.holdQueries = false
    check(FileBrowser.listFolder(5, 8, "saved", generation, "", "csv", false))
    val superseded = awaitRecord()
    check(superseded == mapOf("id" to 4L, "kind" to "listing", "status" to "cancelled")) { "superseded $superseded" }
    check(older.signal.isCanceled) { "a newer listing in the session cancels the older query" }
    val newer = awaitRecord()
    check(newer["id"] == 5L && newer["status"] == "ok") { "the newer listing $newer" }
    check(!platform.lastQuery().signal.isCanceled) { "the newer query's own signal stays" }
    check(FileBrowser.pollRecord().isEmpty()) { "each request answered once" }

    // A query that finds no provider is unreadable, never an empty folder.
    install(object : BrowserPlatformDelegate(FakePlatform(36)) {
        override fun query(uri: Uri, projection: Array<String>, selection: String, args: Array<String>,
            signal: CancellationSignal): Cursor? = null
    })
    val (again, _) = roots()
    check(FileBrowser.listFolder(6, 1, "saved", again, "", "csv", false))
    check(awaitRecord()["status"] == "unreadable") { "a null cursor is unreadable" }
}

/** Forwards everything to [base]; a test overrides the one call it changes. */
private open class BrowserPlatformDelegate(private val base: FakePlatform) : BrowserPlatform by base

private fun savedQueryTimeout() {
    // The watchdog's timedOut cancels the query's signal too, so a provider stuck in the query frees its place.
    val platform = install(FakePlatform(36).apply { holdQueries = true }, watchdogMs = 200)
    val (generation, _) = roots()
    check(FileBrowser.listFolder(1, 1, "saved", generation, "", "csv", false))
    val query = awaitQuery(platform, 1)
    check(awaitRecord() == mapOf("id" to 1L, "kind" to "listing", "status" to "timedOut")) { "Saved query timed out" }
    awaitWorkerGone(1)
    check(query.signal.isCanceled) { "the watchdog cancels the query's CancellationSignal" }
    check(FileBrowser.pollRecord().isEmpty()) { "answered once" }
}

private fun install(platform: BrowserPlatformDelegate) {
    val fs = GateFs()
    FileBrowser.install(BrowserCore(platform, FileListing(fs = fs, watchdogMs = WAIT_MS, nowMs = { 0L })))
    while (FileBrowser.pollRecord().isNotEmpty()) Unit
}

/** Waits for the worker thread of request [id] to return; FileListing names each one after its request. */
private fun awaitWorkerGone(id: Long) {
    val until = System.nanoTime() + WAIT_MS * 1_000_000
    while (Thread.getAllStackTraces().keys.any { it.name == "dsd-file-browser-$id" }) {
        check(System.nanoTime() < until) { "the worker of request $id never returned" }
        Thread.sleep(2)
    }
}

private fun awaitQuery(platform: FakePlatform, count: Int): Query {
    val until = System.nanoTime() + WAIT_MS * 1_000_000
    while (System.nanoTime() < until) {
        synchronized(platform.queries) { if (platform.queries.size >= count) return platform.queries[count - 1] }
        Thread.sleep(2)
    }
    error("query $count never came")
}

private fun savedHandoff() {
    val platform = install(FakePlatform(36).apply {
        rows += row(42, "dmr.csv", PKG)
        rows += row(43, "other.csv", "com.android.shell")
    })
    val (generation, _) = roots()
    check(FileBrowser.checkHandoff(1, 1, "saved", generation, "$DOWNLOADS/42"))
    val ok = awaitRecord()
    check(ok == mapOf("id" to 1L, "kind" to "handoff", "ok" to true, "reference" to "$DOWNLOADS/42", "name" to "dmr.csv",
        "reason" to "")) { "Saved hand-off $ok" }
    val query = platform.lastQuery()
    check(query.selection == "_id = ? AND relative_path = ? AND owner_package_name = ?") { "re-query ${query.selection}" }
    check(query.args == listOf("42", "Download/DSD-neo/", PKG)) { "re-query args ${query.args}" }
    check(query.uri == DOWNLOADS) { "re-query uri ${query.uri}" }

    check(FileBrowser.checkHandoff(2, 1, "saved", generation, "$DOWNLOADS/43"))
    check(awaitRecord() == mapOf("id" to 2L, "kind" to "handoff", "ok" to false, "reference" to "", "name" to "",
        "reason" to "gone")) { "another owner's row is gone" }
    platform.rows.clear()
    check(FileBrowser.checkHandoff(3, 1, "saved", generation, "$DOWNLOADS/42"))
    check(awaitRecord()["reason"] == "gone") { "a deleted row is gone" }
    check(FileBrowser.checkHandoff(4, 1, "saved", generation + 1, "$DOWNLOADS/42"))
    check(awaitRecord()["reason"] == "stale") { "a moved generation is stale" }

    // Stale is decided at once, without a worker: with all three places held it is still stale, never busy.
    platform.rows += row(42, "dmr.csv", PKG)
    platform.holdQueries = true
    val before = platform.queries.size
    for (n in 1..3) check(FileBrowser.listFolder(20L + n, 20L + n, "saved", generation, "", "csv", false))
    awaitQuery(platform, before + 3)
    check(FileBrowser.checkHandoff(24, 1, "saved", generation + 1, "$DOWNLOADS/42"))
    check(FileBrowser.checkHandoff(25, 1, "inbox", generation + 1, "file:///x.csv"))
    check(awaitRecord() == mapOf("id" to 24L, "kind" to "handoff", "ok" to false, "reference" to "", "name" to "",
        "reason" to "stale")) { "a stale Saved hand-off with every place held" }
    check(awaitRecord()["reason"] == "stale") { "a stale folder hand-off with every place held" }
    check(FileBrowser.checkHandoff(26, 1, "saved", generation, "$DOWNLOADS/42"))
    check(awaitRecord()["reason"] == "busy") { "a current hand-off needs a place" }
    for (n in 1..3) FileBrowser.cancel(20L + n)
    repeat(3) { check(awaitRecord()["status"] == "cancelled") }
    for (n in 1..3) awaitWorkerGone(20L + n)
    platform.holdQueries = false
    val queries = platform.queries.size
    for (bad in listOf("$DOWNLOADS/", "$DOWNLOADS/4x", "content://media/external/images/42", "file:///x.csv", "%")) {
        check(!FileBrowser.checkHandoff(5, 1, "saved", generation, bad)) { "a Saved reference $bad is refused" }
    }
    check(platform.queries.size == queries && FileBrowser.pollRecord().isEmpty()) { "a refused reference asks nothing" }
}

@Suppress("UNCHECKED_CAST")
private fun folderRequests() {
    val platform = install(FakePlatform(36))
    val (generation, listed) = roots()
    val inbox = Paths.get(listed[0]["path"] as String)
    Files.write(inbox.resolve("a b#1%.csv"), byteArrayOf(1))
    Files.write(inbox.resolve("map%20v2.csv"), byteArrayOf(1))
    Files.createDirectories(inbox.resolve("Lists"))

    check(FileBrowser.listFolder(1, 1, "inbox", generation, "", "csv", false))
    val record = awaitRecord()
    val entries = record["entries"] as List<Map<String, Any?>>
    check(entries.map { it["name"] } == listOf("Lists", "a b#1%.csv", "map%20v2.csv")) { "inbox listing $entries" }
    val reference = entries[1]["reference"] as String
    check(reference == "file://$inbox/a%20b%231%25.csv") { "file reference $reference" }
    check(entries[2]["reference"] == "file://$inbox/map%2520v2.csv") { "an escape in a name is encoded once" }
    check(Paths.get(URI(reference)) == inbox.resolve("a b#1%.csv")) { "the reference names the file" }

    check(FileBrowser.listFolder(2, 1, "inbox", generation, inbox.resolve("Lists").toString(), "csv", false))
    check(awaitRecord()["status"] == "ok") { "a folder under the root" }
    check(FileBrowser.listFolder(3, 1, "inbox", generation, scratch.toString(), "csv", false))
    check(awaitRecord()["status"] == "outside") { "a folder above the root is outside" }
    check(FileBrowser.listFolder(4, 1, "inbox", generation + 1, "", "csv", false))
    check(awaitRecord() == mapOf("id" to 4L, "kind" to "listing", "status" to "stale")) { "an old generation is stale" }
    check(FileBrowser.listFolder(5, 1, "removable:1", generation, "", "csv", false))
    check(awaitRecord()["status"] == "stale") { "a root the last roots did not list is stale" }

    check(FileBrowser.checkHandoff(6, 1, "inbox", generation, reference))
    check(awaitRecord() == mapOf("id" to 6L, "kind" to "handoff", "ok" to true, "reference" to reference,
        "name" to "a b#1%.csv", "reason" to "")) { "inbox hand-off" }
    check(FileBrowser.checkHandoff(7, 1, "inbox", generation, scratch.resolve("x.csv").toUri().toASCIIString()))
    check(awaitRecord()["reason"] == "gone") { "a missing file outside is never handed off" }
    check(FileBrowser.checkHandoff(8, 1, "inbox", generation - 1, reference))
    check(awaitRecord()["reason"] == "stale") { "an old generation is stale" }
    for (bad in listOf("$DOWNLOADS/42", "file:relative.csv", "file://host/x.csv", "%zz")) {
        check(!FileBrowser.checkHandoff(9, 1, "inbox", generation, bad)) { "a folder reference $bad is refused" }
    }
    check(FileBrowser.pollRecord().isEmpty()) { "a refused request is never answered here" }

    // A volume change since the roots were read makes every request on them stale.
    synchronized(platform.volumeList) { platform.volumeList += usb() }
    check(FileBrowser.listFolder(10, 1, "inbox", generation, "", "csv", false))
    check(awaitRecord()["status"] == "stale") { "the roots moved on" }

    check(FileBrowser.listMore(11, 1, 999, 0))
    check(awaitRecord() == mapOf("id" to 11L, "kind" to "listing", "status" to "expired")) { "a page of no snapshot" }
}

private fun activity() = Activity()

private fun accessChain() {
    install(FakePlatform(36).apply { declaredPermissions += MANAGE })
    val opened = activity()
    check(FileBrowser.requestAccess(opened, 1))
    check(accessOutcome(1) == "settingsOpened") { "the app's All files access page" }
    check(opened.started.map { it.action } == listOf(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)) {
        "launches ${opened.started.map { it.action }}"
    }
    check(opened.started[0].data.toString() == "package:$PKG") { "the app's page: ${opened.started[0].data}" }
    check(opened.launched.isEmpty() && opened.permissionRequests.isEmpty()) { "no result code, no runtime dialog" }

    val list = activity().apply {
        refusals[Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION] = ActivityNotFoundException()
    }
    check(FileBrowser.requestAccess(list, 2))
    check(accessOutcome(2) == "settingsOpened") { "the all-apps list after the app's page" }
    check(list.started.map { it.action } == listOf(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
        Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)) { "two launches" }
    check(list.started[1].data == null) { "the list takes no package" }

    install(FakePlatform(33).apply { declaredPermissions += MANAGE })
    val none = activity().apply {
        refusals[Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION] = ActivityNotFoundException()
        refusals[Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION] = ActivityNotFoundException()
    }
    check(FileBrowser.requestAccess(none, 3))
    check(accessOutcome(3) == "noRoute") { "no Settings screen on API 33" }
    check(none.started.size == 2) { "API 33 never tries App info: ${none.started.map { it.action }}" }

    val refused = activity().apply {
        refusals[Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION] = SecurityException("not exported")
    }
    check(FileBrowser.requestAccess(refused, 4))
    check(accessOutcome(4) == "failed" && refused.started.size == 1) { "another exception fails at once" }

    install(FakePlatform(36))
    val undeclared = activity()
    check(FileBrowser.requestAccess(undeclared, 5))
    check(accessOutcome(5) == "noRoute" && undeclared.started.isEmpty()) { "not declared: nothing launched" }
}

private fun appInfoRouteOn30to32() {
    install(FakePlatform(31).apply { declaredPermissions += MANAGE })
    val appInfo = activity().apply {
        refusals[Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION] = ActivityNotFoundException()
        refusals[Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION] = ActivityNotFoundException()
    }
    check(FileBrowser.requestAccess(appInfo, 1))
    check(accessOutcome(1) == "settingsOpened") { "App info on API 31" }
    check(appInfo.started.size == 3 && appInfo.started[2].action == Settings.ACTION_APPLICATION_DETAILS_SETTINGS &&
        appInfo.started[2].data.toString() == "package:$PKG") { "third launch ${appInfo.started.map { it.action }}" }

    val nothing = activity().apply {
        refusals[Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION] = ActivityNotFoundException()
        refusals[Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION] = ActivityNotFoundException()
        refusals[Settings.ACTION_APPLICATION_DETAILS_SETTINGS] = ActivityNotFoundException()
    }
    check(FileBrowser.requestAccess(nothing, 2))
    check(accessOutcome(2) == "noRoute" && nothing.started.size == 3) { "no App info either" }

    install(FakePlatform(33).apply { declaredPermissions += MANAGE })
    val api33 = activity().apply {
        refusals[Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION] = ActivityNotFoundException()
        refusals[Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION] = ActivityNotFoundException()
    }
    check(FileBrowser.requestAccess(api33, 3))
    check(accessOutcome(3) == "noRoute" && api33.started.none { it.action == Settings.ACTION_APPLICATION_DETAILS_SETTINGS }) {
        "API 33 never tries App info"
    }
}

private fun permission29() {
    install(FakePlatform(29).apply { declaredPermissions += READ })
    val granting = activity()
    check(FileBrowser.requestAccess(granting, 1))
    check(accessRecords().isEmpty()) { "the request stays live until its result" }
    check(granting.permissionRequests == listOf(listOf(READ) to FIRST_CODE)) { "asked ${granting.permissionRequests}" }
    check(granting.started.isEmpty()) { "no Settings screen on API 29" }
    check(FileBrowser.onRequestPermissionsResult(granting, FIRST_CODE, arrayOf(READ),
        intArrayOf(PackageManager.PERMISSION_GRANTED)))
    check(accessOutcome(1) == "granted") { "granted" }

    val denying = activity().apply { rationale = true }
    check(FileBrowser.requestAccess(denying, 2))
    Handler.drain()
    check(denying.permissionRequests.single().second == FIRST_CODE + 1) { "a code of its own" }
    check(FileBrowser.onRequestPermissionsResult(denying, FIRST_CODE + 1, arrayOf(READ),
        intArrayOf(PackageManager.PERMISSION_DENIED)))
    check(accessOutcome(2) == "denied" && denying.started.isEmpty()) { "denied, may ask again" }

    val blocked = activity().apply { rationale = false }
    check(FileBrowser.requestAccess(blocked, 3))
    Handler.drain()
    check(FileBrowser.onRequestPermissionsResult(blocked, FIRST_CODE + 2, arrayOf(READ),
        intArrayOf(PackageManager.PERMISSION_DENIED)))
    check(accessOutcome(3) == "blocked") { "don't ask again" }
    check(blocked.started.map { it.action } == listOf(Settings.ACTION_APPLICATION_DETAILS_SETTINGS) &&
        blocked.started[0].data.toString() == "package:$PKG") { "App info opened: ${blocked.started.map { it.action }}" }

    val interrupted = activity()
    check(FileBrowser.requestAccess(interrupted, 4))
    Handler.drain()
    check(FileBrowser.onRequestPermissionsResult(interrupted, FIRST_CODE + 3, arrayOf(), intArrayOf()))
    check(accessOutcome(4) == "cancelled") { "an interrupted dialog answers no arrays" }

    install(FakePlatform(29))
    val undeclared = activity()
    check(FileBrowser.requestAccess(undeclared, 5))
    check(accessOutcome(5) == "noRoute" && undeclared.permissionRequests.isEmpty()) { "API 29 without READ" }
}

private fun reentrantPermissionAnswer() {
    // Android 10 answers a request made while another is pending at once, with empty arrays, from inside
    // requestPermissions(): the new request is cancelled, once, and nothing stays live.
    install(FakePlatform(29).apply { declaredPermissions += READ })
    val pending = activity()
    check(FileBrowser.requestAccess(pending, 1))
    Handler.drain()
    val busy = activity().apply {
        answerAtOnce = { code -> check(FileBrowser.onRequestPermissionsResult(this, code, arrayOf(), intArrayOf())) }
    }
    check(FileBrowser.requestAccess(busy, 2))
    val records = accessRecords()
    check(records == listOf(mapOf("id" to 1L, "kind" to "access", "outcome" to "cancelled"),
        mapOf("id" to 2L, "kind" to "access", "outcome" to "cancelled"))) { "re-entrant answer: $records" }
    val code = busy.permissionRequests.single().second
    check(FileBrowser.onRequestPermissionsResult(busy, code, arrayOf(READ), intArrayOf(PackageManager.PERMISSION_GRANTED)))
    FileBrowser.cancel(2)
    check(accessRecords().isEmpty()) { "nothing stays live after the re-entrant answer" }
}

private fun lateCallbackOfCancelledLaunch() {
    install(FakePlatform(29).apply { declaredPermissions += READ })
    val first = activity()
    check(FileBrowser.requestAccess(first, 1))
    Handler.drain()
    check(first.permissionRequests.single().second == FIRST_CODE) { "A on 0x5E00" }
    FileBrowser.cancel(1)
    check(accessOutcome(1) == "cancelled") { "cancel answers cancelled" }

    val second = activity()
    check(FileBrowser.requestAccess(second, 2))
    Handler.drain()
    check(second.permissionRequests.single().second == FIRST_CODE + 1) { "B on 0x5E01" }
    check(FileBrowser.onRequestPermissionsResult(first, FIRST_CODE, arrayOf(READ),
        intArrayOf(PackageManager.PERMISSION_GRANTED))) { "A's late callback is claimed" }
    check(accessRecords().isEmpty()) { "A's late callback settles nothing" }
    check(FileBrowser.onRequestPermissionsResult(second, FIRST_CODE + 1, arrayOf(READ),
        intArrayOf(PackageManager.PERMISSION_GRANTED)))
    check(accessOutcome(2) == "granted") { "B settles from its own code" }

    // A newer request settles the live one cancelled; the codes wrap inside the range.
    val codes = mutableListOf<Int>()
    for (id in 3L..17L) {
        val next = activity()
        check(FileBrowser.requestAccess(next, id))
        Handler.drain()
        codes += next.permissionRequests.single().second
    }
    check(codes == (FIRST_CODE + 2..LAST_CODE).toList() + FIRST_CODE) { "codes ${codes.map { "%x".format(it) }}" }
    val superseded = accessRecords()
    check(superseded.size == 14 && superseded.all { it["outcome"] == "cancelled" }) { "superseded $superseded" }

    for (foreign in listOf(0x5DFFFF, 4711, 4303, LAST_CODE + 1, FIRST_CODE - 1)) {
        check(!FileBrowser.onRequestPermissionsResult(first, foreign, arrayOf(READ), intArrayOf(0))) {
            "code $foreign is not the browser's"
        }
    }
    FileBrowser.cancel(17)
    check(accessOutcome(17) == "cancelled") { "the last one cancelled" }
    FileBrowser.cancel(17)
    check(accessRecords().isEmpty()) { "a second cancel answers nothing" }
}

/** The context the app gives [FileBrowser.start], with the storage service and package a test arranges. */
private class BrowserContext(private val storage: StorageManager, private val files: File) : Context() {
    override val packageName = PKG

    override fun getExternalFilesDir(type: String?): File? = files

    @Suppress("UNCHECKED_CAST")
    override fun <T> getSystemService(serviceClass: Class<T>): T? =
        if (serviceClass == StorageManager::class.java) storage as T else null
}

@Suppress("UNCHECKED_CAST")
private fun refreshFlags() {
    // Through start() and the Android platform: the volume callback is the one it registered.
    Build.VERSION.SDK_INT = 36
    val storage = StorageManager()
    val usbDir = folder("usb").toFile()
    storage.volumes += StorageVolume(folder("emulated").toFile(), Environment.MEDIA_MOUNTED, false, true, null, "Phone")
    storage.volumes += StorageVolume(usbDir, Environment.MEDIA_MOUNTED, true, false, "6187-25A8", "USBSTICK")
    val context = BrowserContext(storage, folder("files").toFile())
    context.packageManager.requested = arrayOf(READ, MANAGE)
    Environment.externalStorageManager = true
    FileBrowser.install(null)
    FileBrowser.start(context)
    FileBrowser.start(context)
    check(storage.callbacks.size == 1) { "start registers one volume callback, once" }
    while (FileBrowser.pollRecord().isNotEmpty()) Unit

    check(!FileBrowser.takeAccessRefresh()) { "nothing to refresh at start" }
    FileBrowser.onResume()
    check(FileBrowser.takeAccessRefresh() && !FileBrowser.takeAccessRefresh()) { "a resume refreshes once" }
    val usbVolume = storage.volumes[1]
    storage.callbacks.single().onStateChanged(usbVolume)
    check(FileBrowser.takeAccessRefresh() && !FileBrowser.takeAccessRefresh()) { "the volume callback refreshes once" }
    check(FileBrowser.onRequestPermissionsResult(Activity(), FIRST_CODE, arrayOf(READ), intArrayOf(0)))
    check(FileBrowser.takeAccessRefresh() && !FileBrowser.takeAccessRefresh()) { "a permission result refreshes once" }
    FileBrowser.onResume()
    storage.callbacks.single().onStateChanged(usbVolume)
    check(FileBrowser.takeAccessRefresh() && !FileBrowser.takeAccessRefresh()) { "both drain in one take" }

    // The Android platform maps the context: the declared permission, the grant and the volumes.
    val state = accessState()
    check(state["declared"] == true && state["granted"] == true && state["route"] == "settings" &&
        state["apiLevel"] == 36L) { "Android platform access state $state" }
    val (_, listed) = roots()
    check(states(listed) == listOf("inbox ready", "saved ready", "storage ready", "removable:6187-25A8 ready")) {
        "Android platform roots ${states(listed)}"
    }
    val removable = listed[3]
    check(removable["title"] == "USBSTICK" && removable["path"] == usbDir.path && removable["detail"] == "mounted") {
        "Android platform removable root $removable"
    }
    Environment.externalStorageManager = false
    FileBrowser.install(null)
    check(FileBrowser.accessState() == "{\"declared\":false,\"granted\":false,\"route\":\"none\",\"apiLevel\":0," +
        "\"rootsGeneration\":0}") { "not started: ${FileBrowser.accessState()}" }
    check(!FileBrowser.listRoots(1, 1) && !FileBrowser.requestAccess(Activity(), 2)) { "not started: refused" }
    check(FileBrowser.pollRecord().isEmpty()) { "and nothing answered" }
    Build.VERSION.SDK_INT = 30
}

private fun androidPlatformOn29() {
    // On Android 10 the one volume comes from StorageManager's primary volume (its state and description) and the path
    // Environment keeps; the READ grant is the access.
    Build.VERSION.SDK_INT = 29
    val storage = StorageManager()
    storage.volumes += StorageVolume(null, Environment.MEDIA_MOUNTED, false, true, null, "Internal shared storage")
    val context = BrowserContext(storage, folder("files").toFile())
    context.packageManager.requested = arrayOf(READ)
    context.grantedPermissions += READ
    FileBrowser.install(null)
    FileBrowser.start(context)
    check(storage.callbacks.isEmpty()) { "no volume callback before API 30" }
    val state = accessState()
    check(state["declared"] == true && state["granted"] == true && state["route"] == "permission" &&
        state["apiLevel"] == 29L) { "API 29 access state $state" }
    val storageRoot = roots().second.single { it["kind"] == "storage" }
    check(storageRoot == mapOf("key" to "storage", "kind" to "storage", "title" to "Internal shared storage",
        "path" to "/storage/emulated/0", "state" to "ready", "detail" to "mounted")) { "API 29 storage $storageRoot" }
    storage.volumes[0] = StorageVolume(null, Environment.MEDIA_UNMOUNTED, false, true, null, "Internal shared storage")
    check(roots().second.single { it["kind"] == "storage" }["state"] == "removed") { "API 29 shared storage away" }
    FileBrowser.install(null)
    Build.VERSION.SDK_INT = 30
}

@Suppress("UNCHECKED_CAST")
private fun recordJson() {
    install(FakePlatform(36).apply {
        declaredPermissions += MANAGE
        volumeList += primary()
    })
    check(FileBrowser.listRoots(1, 1))
    val roots = awaitRecord()
    check(roots.keys.toList() == listOf("id", "kind", "status", "generation", "roots")) { "roots keys ${roots.keys}" }
    for (root in roots["roots"] as List<Map<String, Any?>>) {
        check(root.keys.toList() == listOf("key", "kind", "title", "path", "state", "detail")) { "root keys ${root.keys}" }
    }
    val generation = roots["generation"] as Long
    val inbox = Paths.get((roots["roots"] as List<Map<String, Any?>>)[0]["path"] as String)
    Files.write(inbox.resolve("say \"hi\" \\ there.csv"), byteArrayOf(1, 2, 3))

    check(FileBrowser.listFolder(2, 1, "inbox", generation, "", "csv", false))
    val listing = awaitRecord()
    check(listing.keys.toList() == listOf("id", "kind", "status", "snapshot", "offset", "entries", "more",
        "hiddenByFilter", "hiddenByName")) { "listing keys ${listing.keys}" }
    val entry = (listing["entries"] as List<Map<String, Any?>>).single()
    check(entry.keys.toList() == listOf("name", "path", "reference", "dir", "size", "modifiedMs")) { "entry ${entry.keys}" }
    check(entry["name"] == "say \"hi\" \\ there.csv" && entry["size"] == 3L && entry["dir"] == false) { "entry $entry" }

    check(FileBrowser.listMore(3, 1, (listing["snapshot"] as Long) + 1, 0))
    val failed = awaitRecord()
    check(failed.keys.toList() == listOf("id", "kind", "status")) { "a non-ok listing ${failed.keys}" }

    check(FileBrowser.checkHandoff(4, 1, "inbox", generation, entry["reference"] as String))
    val handoff = awaitRecord()
    check(handoff.keys.toList() == listOf("id", "kind", "ok", "reference", "name", "reason")) { "handoff ${handoff.keys}" }

    check(FileBrowser.requestAccess(activity(), 5))
    val access = accessRecords().single()
    check(access.keys.toList() == listOf("id", "kind", "outcome") && access["kind"] == "access") { "access $access" }
}

fun main() {
    rootsByApiAndGrant()
    rootStatePrecedence()
    rootsGenerationMoves()
    gidAtStart()
    savedQuery()
    savedQueryTimeout()
    savedHandoff()
    folderRequests()
    accessChain()
    appInfoRouteOn30to32()
    permission29()
    reentrantPermissionAnswer()
    lateCallbackOfCancelledLaunch()
    refreshFlags()
    androidPlatformOn29()
    recordJson()
    FileBrowser.install(null)
    Files.walk(scratch).use { paths -> paths.sorted(Comparator.reverseOrder()).forEach(Files::delete) }
    println("PASS: file browser roots, Saved by DSD-neo, storage access and records")
}
