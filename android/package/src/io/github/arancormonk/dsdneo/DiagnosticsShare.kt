// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.ClipData
import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import java.io.File

class DiagnosticsProvider : FileProvider()

object DiagnosticsShare {
    // No path/URI input: systems.json, preferences and imports cannot be selected.
    @JvmStatic fun share(context: Context, text: String, title: String) {
        try {
            val directory = File(context.cacheDir, "diagnostics")
            if (!directory.isDirectory && !directory.mkdirs()) return
            val cutoff = System.currentTimeMillis() - 60 * 60 * 1000
            directory.listFiles()?.filter { it.isFile && it.lastModified() < cutoff }?.forEach { it.delete() }
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
}
