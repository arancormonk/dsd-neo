// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.core.content.FileProvider
import java.io.File
import java.nio.file.Files

private val scratch: File = Files.createTempDirectory("dsd-recording-share").toRealPath().toFile()

/** An app whose shared and internal storage are folders under [scratch]; shared storage may be away (null). */
private class App(private val external: File?) : Context() {
    override val filesDir = File(scratch, "internal/files").apply { mkdirs() }
    override val cacheDir = File(scratch, "internal/cache").apply { mkdirs() }

    override fun getExternalFilesDir(type: String?): File? {
        check(type == null) { "the app's own folder, not a media type's" }
        return external
    }
}

private val sharedFiles = File(scratch, "storage/Android/data/io.github.arancormonk.dsdneo/files").apply { mkdirs() }

private fun recording(root: File, relative: String, content: String = "RIFF"): File =
    File(root, relative).apply {
        parentFile.mkdirs()
        writeText(content)
    }

private fun rootsAreSharedStorageThenInternal() {
    val roots = RecordingShare.roots(App(sharedFiles)).toList()
    check(roots == listOf(File(sharedFiles, "Call recordings").path, File(scratch, "internal/files/Call recordings").path)) {
        "shared storage first, internal storage last: $roots"
    }
    // Shared storage away: its root is still named, at the path Android gives the app there.
    val away = RecordingShare.roots(App(null)).toList()
    val awayPath = File("/storage/emulated/0", "Android/data/io.github.arancormonk.dsdneo/files/Call recordings").path
    check(away == listOf(awayPath, roots[1])) {
        "a root whose storage is away is still named: $away"
    }
}

private fun onlyRecordingsInsideARootAreShared() {
    val app = App(sharedFiles)
    val roots = RecordingShare.roots(app).map(::File)
    val external = recording(roots[0], "Talkgroups (abcd1234)/call.wav")
    val internal = recording(roots[1], "Explore/call.WAV")
    check(RecordingShare.sharable(external.path, roots) == external) { "a recording on shared storage is shared" }
    check(RecordingShare.sharable(internal.path, roots) == internal) { "a recording in internal storage is shared" }
    check(RecordingShare.canShare(app, external.path)) { "canShare() asks the same" }

    val refused = mapOf(
        "a file outside the roots" to recording(File(scratch, "internal/files"), "systems.json").path,
        "a .wav file outside the roots" to recording(File(scratch, "internal/files"), "imports/replay.wav").path,
        "a sibling folder whose name starts like a root" to recording(sharedFiles, "Call recordings 2/x.wav").path,
        "a file that is no recording" to recording(roots[0], "Explore/folder.json").path,
        "a path that climbs out" to File(roots[0], "../imports/replay.wav").also { recording(sharedFiles, "imports/replay.wav") }.path,
        "a relative path" to "Call recordings/Explore/call.wav",
        "a folder" to File(roots[0], "Talkgroups (abcd1234).wav").apply { mkdirs() }.path,
        "a file that is gone" to File(roots[0], "Explore/gone.wav").path,
    )
    for ((what, path) in refused) {
        check(RecordingShare.sharable(path, roots) == null) { "$what is not shared: $path" }
    }

    // A link inside a root, to a file outside it or inside it, is never followed. Where this host cannot make links
    // (Windows without the privilege) these cases are skipped; Android, like Linux and macOS, makes them.
    val secret = recording(File(scratch, "internal/files"), "secret.wav")
    val outward = File(roots[0], "Explore/outward.wav")
    if (!link(outward, secret)) {
        println("SKIP: symbolic links cannot be made here; the link cases did not run")
        return
    }
    check(RecordingShare.sharable(outward.path, roots) == null) { "a link to a file outside the roots is not shared" }
    val inward = File(roots[0], "Explore/inward.wav")
    check(link(inward, internal)) { "a second link" }
    check(RecordingShare.sharable(inward.path, roots) == null) { "a link is not shared even when it points inside" }
    // A folder inside a root that links out: the real path is outside, so nothing under it is shared.
    val linkedFolder = File(roots[0], "Linked")
    check(link(linkedFolder, File(scratch, "internal/files"))) { "a folder link" }
    check(RecordingShare.sharable(File(linkedFolder, "secret.wav").path, roots) == null) {
        "a recording reached through a linked folder outside the roots is not shared"
    }
}

