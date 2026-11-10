// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.Manifest
import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.ContentUris
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.os.CancellationSignal
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.os.storage.StorageManager
import android.os.storage.StorageVolume
import android.provider.MediaStore
import android.provider.Settings
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.URI
import java.nio.file.Path
import java.nio.file.Paths
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicBoolean

/**
 * One storage volume as the platform reports it. [directory] is null unless it is mounted; [state] is its
 * `Environment.MEDIA_*` state.
 */
internal data class BrowserVolume(
    val directory: String?,
    val state: String,
    val primary: Boolean,
    val removable: Boolean,
    val uuid: String?,
    val description: String?,
)

/** A root the browser offers, as its `roots` record lists it. */
internal data class BrowserRoot(
    val key: String,
    val kind: String,
    val title: String,
    val path: String,
    val state: String,
    val detail: String,
)

/** What the browser asks of Android: [AndroidBrowserPlatform] in the app, a fake in the JVM tests. */
internal interface BrowserPlatform {
    val apiLevel: Int
    val packageName: String

    /** The storage volumes (on API 29 the primary only). A StorageManager call, never a filesystem one. */
    fun volumes(): List<BrowserVolume>

    /** `getExternalFilesDir(null)`, which creates the folder: a filesystem call. Null while shared storage is away. */
    fun externalFilesDir(): File?

    /** Whether the manifest requests [permission] on this API level. */
    fun declares(permission: String): Boolean

    /** All files access (API 30+). */
    fun allFilesAccess(): Boolean

    /** Whether the app holds the runtime [permission]. */
    fun holds(permission: String): Boolean

    /** `/proc/self/status`, or null when it cannot be read. */
    fun groupsText(): String?

    /** Call [onChange] whenever a volume changes state (API 30+; no MEDIA_* broadcast covers USB). */
    fun watchVolumes(onChange: () -> Unit)

    /** A MediaStore query; may block, and throws OperationCanceledException once [signal] is cancelled. */
    fun query(
        uri: Uri,
        projection: Array<String>,
        selection: String,
        args: Array<String>,
        signal: CancellationSignal,
    ): Cursor?
}

/** The browser's platform through the application [context]. */
internal class AndroidBrowserPlatform(private val context: Context) : BrowserPlatform {
    private val storage: StorageManager? = context.getSystemService(StorageManager::class.java)

    // The manifest's requests never change while the process lives.
    private val requested: Set<String> by lazy {
        try {
            @Suppress("DEPRECATION")
            val info = context.packageManager.getPackageInfo(context.packageName, PackageManager.GET_PERMISSIONS)
            info.requestedPermissions?.toSet().orEmpty()
        } catch (_: Exception) {
            emptySet()
        }
    }

    override val apiLevel: Int = Build.VERSION.SDK_INT
    override val packageName: String = context.packageName

    override fun volumes(): List<BrowserVolume> {
        if (Build.VERSION.SDK_INT < 30) {
            // StorageVolume.getDirectory() is API 30; shared storage is the one volume the browser offers on 29.
            @Suppress("DEPRECATION")
            val path = Environment.getExternalStorageDirectory().path
            return listOf(BrowserVolume(path, Environment.getExternalStorageState(), true, false, null, null))
        }
        return storage?.storageVolumes.orEmpty().map {
            val description = it.getDescription(context)
            BrowserVolume(it.directory?.path, it.state, it.isPrimary, it.isRemovable, it.uuid, description)
        }
    }

    override fun externalFilesDir(): File? = context.getExternalFilesDir(null)

    override fun declares(permission: String) = permission in requested

    override fun allFilesAccess() = Build.VERSION.SDK_INT >= 30 && Environment.isExternalStorageManager()

    override fun holds(permission: String) =
        context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    override fun groupsText(): String? = try {
        File("/proc/self/status").readText()
    } catch (_: IOException) {
        null
    }

    override fun watchVolumes(onChange: () -> Unit) {
        if (Build.VERSION.SDK_INT < 30) return
        val callback = object : StorageManager.StorageVolumeCallback() {
            override fun onStateChanged(volume: StorageVolume) = onChange()
        }
        // The callback only raises a flag, so it runs on whatever thread delivers it.
        storage?.registerStorageVolumeCallback(Executor { it.run() }, callback)
    }

