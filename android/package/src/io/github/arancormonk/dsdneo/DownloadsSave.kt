// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.ContentResolver
import android.content.ContentValues
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.CancellationSignal
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
import java.util.concurrent.Callable
import java.util.concurrent.ExecutionException
import java.util.concurrent.Future
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicInteger

/**
 * The saves that go to Download/DSD-neo when no app takes CREATE_DOCUMENT (#829): the owner of the app's pending
 * MediaStore rows.
 *
 * [insert] makes a row with IS_PENDING set, which only this app sees; the write into it goes through
 * [DocumentCopies.beginWrite] as any document's does. The two write entries (DocumentCopies.copyFileToDocument() and
 * RecordingExport.transfer()) call [settle] at every exit: a complete write publishes the row (IS_PENDING cleared,
 * its final name read back, since MediaStore renames a clash then), anything else deletes it. A write that never
 * reaches them (the C++ side refused it, or the picker's request went) [discard]s it. Settling or discarding a row
 * first claims it, so one row is published or deleted, never both. A write [take]s its row at its start, and a discard
 * never claims a row a write holds; a discard that comes before the write took its row wins, and that write then fails
 * (the last rows discarded are remembered for it), so it never reports a file the discard deletes. A row left pending
 * by a crash or a teardown abort stays hidden until MediaStore deletes it at its expiry.
 *
 * Every provider call runs on one worker thread of its own; the lock guards only the in-memory rows and names and is
 * never held across one.
 */
object DownloadsSave {
    private const val TAG = "dsd-neo"
    private const val PUBLISHED_LIMIT = 16
    private const val DISCARDED_LIMIT = 16
    private const val WAIT_SLICE_MS = 100L
    private const val EXPIRY_SECONDS = 7L * 24 * 60 * 60

    // Android 11 and later set a pending row's expiry themselves; Android 10 sets none, but keeps the app's.
    private const val APP_SETS_EXPIRY = Build.VERSION_CODES.Q

    private val FOLDER = Environment.DIRECTORY_DOWNLOADS + "/DSD-neo"

    private val lock = Any()
    // The rows inserted and not settled: those no write has taken yet, and those a write holds.
    private val pending = HashSet<String>()
    private val writing = HashSet<String>()
    // The last rows a discard claimed, oldest first: a write that comes for one of them later fails.
    private val discarded = LinkedHashSet<String>()
    // The final names of the last rows published, oldest first, for savedDocumentName().
    private val published = LinkedHashMap<String, String>()

    @Volatile
    private var resolver: ContentResolver? = null

    /** Wall-clock seconds, for an Android 10 row's expiry; the tests set it. */
    internal var nowSeconds: () -> Long = { System.currentTimeMillis() / 1000 }

