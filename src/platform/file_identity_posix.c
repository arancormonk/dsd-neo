// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * File identities (file_compat.h) on POSIX, in a file of their own: the project does not build with 64-bit file offsets
 * everywhere, and on a 32-bit build without them lstat() fails with EOVERFLOW for a file over 2 GiB, which a recording
 * can be. Defined before any system header, for this file only: no struct stat leaves it, so nothing else sees another
 * layout.
 */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE

#include <errno.h>
#include <sys/stat.h>

_Static_assert(sizeof(off_t) >= 8, "file identities need 64-bit file offsets: a recording can pass 2 GiB");

/* The identity in @p st, a regular file's; -1 with errno for anything else (a folder, a link, a FIFO, a socket, a
   device). */
static int
dsd_stat_file_identity(const struct stat* st, dsd_file_identity_t* out) {
    if (S_ISDIR(st->st_mode)) {
        errno = EISDIR;
        return -1;
    }
    if (S_ISLNK(st->st_mode)) {
        errno = ELOOP;
        return -1;
    }
    if (!S_ISREG(st->st_mode)) {
        /* Read, and known to be no file: a FIFO, a socket or a device. ENODEV, never EIO, which a failed read gives. */
        errno = ENODEV;
        return -1;
    }
    out->kind = 1;
    out->size = (int64_t)st->st_size;
#ifdef __APPLE__
    out->mtime_s = (int64_t)st->st_mtimespec.tv_sec;
    out->mtime_ns = (int32_t)st->st_mtimespec.tv_nsec;
#else
    out->mtime_s = (int64_t)st->st_mtim.tv_sec;
    out->mtime_ns = (int32_t)st->st_mtim.tv_nsec;
#endif
    return 0;
}

int
dsd_path_file_identity(const char* path, dsd_file_identity_t* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof *out);
    }
    if (!path || path[0] == '\0' || !out) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) != 0) {
        return -1;
    }
    return dsd_stat_file_identity(&st, out);
}

int
dsd_file_stream_identity(FILE* fp, dsd_file_identity_t* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof *out);
    }
    const int fd = fp ? fileno(fp) : -1;
    if (fd < 0 || !out) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    return dsd_stat_file_identity(&st, out);
}

int
dsd_file_identity_equal(const dsd_file_identity_t* a, const dsd_file_identity_t* b) {
    return a != NULL && b != NULL && a->kind != 0 && b->kind != 0 && a->size == b->size && a->mtime_s == b->mtime_s
           && a->mtime_ns == b->mtime_ns;
}

#endif /* !DSD_PLATFORM_WIN_NATIVE */
