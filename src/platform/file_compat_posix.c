// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <stdio.h>
#include <sys/types.h>

#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE

#include <dirent.h>
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
dsd_stat_path(const char* path, dsd_stat_t* st) {
    return stat(path, st);
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

static int
dsd_local_file_name_is_unsafe(const char* requested) {
    return requested == NULL || requested[0] == '\0' || requested[0] == '/' || strchr(requested, '/') != NULL
           || strchr(requested, '\\') != NULL || strstr(requested, "..") != NULL;
}

static int
dsd_copy_resolved_local_name(const char* name, char* out, size_t out_size) {
    int n = DSD_SNPRINTF(out, out_size, "%s", name);
    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static FILE*
dsd_open_resolved_local_entry(const char* name, char* out, size_t out_size, int* saved_errno) {
    if (dsd_copy_resolved_local_name(name, out, out_size) != 0) {
        *saved_errno = errno;
        return NULL;
    }

    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(out, flags);
    if (fd < 0) {
        *saved_errno = errno ? errno : EINVAL;
        return NULL;
    }

    dsd_stat_t st;
    if (fstat(fd, &st) != 0) {
        *saved_errno = errno ? errno : EINVAL;
        close(fd);
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        *saved_errno = EINVAL;
        close(fd);
        return NULL;
    }

    FILE* result = fdopen(fd, "r");
    if (!result) {
        *saved_errno = errno ? errno : EINVAL;
        close(fd);
        return NULL;
    }
    *saved_errno = 0;
    return result;
}

FILE*
dsd_fopen_existing_local_file(const char* requested, char* out, size_t out_size) {
    if (!out || out_size == 0 || dsd_local_file_name_is_unsafe(requested)) {
        errno = EINVAL;
        return NULL;
    }
    out[0] = '\0';

    DIR* dir = opendir(".");
    if (!dir) {
        return NULL;
    }

    FILE* result = NULL;
    int saved_errno = ENOENT;
    int found = 0;
    errno = 0;
    const struct dirent* ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, requested) != 0) {
            continue;
        }
        found = 1;
        result = dsd_open_resolved_local_entry(ent->d_name, out, out_size, &saved_errno);
        break;
    }
    if (!found && errno != 0) {
        saved_errno = errno;
    }
    if (closedir(dir) != 0 && result) {
        int close_errno = errno;
        fclose(result);
        errno = close_errno;
        return NULL;
    }
    errno = saved_errno;
    return result;
}

int
dsd_resolve_existing_local_file(const char* requested, char* out, size_t out_size) {
    FILE* fp = dsd_fopen_existing_local_file(requested, out, out_size);
    if (!fp) {
        return -1;
    }
    return fclose(fp);
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