    /** The one thread every MediaStore call here runs on. */
    internal val worker = ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, LinkedBlockingQueue()) { task ->
        Thread(task, "dsd-downloads").apply { isDaemon = true }
    }

    /**
     * At host start: hold the application's [resolver] for the picker's inserts and the discards, and release every
     * persisted document grant the app still holds (#829 B7) on the worker. Qt's old file dialog took one on every
     * pick and never gave it back; nothing in the app uses one, and the platform caps how many an app keeps.
     */
    @JvmStatic
    fun start(resolver: ContentResolver) {
        this.resolver = resolver
        try {
            worker.execute { releaseGrants(resolver) }
        } catch (e: RejectedExecutionException) {
            Log.w(TAG, "could not release the leftover document grants", e)
        }
    }

    /** The picker's insert: [insert] on the worker, its answer to [onDone] there; "" at once with no resolver held. */
    fun insertLater(name: String, mime: String, onDone: (String) -> Unit) {
        val held = resolver ?: return onDone("")
        try {
            worker.execute { onDone(insert(held, name, mime)) }
        } catch (e: RejectedExecutionException) {
            Log.w(TAG, "could not make a row in the Download folder", e)
            onDone("")
        }
    }

    /** A new pending row named [name] of type [mime] in Download/DSD-neo: its URI, or "" when MediaStore made none. */
    internal fun insert(resolver: ContentResolver, name: String, mime: String): String {
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, name)
            put(MediaStore.MediaColumns.MIME_TYPE, mime)
            put(MediaStore.MediaColumns.RELATIVE_PATH, FOLDER)
            put(MediaStore.MediaColumns.IS_PENDING, 1)
            if (Build.VERSION.SDK_INT == APP_SETS_EXPIRY) {
                put(MediaStore.MediaColumns.DATE_EXPIRES, nowSeconds() + EXPIRY_SECONDS)
            }
        }
        val uri = try {
            resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)?.toString().orEmpty()
        } catch (e: Exception) {
            Log.w(TAG, "could not make a row in the Download folder", e)
            ""
        }
        if (uri.isNotEmpty()) {
            synchronized(lock) { pending += uri }
        }
        return uri
    }

    /**
     * A write into [uriText] begins: a pending row is the write's from now on, for [settle], never for [discard].
     *
     * @return false when a discard already took the row: the write must fail, since its file is being deleted.
     */
    fun take(uriText: String): Boolean = synchronized(lock) {
        if (pending.remove(uriText)) {
            writing += uriText
        }
        uriText !in discarded
    }

    /** Whether [uriText] is a row [insert] made that is not settled yet: its write must leave it to [settle]. */
    fun isPending(uriText: String): Boolean = synchronized(lock) { uriText in pending || uriText in writing }

    /** The name a published row [uriText] got, or "" when none of the last ones published is it. No I/O. */
    @JvmStatic
    fun publishedName(uriText: String): String = synchronized(lock) { published[uriText] } ?: ""

    /**
     * Delete [uriText] on the worker when it is a row still pending that no write took, without waiting: nothing will
     * write it. From any thread; never blocks.
     */
    @JvmStatic
    fun discard(uriText: String) {
        val held = resolver ?: return
        val claimed = synchronized(lock) {
            pending.remove(uriText).also { if (it) bounded(discarded, uriText) }
        }
        if (!claimed) {
            return
        }
        try {
            worker.execute { delete(held, Uri.parse(uriText)) }
        } catch (e: RejectedExecutionException) {
            Log.w(TAG, "could not delete an unused row in the Download folder", e)
        }
    }

    /**
     * End the write into [uriText]: publish its row when [ok] (the bytes are all written and the document closed),
     * else delete it. A document that is no pending row is left alone, unless a discard took it (see [take]).
     *
     * The work runs on the worker, and the caller waits for it in slices: it gives up at the teardown abort, or when
     * [signal] (the write's own) is cancelled before the publication began, and the row is then deleted instead. A
     * cancel that comes once the publication began is too late: the document is saved.
     *
     * @return whether the document stands: true when its row was published, or when it is no row of this app's and no
     *     discard took it.
     */
    fun settle(resolver: ContentResolver, uriText: String, ok: Boolean, signal: CancellationSignal?): Boolean {
        if (!claim(uriText)) {
            return synchronized(lock) { uriText !in discarded }
        }
        val settlement = Settlement()
        val task = try {
            worker.submit(Callable { finish(resolver, Uri.parse(uriText), ok, signal, settlement) })
        } catch (e: RejectedExecutionException) {
            // No worker to run it on: the row this settle claimed is deleted here, so it is not left pending.
            Log.w(TAG, "could not settle a row in the Download folder", e)
            delete(resolver, Uri.parse(uriText))
            return false
        }
        return await(task, settlement, signal)
    }

    /** How many rows are not settled; the tests check that none leaks. */
    internal fun pendingCount(): Int = synchronized(lock) { pending.size + writing.size }

    /** Whether a settle may still publish its row; the worker and the waiting writer race for it. */
    private class Settlement {
        private val state = AtomicInteger(WAITING)

        /** The worker's start of a publication; false once the writer gave up. */
        fun publish() = state.compareAndSet(WAITING, PUBLISHING)

        /** The writer gives up; false once the publication began. */
        fun abandon() = state.compareAndSet(WAITING, ABANDONED)

        private companion object {
            const val WAITING = 0
            const val PUBLISHING = 1
            const val ABANDONED = 2
        }
    }

    /** An interrupt counts as a cancel: it ends the wait unless the publication began, and stays raised. */
    private fun await(task: Future<Boolean>, settlement: Settlement, signal: CancellationSignal?): Boolean {
        var interrupted = false
        try {
            while (true) {
                try {
                    return task.get(WAIT_SLICE_MS, TimeUnit.MILLISECONDS)
                } catch (_: TimeoutException) {
                    // Still running, or still queued behind another row's call.
                } catch (e: ExecutionException) {
                    Log.w(TAG, "a Download folder row failed to settle", e)
                    return false
                } catch (_: InterruptedException) {
                    interrupted = true
                }
                if (DocumentTransfer.isAborted) {
                    // Teardown: whatever the worker is doing goes on without this thread.
                    settlement.abandon()
                    return false
                }
                if ((interrupted || signal?.isCanceled == true) && settlement.abandon()) {
                    return false
                }
            }
        } finally {
            if (interrupted) {
                Thread.currentThread().interrupt()
            }
        }
    }

    /** On the worker: publish [uri] when the write is to stand and the writer still waits, else delete it. */
    private fun finish(
        resolver: ContentResolver,
        uri: Uri,
        ok: Boolean,
        signal: CancellationSignal?,
        settlement: Settlement,
    ): Boolean {
        if (ok && !DocumentTransfer.isAborted && signal?.isCanceled != true && settlement.publish()) {
            val name = publish(resolver, uri)
            if (name != null) {
                remember(uri.toString(), name)
                return true
            }
        }
        delete(resolver, uri)
        return false
    }

    /** Clear [uri]'s IS_PENDING (and Android 10's expiry), then read its final name back; null on any failure. */
    private fun publish(resolver: ContentResolver, uri: Uri): String? = try {
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.IS_PENDING, 0)
            if (Build.VERSION.SDK_INT == APP_SETS_EXPIRY) {
                putNull(MediaStore.MediaColumns.DATE_EXPIRES)
            }
        }
        if (resolver.update(uri, values, null, null) == 1) displayName(resolver, uri) else null
    } catch (e: Exception) {
        Log.w(TAG, "could not publish a row in the Download folder", e)
        null
    }

    private fun displayName(resolver: ContentResolver, uri: Uri): String? =
        resolver.query(uri, arrayOf(MediaStore.MediaColumns.DISPLAY_NAME), null, null, null, null)?.use { cursor ->
            val column = cursor.getColumnIndex(MediaStore.MediaColumns.DISPLAY_NAME)
            if (column >= 0 && cursor.moveToFirst() && !cursor.isNull(column)) cursor.getString(column) else null
        }?.takeIf { it.isNotEmpty() }

    private fun delete(resolver: ContentResolver, uri: Uri) {
        try {
            if (resolver.delete(uri, null, null) != 1) {
                Log.w(TAG, "could not delete a row in the Download folder")
            }
        } catch (e: Exception) {
            Log.w(TAG, "could not delete a row in the Download folder", e)
        }
    }

    private fun claim(uriText: String): Boolean = synchronized(lock) {
        writing.remove(uriText) || pending.remove(uriText)
    }

    private fun remember(uriText: String, name: String) = synchronized(lock) {
        published.remove(uriText)
        published[uriText] = name
        while (published.size > PUBLISHED_LIMIT) {
            published.remove(published.keys.first())
        }
    }

    /** Add [uriText] to [set] as its newest, dropping the oldest past [DISCARDED_LIMIT]; under [lock]. */
    private fun bounded(set: LinkedHashSet<String>, uriText: String) {
        set.remove(uriText)
        set += uriText
        while (set.size > DISCARDED_LIMIT) {
            set.remove(set.first())
        }
    }

    /** Release each persisted grant in the modes it holds; one that fails does not stop the rest. */
    internal fun releaseGrants(resolver: ContentResolver): Int {
        val grants = try {
            resolver.persistedUriPermissions
        } catch (e: Exception) {
            Log.w(TAG, "could not list the persisted document grants", e)
            return 0
        }
        var released = 0
        for (grant in grants) {
            val modes = (if (grant.isReadPermission) Intent.FLAG_GRANT_READ_URI_PERMISSION else 0) or
                (if (grant.isWritePermission) Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
            try {
                resolver.releasePersistableUriPermission(grant.uri, modes)
                released++
            } catch (e: Exception) {
                Log.w(TAG, "could not release a persisted document grant", e)
            }
        }
        Log.i(TAG, "released $released of ${grants.size} persisted document grants")
        return released
    }
}
