// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ComponentName
import android.content.ContentResolver
import android.content.ContentValues
import android.content.Intent
import android.content.UriPermission
import android.content.pm.PackageManager
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.os.CancellationSignal
import android.os.Handler
import android.provider.OpenableColumns
import org.json.JSONObject
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

// The issue's reference: a mixed-case authority and escapes that a QUrl round trip would change (#829 B1).
private const val PICKED = "content://com.Example.Provider/document/primary%3ADownload%2Fa%20b.csv"
private const val ROW = "content://media/external/downloads/42"
private const val NOT_FOUND_CODE = 0x5C_FFFF

private val photos = ComponentName("com.google.android.photopicker", "PhotopickerGetContentActivity")
private val photosModule = ComponentName("com.google.android.providers.media.module", "PhotoPickerGetContentActivity")
private val drive = ComponentName("com.google.android.apps.docs", ".app.GetContentActivity")
private val files = ComponentName("com.example.files", ".PickActivity")
private val music = ComponentName("com.android.music", ".MusicPicker")

/**
 * Plays the activity: each launch is kept with its code, and the next refusal in line is thrown from it; each code it is
 * asked to close is kept.
 */
private class FakeHost : PickerHost {
    val launches = mutableListOf<Pair<PickerLaunch, Int>>()
    val stopped = mutableListOf<Int>()
    val refusals = ArrayDeque<Exception?>()
    val handlers = mutableMapOf<String, Set<ComponentName>>()
    var queriesFail = false
    override var names = NameLookup { _, _ -> "a b.csv" }

    override fun start(launch: PickerLaunch, code: Int) {
        launches += launch to code
        refusals.removeFirstOrNull()?.let { throw it }
    }

    override fun stop(code: Int) {
        stopped += code
    }

    override fun getContentHandlers(type: String): Set<ComponentName> {
        check(!queriesFail) { "query failed" }
        return handlers[type].orEmpty()
    }

    val lastCode get() = launches.last().second
}

/** The Download folder: each insert waits for the test to answer it; discards are kept. */
private class FakeDownloads : DownloadsFallback {
    val inserts = mutableListOf<Triple<String, String, (String) -> Unit>>()
    val discarded = mutableListOf<String>()

    override fun insert(name: String, mime: String, onDone: (String) -> Unit) {
        inserts += Triple(name, mime, onDone)
    }

    override fun discard(uri: String) {
        discarded += uri
    }
}

private fun records(): List<JSONObject> {
    val out = mutableListOf<JSONObject>()
    while (true) out += JSONObject(DocumentPicker.pollResult().ifEmpty { return out })
}

private fun open(host: PickerHost, id: Long) {
    DocumentPicker.openWith(host, id)
    Handler.drain()
}

private fun save(host: PickerHost, id: Long, name: String = "backup.dsdbak", mime: String = "application/octet-stream") {
    DocumentPicker.createWith(host, id, name, mime)
    Handler.drain()
}

private fun answer(code: Int, resultCode: Int, data: Intent? = null): Boolean {
    val claimed = DocumentPicker.onActivityResult(Activity(), code, resultCode, data)
    Handler.drain()
    return claimed
}

private fun picked(uri: String) = Intent().also { it.data = Uri.parse(uri) }

/**
 * Waits until the name lookups and DownloadsSave's worker have run everything handed to them (each runs its tasks in
 * order on one thread), then runs what they posted to the main looper. A worker that beats the test thread has its post
 * run earlier, by the drain after a result.
 */
private fun awaitMain() {
    for (worker in listOf(DocumentPicker.lookups, DownloadsSave.worker)) worker.submit {}.get(5, TimeUnit.SECONDS)
    Handler.drain()
}

private fun JSONObject.isPicked(id: Long, uri: String, name: String, downloads: Boolean) =
    optLong("id") == id && optString("kind") == "picked" && optString("uri") == uri && optString("name") == name &&
        optBoolean("downloads") == downloads && has("downloads")