/** A symbolic link at [at] to [target]; false where this host cannot make one. */
private fun link(at: File, target: File): Boolean = try {
    Files.createSymbolicLink(at.toPath(), target.toPath())
    true
} catch (_: UnsupportedOperationException) {
    false
} catch (_: java.io.IOException) {
    false
} catch (_: SecurityException) {
    false
}

private val shareFolder = File(scratch, "internal/cache/${RecordingShare.SHARE_FOLDER}")

/** Stage [file] and offer the copy, as the host does: the copy on its worker, the chooser on the main thread. */
private fun share(app: Context, file: File, title: String = "Call recording"): Boolean {
    val copy = RecordingShare.stage(app, file.path)
    return copy.isNotEmpty() && RecordingShare.offer(app, copy, file.path, title)
}

private fun aShareOffersOneRecordingReadOnly() {
    val app = App(sharedFiles)
    val roots = RecordingShare.roots(app).map(::File)
    val file = recording(roots[0], "Talkgroups (abcd1234)/share.wav")
    check(share(app, file)) { "a share answers that its chooser started" }
    check(app.started.size == 1) { "a share starts one chooser" }
    val chooser = app.started[0]
    check(chooser.action == Intent.ACTION_CHOOSER) { "the user picks the app" }
    check(chooser.flags and Intent.FLAG_ACTIVITY_NEW_TASK != 0) { "started from the application context" }
    val send = chooser.extras[Intent.EXTRA_INTENT] as Intent
    val uri = send.extras[Intent.EXTRA_STREAM] as Uri
    check(send.action == Intent.ACTION_SEND && send.type == "audio/wav") { "one WAV is sent" }
    val offered = offeredFile(uri)
    check(offered.parentFile.parentFile == shareFolder && offered.name == "share.wav") {
        "a copy, in a folder of its own in the cache's share folder, under the recording's name: $uri"
    }
    check(send.clipData?.uri == uri) { "the grant covers the clip" }
    check(send.flags and Intent.FLAG_GRANT_READ_URI_PERMISSION != 0) { "read access is granted" }
    check(send.flags and Intent.FLAG_GRANT_WRITE_URI_PERMISSION == 0 &&
        chooser.flags and Intent.FLAG_GRANT_WRITE_URI_PERMISSION == 0) { "write access never is" }
    check(share(app, file) && offeredFile(app.started[1]) != offered && offered.isFile) {
        "each share has a copy of its own"
    }

    check(RecordingShare.takeFailure().isEmpty()) { "a share that started is no failure" }
    val outside = recording(File(scratch, "internal/files"), "prefs.wav").path
    check(RecordingShare.stage(app, outside).isEmpty()) { "a file outside the roots is never copied" }
    check(app.started.size == 2) { "a file outside the roots starts nothing" }
    check(RecordingShare.takeFailure() == outside && RecordingShare.takeFailure().isEmpty()) {
        "and the refusal waits once for the host to take it"
    }
    check(!RecordingShare.offer(app, file.path, file.path, "Call recording") && app.started.size == 2) {
        "only a share copy is ever offered, never the original"
    }
    check(RecordingShare.takeFailure() == file.path) { "and that refusal is reported too" }
}

private fun offeredFile(uri: Uri): File =
    File(uri.toString().removePrefix("content://io.github.arancormonk.dsdneo.recordings"))

private fun offeredFile(chooser: Intent): File =
    offeredFile((chooser.extras[Intent.EXTRA_INTENT] as Intent).extras[Intent.EXTRA_STREAM] as Uri)

/** #708: retention or a delete can take the original while the chooser, or the app it went to, opens the share. */
private fun aShareOutlivesItsOriginal() {
    val app = App(sharedFiles)
    val roots = RecordingShare.roots(app).map(::File)
    val original = recording(roots[0], "Talkgroups (abcd1234)/oldest.wav", "RIFF oldest")
    check(share(app, original)) { "the chooser started" }
    val offered = offeredFile(app.started.last())
    original.delete() // retention, while the chooser is open
    check(offered.isFile && offered.readText() == "RIFF oldest") {
        "the app it went to can still open what was offered"
    }
}

