// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.Context
import android.content.Intent
import android.net.Uri
import java.io.File
import java.nio.file.Files

private val scratch: File = Files.createTempDirectory("dsd-recording-share").toFile().canonicalFile

/** An app whose shared and internal storage are folders under [scratch]; shared storage may be away (null). */
private class App(private val external: File?) : Context() {
    override val filesDir = File(scratch, "internal/files").apply { mkdirs() }

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
    check(away == listOf("/storage/emulated/0/Android/data/io.github.arancormonk.dsdneo/files/Call recordings", roots[1])) {
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

    // A link inside a root, to a file outside it or inside it, is never followed.
    val secret = recording(File(scratch, "internal/files"), "secret.wav")
    val outward = File(roots[0], "Explore/outward.wav")
    Files.createSymbolicLink(outward.toPath(), secret.toPath())
    check(RecordingShare.sharable(outward.path, roots) == null) { "a link to a file outside the roots is not shared" }
    val inward = File(roots[0], "Explore/inward.wav")
    Files.createSymbolicLink(inward.toPath(), internal.toPath())
    check(RecordingShare.sharable(inward.path, roots) == null) { "a link is not shared even when it points inside" }
    // A folder inside a root that links out: the real path is outside, so nothing under it is shared.
    val linkedFolder = File(roots[0], "Linked")
    Files.createSymbolicLink(linkedFolder.toPath(), File(scratch, "internal/files").toPath())
    check(RecordingShare.sharable(File(linkedFolder, "secret.wav").path, roots) == null) {
        "a recording reached through a linked folder outside the roots is not shared"
    }
}

private fun aShareOffersOneRecordingReadOnly() {
    val app = App(sharedFiles)
    val roots = RecordingShare.roots(app).map(::File)
    val file = recording(roots[0], "Talkgroups (abcd1234)/share.wav")
    RecordingShare.share(app, file.path, "Call recording")
    check(app.started.size == 1) { "a share starts one chooser" }
    val chooser = app.started[0]
    check(chooser.action == Intent.ACTION_CHOOSER) { "the user picks the app" }
    check(chooser.flags and Intent.FLAG_ACTIVITY_NEW_TASK != 0) { "started from the application context" }
    val send = chooser.extras[Intent.EXTRA_INTENT] as Intent
    val uri = send.extras[Intent.EXTRA_STREAM] as Uri
    check(send.action == Intent.ACTION_SEND && send.type == "audio/wav") { "one WAV is sent" }
    check(uri.toString() == "content://io.github.arancormonk.dsdneo.recordings${file.path}") {
        "through the recordings provider: $uri"
    }
    check(send.clipData?.uri == uri) { "the grant covers the clip" }
    check(send.flags and Intent.FLAG_GRANT_READ_URI_PERMISSION != 0) { "read access is granted" }
    check(send.flags and Intent.FLAG_GRANT_WRITE_URI_PERMISSION == 0 &&
        chooser.flags and Intent.FLAG_GRANT_WRITE_URI_PERMISSION == 0) { "write access never is" }

    RecordingShare.share(app, recording(File(scratch, "internal/files"), "prefs.wav").path, "Call recording")
    check(app.started.size == 1) { "a file outside the roots starts nothing" }
}

fun main() {
    try {
        rootsAreSharedStorageThenInternal()
        onlyRecordingsInsideARootAreShared()
        aShareOffersOneRecordingReadOnly()
    } finally {
        scratch.deleteRecursively()
    }
    println("PASS: call recordings live in the two roots, and only a recording inside one is shared, read-only")
}
