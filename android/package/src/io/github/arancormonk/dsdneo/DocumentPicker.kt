// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.ComponentName
import android.content.ContentResolver
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.CancellationSignal
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit

/**
 * Where a save goes when no app takes CREATE_DOCUMENT: a pending row in the Download folder (#829).
 *
 * [insert] starts making a row named `name` of type `mime` off the main thread and hands its URI, or "" when it could
 * not, to `onDone` once, from any thread. [discard] deletes a row [insert] made that nothing will write; it never
 * blocks.
 */
internal interface DownloadsFallback {
    fun insert(name: String, mime: String, onDone: (String) -> Unit)
    fun discard(uri: String)
}

/** No Download-folder save: every insert fails, so a save with no save picker ends unavailable("noSavePicker"). */
internal object NoDownloadsFallback : DownloadsFallback {
    override fun insert(name: String, mime: String, onDone: (String) -> Unit) = onDone("")
    override fun discard(uri: String) {}
}

/** One activity launch the picker makes; [intentFor] builds its Intent. */
internal sealed class PickerLaunch {
    object Open : PickerLaunch()
    class Create(val title: String, val mime: String) : PickerLaunch()
    /** The GET_CONTENT fallback straight to the one app that can answer it. */
    class GetContent(val component: ComponentName) : PickerLaunch()
    /** The GET_CONTENT fallback through the chooser, without the apps that pick only photos and videos. */
    class Chooser(val exclude: List<ComponentName>) : PickerLaunch()
}

private const val ANY_TYPE = "*/*"

/** A document's display name, asked of its provider on a worker thread; null when it has none. */
internal fun interface NameLookup {
    fun displayName(uri: String, signal: CancellationSignal): String?
}

/** What a request needs of the activity: [ActivityPickerHost] in the app, a fake in the JVM tests. */
internal interface PickerHost {
    /** Starts [launch] for a result answered with [code]; throws ActivityNotFoundException when no app takes it. */
    fun start(launch: PickerLaunch, code: Int)

    /** The activities that answer GET_CONTENT + OPENABLE for [type]. */
    fun getContentHandlers(type: String): Set<ComponentName>

    val names: NameLookup
}

/** The request codes the picker launches with, one per launch, wrapping inside a range no other caller uses. */
internal class LaunchCodes {
    private var next = FIRST

    fun next(): Int = next.also { next = if (it == LAST) FIRST else it + 1 }

    companion object {
        const val FIRST = 0x5D0000
        const val LAST = 0x5DFFFF

        fun owns(code: Int) = code in FIRST..LAST
    }
}

/**
 * The app's own document picker (#829): ACTION_OPEN_DOCUMENT, then a filtered GET_CONTENT; ACTION_CREATE_DOCUMENT,
 * then the Download folder. DecoderHostAndroid starts and cancels requests from the Qt thread and polls their answers
 * with [pollResult] on its refresh, like [LocationSupport]'s.
 *
 * Request state belongs to the main looper. A request is its id; while an activity it launched is out it also holds
 * that launch's request code, and only a result for that code settles it. A launch that finds no app gives its code up
 * at once: the platform still answers that code RESULT_CANCELED later, which must not end the fallback the request
 * went on to. A result with no request waiting (after process death, or for a request since replaced) is dropped. Each
 * request settles exactly once, into one record: `picked` (with `downloads` for a Download-folder row), `cancelled` or
 * `unavailable` with its reason. A newer request settles the one it replaces as cancelled; [cancel] ends one with no
 * record. No persistable grant is taken: the flows read or write while the activity that got the result lives.
 */
object DocumentPicker {
    private const val NAME_DEADLINE_MS = 2_000L
    private const val INSERT_DEADLINE_MS = 10_000L
    // A GET_CONTENT handler counts only if it takes one of these: backups and unknown extensions are application/*,
    // CSVs text/* or application/*, replay recordings audio/*. A "*/*" query also matches the system photo picker,
    // which offers images and videos only and, with no DocumentsUI to hand other types to, relaunches itself until
    // Android removes the app's task.
    private val FAMILIES = listOf("application/*", "text/*", "audio/*")

