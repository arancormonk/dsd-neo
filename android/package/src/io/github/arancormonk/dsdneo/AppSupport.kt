// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import android.util.Log
import java.io.File

/** Platform odds and ends the Qt host reaches through QJniObject. */
object AppSupport {
    private const val TAG = "dsd-neo"
    private const val REQUEST_NOTIFICATIONS = 4711

    /**
     * Ask for POST_NOTIFICATIONS on API 33+. The service runs without it, but its
     * notification stays invisible, which reads as "nothing happened".
     */
    @JvmStatic
    fun ensureNotificationPermission(activity: Activity) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return
        }
        if (activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
            == PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        activity.requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
    }

    /**
     * The name to give the materialized copy.
     *
     * Only the provider knows what a document is called: a SAF URI's last path segment
     * is an opaque document id, and for the Downloads provider it is the whole source
     * path percent-encoded into one segment. Naming the cache file after it produced
     * `raw%3A%2Fstorage%2Femulated%2F0%2FDownload%2Fcapture.wav`, and for providers
     * whose ids are bare numbers it would drop the extension the engine dispatches on.
     * So ask the resolver first and treat anything derived from the URI as a fallback.
     */
    private fun displayNameFor(context: Context, uri: Uri, fallback: String): String {
        val resolved = try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { cursor ->
                    val column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (column >= 0 && cursor.moveToFirst()) cursor.getString(column) else null
                }
        } catch (e: Exception) {
            Log.w(TAG, "display name lookup failed for $uri", e)
            null
        }

        // Decoded, because a fallback drawn from the URI text is still percent-encoded.
        val candidate = resolved
            ?: fallback.takeIf { it.isNotBlank() }?.let { Uri.decode(it) }
            ?: Uri.decode(uri.lastPathSegment.orEmpty())

        // File(..).name strips directory components: the name reaches us from outside
        // the app, and it is about to be joined onto cacheDir.
        val basename = File(candidate.substringAfterLast('/')).name.trim()
        return if (basename.isEmpty() || basename == "." || basename == "..") "import.bin" else basename
    }

    /**
     * Copy a SAF content URI into cacheDir and return the real path.
     *
     * The C core opens real filesystem paths, so a content URI has to be materialized
     * before it can be handed to the engine.
     *
     * @return absolute path, or an empty string on failure.
     */
    @JvmStatic
    fun copyContentUriToCache(context: Context, uriText: String, fileName: String): String {
        return try {
            val uri = Uri.parse(uriText)
            val target = File(context.cacheDir, displayNameFor(context, uri, fileName))
            context.contentResolver.openInputStream(uri).use { input ->
                if (input == null) {
                    return ""
                }
                target.outputStream().use { output -> input.copyTo(output) }
            }
            target.absolutePath
        } catch (e: Exception) {
            Log.e(TAG, "failed to copy $uriText", e)
            ""
        }
    }
}
