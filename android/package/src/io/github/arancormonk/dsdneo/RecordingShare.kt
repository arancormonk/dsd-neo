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
import java.io.IOException
import java.nio.file.Files
import java.nio.file.LinkOption
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import kotlin.math.abs

/**
 * #708: its own provider, so only the call recording folders and the share copies' cache folder
 * (res/xml/recordings_paths.xml) are ever served.
 */
class RecordingsProvider : FileProvider()

/**
 * Where call recordings live, and sharing one (#708, D1): the Android half that DecoderHostAndroid calls over JNI.
 *
 * Recordings go to the app's folder on shared storage, which a computer sees over USB under
 * Android/data/<package>/files and which needs no permission, and fall back to the app's internal storage. Both roots
 * are always named, mounted or not, so the recording store can tell storage that is away from recordings deleted.
 * A share hands a copy of one recording, read-only, through [RecordingsProvider] to the app the user picks: the
 * original stays under the store's retention and deletes, and the app it went to may open it long after the chooser.
 */
object RecordingShare {
    private const val TAG = "dsd-neo"

    /** The folder in each root. Must match DecoderHost::recordingRoots() and res/xml/recordings_paths.xml. */
    const val FOLDER = "Call recordings"

    /** The provider's authority after the package name, as in AndroidManifest.xml. */
    const val AUTHORITY_SUFFIX = ".recordings"

    /** The cache folder share copies go in, a folder each. Must match res/xml/recordings_paths.xml. */
    const val SHARE_FOLDER = "recording-shares"

    /** A share copy is kept this long (by its modification time), and removed at the next share or app start after. */
    internal const val SHARE_KEEP_MS = 24 * 60 * 60 * 1000L

    /** A copy a share in this process made is never removed sooner than this, whatever the wall clock says. */
    internal const val SHARE_HOLD_MS = 60 * 60 * 1000L

    /** The copies this process made, by folder name, with when they were made ([monotonicMs]). */
    private val madeHere = ConcurrentHashMap<String, Long>()

    /** The recording roots, preferred first: shared storage, then internal storage. */
    @JvmStatic
    fun roots(context: Context): Array<String> = rootFolders(context).map(File::getPath).toTypedArray()

    /** Whether [path] is a recording a share may hand out: a .wav file, not a link, inside a root. */
    @JvmStatic
    fun canShare(context: Context, path: String): Boolean = sharable(path, rootFolders(context)) != null

    /** Shares that failed after [canShare] said yes, by path, oldest first, until the host takes them. */
    private val failures = ConcurrentLinkedQueue<String>()

    /** The oldest share that failed after [canShare] said yes, or empty: DecoderHostAndroid polls it to tell the UI. */
    @JvmStatic
    fun takeFailure(): String = failures.poll() ?: ""

    /**
     * Copy the recording at [path] for a share, off the main thread (the host's transfer worker), and remove the share
     * copies that are past [SHARE_KEEP_MS]; nothing outside the roots is ever copied.
     *
     * The copy keeps the recording's name, in a folder of its own under [SHARE_FOLDER].
     * @return the copy's path; empty when it cannot be made (the recording went, the cache is full), and then [path]
     *     is queued for [takeFailure].
     */
    @JvmStatic
    fun stage(context: Context, path: String): String {
        try {
            val file = sharable(path, rootFolders(context)) ?: throw IllegalArgumentException("not a call recording")
            val shares = shareFolder(context)
            clean(shares, System.currentTimeMillis(), monotonicMs())
            return copyForShare(file, shares, monotonicMs()).path
        } catch (e: Exception) {
            Log.w(TAG, "could not copy a call recording to share it", e)
            failures.add(path)
            return ""
        }
    }

    /**
     * Offer [copy], which [stage] made of the recording at [path], to another app under [title], on the main thread.
     * Nothing but a share copy is ever offered.
     *
     * @return whether the chooser started. A share that cannot (the provider refuses the path, nothing can take it)
     *     removes its copy and queues [path] for [takeFailure], since the host hands it over without waiting.
     */
    @JvmStatic
    fun offer(context: Context, copy: String, path: String, title: String): Boolean {
        val file = File(copy)
        try {
            if (!isShareCopy(file, shareFolder(context))) {
                throw IllegalArgumentException("not a share copy")
            }
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
            return true
        } catch (e: Exception) {
            Log.w(TAG, "could not share a call recording", e)
            if (isShareCopy(file, shareFolder(context))) {
                madeHere.remove(file.parentFile.name)
                file.parentFile.deleteRecursively()
            }
            failures.add(path)
            return false
        }
    }

    /** Remove the share copies past [SHARE_KEEP_MS], at app start; off the main thread (the host's transfer worker). */
    @JvmStatic
    fun clean(context: Context) {
        try {
            clean(shareFolder(context), System.currentTimeMillis(), monotonicMs())
        } catch (e: Exception) {
            Log.w(TAG, "could not remove old call recording share copies", e)
        }
    }

    /**
     * Remove each share copy in [shares] whose modification time is more than [SHARE_KEEP_MS] from [wallNowMs] (either
     * way, so a clock set back does not keep it for good), except one this process made less than [SHARE_HOLD_MS]
     * before [monoNowMs]: the app it went to may still be opening it.
     */
    internal fun clean(shares: File, wallNowMs: Long, monoNowMs: Long) {
        for (entry in shares.listFiles() ?: return) {
            val made = madeHere[entry.name]
            if (made != null && monoNowMs - made < SHARE_HOLD_MS) {
                continue
            }
            if (abs(wallNowMs - entry.lastModified()) > SHARE_KEEP_MS) {
                entry.deleteRecursively()
                madeHere.remove(entry.name)
            }
        }
    }

    /** Copy [file] into a new folder of its own in [shares], made at [monoNowMs]; the copy keeps [file]'s name. */
    internal fun copyForShare(file: File, shares: File, monoNowMs: Long): File {
        if (!shares.isDirectory && !shares.mkdirs()) {
            throw IOException("cannot make ${shares.path}")
        }
        val folder = Files.createTempDirectory(shares.toPath(), "share-").toFile()
        madeHere[folder.name] = monoNowMs
        try {
            val copy = File(folder, file.name)
            Files.copy(file.toPath(), copy.toPath())
            return copy
        } catch (e: Exception) {
            madeHere.remove(folder.name)
            folder.deleteRecursively()
            throw e
        }
    }

    private fun shareFolder(context: Context): File = File(context.cacheDir, SHARE_FOLDER)

    /** Whether [file] is a regular file, not a link, one folder down in [shares]: where [copyForShare] puts copies. */
    private fun isShareCopy(file: File, shares: File): Boolean =
        file.isAbsolute && file.parentFile?.parentFile == shares &&
            Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)

    private fun monotonicMs(): Long = System.nanoTime() / 1_000_000

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
     *
     * Real paths come from toRealPath(), which resolves every link on the way (a linked folder included) on every
     * platform; getCanonicalFile() does not on some Windows JDKs. A root that is not there holds nothing to share.
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
            val real = file.toPath().toRealPath()
            val inside = roots.any { root ->
                val folder = try {
                    root.toPath().toRealPath()
                } catch (_: IOException) {
                    null
                }
                folder != null && real.parent?.startsWith(folder) == true
            }
            if (inside) real.toFile() else null
        } catch (e: Exception) {
            Log.w(TAG, "could not resolve a call recording's path", e)
            null
        }
    }
}