    private val main = Handler(Looper.getMainLooper())
    private val codes = LaunchCodes()
    private var active: Request? = null
    // The records not yet polled, oldest first; the one state shared with the Qt thread.
    private val records = ArrayDeque<String>()

    /** One thread for display-name lookups, so a provider that never answers delays only later names. */
    internal val lookups = ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, LinkedBlockingQueue()) { task ->
        Thread(task, "dsd-picker-names").apply { isDaemon = true }
    }
    internal var downloads: DownloadsFallback = NoDownloadsFallback

    private class Request(val id: Long, val host: PickerHost) {
        var launchCode: Int? = null
        var deadline: Runnable? = null
        var signal: CancellationSignal? = null
        var lookup: Runnable? = null
    }

    private enum class Launched { STARTED, NOT_FOUND, FAILED }

    /** Ask for a document to read. Always true, so a caller can tell a call that never reached here. */
    @JvmStatic fun open(activity: Activity, id: Long): Boolean {
        openWith(ActivityPickerHost(activity), id)
        return true
    }

    /** Ask where to write a new document named [title] of type [mime]. Always true, as [open]. */
    @JvmStatic fun create(activity: Activity, id: Long, title: String, mime: String): Boolean {
        createWith(ActivityPickerHost(activity), id, title, mime)
        return true
    }

    /** End request [id] with no record; a later answer for it is dropped. Other ids are ignored. */
    @JvmStatic fun cancel(id: Long) {
        main.post { active?.takeIf { it.id == id }?.let { retire(it) } }
    }

    /**
     * The oldest record not yet polled, or "". From any thread. Locked on the queue itself: a `@Synchronized
     * @JvmStatic` member of an object locks its class, not the object.
     */
    @JvmStatic fun pollResult(): String = synchronized(records) { records.removeFirstOrNull() } ?: ""

    /** True for every request code the picker launches with, which nothing else may handle. On the main thread. */
    @JvmStatic fun onActivityResult(activity: Activity, requestCode: Int, resultCode: Int, data: Intent?): Boolean {
        if (!LaunchCodes.owns(requestCode)) return false
        result(requestCode, resultCode == Activity.RESULT_OK, resultUri(data))
        return true
    }

    internal fun openWith(host: PickerHost, id: Long) {
        main.post {
            val request = begin(host, id)
            when (launch(request, PickerLaunch.Open)) {
                Launched.NOT_FOUND -> openFallback(request)
                Launched.FAILED -> finish(request, unavailable(id, "failed"))
                Launched.STARTED -> {}
            }
        }
    }

    internal fun createWith(host: PickerHost, id: Long, title: String, mime: String) {
        main.post {
            val request = begin(host, id)
            when (launch(request, PickerLaunch.Create(title, mime))) {
                Launched.NOT_FOUND -> saveToDownloads(request, title, mime)
                Launched.FAILED -> finish(request, unavailable(id, "failed"))
                Launched.STARTED -> {}
            }
        }
    }

    /** A result for [code]: [uri] is the document the picker returned, if any. */
    internal fun result(code: Int, ok: Boolean, uri: String?) {
        val request = active ?: return
        if (request.launchCode != code) return
        request.launchCode = null
        when {
            !ok -> finish(request, cancelled(request.id))
            uri == null -> finish(request, unavailable(request.id, "noResult"))
            !uri.substringBefore(':', "").equals("content", ignoreCase = true) ->
                finish(request, unavailable(request.id, "unreadable"))
            else -> lookUpName(request, uri)
        }
    }

    private fun begin(host: PickerHost, id: Long): Request {
        active?.let { finish(it, cancelled(it.id)) }
        return Request(id, host).also { active = it }
    }

    private fun launch(request: Request, launch: PickerLaunch): Launched {
        val code = codes.next()
        request.launchCode = code
        return try {
            request.host.start(launch, code)
            Launched.STARTED
        } catch (_: ActivityNotFoundException) {
            request.launchCode = null
            Launched.NOT_FOUND
        } catch (_: Exception) {
            request.launchCode = null
            Launched.FAILED
        }
    }

    /** No app takes OPEN_DOCUMENT: GET_CONTENT, through an app that takes the documents the flows read. */
    private fun openFallback(request: Request) {
        val fallback = try {
            val all = request.host.getContentHandlers(ANY_TYPE)
            val capable = FAMILIES.flatMapTo(LinkedHashSet()) { request.host.getContentHandlers(it) }
            when (capable.size) {
                0 -> null
                1 -> PickerLaunch.GetContent(capable.first())
                else -> PickerLaunch.Chooser(all.filterNot { it in capable })
            }
        } catch (_: Exception) {
            finish(request, unavailable(request.id, "failed"))
            return
        }
        val launched = if (fallback == null) Launched.NOT_FOUND else launch(request, fallback)
        when (launched) {
            Launched.NOT_FOUND -> finish(request, unavailable(request.id, "noPicker"))
            Launched.FAILED -> finish(request, unavailable(request.id, "failed"))
            Launched.STARTED -> {}
        }
    }

    /** No app takes CREATE_DOCUMENT: a row in the Download folder, if one comes before the deadline. */
    private fun saveToDownloads(request: Request, name: String, mime: String) {
        val fallback = downloads
        val id = request.id
        request.deadline = Runnable { finish(request, unavailable(id, "noSavePicker")) }
            .also { main.postDelayed(it, INSERT_DEADLINE_MS) }
        try {
            fallback.insert(name, mime) { uri ->
                main.post {
                    if (active === request) {
                        finish(request, if (uri.isEmpty()) unavailable(id, "noSavePicker") else picked(id, uri, name, true))
                    } else if (uri.isNotEmpty()) {
                        // Too late, or the request went: nothing will write the row.
                        fallback.discard(uri)
                    }
                }
            }
        } catch (_: Exception) {
            finish(request, unavailable(id, "noSavePicker"))
        }
    }

    /** The display name off the main thread; at the deadline, or when the provider gives none, the URI's last segment. */
    private fun lookUpName(request: Request, uri: String) {
        val id = request.id
        val signal = CancellationSignal()
        val names = request.host.names
        val task = Runnable {
            // A request retired while this waited in the queue never reaches its provider.
            if (signal.isCanceled) return@Runnable
            val name = try { names.displayName(uri, signal) } catch (_: Exception) { null }
            main.post {
                val live = active
                if (live != null && live.id == id && live.signal === signal) {
                    finish(live, picked(id, uri, name?.takeIf { it.isNotEmpty() } ?: lastSegmentName(uri), false))
                }
            }
        }
        request.signal = signal
        request.lookup = task
        request.deadline = Runnable {
            signal.cancel()
            finish(request, picked(id, uri, lastSegmentName(uri), false))
        }.also { main.postDelayed(it, NAME_DEADLINE_MS) }
        try {
            lookups.execute(task)
        } catch (_: RejectedExecutionException) {
            finish(request, picked(id, uri, lastSegmentName(uri), false))
        }
    }

    /** Stops everything [request] waits for; it is no longer the live request. */
    private fun retire(request: Request) {
        if (active === request) active = null
        request.launchCode = null
        request.deadline?.let { main.removeCallbacks(it) }
        request.deadline = null
        request.signal?.cancel()
        request.lookup?.let { lookups.remove(it) }
        request.lookup = null
    }

    private fun finish(request: Request, record: JSONObject) {
        if (active !== request) return
        retire(request)
        synchronized(records) { records.addLast(record.toString()) }
    }

    private fun picked(id: Long, uri: String, name: String, downloads: Boolean) =
        JSONObject().put("id", id).put("kind", "picked").put("uri", uri).put("name", name).put("downloads", downloads)

    private fun cancelled(id: Long) = JSONObject().put("id", id).put("kind", "cancelled")

    private fun unavailable(id: Long, reason: String) =
        JSONObject().put("id", id).put("kind", "unavailable").put("reason", reason)
}

