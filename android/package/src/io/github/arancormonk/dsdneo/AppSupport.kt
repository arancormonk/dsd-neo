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
            val safeName = if (fileName.isBlank()) "import.bin" else File(fileName).name
            val target = File(context.cacheDir, safeName)
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