private fun JSONObject.isUnavailable(id: Long, reason: String) =
    optLong("id") == id && optString("kind") == "unavailable" && optString("reason") == reason

private fun JSONObject.isCancelled(id: Long) = optLong("id") == id && optString("kind") == "cancelled"

/** OPEN_DOCUMENT first; the document's name from its provider; the URI exactly as the picker gave it. */
private fun openDocumentPicks() {
    val host = FakeHost()
    open(host, 1)
    check(host.launches.size == 1 && host.launches[0].first === PickerLaunch.Open) { "OPEN_DOCUMENT goes first" }
    check(LaunchCodes.owns(host.lastCode))
    check(records().isEmpty()) { "nothing settles while the picker is out" }
    check(answer(host.lastCode, Activity.RESULT_OK, picked(PICKED)))
    awaitMain()
    val result = records()
    check(result.size == 1 && result[0].isPicked(1, PICKED, "a b.csv", false)) { "picked: $result" }
    check(Handler.timeouts.isEmpty()) { "the name deadline goes with the settle" }
    check(host.stopped.isEmpty()) { "a picker that answered is not closed" }
    check(answer(host.lastCode, Activity.RESULT_OK, picked(PICKED)) && records().isEmpty()) {
        "a request settles once; a second result for its code is dropped"
    }
}

/** No OPEN_DOCUMENT handler: GET_CONTENT through the apps that take documents, never the photo picker alone. */
private fun getContentFallback() {
    // Several capable: the chooser, without the handlers no family query returns (both photo-picker aliases).
    val several = FakeHost().apply {
        refusals.addAll(listOf(ActivityNotFoundException(), null))
        handlers["*/*"] = setOf(photos, photosModule, drive, files, music)
        handlers["application/*"] = setOf(drive, files)
        handlers["text/*"] = setOf(files)
        // An audio-only picker stays, through its own family.
        handlers["audio/*"] = setOf(music)
    }
    open(several, 2)
    check(several.launches.size == 2) { "the fallback launches once" }
    val (openLaunch, openCode) = several.launches[0]
    val (chooser, chooserCode) = several.launches[1]
    check(openLaunch === PickerLaunch.Open && chooser is PickerLaunch.Chooser)
    check(chooser.exclude == listOf(photos, photosModule)) { "excluded ${chooser.exclude}" }
    check(chooserCode != openCode && LaunchCodes.owns(chooserCode)) { "the fallback has its own code" }
    // The platform's late RESULT_CANCELED for the code that threw is claimed and dropped; the chooser stays pending.
    check(answer(openCode, Activity.RESULT_CANCELED) && records().isEmpty()) { "the stale cancel ends nothing" }
    check(answer(chooserCode, Activity.RESULT_OK, picked(PICKED)))
    awaitMain()
    check(records().single().isPicked(2, PICKED, "a b.csv", false))

    // One capable (a file manager with separate media and general filters): straight to it, no chooser.
    val one = FakeHost().apply {
        refusals.addAll(listOf(ActivityNotFoundException(), null))
        handlers["*/*"] = setOf(photos, files)
        handlers["application/*"] = setOf(files)
        handlers["text/*"] = setOf(files)
    }
    open(one, 3)
    val direct = one.launches[1].first
    check(direct is PickerLaunch.GetContent && direct.component == files) { "one capable app is launched directly" }
    check(answer(one.lastCode, Activity.RESULT_CANCELED) && records().single().isCancelled(3))

    // Only media pickers: no picker, and nothing launched after OPEN_DOCUMENT.
    val none = FakeHost().apply {
        refusals.add(ActivityNotFoundException())
        handlers["*/*"] = setOf(photos, photosModule)
    }
    open(none, 4)
    check(none.launches.size == 1 && records().single().isUnavailable(4, "noPicker")) { "media-only devices: noPicker" }
    check(none.stopped.isEmpty()) { "a launch that found no app has nothing to close" }

    // The fallback's own launch finds nothing, or fails; a query that fails; an OPEN_DOCUMENT launch that fails.
    val gone = FakeHost().apply {
        refusals.addAll(listOf(ActivityNotFoundException(), ActivityNotFoundException()))
        handlers["text/*"] = setOf(files)
    }
    open(gone, 5)
    check(records().single().isUnavailable(5, "noPicker"))
    check(answer(gone.lastCode, Activity.RESULT_CANCELED) && records().isEmpty()) { "its stale cancel is dropped too" }
    val refused = FakeHost().apply {
        refusals.addAll(listOf(ActivityNotFoundException(), SecurityException("not exported")))
        handlers["text/*"] = setOf(files)
    }
    open(refused, 6)
    check(records().single().isUnavailable(6, "failed"))
    val blind = FakeHost().apply {
        refusals.add(ActivityNotFoundException())
        queriesFail = true
    }
    open(blind, 7)
    check(records().single().isUnavailable(7, "failed"))
    val broken = FakeHost().apply { refusals.add(IllegalStateException("not attached")) }
    open(broken, 8)
    check(broken.launches.size == 1 && records().single().isUnavailable(8, "failed")) { "a failed launch: failed" }
}