/** The document a picker returned: its data URI, else the first ClipData item's (some pickers fill only that). */
internal fun resultUri(data: Intent?): String? =
    (data?.data ?: data?.clipData?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.uri)?.toString()

/** The URI's last path segment, decoded: the name shown when the provider gives none in time. */
internal fun lastSegmentName(uri: String): String {
    val path = uri.substringBefore('#').substringBefore('?')
    val hierarchical = path.substringAfter("://", "")
    val segments = if (hierarchical.isEmpty()) path.substringAfter(':') else hierarchical.substringAfter('/', "")
    return percentDecoded(segments.split('/').lastOrNull { it.isNotEmpty() } ?: "")
}

/** [text] with each %XX escape decoded as UTF-8; a malformed escape stays as it is. */
private fun percentDecoded(text: String): String {
    val bytes = ByteArrayOutputStream()
    var start = 0
    var index = 0
    while (index < text.length) {
        val high = if (text[index] == '%' && index + 2 < text.length) Character.digit(text[index + 1], 16) else -1
        val low = if (high >= 0) Character.digit(text[index + 2], 16) else -1
        if (low < 0) {
            index++
            continue
        }
        bytes.write(text.substring(start, index).toByteArray(Charsets.UTF_8))
        bytes.write(high * 16 + low)
        index += 3
        start = index
    }
    bytes.write(text.substring(start).toByteArray(Charsets.UTF_8))
    return String(bytes.toByteArray(), Charsets.UTF_8)
}

