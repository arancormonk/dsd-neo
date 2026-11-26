// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import java.io.File
import java.io.FileDescriptor

/**
 * Opens a call recording the store listed and checks it is still that file (#708 export, #909 playback): what is read
 * is what was checked. RecordingExport reads through it, and RecordingPlayback hands its descriptor to the player.
 */
internal object RecordingDescriptor {
    /** What [open] found. */
    sealed class Result {
        /** The file, open read-only; the caller closes [fd] (see [close]). */
        class Opened(val fd: FileDescriptor) : Result()

        /** No file at the name, in a folder that is there. */
        object Gone : Result()

        /** Another file, a link, or a name that cannot be opened or read: not the file listed, as far as is known. */
        object Changed : Result()
    }

    /**
     * The recording at [path] opened once to read, never through a link at its name (O_NOFOLLOW; O_NONBLOCK, so a FIFO
     * never holds the open), and the file opened checked on its descriptor to be a regular file with [size] bytes and
     * the modification time [seconds].[nanos].
     */
    fun open(path: String, size: Long, seconds: Long, nanos: Long): Result {
        val fd = try {
            val flags = OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_NONBLOCK or
                OsConstants.O_CLOEXEC
            Os.open(path, flags, 0)
        } catch (e: ErrnoException) {
            // Gone only when the name is not in its folder and that folder is there (a parent that is a dangling link
            // never looked the name up). A link (ELOOP), a parent that does not resolve (ENOTDIR), no access to it, an
            // I/O error: not the file listed as far as can be told, and the store looks again.
            return if (e.errno == OsConstants.ENOENT && parentIsFolder(path)) Result.Gone else Result.Changed
        }
        val same = try {
            val stat = Os.fstat(fd)
            OsConstants.S_ISREG(stat.st_mode) && stat.st_size == size &&
                stat.st_mtim.tv_sec == seconds && stat.st_mtim.tv_nsec == nanos
        } catch (e: ErrnoException) {
            false
        }
        if (!same) {
            close(fd)
            return Result.Changed
        }
        return Result.Opened(fd)
    }

    /** Close [fd], quietly: a read-only descriptor has nothing to lose. */
    fun close(fd: FileDescriptor) {
        try {
            Os.close(fd)
        } catch (e: ErrnoException) {
            // Already closed, or the close failed.
        }
    }

    /** Whether the folder [path] is in is there and a folder, never followed if a link. */
    private fun parentIsFolder(path: String): Boolean {
        val parent = File(path).parent ?: return true
        return try {
            OsConstants.S_ISDIR(Os.lstat(parent).st_mode)
        } catch (e: ErrnoException) {
            false
        }
    }
}