    override fun query(
        uri: Uri,
        projection: Array<String>,
        selection: String,
        args: Array<String>,
        signal: CancellationSignal,
    ): Cursor? = context.contentResolver.query(uri, projection, selection, args, null, signal)
}

/**
 * The in-app file browser's Android side (#830): the roots and their states, Saved by DSD-neo through MediaStore,
 * storage access, and the records the host polls. [FileListing] does the folder reads. See [FileBrowser].
 */
internal class BrowserCore(
    private val platform: BrowserPlatform,
    private val listing: FileListing,
    private val fs: BrowserFs = NioFs(),
) {
    /** A Saved query's signal, cancelled with its request: a provider call never sees the request's flag. */
    private class Query(val session: Long, val kind: String, val signal: CancellationSignal)

    private class AccessRequest(val id: Long, val code: Int)

    private val main = Handler(Looper.getMainLooper())

    // A process gets gid 1077 (external_storage) only when it starts holding All files access; it keeps the groups it
    // started with.
    private val gidAtStart = hasStorageGid(platform.groupsText())
    private val refreshDue = AtomicBoolean()
    private val volumesChanged = AtomicBoolean()

    // Guarded by this object's monitor.
    private var fingerprint: String? = null
    private var generation = 0L
    private var rootPaths: Map<String, String> = emptyMap()
    private var rootPathsGeneration = -1L
    private val queries = HashMap<Long, Query>()

    // The main looper's.
    private var access: AccessRequest? = null
    private var nextCode = FIRST_PERMISSION_CODE

    // Answers to access requests not yet polled; locked on itself.
    private val accessRecords = ArrayDeque<String>()

    init {
        platform.watchVolumes { volumesChanged.set(true) }
    }

    fun onResume() = refreshDue.set(true)

    /** Both flags drained in one take. */
    fun takeAccessRefresh() = refreshDue.getAndSet(false) or volumesChanged.getAndSet(false)

    fun accessState(): String {
        val declared = declared()
        val granted = granted()
        return JSONObject()
            .put("declared", declared)
            .put("granted", granted)
            .put("route", route(declared))
            .put("apiLevel", platform.apiLevel)
            .put("rootsGeneration", generationOf(granted, platform.volumes()))
            .toString()
    }

    fun listRoots(id: Long, session: Long): Boolean {
        listing.submit(id, session, FileListing.ROOTS) { readRoots() }
        return true
    }

    fun listFolder(
        id: Long,
        session: Long,
        rootKey: String,
        generation: Long,
        path: String,
        kind: String,
        showAll: Boolean,
    ): Boolean {
        val rootPath = rootPath(rootKey, generation)
        if (rootKey == SAVED) {
            listing.listEntries(id, session, kind, showAll, rootPath != null) { cancelled ->
                querySaved(id, session, cancelled)
            }
        } else if (rootPath.isNullOrEmpty()) {
            listing.listEntries(id, session, kind, showAll, generationOk = false) { null }
        } else {
            val root = pathOf(rootPath) ?: return false
            val dir = if (path.isEmpty()) root else pathOf(path) ?: return false
            listing.listFolder(id, session, root, dir, kind, showAll, generationOk = true)
        }
        cancelOtherListings(session, id)
        return true
    }

    fun listMore(id: Long, session: Long, snapshot: Long, offset: Int): Boolean {
        listing.listMore(id, session, snapshot, offset)
        cancelOtherListings(session, id)
        return true
    }

    fun checkHandoff(id: Long, session: Long, rootKey: String, generation: Long, reference: String): Boolean {
        val rootPath = rootPath(rootKey, generation)
        if (rootKey == SAVED) {
            val row = savedRow(reference) ?: return false
            listing.submit(id, session, FileListing.HANDOFF) { cancelled ->
                if (rootPath == null) {
                    FileListing.failureFields(FileListing.HANDOFF, FileListing.STALE)
                } else {
                    checkSaved(id, session, row, cancelled)
                }
            }
            return true
        }
        val file = fileOf(reference) ?: return false
        val root = rootPath?.takeIf { it.isNotEmpty() }?.let { pathOf(it) ?: return false }
        listing.checkFile(id, session, root ?: file, file, generationOk = root != null)
        return true
    }

    fun requestAccess(activity: Activity, id: Long): Boolean {
        main.post { startAccess(activity, id) }
        return true
    }

    /** Answer request [id] `cancelled`, whichever kind it is, and cancel its query; other ids are ignored. */
    fun cancel(id: Long) {
        listing.cancel(id)
        cancelQueries { requestId, _ -> requestId == id }
        main.post { access?.takeIf { it.id == id }?.let { settleAccess(it, CANCELLED) } }
    }

    fun closeSession(session: Long) {
        listing.closeSession(session)
        cancelQueries { _, query -> query.session == session }
    }

    fun pollRecord(): String {
        listing.poll()?.let { return recordJson(it) }
        return synchronized(accessRecords) { accessRecords.removeFirstOrNull() } ?: ""
    }

    /** A permission result for one of the browser's codes, on the main thread. */
    fun onPermissionResult(activity: Activity, code: Int, permissions: Array<out String>, grants: IntArray) {
        refreshDue.set(true)
        // A late answer to a cancelled or replaced launch has no request with its code.
        val request = access?.takeIf { it.code == code } ?: return
        val outcome = when {
            // An interrupted dialog answers with no arrays; Android says to treat that as a cancellation.
            permissions.isEmpty() || grants.isEmpty() -> CANCELLED
            grants[0] == PackageManager.PERMISSION_GRANTED -> "granted"
            activity.shouldShowRequestPermissionRationale(Manifest.permission.READ_EXTERNAL_STORAGE) -> "denied"
            else -> {
                // "Don't ask again": only App info can grant it now.
                openAppInfo(activity)
                "blocked"
            }
        }
        settleAccess(request, outcome)
    }

    private fun declared() = platform.declares(broadPermission())

    private fun granted() = if (platform.apiLevel >= 30) {
        platform.allFilesAccess()
    } else {
        platform.holds(Manifest.permission.READ_EXTERNAL_STORAGE)
    }

    /** All files access from Android 11; before that the legacy view of shared storage, through READ. */
    private fun broadPermission() = if (platform.apiLevel >= 30) {
        Manifest.permission.MANAGE_EXTERNAL_STORAGE
    } else {
        Manifest.permission.READ_EXTERNAL_STORAGE
    }

    private fun route(declared: Boolean) = when {
        !declared -> "none"
        platform.apiLevel >= 30 -> "settings"
        else -> "permission"
    }

    /** The roots' generation: it moves when the grant, the volume set or a volume's state does. No filesystem call. */
    private fun generationOf(granted: Boolean, volumes: List<BrowserVolume>): Long {
        val print = buildString {
            append(granted)
            for (volume in volumes) {
                append('|').append(volume.uuid).append(',').append(volume.directory).append(',').append(volume.state)
                append(',').append(volume.primary).append(',').append(volume.removable)
            }
        }
        synchronized(this) {
            if (print != fingerprint) {
                fingerprint = print
                generation++
            }
            return generation
        }
    }

    /** The path of [key] in the last roots listed, or null when they are not at [generation] any more. */
    private fun rootPath(key: String, generation: Long): String? {
        val current = generationOf(granted(), platform.volumes())
        synchronized(this) {
            if (generation != current || generation != rootPathsGeneration) return null
            return rootPaths[key]
        }
    }

    /** On a worker: the roots, read at the generation they carry. */
    private fun readRoots(): Map<String, Any?> {
        val granted = granted()
        val volumes = platform.volumes()
        val generation = generationOf(granted, volumes)
        val roots = ArrayList<BrowserRoot>()
        roots += inboxRoot()
        roots += BrowserRoot(SAVED, SAVED, "", "", READY, "")
        if (declared()) roots += broadRoots(volumes, granted)
        synchronized(this) {
            rootPaths = roots.associate { it.key to it.path }
            rootPathsGeneration = generation
        }
        return mapOf(FileListing.STATUS to FileListing.OK, "generation" to generation, "roots" to roots)
    }

    /** `<external files>/Inbox`, made if missing: files copied there over USB or adb need no permission to read. */
    private fun inboxRoot(): BrowserRoot {
        val files = try {
            platform.externalFilesDir()
        } catch (_: Exception) {
            null
        } ?: return BrowserRoot(INBOX, INBOX, "", "", UNAVAILABLE, "")
        val inbox = File(files, "Inbox")
        val ready = try {
            inbox.mkdirs() || inbox.isDirectory
        } catch (_: SecurityException) {
            false
        }
        return BrowserRoot(INBOX, INBOX, "", inbox.path, if (ready) READY else UNAVAILABLE, "")
    }

    /** Shared storage, then each removable volume (API 30+; USB stays with Android's picker on 29). */
    private fun broadRoots(volumes: List<BrowserVolume>, granted: Boolean): List<BrowserRoot> {
        val storage = volumes.filter { it.primary }.map { broadRoot(STORAGE, STORAGE, it, granted) }
        if (platform.apiLevel < 30) return storage
        val removable = volumes.withIndex().filter { (_, volume) -> !volume.primary && volume.removable }
            .map { (index, volume) -> broadRoot("$REMOVABLE:${volume.uuid ?: index}", REMOVABLE, volume, granted) }
        return storage + removable
    }

    private fun broadRoot(key: String, kind: String, volume: BrowserVolume, granted: Boolean): BrowserRoot {
        val state = rootState(volume, granted)
        return BrowserRoot(key, kind, volume.description ?: "", volume.directory ?: "", state, volume.state)
    }

    /**
     * Unmounted (no directory) is `removed`; without access, `needsAccess`; readable, `ready`. An invisible USB volume
     * is readable only by a process that started with All files access, and until then its root does not even exist,
     * like a removed drive: granted but started without gid 1077, it is `needsRestart`. Anything else, `unavailable`.
     */
    private fun rootState(volume: BrowserVolume, granted: Boolean): String {
        val dir = volume.directory
        val mounted = volume.state == Environment.MEDIA_MOUNTED || volume.state == Environment.MEDIA_MOUNTED_READ_ONLY
        return when {
            dir == null || !mounted -> REMOVED
            !granted -> NEEDS_ACCESS
            readable(dir) -> READY
            dir.startsWith(INVISIBLE_VOLUMES) && !gidAtStart -> NEEDS_RESTART
            else -> UNAVAILABLE
        }
    }

    private fun readable(dir: String) = try {
        fs.open(Paths.get(dir)).close()
        true
    } catch (_: Exception) {
        false
    }

    /** On a worker: the app's own rows in Download/DSD-neo; null when MediaStore did not answer. */
    private fun querySaved(id: Long, session: Long, cancelled: () -> Boolean): List<ListEntry>? {
        val signal = beginQuery(id, session, FileListing.LISTING, cancelled)
        try {
            val cursor = platform.query(DOWNLOADS_URI, SAVED_PROJECTION, SAVED_SELECTION, savedArgs(), signal)
                ?: return null
            return cursor.use { rows ->
                val idColumn = rows.getColumnIndex(MediaStore.MediaColumns._ID)
                val nameColumn = rows.getColumnIndex(MediaStore.MediaColumns.DISPLAY_NAME)
                val sizeColumn = rows.getColumnIndex(MediaStore.MediaColumns.SIZE)
                val modifiedColumn = rows.getColumnIndex(MediaStore.MediaColumns.DATE_MODIFIED)
                val entries = ArrayList<ListEntry>()
                // One row past the listing's cap is enough to answer tooMany.
                while (entries.size <= SAVED_ROW_LIMIT && !cancelled() && rows.moveToNext()) {
                    val name = if (rows.isNull(nameColumn)) continue else rows.getString(nameColumn) ?: continue
                    val size = if (rows.isNull(sizeColumn)) 0L else rows.getLong(sizeColumn)
                    // DATE_MODIFIED is in seconds.
                    val modified = if (rows.isNull(modifiedColumn)) 0L else rows.getLong(modifiedColumn) * 1000
                    entries += ListEntry(name, "", savedReference(rows.getLong(idColumn)), false, size, modified)
                }
                entries
            }
        } finally {
            endQuery(id, signal)
        }
    }

    /** On a worker: row [row] again, by id and with the owner predicate, so only the app's own row is handed off. */
    private fun checkSaved(id: Long, session: Long, row: Long, cancelled: () -> Boolean): Map<String, Any?> {
        val signal = beginQuery(id, session, FileListing.HANDOFF, cancelled)
        try {
            val args = arrayOf(row.toString(), *savedArgs())
            val cursor = platform.query(DOWNLOADS_URI, HANDOFF_PROJECTION, HANDOFF_SELECTION, args, signal)
                ?: return FileListing.failureFields(FileListing.HANDOFF, FileListing.FAILED)
            val name = cursor.use { rows ->
                val nameColumn = rows.getColumnIndex(MediaStore.MediaColumns.DISPLAY_NAME)
                if (rows.moveToNext() && !rows.isNull(nameColumn)) rows.getString(nameColumn) else null
            } ?: return FileListing.failureFields(FileListing.HANDOFF, FileListing.GONE)
            return mapOf("ok" to true, "reference" to savedReference(row), "name" to name, "reason" to "")
        } finally {
            endQuery(id, signal)
        }
    }

    private fun savedArgs() = arrayOf(SAVED_FOLDER, platform.packageName)

    /** Registered before the query starts; a request already answered gets a cancelled signal. */
    private fun beginQuery(id: Long, session: Long, kind: String, cancelled: () -> Boolean): CancellationSignal {
        val signal = CancellationSignal()
        synchronized(this) { queries[id] = Query(session, kind, signal) }
        if (cancelled()) signal.cancel()
        return signal
    }

    private fun endQuery(id: Long, signal: CancellationSignal) {
        synchronized(this) { if (queries[id]?.signal === signal) queries.remove(id) }
    }

    /** After a listing or page in [session] superseded the others: cancel their Saved queries. */
    private fun cancelOtherListings(session: Long, id: Long) = cancelQueries { requestId, query ->
        requestId != id && query.session == session && query.kind == FileListing.LISTING
    }

    /**
     * Cancels the signals of the queries that [match]; called after [FileListing] answered their requests, so the
     * exception a cancelled query throws only ends a worker whose answer is already dropped.
     */
    private fun cancelQueries(match: (Long, Query) -> Boolean) {
        val signals = synchronized(this) { queries.filter { (id, query) -> match(id, query) }.values.map { it.signal } }
        // Outside the lock: a cancel calls into the provider.
        signals.forEach { it.cancel() }
    }

    private fun startAccess(activity: Activity, id: Long) {
        access?.let { settleAccess(it, CANCELLED) }
        when {
            !declared() -> addAccessRecord(id, NO_ROUTE)
            platform.apiLevel >= 30 -> addAccessRecord(id, openAllFilesSettings(activity))
            else -> askForRead(activity, id)
        }
    }

    /**
     * The app's own All files access page, else the list of apps; plain launches, as Android sends no result. Android
     * 11 and 12 also offer "Allow management of all files" in App info › Permissions › Files and media when the app
     * requests a storage permission too (the sideload manifest's READ_EXTERNAL_STORAGE up to API 32), so a head unit
     * without either Settings screen still has a way.
     */
    private fun openAllFilesSettings(activity: Activity): String {
        val app = Uri.parse("package:${platform.packageName}")
        val launches = mutableListOf(
            Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, app),
            Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
        )
        if (platform.apiLevel <= 32) launches += Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, app)
        for (intent in launches) {
            try {
                activity.startActivity(intent)
                return "settingsOpened"
            } catch (_: ActivityNotFoundException) {
                continue
            } catch (_: Exception) {
                return FAILED
            }
        }
        return NO_ROUTE
    }

    /** Android 10: the runtime dialog, with a code of its own, so a late answer to an older launch settles nothing. */
    private fun askForRead(activity: Activity, id: Long) {
        val code = nextCode
        nextCode = if (code == LAST_PERMISSION_CODE) FIRST_PERMISSION_CODE else code + 1
        val request = AccessRequest(id, code)
        access = request
        try {
            activity.requestPermissions(arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE), code)
        } catch (_: Exception) {
            settleAccess(request, FAILED)
        }
    }

    private fun openAppInfo(activity: Activity) {
        try {
            activity.startActivity(
                Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${platform.packageName}")),
            )
        } catch (_: Exception) {
            // The answer stands; the browser's text says where to allow it.
        }
    }

    private fun settleAccess(request: AccessRequest, outcome: String) {
        if (access === request) access = null
        addAccessRecord(request.id, outcome)
    }

    private fun addAccessRecord(id: Long, outcome: String) {
        val record = JSONObject().put("id", id).put("kind", "access").put("outcome", outcome).toString()
        synchronized(accessRecords) { accessRecords.addLast(record) }
    }

    internal companion object {
        const val FIRST_PERMISSION_CODE = 0x5E00
        const val LAST_PERMISSION_CODE = 0x5E0F

        private const val INBOX = "inbox"
        private const val SAVED = "saved"
        private const val STORAGE = "storage"
        private const val REMOVABLE = "removable"

        private const val READY = "ready"
        private const val NEEDS_ACCESS = "needsAccess"
        private const val NEEDS_RESTART = "needsRestart"
        private const val REMOVED = "removed"
        private const val UNAVAILABLE = "unavailable"

        private const val CANCELLED = "cancelled"
        private const val FAILED = "failed"
        private const val NO_ROUTE = "noRoute"

        /** Where Android mounts a volume that apps do not see through /storage, such as a USB drive. */
        private const val INVISIBLE_VOLUMES = "/mnt/media_rw/"

        // The trailing slash matters: MediaStore compares RELATIVE_PATH as stored.
        private const val SAVED_FOLDER = "Download/DSD-neo/"
        private const val SAVED_SELECTION = "relative_path = ? AND owner_package_name = ?"
        private const val HANDOFF_SELECTION = "_id = ? AND relative_path = ? AND owner_package_name = ?"
        private const val SAVED_ROW_LIMIT = 50_000

        private val DOWNLOADS_URI: Uri = MediaStore.Downloads.EXTERNAL_CONTENT_URI
        private val SAVED_PROJECTION = arrayOf(
            MediaStore.MediaColumns._ID,
            MediaStore.MediaColumns.DISPLAY_NAME,
            MediaStore.MediaColumns.SIZE,
            MediaStore.MediaColumns.DATE_MODIFIED,
        )
        private val HANDOFF_PROJECTION = arrayOf(MediaStore.MediaColumns._ID, MediaStore.MediaColumns.DISPLAY_NAME)

        fun ownsPermissionCode(code: Int) = code in FIRST_PERMISSION_CODE..LAST_PERMISSION_CODE

        private fun savedReference(row: Long) = ContentUris.withAppendedId(DOWNLOADS_URI, row).toString()

        /** The row id of a Saved by DSD-neo reference, or null for anything else. */
        private fun savedRow(reference: String): Long? {
            val digits = reference.removePrefix("$DOWNLOADS_URI/")
            if (digits == reference || digits.isEmpty() || !digits.all { it in '0'..'9' }) return null
            return digits.toLongOrNull()
        }

        /** The absolute path a `file:` reference names, decoded once, or null. */
        private fun fileOf(reference: String): Path? = try {
            val uri = URI(reference)
            if (uri.scheme == "file") Paths.get(uri) else null
        } catch (_: Exception) {
            null
        }

        private fun pathOf(text: String): Path? = try {
            Paths.get(text).takeIf { it.isAbsolute }
        } catch (_: Exception) {
            null
        }

        private fun recordJson(record: BrowserRecord): String {
            val json = JSONObject().put("id", record.id).put("kind", record.kind)
            for ((key, value) in record.fields) json.put(key, jsonValue(value))
            return json.toString()
        }

        private fun jsonValue(value: Any?): Any = when (value) {
            is ListEntry -> JSONObject()
                .put("name", value.name)
                .put("path", value.path)
                .put("reference", value.reference)
                .put("dir", value.dir)
                .put("size", value.size)
                .put("modifiedMs", value.modifiedMs)
            is BrowserRoot -> JSONObject()
                .put("key", value.key)
                .put("kind", value.kind)
                .put("title", value.title)
                .put("path", value.path)
                .put("state", value.state)
                .put("detail", value.detail)
            is List<*> -> JSONArray().also { array -> value.forEach { array.put(jsonValue(it)) } }
            null -> ""
            else -> value
        }
    }
}