/** What a picker can hand back, and what each result code means. */
private fun resultShapes() {
    fun pick(id: Long, resultCode: Int, data: Intent?): JSONObject {
        val host = FakeHost()
        open(host, id)
        check(answer(host.lastCode, resultCode, data))
        if (resultCode == Activity.RESULT_OK && data != null && resultUri(data)?.startsWith("content", true) == true) {
            awaitMain()
        }
        return records().single()
    }
    // Some pickers fill only ClipData.
    val clipOnly = Intent().also { it.clipData = ClipData(Uri.parse(PICKED)) }
    check(pick(10, Activity.RESULT_OK, clipOnly).isPicked(10, PICKED, "a b.csv", false)) { "ClipData item 0 is read" }
    check(pick(11, Activity.RESULT_OK, null).isUnavailable(11, "noResult")) { "RESULT_OK with no data" }
    check(pick(12, Activity.RESULT_OK, Intent()).isUnavailable(12, "noResult")) { "RESULT_OK with neither" }
    val emptyClip = Intent().also { it.clipData = ClipData() }
    check(pick(13, Activity.RESULT_OK, emptyClip).isUnavailable(13, "noResult")) { "an empty ClipData" }
    check(pick(14, Activity.RESULT_OK, picked("file:///sdcard/Download/a.csv")).isUnavailable(14, "unreadable"))
    check(pick(15, Activity.RESULT_OK, picked("CONTENT://x/y.csv")).isPicked(15, "CONTENT://x/y.csv", "a b.csv", false)) {
        "the scheme compares without case, and the reference stays as it came"
    }
    check(pick(16, Activity.RESULT_CANCELED, picked(PICKED)).isCancelled(16))
    check(pick(17, Activity.RESULT_FIRST_USER, picked(PICKED)).isCancelled(17)) { "a custom result code is a cancel" }
}

/** Codes outside the picker's range are not its; inside it, only the live launch's code settles anything. */
private fun codeRouting() {
    check(records().isEmpty())
    check(!DocumentPicker.onActivityResult(Activity(), 1305, Activity.RESULT_OK, picked(PICKED))) { "Qt's code" }
    check(!DocumentPicker.onActivityResult(Activity(), NOT_FOUND_CODE, Activity.RESULT_OK, null))
    check(!DocumentPicker.onActivityResult(Activity(), LaunchCodes.LAST + 1, Activity.RESULT_OK, null))
    // No request waiting (a result delivered to a new process after the old one died): claimed and dropped.
    check(answer(LaunchCodes.FIRST, Activity.RESULT_OK, picked(PICKED)) && records().isEmpty())
    val host = FakeHost()
    open(host, 20)
    val other = if (host.lastCode == LaunchCodes.LAST) LaunchCodes.FIRST else host.lastCode + 1
    check(answer(other, Activity.RESULT_OK, picked(PICKED)) && records().isEmpty()) { "another code in the range" }
    check(answer(host.lastCode, Activity.RESULT_CANCELED) && records().single().isCancelled(20))

    val codes = LaunchCodes()
    check(codes.next() == LaunchCodes.FIRST)
    repeat(LaunchCodes.LAST - LaunchCodes.FIRST - 1) { codes.next() }
    check(codes.next() == LaunchCodes.LAST && codes.next() == LaunchCodes.FIRST) { "codes wrap inside the range" }
}