/** The Intent for each launch. */
internal fun intentFor(launch: PickerLaunch): Intent = when (launch) {
    PickerLaunch.Open -> Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType(ANY_TYPE)
    is PickerLaunch.Create -> Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
        .setType(launch.mime).putExtra(Intent.EXTRA_TITLE, launch.title)
    is PickerLaunch.GetContent -> getContent(ANY_TYPE).setComponent(launch.component)
    // No title: the platform's own chooser heading, in the device's language.
    is PickerLaunch.Chooser -> Intent.createChooser(getContent(ANY_TYPE), null)
        .putExtra(Intent.EXTRA_EXCLUDE_COMPONENTS, launch.exclude.toTypedArray())
}

private fun getContent(type: String) = Intent(Intent.ACTION_GET_CONTENT).addCategory(Intent.CATEGORY_OPENABLE).setType(type)

/** The activities that would answer GET_CONTENT + OPENABLE for [type]; the manifest's <queries> makes them visible. */
@Suppress("DEPRECATION")
internal fun getContentHandlers(packages: PackageManager, type: String): Set<ComponentName> {
    val intent = getContent(type)
    val found = if (Build.VERSION.SDK_INT >= 33) {
        packages.queryIntentActivities(intent, PackageManager.ResolveInfoFlags.of(PackageManager.MATCH_DEFAULT_ONLY.toLong()))
    } else {
        packages.queryIntentActivities(intent, PackageManager.MATCH_DEFAULT_ONLY)
    }
    return found.mapTo(LinkedHashSet()) { ComponentName(it.activityInfo.packageName, it.activityInfo.name) }
}

/** A provider's display name for a document, through [resolver]; null when it has none. */
internal class ResolverNames(private val resolver: ContentResolver) : NameLookup {
    override fun displayName(uri: String, signal: CancellationSignal): String? =
        resolver.query(Uri.parse(uri), arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null, signal)?.use { cursor ->
            val column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (column >= 0 && cursor.moveToFirst() && !cursor.isNull(column)) cursor.getString(column) else null
        }
}

/** The picker's requests through [activity]: its launches, its package queries and the application's resolver. */
internal class ActivityPickerHost(private val activity: Activity) : PickerHost {
    // The application's resolver only, so a lookup left running never holds the activity.
    override val names: NameLookup = ResolverNames(activity.applicationContext.contentResolver)

    override fun start(launch: PickerLaunch, code: Int) = activity.startActivityForResult(intentFor(launch), code)

    override fun getContentHandlers(type: String) = getContentHandlers(activity.packageManager, type)
}
