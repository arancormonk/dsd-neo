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
#include <stdlib.h>
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
dsd_stat_is_regular(const dsd_stat_t* st) {
    return st && S_ISREG(st->st_mode);
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

FILE*
dsd_fopen_existing_regular_file(const char* path, const char* mode) {
    if (!path || path[0] == '\0' || !mode || mode[0] != 'r' || strchr(mode, '+') != NULL) {
        errno = EINVAL;
        return NULL;
    }

    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        return NULL;
    }

    dsd_stat_t st;
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        errno = saved_errno;
        return NULL;
    }
    if (!dsd_stat_is_regular(&st)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }

    FILE* fp = fdopen(fd, mode);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        errno = saved_errno;
    }
    return fp;
}

static void
dsd_set_cloexec_best_effort(int fd) {
#ifdef FD_CLOEXEC
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
#else
    (void)fd;
#endif
}

FILE*
dsd_fopen_private_temp_for_replace(const char* final_path, char* tmp_path, size_t tmp_path_size, const char* mode) {
    if (!final_path || final_path[0] == '\0' || !tmp_path || tmp_path_size == 0 || !mode || mode[0] == 'r') {
        errno = EINVAL;
        return NULL;
    }

    int n = DSD_SNPRINTF(tmp_path, tmp_path_size, "%s.tmp.XXXXXX", final_path);
    if (n < 0 || (size_t)n >= tmp_path_size) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        return NULL;
    }
    dsd_set_cloexec_best_effort(fd);
    (void)fchmod(fd, (mode_t)0600);

    FILE* fp = fdopen(fd, mode);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        (void)remove(tmp_path);
        errno = saved_errno;
        return NULL;
    }
    return fp;
}

static void
dsd_fsync_parent_dir_best_effort(const char* path) {
    if (!path || path[0] == '\0') {
        return;
    }

    const char* slash = strrchr(path, '/');
    char dir_buf[4096];
    const char* dir = ".";
    if (slash && slash != path) {
        size_t len = (size_t)(slash - path);
        if (len >= sizeof(dir_buf)) {
            return;
        }
        DSD_MEMCPY(dir_buf, path, len);
        dir_buf[len] = '\0';
        dir = dir_buf;
    } else if (slash == path) {
        dir = "/";
    }

    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(dir, flags);
    if (fd >= 0) {
        (void)fsync(fd);
        (void)close(fd);
    }
}

int
dsd_replace_file_with_temp(const char* tmp_path, const char* final_path) {
    if (!tmp_path || tmp_path[0] == '\0' || !final_path || final_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (rename(tmp_path, final_path) != 0) {
        return -1;
    }
    dsd_fsync_parent_dir_best_effort(final_path);
    return 0;
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
    if (!dsd_stat_is_regular(&st)) {
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