/** A newer request replaces the live one, which is answered cancelled first; the replaced launch's result is dropped. */
private fun supersedeAndCancel() {
    val first = FakeHost()
    open(first, 30)
    val second = FakeHost()
    open(second, 31)
    check(records().single().isCancelled(30)) { "the replaced request is cancelled" }
    check(first.stopped == listOf(first.lastCode)) { "the replaced request's picker is closed: ${first.stopped}" }
    check(second.stopped.isEmpty() && second.launches.size == 1)
    check(answer(first.lastCode, Activity.RESULT_OK, picked(PICKED)) && records().isEmpty())
    check(answer(second.lastCode, Activity.RESULT_OK, picked(PICKED)))
    awaitMain()
    check(records().single().isPicked(31, PICKED, "a b.csv", false))
    check(second.stopped.isEmpty()) { "a picker that answered is not closed" }

    // Records keep their order: the replaced request's, then the newer one's.
    val third = FakeHost()
    open(third, 32)
    val fourth = FakeHost().apply { refusals.add(IllegalStateException()) }
    open(fourth, 33)
    val ordered = records()
    check(ordered.size == 2 && ordered[0].isCancelled(32) && ordered[1].isUnavailable(33, "failed")) { "FIFO: $ordered" }
    check(third.stopped == listOf(third.lastCode) && fourth.stopped.isEmpty()) { "a failed launch has nothing to close" }

    // A cancel ends the request with no record; its result, and other ids' cancels, change nothing.
    val cancelled = FakeHost()
    open(cancelled, 34)
    DocumentPicker.cancel(99)
    Handler.drain()
    check(cancelled.stopped.isEmpty()) { "another id's cancel closes nothing" }
    DocumentPicker.cancel(34)
    Handler.drain()
    check(records().isEmpty() && Handler.timeouts.isEmpty())
    check(cancelled.stopped == listOf(cancelled.lastCode)) { "a cancel closes the request's picker" }
    check(answer(cancelled.lastCode, Activity.RESULT_OK, picked(PICKED)) && records().isEmpty())
}

/**
 * The display name: a provider that never answers costs a request the 2 s deadline, then the URI's last segment; a
 * request retired meanwhile has its queued lookup removed, and the late answer changes nothing. A save never waits on
 * the lookup thread.
 */
