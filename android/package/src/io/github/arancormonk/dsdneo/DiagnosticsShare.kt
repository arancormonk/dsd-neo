// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.ClipData
import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import java.io.File
import kotlin.math.abs

class DiagnosticsProvider : FileProvider()

object DiagnosticsShare {
    /** The cache folder share copies go in. Must match res/xml/diagnostics_paths.xml. */
    internal const val SHARE_FOLDER = "diagnostics"

    /** A share copy is kept this long (by its modification time), and removed at the next share or app start after. */
    internal const val SHARE_KEEP_MS = 60 * 60 * 1000L

    // No path/URI input: systems.json, preferences and imports cannot be selected.
    @JvmStatic fun share(context: Context, text: String, title: String) {
        try {
            val directory = File(context.cacheDir, SHARE_FOLDER)
            if (!directory.isDirectory && !directory.mkdirs()) return
            clean(directory, System.currentTimeMillis())
            val file = File.createTempFile("diagnostics-", ".txt", directory)
            file.writeText(text, Charsets.UTF_8)
            val uri = FileProvider.getUriForFile(context, context.packageName + ".diagnostics", file)
            val send = Intent(Intent.ACTION_SEND).apply {
                type = "text/plain"
                putExtra(Intent.EXTRA_STREAM, uri)
                putExtra(Intent.EXTRA_SUBJECT, title)
                clipData = ClipData.newRawUri("diagnostics", uri)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            context.startActivity(Intent.createChooser(send, title).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION)
            })
        } catch (_: Exception) {
            DsdNative.nativeHostDiagnostic("Diagnostics sharing failed")
        }
    }

    /**
     * Remove the share copies past [SHARE_KEEP_MS], at app start (issue #800, D10): a copy an older build shared can
     * hold position lines the log itself no longer keeps, so it does not wait for the next share. Off the main thread
     * (the host's transfer worker).
     */
    @JvmStatic fun clean(context: Context) {
        try {
            clean(File(context.cacheDir, SHARE_FOLDER), System.currentTimeMillis())
        } catch (_: Exception) {
            // Nothing to report: the next share or start tries again.
        }
    }

    /**
     * Remove each share copy in [directory] whose modification time is more than [SHARE_KEEP_MS] from [wallNowMs],
     * either way, so a clock set back does not keep it for good.
     */
    internal fun clean(directory: File, wallNowMs: Long) {
        directory.listFiles()?.filter { it.isFile && abs(wallNowMs - it.lastModified()) > SHARE_KEEP_MS }
            ?.forEach { it.delete() }
    }
}
