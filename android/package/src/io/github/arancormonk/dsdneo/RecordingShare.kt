// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.os.Environment
import android.util.Log
import androidx.core.content.FileProvider
import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption

/** #708: its own provider, so only the call recording folders (res/xml/recordings_paths.xml) are ever served. */
class RecordingsProvider : FileProvider()

/**
 * Where call recordings live, and sharing one (#708, D1): the Android half that DecoderHostAndroid calls over JNI.
 *
 * Recordings go to the app's folder on shared storage, which a computer sees over USB under
 * Android/data/<package>/files and which needs no permission, and fall back to the app's internal storage. Both roots
 * are always named, mounted or not, so the recording store can tell storage that is away from recordings deleted.
 * A share hands one recording, read-only, through [RecordingsProvider] to the app the user picks.
 */
object RecordingShare {
    private const val TAG = "dsd-neo"

    /** The folder in each root. Must match DecoderHost::recordingRoots() and res/xml/recordings_paths.xml. */
    const val FOLDER = "Call recordings"

    /** The provider's authority after the package name, as in AndroidManifest.xml. */
    const val AUTHORITY_SUFFIX = ".recordings"

    /** The recording roots, preferred first: shared storage, then internal storage. */
    @JvmStatic
    fun roots(context: Context): Array<String> = rootFolders(context).map(File::getPath).toTypedArray()

    /** Whether [path] is a recording a share may hand out: a .wav file, not a link, inside a root. */
    @JvmStatic
    fun canShare(context: Context, path: String): Boolean = sharable(path, rootFolders(context)) != null

    /** Offer the recording at [path] to another app under [title]; nothing outside the roots is ever offered. */
    @JvmStatic
    fun share(context: Context, path: String, title: String) {
        try {
            val file = sharable(path, rootFolders(context)) ?: return
            val uri = FileProvider.getUriForFile(context, context.packageName + AUTHORITY_SUFFIX, file)
            val send = Intent(Intent.ACTION_SEND).apply {
                type = "audio/wav"
                putExtra(Intent.EXTRA_STREAM, uri)
                putExtra(Intent.EXTRA_SUBJECT, title)
                clipData = ClipData.newRawUri(title, uri)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            context.startActivity(Intent.createChooser(send, title).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION)
            })
        } catch (e: Exception) {
            Log.w(TAG, "could not share a call recording", e)
        }
    }

    private fun rootFolders(context: Context): List<File> {
        // Null while shared storage is away; its path stays the one Android gives this app there.
        val external = context.getExternalFilesDir(null) ?: sharedStorageFiles(context)
        return listOf(File(external, FOLDER), File(context.filesDir, FOLDER))
    }

    @Suppress("DEPRECATION") // The path only, for a volume that is not mounted: nothing is read through it.
    private fun sharedStorageFiles(context: Context): File =
        File(Environment.getExternalStorageDirectory(), "Android/data/${context.packageName}/files")

    /**
     * [path] as a file to share when it is a regular .wav file, not a symbolic link, whose real path lies inside one
     * of [roots] (compared folder by folder, so "Call recordings 2" is not inside "Call recordings"); else null.
     */
    internal fun sharable(path: String, roots: List<File>): File? {
        val file = File(path)
        if (!file.isAbsolute || !file.name.endsWith(".wav", ignoreCase = true)) {
            return null
        }
        return try {
            if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                return null
            }
            val real = file.canonicalFile
            val inside = roots.any { root ->
                val folder = root.canonicalFile
                generateSequence(real.parentFile, File::getParentFile).any { it == folder }
            }
            if (inside) real else null
        } catch (e: Exception) {
            Log.w(TAG, "could not resolve a call recording's path", e)
            null
        }
    }
}