private fun nameDeadline() {
    val release = CountDownLatch(1)
    val entered = CountDownLatch(1)
    val stuck = FakeHost().apply {
        names = NameLookup { _, _ ->
            entered.countDown()
            release.await(30, TimeUnit.SECONDS)
            "too late.csv"
        }
    }
    try {
        open(stuck, 40)
        check(answer(stuck.lastCode, Activity.RESULT_OK, picked(PICKED)))
        check(entered.await(5, TimeUnit.SECONDS)) { "the lookup reached its provider" }
        Handler.advanceBy(1_999)
        check(records().isEmpty()) { "no name before the deadline" }
        Handler.advanceBy(1)
        check(records().single().isPicked(40, PICKED, "primary:Download/a b.csv", false)) { "the decoded last segment" }

        // Queued behind the stuck lookup, then retired: removed from the queue, so its provider is never asked.
        var asked = false
        val queued = FakeHost().apply { names = NameLookup { _, _ -> asked = true; "x" } }
        open(queued, 41)
        check(answer(queued.lastCode, Activity.RESULT_OK, picked(PICKED)))
        check(DocumentPicker.lookups.queue.size == 1)
        DocumentPicker.cancel(41)
        Handler.drain()
        check(DocumentPicker.lookups.queue.isEmpty()) { "a retired lookup leaves the queue" }
        check(records().isEmpty() && queued.stopped.isEmpty()) { "a picker that answered is not closed" }

        // A Download-folder save while the lookup thread is still stuck.
        val downloads = FakeDownloads()
        DocumentPicker.downloads = downloads
        val saving = FakeHost().apply { refusals.add(ActivityNotFoundException()) }
        save(saving, 42)
        downloads.inserts.single().third(ROW)
        Handler.drain()
        check(records().single().isPicked(42, ROW, "backup.dsdbak", true)) { "the save settles without the lookup thread" }

        release.countDown()
        awaitMain()
        check(records().isEmpty() && !asked) { "the stuck lookup's answer is dropped" }
    } finally {
        release.countDown()
        DocumentPicker.downloads = DownloadsFolder
    }

    // A provider that throws, or names nothing, gets the same fallback name.
    for ((id, lookup) in listOf(43L to NameLookup { _, _ -> error("provider died") }, 44L to NameLookup { _, _ -> "" })) {
        val host = FakeHost().apply { names = lookup }
        open(host, id)
        check(answer(host.lastCode, Activity.RESULT_OK, picked(PICKED)))
        awaitMain()
        check(records().single().isPicked(id, PICKED, "primary:Download/a b.csv", false))
    }
}

