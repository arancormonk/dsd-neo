// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.Context
import android.content.Intent
import java.io.File
import java.nio.file.Files

private val scratch: File = Files.createTempDirectory("dsd-diagnostics-share").toRealPath().toFile()

private class App : Context() {
    override val cacheDir = File(scratch, "cache").apply { mkdirs() }
}

private fun copy(folder: File, name: String, wallMs: Long): File =
    File(folder, name).apply {
        writeText("[stderr] [info]  Lat: 22.50000 Lon: 45.00000\n")
        check(setLastModified(wallMs)) { "the copy's time can be set" }
    }

/**
 * Issue #800, D10: a share copy an older build made can hold position lines, so the app start removes the copies past
 * their hour (either way of the clock) without waiting for the next share; a copy younger than that stays (the app it
 * went to may still be reading it), and nothing but the folder's files is touched.
 */
private fun oldShareCopiesGoAtAppStart() {
    val app = App()
    val folder = File(app.cacheDir, DiagnosticsShare.SHARE_FOLDER).apply { mkdirs() }
    val now = System.currentTimeMillis()
    val hour = DiagnosticsShare.SHARE_KEEP_MS
    val old = copy(folder, "diagnostics-old.txt", now - 2 * hour)
    val ahead = copy(folder, "diagnostics-ahead.txt", now + 2 * hour)
    val recent = copy(folder, "diagnostics-recent.txt", now - hour / 2)
    val nested = File(folder, "kept").apply { mkdirs() }
    DiagnosticsShare.clean(app)
    check(!old.exists()) { "a copy past its hour is removed at app start" }
    check(!ahead.exists()) { "a copy dated ahead of the clock by more than an hour goes too" }
    check(recent.isFile) { "a copy within its hour stays" }
    check(nested.isDirectory) { "only the folder's files are copies" }

    // The app start with no share folder yet does nothing.
    DiagnosticsShare.clean(object : Context() {
        override val cacheDir = File(scratch, "empty-cache")
    })

    // A share cleans as it copies, and offers its own copy.
    check(recent.setLastModified(now - 2 * hour)) { "the copy's time can be set" }
    DiagnosticsShare.share(app, "[host] [info] text", "Diagnostics")
    check(!recent.exists()) { "a share removes old copies" }
    check(app.started.size == 1 && app.started[0].action == Intent.ACTION_CHOOSER) { "a share starts one chooser" }
    check(folder.listFiles()!!.count { it.isFile } == 1) { "the share's own copy is there" }
    check(DsdNative.diagnostics.isEmpty()) { "the share did not fail" }
}

/**
 * Run [body], then remove [root] and everything in it without following a link, failing when anything cannot be
 * removed (on Windows, a file a handle still holds), so the test cannot pass with its temp folder left behind. A
 * failure of [body] stays the failure, with the removal's own attached.
 */
private fun withScratchRemoved(root: File, body: () -> Unit) {
    try {
        body()
    } catch (e: Throwable) {
        runCatching { removeTree(root) }.exceptionOrNull()?.let(e::addSuppressed)
        throw e
    }
    removeTree(root)
}

private fun removeTree(root: File) {
    Files.walk(root.toPath()).use { paths -> paths.sorted(Comparator.reverseOrder()).forEach(Files::delete) }
}

fun main() {
    withScratchRemoved(scratch) {
        oldShareCopiesGoAtAppStart()
    }
    println("PASS: diagnostics share copies past their hour go at app start and at the next share")
}
