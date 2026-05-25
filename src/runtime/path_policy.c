// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/path_policy.h>

#include "dsd-neo/core/safe_api.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if DSD_PLATFORM_WIN_NATIVE
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#endif

static int
path_copy_checked(const char* src, char* out, size_t out_size) {
    if (!src || !out || out_size == 0) {
        errno = EINVAL;
        return -1;
    }
    int n = DSD_SNPRINTF(out, out_size, "%s", src);
    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int
dsd_path_expand_user(const char* requested, char* out, size_t out_size) {
    if (!requested || !out || out_size == 0 || requested[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    char expanded[2048];
    if (dsd_config_expand_path(requested, expanded, sizeof expanded) != 0 || expanded[0] == '\0') {
        errno = ENAMETOOLONG;
        return -1;
    }

    return path_copy_checked(expanded, out, out_size);
}

int
dsd_path_is_absolute(const char* path) {
    if (!path || path[0] == '\0') {
        return 0;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return 1;
    }
#if DSD_PLATFORM_WIN_NATIVE
    return ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':'
           && (path[2] == '\\' || path[2] == '/');
#else
    return 0;
#endif
}

static const char*
last_path_separator(const char* path) {
    const char* slash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    if (!slash) {
        return bslash;
    }
    if (!bslash) {
        return slash;
    }
    return slash > bslash ? slash : bslash;
}

int
dsd_path_resolve_relative_to_file(const char* base_file_path, const char* requested, char* out, size_t out_size) {
    char expanded[2048];
    if (dsd_path_expand_user(requested, expanded, sizeof expanded) != 0) {
        return -1;
    }

    if (dsd_path_is_absolute(expanded) || !base_file_path || base_file_path[0] == '\0' || base_file_path[0] == '<') {
        return path_copy_checked(expanded, out, out_size);
    }

    const char* sep = last_path_separator(base_file_path);
    if (!sep) {
        return path_copy_checked(expanded, out, out_size);
    }

    size_t dir_len = (size_t)(sep - base_file_path + 1);
    size_t path_len = strlen(expanded);
    if (dir_len + path_len + 1 > out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    DSD_MEMCPY(out, base_file_path, dir_len);
    DSD_MEMCPY(out + dir_len, expanded, path_len + 1);
    return 0;
}

#if DSD_PLATFORM_WIN_NATIVE
static int
path_reject_windows_reparse_or_directory(const char* path) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        errno = ENOENT;
        return -1;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
#endif

static int
path_open_read_fd(const char* resolved) {
#if DSD_PLATFORM_WIN_NATIVE
    if (path_reject_windows_reparse_or_directory(resolved) != 0) {
        return -1;
    }
    return _open(resolved, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    return open(resolved, flags);
#endif
}

static int
path_require_regular_file(int fd) {
    dsd_stat_t st;
    if (dsd_fstat(fd, &st) != 0) {
        errno = errno ? errno : EINVAL;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static FILE*
path_fdopen_read_binary(int fd) {
#if DSD_PLATFORM_WIN_NATIVE
    return _fdopen(fd, "rb");
#else
    return fdopen(fd, "rb");
#endif
}

FILE*
dsd_path_fopen_user_read_file(const char* requested, char* out, size_t out_size) {
    /*
     * This is the runtime's reviewed sink for local files selected explicitly by
     * CLI/env/UI/config. Do not call it for paths derived from protocol or
     * network payloads.
     */
    char resolved[2048];
    if (dsd_path_expand_user(requested, resolved, sizeof resolved) != 0) {
        return NULL;
    }

    int fd = path_open_read_fd(resolved);
    if (fd < 0) {
        return NULL;
    }

    if (path_require_regular_file(fd) != 0) {
        int saved_errno = errno;
        dsd_close(fd);
        errno = saved_errno;
        return NULL;
    }

    if (out && out_size > 0 && path_copy_checked(resolved, out, out_size) != 0) {
        int saved_errno = errno;
        dsd_close(fd);
        errno = saved_errno;
        return NULL;
    }

    FILE* fp = path_fdopen_read_binary(fd);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        dsd_close(fd);
        errno = saved_errno;
        return NULL;
    }

    return fp;
}

int
dsd_path_resolve_user_read_file(const char* requested, char* out, size_t out_size) {
    FILE* fp = dsd_path_fopen_user_read_file(requested, out, out_size);
    if (!fp) {
        return -1;
    }
    return fclose(fp);
}