/** #708: a share the host handed over after canShare() said yes can still fail; the host learns it from takeFailure(). */
private fun aShareThatFailsLaterIsReported() {
    val app = App(sharedFiles)
    val roots = RecordingShare.roots(app).map(::File)
    val gone = recording(roots[0], "Explore/gone-later.wav")
    check(RecordingShare.canShare(app, gone.path)) { "it could be shared when the host asked" }
    gone.delete()
    check(RecordingShare.stage(app, gone.path).isEmpty() && RecordingShare.takeFailure() == gone.path) {
        "a recording gone by the time it would be copied is a failure the host learns"
    }

    val refused = recording(roots[0], "Explore/refused.wav")
    val refusedCopy = File(RecordingShare.stage(app, refused.path))
    FileProvider.refuses = true
    try {
        check(!RecordingShare.offer(app, refusedCopy.path, refused.path, "Call recording")) {
            "a provider that refuses the path fails"
        }
    } finally {
        FileProvider.refuses = false
    }
    check(RecordingShare.takeFailure() == refused.path) { "and the host learns it, by the recording's path" }
    check(!refusedCopy.parentFile.exists()) { "a copy no app was offered goes at once" }

    val nobody = object : Context() {
        override val filesDir = app.filesDir
        override val cacheDir = app.cacheDir

        override fun getExternalFilesDir(type: String?): File = sharedFiles

        override fun startActivity(intent: Intent) = throw IllegalStateException("No Activity found to handle Intent")
    }
    val nobodyCopy = RecordingShare.stage(nobody, refused.path)
    check(!RecordingShare.offer(nobody, nobodyCopy, refused.path, "Call recording") &&
        RecordingShare.takeFailure() == refused.path) {
        "a chooser that cannot start is a failure the host learns"
    }
    check(RecordingShare.takeFailure().isEmpty()) { "each failure once" }
}

/**
 * #708: share copies go once they are a day old, at the next share or app start, except one this process made less
 * than an hour ago, whatever the wall clock says.
 */
private fun oldShareCopiesAreCleaned() {
    val app = App(sharedFiles)
    shareFolder.deleteRecursively()
    val now = System.currentTimeMillis()
    val day = RecordingShare.SHARE_KEEP_MS
    // Left by an earlier process: one old, one recent, one dated far ahead by a clock that was set back since.
    fun leftBehind(name: String, modified: Long): File =
        File(shareFolder, name).apply {
            mkdirs()
            File(this, "call.wav").writeText("RIFF")
            check(setLastModified(modified)) { "a copy's time can be set" }
        }
    val old = leftBehind("share-old", now - day - 60_000)
    val recent = leftBehind("share-recent", now - day + 60_000)
    val ahead = leftBehind("share-ahead", now + day + 60_000)
    RecordingShare.clean(app)
    check(!old.exists() && !ahead.exists()) { "copies a day old, or dated more than a day ahead, go at app start" }
    check(recent.isDirectory) { "a copy less than a day old stays" }

    // Made here, then dated two days back (the wall clock jumped ahead since): kept for its hour, gone after it.
    val original = recording(RecordingShare.roots(app).map(::File)[0], "Explore/held.wav")
    val mono = 1_000_000L
    val held = RecordingShare.copyForShare(original, shareFolder, mono).parentFile
    check(held.setLastModified(now - 2 * day)) { "the copy's time can be set" }
    RecordingShare.clean(shareFolder, now, mono + RecordingShare.SHARE_HOLD_MS - 1)
    check(held.isDirectory) { "a copy this process made less than an hour ago is never removed" }
    RecordingShare.clean(shareFolder, now, mono + RecordingShare.SHARE_HOLD_MS)
    check(!held.exists()) { "and goes, a day old, once its hour is up" }

    // A share cleans as it copies.
    recent.setLastModified(now - day - 60_000)
    check(RecordingShare.stage(app, original.path).isNotEmpty() && !recent.exists()) { "a share removes old copies" }
}

fun main() {
    try {
        rootsAreSharedStorageThenInternal()
        onlyRecordingsInsideARootAreShared()
        aShareOffersOneRecordingReadOnly()
        aShareThatFailsLaterIsReported()
        aShareOutlivesItsOriginal()
        oldShareCopiesAreCleaned()
    } finally {
        scratch.deleteRecursively()
    }
    println("PASS: call recordings live in the two roots, only a recording inside one is shared, read-only, as a copy")
}