/** CREATE_DOCUMENT first; with no app for it, a Download-folder row, which is discarded when it comes too late. */
private fun saveFallback() {
    var asked = false
    val host = FakeHost().apply { names = NameLookup { _, _ -> asked = true; "a b.csv" } }
    save(host, 50, "dsd-neo-backup-2026-11-08.dsdbak")
    val create = host.launches.single().first
    check(create is PickerLaunch.Create && create.title == "dsd-neo-backup-2026-11-08.dsdbak") { "CREATE_DOCUMENT" }
    check(create.mime == "application/octet-stream")
    check(answer(host.lastCode, Activity.RESULT_OK, picked(PICKED)))
    // No name lookup: the answer settles at once, named by the title asked for.
    check(records().single().isPicked(50, PICKED, "dsd-neo-backup-2026-11-08.dsdbak", false)) {
        "a save picker's document settles at once and is no download"
    }
    check(!asked && Handler.timeouts.isEmpty() && DocumentPicker.lookups.queue.isEmpty()) { "a save asks no name" }
    // Its other answers are an open's.
    val unreadable = FakeHost()
    save(unreadable, 49)
    check(answer(unreadable.lastCode, Activity.RESULT_OK, picked("file:///sdcard/Download/backup.dsdbak")))
    check(records().single().isUnavailable(49, "unreadable"))
    // Cancelled while the save picker is up: the picker is closed, so no document can be made there; were one still
    // made, its answer is dropped.
    val abandoned = FakeHost()
    save(abandoned, 48)
    DocumentPicker.cancel(48)
    Handler.drain()
    check(abandoned.stopped == listOf(abandoned.lastCode)) { "a cancelled save closes its picker" }
    check(answer(abandoned.lastCode, Activity.RESULT_OK, picked(PICKED)) && records().isEmpty())

    val downloads = FakeDownloads()
    DocumentPicker.downloads = downloads
    try {
        // The stale cancel for CREATE_DOCUMENT's code arrives while the insert is held; then the insert succeeds.
        val held = FakeHost().apply { refusals.add(ActivityNotFoundException()) }
        save(held, 51, "calls.zip", "application/zip")
        check(downloads.inserts.single().first == "calls.zip" && downloads.inserts.single().second == "application/zip")
        check(answer(held.lastCode, Activity.RESULT_CANCELED) && records().isEmpty()) { "the stale cancel is dropped" }
        Handler.advanceBy(9_999)
        downloads.inserts.single().third(ROW)
        Handler.drain()
        check(records().single().isPicked(51, ROW, "calls.zip", true)) { "the insert in time is the answer" }
        check(downloads.discarded.isEmpty() && Handler.timeouts.isEmpty())

        // No row: noSavePicker.
        downloads.inserts.clear()
        save(FakeHost().apply { refusals.add(ActivityNotFoundException()) }, 52)
        downloads.inserts.single().third("")
        Handler.drain()
        check(records().single().isUnavailable(52, "noSavePicker"))

        // Past the 10 s deadline: noSavePicker, and the late row is deleted; a late failure needs nothing.
        downloads.inserts.clear()
        save(FakeHost().apply { refusals.add(ActivityNotFoundException()) }, 53)
        Handler.advanceBy(10_000)
        check(records().single().isUnavailable(53, "noSavePicker"))
        downloads.inserts.single().third(ROW)
        Handler.drain()
        check(records().isEmpty() && downloads.discarded == listOf(ROW)) { "a late row is discarded" }

        // Cancelled, or replaced, while the insert is held: no answer, and the row is discarded.
        downloads.inserts.clear()
        downloads.discarded.clear()
        val noPicker = FakeHost().apply { refusals.add(ActivityNotFoundException()) }
        save(noPicker, 54)
        DocumentPicker.cancel(54)
        Handler.drain()
        check(noPicker.stopped.isEmpty()) { "a save with no picker out closes nothing" }
        save(FakeHost().apply { refusals.add(ActivityNotFoundException()) }, 55)
        open(FakeHost(), 56)
        check(records().single().isCancelled(55))
        downloads.inserts[0].third("$ROW/54")
        downloads.inserts[1].third("$ROW/55")
        Handler.drain()
        check(records().isEmpty() && downloads.discarded == listOf("$ROW/54", "$ROW/55"))
        DocumentPicker.cancel(56)
        Handler.drain()

        // Cancelled after the row came: the record stands (the host drops it and discards the row); nothing more here.
        downloads.inserts.clear()
        downloads.discarded.clear()
        save(FakeHost().apply { refusals.add(ActivityNotFoundException()) }, 57)
        downloads.inserts.single().third(ROW)
        Handler.drain()
        DocumentPicker.cancel(57)
        Handler.drain()
        check(records().single().isPicked(57, ROW, "backup.dsdbak", true) && downloads.discarded.isEmpty())
    } finally {
        DocumentPicker.downloads = DownloadsFolder
    }

    // The real Download folder: before the host handed DownloadsSave the resolver, it has no row to give.
    check(DocumentPicker.downloads === DownloadsFolder) { "the picker saves through DownloadsSave" }
    save(FakeHost().apply { refusals.add(ActivityNotFoundException()) }, 58)
    check(records().single().isUnavailable(58, "noSavePicker"))
    save(FakeHost().apply { refusals.add(IllegalStateException()) }, 59)
    check(records().single().isUnavailable(59, "failed")) { "a failed CREATE_DOCUMENT launch is no reason to save" }

    // With it: the row is made on DownloadsSave's thread, and the record names the requested name.
    val resolver = MediaRows()
    DownloadsSave.start(resolver)
    save(FakeHost().apply { refusals.add(ActivityNotFoundException()) }, 60, "calls.zip", "application/zip")
    awaitMain()
    check(records().single().isPicked(60, ROW, "calls.zip", true)) { "a Download-folder row through DownloadsSave" }
    val inserted = resolver.inserts.single()
    check(inserted.first == "dsd-downloads" && inserted.second["_display_name"] == "calls.zip") { "inserted off main" }
    check(DownloadsSave.isPending(ROW)) { "the row waits for its write" }
    DownloadsSave.discard(ROW)
    DownloadsSave.worker.submit {}.get(5, TimeUnit.SECONDS)
    check(resolver.deleted == listOf(ROW) && !DownloadsSave.isPending(ROW)) { "a discarded row is deleted" }
}

