// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/file_compat.h>

#if DSD_PLATFORM_WIN_NATIVE

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

int
dsd_fileno(FILE* fp) {
    return fp ? _fileno(fp) : -1;
}

int
dsd_isatty(int fd) {
    return _isatty(fd);
}

int
dsd_dup(int oldfd) {
    return _dup(oldfd);
}

int
dsd_dup2(int oldfd, int newfd) {
    return _dup2(oldfd, newfd);
}

int
dsd_close(int fd) {
    return _close(fd);
}

int
dsd_fsync(int fd) {
    /* _commit is Windows equivalent of fsync */
    return _commit(fd);
}

int
dsd_fstat(int fd, dsd_stat_t* st) {
    return _fstat(fd, st);
}

int
dsd_fchmod(int fd, int mode) {
    (void)fd;
    (void)mode;
    /* Windows lacks descriptor-based chmod; treat as no-op */
    return 0;
}

static int
dsd_private_open_flags(const char* mode) {
    if (!mode || mode[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    int flags = _O_NOINHERIT;
    switch (mode[0]) {
        case 'w': flags |= _O_CREAT | _O_TRUNC; break;
        case 'a': flags |= _O_CREAT | _O_APPEND; break;
        default: errno = EINVAL; return -1;
    }

    flags |= (strchr(mode, '+') != NULL) ? _O_RDWR : _O_WRONLY;
    flags |= (strchr(mode, 'b') != NULL) ? _O_BINARY : _O_TEXT;
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

    int fd = _open(path, flags, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        return NULL;
    }

    FILE* fp = _fdopen(fd, mode);
    if (!fp) {
        _close(fd);
    }
    return fp;
}

ssize_t
dsd_read(int fd, void* buf, size_t count) {
    return (ssize_t)_read(fd, buf, (unsigned int)count);
}

ssize_t
dsd_write(int fd, const void* buf, size_t count) {
    return (ssize_t)_write(fd, buf, (unsigned int)count);
}

const char*
dsd_null_device(void) {
    return "NUL";
}

#endif /* DSD_PLATFORM_WIN_NATIVE */