/** Whether the `Groups:` line of a `/proc/self/status` [status] lists gid 1077 (external_storage). */
internal fun hasStorageGid(status: String?): Boolean {
    val groups = status?.lineSequence()?.firstOrNull { it.startsWith("Groups:") } ?: return false
    return groups.removePrefix("Groups:").trim().split(Regex("\\s+")).contains("1077")
}

/**
 * The in-app file browser's Android side (#830), for DecoderHostAndroid: it starts the browser, asks for roots,
 * listings, pages, hand-off checks and storage access from the Qt thread, and polls each answer as one JSON record with
 * [pollRecord] on its refresh. Every request is answered exactly once, never inside the call.
 *
 * - Roots: the inbox (`<external files>/Inbox`, made here; no permission), Saved by DSD-neo (the app's own MediaStore
 *   rows in Download/DSD-neo, selected by folder and owner, since a manager or an Android 10 legacy reader sees other
 *   apps' rows too), and, when the manifest requests broad access, shared storage and each removable volume (API 30+).
 *   A root's state: unmounted `removed`, without access `needsAccess`, readable `ready`, an invisible USB volume in a
 *   process started without gid 1077 `needsRestart`, else `unavailable`. Their generation moves with the grant, the
 *   volume set and each volume's state, and is read with no filesystem call; a request naming an older one is `stale`.
 * - Folder reads, pages and hand-off checks go through [FileListing]'s worker threads. A Saved query has a
 *   CancellationSignal that a cancel, a session close or a newer listing in the session cancels.
 * - Access, on the main looper: API 30+ opens the app's All files access page, else the all-apps list, else (API
 *   30-32) App info, as plain launches answered at once (`settingsOpened`, `noRoute` or `failed`); the grant shows on
 *   resume. API 29 asks for READ_EXTERNAL_STORAGE with a code from `0x5E00..0x5E0F`, one per launch, and stays live
 *   until the result: `granted`, `denied`, `blocked` (App info opened) or `cancelled`. [onRequestPermissionsResult]
 *   claims the whole range, so a late answer to a cancelled launch is dropped, never another caller's.
 * - [takeAccessRefresh] is true once after a resume, a volume change or a permission result: the host then reads
 *   [accessState] again.
 */