/** MediaStore for the picker's own save: every insert makes [ROW]; deletes are kept. */
private class MediaRows : ContentResolver() {
    val inserts: MutableList<Pair<String, Map<String, Any?>>> = Collections.synchronizedList(mutableListOf())
    val deleted: MutableList<String> = Collections.synchronizedList(mutableListOf())
    override val persistedUriPermissions: List<UriPermission> get() = emptyList()

    override fun insert(url: Uri, values: ContentValues?): Uri? {
        inserts += Thread.currentThread().name to values!!.values.toMap()
        return Uri.parse(ROW)
    }

    override fun delete(url: Uri, where: String?, selectionArgs: Array<String>?): Int {
        deleted += url.toString()
        return 1
    }
}

private fun fallbackNames() {
    check(lastSegmentName(PICKED) == "primary:Download/a b.csv")
    check(lastSegmentName("content://media/external/downloads/42") == "42")
    check(lastSegmentName("content://a/b/%C3%A9t%C3%A9.csv?x=1#y") == "été.csv") { "UTF-8 escapes; no query or fragment" }
    check(lastSegmentName("content://a/b/c/") == "c")
    check(lastSegmentName("content://a/50%25%2") == "50%%2") { "a malformed escape stays" }
    check(lastSegmentName("content://a/x%zz%+f") == "x%zz%+f")
    check(lastSegmentName("content://authority") == "")
}

