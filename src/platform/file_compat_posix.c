// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/file_compat.h>
#include <stdio.h>
#include <sys/types.h>

#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int
dsd_fileno(FILE* fp) {
    return fp ? fileno(fp) : -1;
}

int
dsd_isatty(int fd) {
    return isatty(fd);
}

int
dsd_dup(int oldfd) {
    return dup(oldfd);
}

int
dsd_dup2(int oldfd, int newfd) {
    return dup2(oldfd, newfd);
}

int
dsd_close(int fd) {
    return close(fd);
}

int
dsd_fsync(int fd) {
    return fsync(fd);
}

int
dsd_fstat(int fd, dsd_stat_t* st) {
    return fstat(fd, st);
}

int
dsd_fchmod(int fd, int mode) {
    return fchmod(fd, (mode_t)mode);
}

static int
dsd_private_open_flags(const char* mode) {
    if (!mode || mode[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    int flags = 0;
    switch (mode[0]) {
        case 'w': flags = O_CREAT | O_TRUNC; break;
        case 'a': flags = O_CREAT | O_APPEND; break;
        default: errno = EINVAL; return -1;
    }

    flags |= (strchr(mode, '+') != NULL) ? O_RDWR : O_WRONLY;
    return flags;
}

FILE*
dsd_fopen_private(const char* path, const char* mode) {
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }
    if (mode[0] == 'r') {
        return fopen(path, mode);
    }

    int flags = dsd_private_open_flags(mode);
    if (flags < 0) {
        return NULL;
    }

    int fd = open(path, flags, (mode_t)0600);
    if (fd < 0) {
        return NULL;
    }

    FILE* fp = fdopen(fd, mode);
    if (!fp) {
        close(fd);
    }
    return fp;
}

ssize_t
dsd_read(int fd, void* buf, size_t count) {
    return read(fd, buf, count);
}

ssize_t
dsd_write(int fd, const void* buf, size_t count) {
    return write(fd, buf, count);
}

const char*
dsd_null_device(void) {
    return "/dev/null";
}

#endif /* !DSD_PLATFORM_WIN_NATIVE */
