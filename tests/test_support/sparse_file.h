// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

/*
 * Large sparse files for tests of 64-bit file sizes and offsets. Kept out of test_support.h so that only the tests
 * that need it pull in <windows.h>.
 */

#include <dsd-neo/platform/platform.h>

#include <errno.h>
#include <stdint.h>

#if DSD_PLATFORM_WIN_NATIVE
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Grow the open file @p fd to @p size bytes as a sparse file, writing none of the bytes between.
 *
 * Fails where the filesystem cannot hold a sparse file of that size, or (POSIX) where the file did not stay sparse,
 * in which case the file is cut back to 0 bytes. A test skips its large-file cases on failure.
 *
 * @return 0 on success, -1 with errno set otherwise.
 */
static inline int
dsd_test_grow_sparse(int fd, int64_t size) {
    if (fd < 0 || size < 0) {
        errno = EINVAL;
        return -1;
    }
#if DSD_PLATFORM_WIN_NATIVE
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    DWORD returned = 0;
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL)) {
        errno = ENOSYS;
        return -1;
    }
    if (_chsize_s(fd, (__int64)size) != 0) {
        return -1;
    }
    return 0;
#else
    if ((int64_t)(off_t)size != size) {
        errno = EOVERFLOW;
        return -1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    /* A filesystem without sparse files allocates the whole size: give the space back and skip. */
    if ((int64_t)st.st_blocks * 512 >= size) {
        if (ftruncate(fd, 0) != 0) {
            /* Skip either way: a failed shrink leaves only the test's own temp file large. */
        }
        errno = ENOSPC;
        return -1;
    }
    return 0;
#endif
}

#ifdef __cplusplus
}
#endif
