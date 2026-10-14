// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.Manifest
import android.app.Activity
import android.content.ContentResolver
import android.content.Context
import android.graphics.Rect
import android.view.WindowInsets
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.util.Log
import android.util.TypedValue
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.view.WindowInsetsController
import java.io.File
import java.io.IOException
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/** Platform odds and ends the Qt host reaches through QJniObject. */
object AppSupport {
    private const val TAG = "dsd-neo"
    private const val REQUEST_NOTIFICATIONS = 4711
    fun ownsPermissionRequest(requestCode: Int): Boolean = requestCode == REQUEST_NOTIFICATIONS
    private val pendingBack = AtomicInteger(0)
    private val visibleKeyboardTop = AtomicInteger(-1)
    private val pendingNotificationPermissionRefresh = AtomicBoolean(true)

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

    // Activity callbacks invalidate the reading; the host publishes on the Qt poll thread.
    fun refreshNotificationPermission() { pendingNotificationPermissionRefresh.set(true) }

    @JvmStatic
    fun takeNotificationPermissionRefresh(): Boolean = pendingNotificationPermissionRefresh.getAndSet(false)

    @JvmStatic
    fun notificationPermissionNeeded(context: Context): Boolean =
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED

    /**
     * Ask for POST_NOTIFICATIONS on API 33+. The service runs without it, but its
     * notification stays invisible, which reads as "nothing happened".
     */
    @JvmStatic
    fun ensureNotificationPermission(activity: Activity) {
        if (!notificationPermissionNeeded(activity)) return
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
     * The C core opens real filesystem paths, so a content URI has to be
     * materialized before it can be handed to the engine. The copy must outlive
     * the session: saved systems reference the returned path across restarts,
     * and the engine appends learned talkgroup rows to a group list in place,
     * neither of which survives cache eviction. Name collisions are unique-ified rather than
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
                // App-internal storage already isolates this copy. Tightening file
                // permissions must not reject an otherwise usable SAF document.
                ownerOnly(staging)
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

    /*
     * Backup documents (#580). The two copies run on the Qt backup worker thread, never a main thread: the Qt side
     * calls them straight from that thread and must not wait on either main thread, which teardown may be blocking.
     * They go through the application context's resolver because the Activity may be finishing, and they check
     * DocumentTransfer's abort, which teardown raises.
     */

    /**
     * Copy the document [uriText] names to [destPath], at most [maxBytes] of it: a DocumentTransfer result.
     *
     * The copy lands in a private temp file beside [destPath] and is renamed over it only once complete, so no
     * failure, the cap included, leaves a partial file there. A size the provider declares above the cap is refused
     * before anything is read, sparing a cloud provider the download of a document that cannot fit; the cap still
     * counts the bytes actually read.
     */
    @JvmStatic
    fun copyDocumentToFile(context: Context, uriText: String, destPath: String, maxBytes: Long): Int {
        val directory = File(destPath).parentFile
        if (DocumentTransfer.isAborted || destPath.isEmpty() || maxBytes < 0 || directory == null) {
            return DocumentTransfer.FAILED
        }
        var staging: File? = null
        return try {
            val resolver = context.applicationContext.contentResolver
            val uri = Uri.parse(uriText)
            val declared = declaredSize(resolver, uri)
            if (declared != null && declared > maxBytes) {
                return DocumentTransfer.TOO_LARGE
            }
            val temp = File.createTempFile(".document", ".tmp", directory)
            staging = temp
            ownerOnly(temp)
            val copied = resolver.openInputStream(uri)?.use { input ->
                temp.outputStream().use { output -> DocumentTransfer.copyCapped(input, output, maxBytes) }
            } ?: return DocumentTransfer.FAILED
            when {
                copied < 0 -> DocumentTransfer.TOO_LARGE
                temp.renameTo(File(destPath)) -> DocumentTransfer.COPIED
                else -> DocumentTransfer.FAILED
            }
        } catch (e: Exception) {
            Log.w(TAG, "failed to copy a document", e)
            DocumentTransfer.FAILED
        } finally {
            staging?.delete() // A no-op once the rename has landed.
        }
    }

    /**
     * Write the file at [sourcePath] into the document [uriText] names; false on any failure.
     *
     * Mode "wt" first, so the document loses whatever it held; "w" only where a provider refuses "wt", trimmed to
     * the copy's length afterwards because "w" does not truncate everywhere. A failure once the document is open
     * deletes it, best effort, so a partial backup never looks like a good one. A failure before then leaves it as
     * it was: it may be an older document the user picked to replace.
     */
    @JvmStatic
    fun copyFileToDocument(context: Context, sourcePath: String, uriText: String): Boolean {
        if (DocumentTransfer.isAborted) {
            return false
        }
        val resolver = context.applicationContext.contentResolver
        val uri = Uri.parse(uriText)
        var opened = false
        return try {
            File(sourcePath).inputStream().use { input ->
                val (output, truncated) = openForReplace(resolver, uri)
                opened = true
                output.use {
                    val copied = DocumentTransfer.copyCapped(input, it, Long.MAX_VALUE)
                    if (!truncated) {
                        DocumentTransfer.trimTo(it, copied)
                    }
                    it.flush()
                }
            }
            true
        } catch (e: Exception) {
            Log.w(TAG, "failed to write a document", e)
            if (opened) {
                deletePartialDocument(resolver, uri)
            }
            false
        }
    }

    /**
     * Finish the app for a committed backup restore; called on the main thread.
     *
     * DsdNeoActivity.onDestroy then stops the engine and the service and ends the process, as for any other finish,
     * so the next launch applies the restore before anything reads the stores it replaces.
     */
    @JvmStatic
    fun finishApp(context: Context) {
        (context as? Activity)?.finishAndRemoveTask()
    }

    /** The size the provider reports for [uri], or null when it reports none. */
    private fun declaredSize(resolver: ContentResolver, uri: Uri): Long? = try {
        resolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { cursor ->
            val column = cursor.getColumnIndex(OpenableColumns.SIZE)
            if (column >= 0 && cursor.moveToFirst() && !cursor.isNull(column)) cursor.getLong(column) else null
        }
    } catch (e: Exception) {
        Log.w(TAG, "size lookup failed for a document", e)
        null
    }

    /** Readable and writable by the owner only, best effort: app-internal storage already isolates the file. */
    private fun ownerOnly(file: File) {
        file.setReadable(false, false)
        file.setWritable(false, false)
        file.setReadable(true, true)
        file.setWritable(true, true)
    }

    /** [uri] opened to replace its content, and whether opening already truncated it ("wt") or not ("w"). */
    private fun openForReplace(resolver: ContentResolver, uri: Uri): Pair<OutputStream, Boolean> {
        try {
            resolver.openOutputStream(uri, "wt")?.let { return it to true }
        } catch (e: Exception) {
            // Some providers refuse "wt" (FileNotFoundException, IllegalArgumentException or
            // UnsupportedOperationException, by provider); "w" is the mode every writable document takes.
            Log.i(TAG, "document provider refused mode wt; writing with w", e)
        }
        val output = resolver.openOutputStream(uri, "w") ?: throw IOException("the document provider gave no stream")
        return output to false
    }

    private fun deletePartialDocument(resolver: ContentResolver, uri: Uri) {
        try {
            if (!DocumentsContract.deleteDocument(resolver, uri)) {
                Log.w(TAG, "could not delete a partly written document")
            }
        } catch (e: Exception) {
            Log.w(TAG, "could not delete a partly written document", e)
        }
    }
}