object FileBrowser {
    @Volatile
    private var core: BrowserCore? = null

    private const val NOT_STARTED =
        "{\"declared\":false,\"granted\":false,\"route\":\"none\",\"apiLevel\":0,\"rootsGeneration\":0}"

    /** Once, from the host's constructor: registers the volume callback and reads the process's groups. */
    @JvmStatic fun start(context: Context) {
        synchronized(this) {
            if (core != null) return
            val listing = FileListing(nowMs = SystemClock::uptimeMillis)
            core = BrowserCore(AndroidBrowserPlatform(context.applicationContext), listing)
        }
    }

    /** For the tests: a browser over a fake platform, or none. */
    internal fun install(browser: BrowserCore?) {
        core = browser
    }

    /** `{declared, granted, route, apiLevel, rootsGeneration}`. */
    @JvmStatic fun accessState(): String = core?.accessState() ?: NOT_STARTED

    @JvmStatic fun takeAccessRefresh(): Boolean = core?.takeAccessRefresh() ?: false

    @JvmStatic fun listRoots(id: Long, session: Long): Boolean = core?.listRoots(id, session) ?: false

    @JvmStatic fun listFolder(
        id: Long,
        session: Long,
        rootKey: String,
        generation: Long,
        path: String,
        kind: String,
        showAll: Boolean,
    ): Boolean = core?.listFolder(id, session, rootKey, generation, path, kind, showAll) ?: false