/** The Intents the launches become, the handler query and the activity adapter, over the platform stubs. */
private fun adapter() {
    val open = intentFor(PickerLaunch.Open)
    check(open.action == Intent.ACTION_OPEN_DOCUMENT && open.categories == setOf(Intent.CATEGORY_OPENABLE))
    check(open.mimeType == "*/*" && open.target == null && open.extras.isEmpty())
    val create = intentFor(PickerLaunch.Create("calls.zip", "application/zip"))
    check(create.action == Intent.ACTION_CREATE_DOCUMENT && create.categories == setOf(Intent.CATEGORY_OPENABLE))
    check(create.mimeType == "application/zip" && create.extras == mapOf(Intent.EXTRA_TITLE to "calls.zip"))
    val direct = intentFor(PickerLaunch.GetContent(files))
    check(direct.action == Intent.ACTION_GET_CONTENT && direct.categories == setOf(Intent.CATEGORY_OPENABLE))
    check(direct.mimeType == "*/*" && direct.target == files)
    val chooser = intentFor(PickerLaunch.Chooser(listOf(photos, photosModule)))
    check(chooser.action == Intent.ACTION_CHOOSER && Intent.EXTRA_TITLE !in chooser.extras)
    val inner = chooser.extras[Intent.EXTRA_INTENT] as Intent
    check(inner.action == Intent.ACTION_GET_CONTENT && inner.categories == setOf(Intent.CATEGORY_OPENABLE))
    check(inner.mimeType == "*/*" && inner.target == null)
    @Suppress("UNCHECKED_CAST")
    val excluded = chooser.extras[Intent.EXTRA_EXCLUDE_COMPONENTS] as Array<Any>
    check(excluded.toList() == listOf(photos, photosModule)) { "the chooser excludes the media pickers" }

    val packages = PackageManager().apply { handlers["text/*"] = listOf(files, drive) }
    for (sdk in listOf(29, 33)) {
        Build.VERSION.SDK_INT = sdk
        packages.queries.clear()
        check(getContentHandlers(packages, "text/*") == setOf(files, drive))
        val (query, flags) = packages.queries.single()
        check(query.action == Intent.ACTION_GET_CONTENT && query.categories == setOf(Intent.CATEGORY_OPENABLE))
        check(query.mimeType == "text/*" && flags == PackageManager.MATCH_DEFAULT_ONLY.toLong())
        check(packages.lastOverloadSdk == if (sdk >= 33) 33 else 32) { "the overload for API $sdk" }
    }
    Build.VERSION.SDK_INT = 30

    // The public entry points end to end: no OPEN_DOCUMENT handler, one file manager, a picked document named by its
    // provider through the application's resolver.
    packages.handlers.clear()
    packages.handlers["*/*"] = listOf(photos, files)
    packages.handlers["application/*"] = listOf(files)
    var askedSignal: CancellationSignal? = null
    val resolver = object : ContentResolver() {
        override fun query(
            uri: Uri, projection: Array<String>?, selection: String?, selectionArgs: Array<String>?, sortOrder: String?,
            cancellationSignal: CancellationSignal?,
        ): Cursor? {
            check(uri.toString() == PICKED && projection?.toList() == listOf(OpenableColumns.DISPLAY_NAME))
            askedSignal = cancellationSignal
            return NameCursor("Talkgroups.csv")
        }
    }
    val activity = object : Activity() {
        override val contentResolver = resolver
        override val packageManager = packages
    }
    activity.refusals[Intent.ACTION_OPEN_DOCUMENT] = ActivityNotFoundException()
    check(DocumentPicker.open(activity, 60))
    Handler.drain()
    check(activity.launched.size == 2 && activity.launched[1].first.target == files) { "${activity.launched}" }
    check(DocumentPicker.onActivityResult(activity, activity.launched[1].second, Activity.RESULT_OK, picked(PICKED)))
    awaitMain()
    check(records().single().isPicked(60, PICKED, "Talkgroups.csv", false) && askedSignal != null)

    check(DocumentPicker.create(activity, 61, "calls.zip", "application/zip"))
    Handler.drain()
    val launch = activity.launched.last()
    check(launch.first.action == Intent.ACTION_CREATE_DOCUMENT && launch.first.extras[Intent.EXTRA_TITLE] == "calls.zip")
    check(activity.finished.isEmpty()) { "the answered pickers were never closed" }
    DocumentPicker.cancel(61)
    Handler.drain()
    check(activity.finished == listOf(launch.second)) { "a cancel finishes the launch: ${activity.finished}" }

    // A provider row with no name, and one with none at all.
    check(ResolverNames(nameResolver(NameCursor(null))).displayName(PICKED, CancellationSignal()) == null)
    check(ResolverNames(nameResolver(null)).displayName(PICKED, CancellationSignal()) == null)
}

private class NameCursor(private val name: String?) : Cursor {
    override fun getColumnIndex(columnName: String) = if (columnName == OpenableColumns.DISPLAY_NAME) 0 else -1
    override fun moveToFirst() = true
    override fun isNull(columnIndex: Int) = name == null
    override fun getLong(columnIndex: Int) = error("no size column")
    override fun getString(columnIndex: Int) = name
    override fun close() {}
}

private fun nameResolver(cursor: Cursor?) = object : ContentResolver() {
    override fun query(
        uri: Uri, projection: Array<String>?, selection: String?, selectionArgs: Array<String>?, sortOrder: String?,
        cancellationSignal: CancellationSignal?,
    ): Cursor? = cursor
}

fun main() {
    try {
        openDocumentPicks()
        getContentFallback()
        resultShapes()
        codeRouting()
        supersedeAndCancel()
        nameDeadline()
        saveFallback()
        fallbackNames()
        adapter()
        check(records().isEmpty() && Handler.timeouts.isEmpty()) { "every case ends settled" }
        println("PASS: document picker fallbacks, request codes, results, deadlines, supersede, cancel and adapters")
    } finally {
        DocumentPicker.lookups.shutdownNow()
        DownloadsSave.worker.shutdownNow()
    }
}
