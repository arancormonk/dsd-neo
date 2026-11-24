// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * File identities (file_compat.h) on POSIX, in a file of their own: the project does not build with 64-bit file offsets
 * everywhere, and on a 32-bit build without them lstat() fails with EOVERFLOW for a file over 2 GiB, which a recording
 * can be, or dated past 2038. src/platform/CMakeLists.txt compiles this file alone with _FILE_OFFSET_BITS=64 and
 * _TIME_BITS=64 (definitions on the command line, so they come before the forced safe_api.h include and every system
 * header). No struct stat, off_t or time_t leaves it, so nothing else sees another layout.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "file_compat_internal.h"

_Static_assert(sizeof(off_t) >= 8, "file identities need 64-bit file offsets: a recording can pass 2 GiB");
/* Time too: 32-bit glibc 2.34 and later widen time_t with _TIME_BITS=64 (CMakeLists.txt), so a file dated past 2038 is
 * read. An older glibc, and bionic on armv7, have no 64-bit time on a 32-bit build: there such a file's stat fails
 * (EOVERFLOW) or wraps its time, and the store keeps the recording (its name reads as unreadable) without blocking
 * anything else. */
#if __SIZEOF_POINTER__ >= 8 || (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)))
_Static_assert(sizeof(time_t) >= 8, "file identities need 64-bit time where the C library offers it");
#endif

/* Whether the folder @p path names is in is there and a folder itself, never followed if a link (lstat()). A bare
   name's folder is the current one. */
static int
dsd_parent_is_folder(const char* path) {
    const char* slash = strrchr(path, '/');
    if (slash == NULL) {
        return 1;
    }
    const size_t cut = slash == path ? 1U : (size_t)(slash - path);
    char* parent = (char*)malloc(cut + 1U);
    if (parent == NULL) {
        return 0;
    }
    DSD_MEMCPY(parent, path, cut);
    parent[cut] = '\0';
    struct stat st;
    const int folder = lstat(parent, &st) == 0 && S_ISDIR(st.st_mode);
    free(parent);
    return folder;
}

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
        // lstat()'s own ELOOP comes from resolving a parent (too many links on the way): the name's type was never
        // read, so it is reported apart from a link at the name itself (ELOOP below), as a lookup that failed. ENOENT
        // is a missing name only when its folder is there and a folder: a parent that is a dangling link, or missing,
        // never looked the name up (ENOTDIR).
        const int err = errno;
        errno = err == ELOOP ? EMLINK : (err == ENOENT && !dsd_parent_is_folder(path) ? ENOTDIR : err);
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
dsd_fd_regular_file_internal(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int
dsd_file_identity_equal(const dsd_file_identity_t* a, const dsd_file_identity_t* b) {
    return a != NULL && b != NULL && a->kind != 0 && b->kind != 0 && a->size == b->size && a->mtime_s == b->mtime_s
           && a->mtime_ns == b->mtime_ns;
}

#endif /* !DSD_PLATFORM_WIN_NATIVE */