    @JvmStatic fun listMore(id: Long, session: Long, snapshot: Long, offset: Int): Boolean =
        core?.listMore(id, session, snapshot, offset) ?: false

    @JvmStatic fun checkHandoff(
        id: Long,
        session: Long,
        rootKey: String,
        generation: Long,
        reference: String,
    ): Boolean = core?.checkHandoff(id, session, rootKey, generation, reference) ?: false

    @JvmStatic fun requestAccess(activity: Activity, id: Long): Boolean = core?.requestAccess(activity, id) ?: false

    @JvmStatic fun cancel(id: Long) {
        core?.cancel(id)
    }

    @JvmStatic fun closeSession(session: Long) {
        core?.closeSession(session)
    }

    /** One record, or "" when none waits. From any thread. */
    @JvmStatic fun pollRecord(): String = core?.pollRecord() ?: ""

    /** From DsdNeoActivity.onResume(): All files access and the volumes may have changed while the app was away. */
    fun onResume() {
        core?.onResume()
    }

    /** From DsdNeoActivity first: true for every code of the browser's range, which nothing else may handle. */
    fun onRequestPermissionsResult(
        activity: Activity,
        code: Int,
        permissions: Array<out String>,
        grants: IntArray,
    ): Boolean {
        if (!BrowserCore.ownsPermissionCode(code)) return false
        core?.onPermissionResult(activity, code, permissions, grants)
        return true
    }
}
