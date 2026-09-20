// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.Manifest
import android.app.Activity
import android.content.Context
import android.graphics.Rect
import android.view.WindowInsets
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import android.util.Log
import android.util.TypedValue
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.view.WindowInsetsController
import java.io.File
import java.util.concurrent.atomic.AtomicInteger

/** Platform odds and ends the Qt host reaches through QJniObject. */
object AppSupport {
    private const val TAG = "dsd-neo"
    private const val REQUEST_NOTIFICATIONS = 4711
    fun ownsPermissionRequest(requestCode: Int): Boolean = requestCode == REQUEST_NOTIFICATIONS
    private val pendingBack = AtomicInteger(0)
    private val visibleKeyboardTop = AtomicInteger(-1)

    // Called on the Activity thread by its layout observer. Frame coordinates
    // are physical pixels; the Qt host converts them to window logical units.
    fun updateKeyboardBounds(activity: Activity) {
        val view = activity.window.decorView
        val frame = Rect()
        view.getWindowVisibleDisplayFrame(frame)
        val position = IntArray(2)
        view.getLocationOnScreen(position)
        val occluded = view.height + position[1] - frame.bottom
        val visible = if (Build.VERSION.SDK_INT >= 30)
            view.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) == true
        else occluded > view.height / 5
        visibleKeyboardTop.set(if (visible) frame.bottom - position[1] else -1)
    }

    @JvmStatic
    fun keyboardTopPixels(): Int = visibleKeyboardTop.get()


    fun requestBack() { pendingBack.incrementAndGet() }

    @JvmStatic
    fun takeBackRequests(): Int = pendingBack.getAndSet(0)

    @JvmStatic
    fun fontConfiguration(context: Context): String {
        val config = context.resources.configuration
        return "${config.fontScale}:${config.densityDpi}"
    }

    @JvmStatic
    fun fontPixels(context: Context, sp: Float): Float =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, sp, context.resources.displayMetrics)

    @JvmStatic
    fun setDarkAppearance(activity: Activity, dark: Boolean) {
        if (Build.VERSION.SDK_INT >= 30) {
            val mask = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS or
                WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS
            activity.window.insetsController?.setSystemBarsAppearance(if (dark) 0 else mask, mask)
        } else {
            @Suppress("DEPRECATION")
            val mask = android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR or
                android.view.View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
            @Suppress("DEPRECATION")
            activity.window.decorView.systemUiVisibility = if (dark)
                activity.window.decorView.systemUiVisibility and mask.inv()
            else activity.window.decorView.systemUiVisibility or mask
        }
    }

    @JvmStatic
    fun audioRouteName(context: Context, id: Int): String {
        if (id < 0) return "No device audio output"
        if (id == 0) return "System default"
        val manager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val device = manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull { it.id == id }
        return when (device?.type) {
            AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Speaker"
            AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "Earpiece"
            AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Wired headphones"
            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP, AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
            AudioDeviceInfo.TYPE_BLE_HEADSET, AudioDeviceInfo.TYPE_BLE_SPEAKER -> "Bluetooth"
            AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET -> "USB audio"
            else -> "System output"
        }
    }

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

    /** chan.csv, chan (2).csv, chan (3).csv … first name not already taken. */
    private fun uniqueTarget(dir: File, name: String): File? {
        var target = File(dir, name)
        if (!target.exists()) {
            return target
        }
        val dot = name.lastIndexOf('.')
        val base = if (dot > 0) name.substring(0, dot) else name
        val ext = if (dot > 0) name.substring(dot) else ""
        for (i in 2 until 1000) {
            target = File(dir, "$base ($i)$ext")
            if (!target.exists()) {
                return target
            }
        }
        return null
    }

    /**
     * Copy a SAF content URI into filesDir/imports and return the real path.
     *
     * Unlike copyContentUriToCache, this copy must outlive the session: saved
     * systems reference the returned path across restarts, and the engine
     * appends learned talkgroup rows to a group list in place, neither of which
     * survives cache eviction. Name collisions are unique-ified rather than
     * overwritten — two different documents may share a display name. A
     * non-empty replacePath that resolves inside the imports directory is
     * updated atomically instead (staging file + rename), so a half-copied CSV
     * is never observable; a replacePath outside it is not a write target and
     * falls back to a fresh copy.
     *
     * @return absolute path, or an empty string on failure.
     */
    @JvmStatic
    fun importDocumentToFiles(context: Context, uriText: String, fileName: String, replacePath: String): String {
        return try {
            val uri = Uri.parse(uriText)
            val importsDir = File(context.filesDir, "imports")
            importsDir.mkdirs()

            // A process death mid-copy skips the finally below, and filesDir is
            // not cache, so nothing else ever reclaims the staging file. Sweep
            // leftovers here rather than letting them accumulate forever.
            importsDir.listFiles { f -> f.isFile && f.name.startsWith(".import") && f.name.endsWith(".tmp") }
                ?.forEach { it.delete() }

            var target: File? = null
            if (replacePath.isNotEmpty()) {
                val candidate = File(replacePath)
                if (candidate.canonicalFile.parent == importsDir.canonicalPath) {
                    target = candidate
                }
            }
            if (target == null) {
                target = uniqueTarget(importsDir, displayNameFor(context, uri, fileName))
            }
            if (target == null) {
                return ""
            }

            val staging = File.createTempFile(".import", ".tmp", importsDir)
            try {
                context.contentResolver.openInputStream(uri).use { input ->
                    if (input == null) {
                        return ""
                    }
                    staging.outputStream().use { output -> input.copyTo(output) }
                }
                if (!staging.renameTo(target)) {
                    return ""
                }
            } finally {
                staging.delete() // no-op once the rename has landed
            }
            target.absolutePath
        } catch (e: Exception) {
            Log.e(TAG, "failed to import $uriText", e)
            ""
        }
    }
}
