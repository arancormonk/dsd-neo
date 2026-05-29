// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
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
dsd_stat_path(const char* path, dsd_stat_t* st) {
    return _stat(path, st);
}

int
dsd_stat_is_regular(const dsd_stat_t* st) {
    return st && ((st->st_mode & _S_IFMT) == _S_IFREG);
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

static int
dsd_existing_regular_mode_flags(const char* mode, int* out_flags) {
    if (!mode || mode[0] != 'r' || strchr(mode, '+') != NULL || !out_flags) {
        errno = EINVAL;
        return -1;
    }
    *out_flags = _O_RDONLY | _O_NOINHERIT | ((strchr(mode, 'b') != NULL) ? _O_BINARY : _O_TEXT);
    return 0;
}

static int
dsd_existing_regular_attrs_ok(const char* path) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0
        || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int
dsd_open_existing_regular_fd_win32(const char* path, const char* mode) {
    if (!path || path[0] == '\0' || !mode || mode[0] != 'r' || strchr(mode, '+') != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (dsd_existing_regular_attrs_ok(path) != 0) {
        return -1;
    }

    int flags = 0;
    if (dsd_existing_regular_mode_flags(mode, &flags) != 0) {
        return -1;
    }
    int fd = _open(path, flags);
    if (fd < 0) {
        return -1;
    }

    dsd_stat_t st;
    if (_fstat(fd, &st) != 0) {
        int saved_errno = errno ? errno : EINVAL;
        _close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!dsd_stat_is_regular(&st)) {
        _close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

FILE*
dsd_fopen_existing_regular_file(const char* path, const char* mode) {
    int fd = dsd_open_existing_regular_fd_win32(path, mode);
    if (fd < 0) {
        return NULL;
    }

    FILE* fp = _fdopen(fd, mode);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        _close(fd);
        errno = saved_errno;
    }
    return fp;
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
    if (_mktemp_s(tmp_path, tmp_path_size) != 0) {
        errno = EEXIST;
        return NULL;
    }

    int flags = dsd_private_open_flags(mode);
    if (flags < 0) {
        return NULL;
    }
    flags |= _O_EXCL;

    int fd = _open(tmp_path, flags, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        return NULL;
    }

    FILE* fp = _fdopen(fd, mode);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        _close(fd);
        (void)remove(tmp_path);
        errno = saved_errno;
        return NULL;
    }
    return fp;
}

int
dsd_replace_file_with_temp(const char* tmp_path, const char* final_path) {
    if (!tmp_path || tmp_path[0] == '\0' || !final_path || final_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (!MoveFileExA(tmp_path, final_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static int
dsd_local_file_name_is_unsafe(const char* requested) {
    return requested == NULL || requested[0] == '\0' || requested[0] == '/' || requested[0] == '\\'
           || strchr(requested, '/') != NULL || strchr(requested, '\\') != NULL || strstr(requested, "..") != NULL;
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
dsd_open_resolved_local_entry(const struct _finddata_t* data, char* out, size_t out_size, int* saved_errno) {
    if ((data->attrib & _A_SUBDIR) != 0) {
        *saved_errno = EINVAL;
        return NULL;
    }
    DWORD attrs = GetFileAttributesA(data->name);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        *saved_errno = EINVAL;
        return NULL;
    }
    if (dsd_copy_resolved_local_name(data->name, out, out_size) != 0) {
        *saved_errno = errno;
        return NULL;
    }

    FILE* result = fopen(out, "r");
    *saved_errno = result ? 0 : (errno ? errno : EINVAL);
    return result;
}

FILE*
dsd_fopen_existing_local_file(const char* requested, char* out, size_t out_size) {
    if (!out || out_size == 0 || dsd_local_file_name_is_unsafe(requested)) {
        errno = EINVAL;
        return NULL;
    }
    out[0] = '\0';

    struct _finddata_t data;
    intptr_t handle = _findfirst("*", &data);
    if (handle == -1) {
        return NULL;
    }

    FILE* result = NULL;
    int saved_errno = ENOENT;
    do {
        if (strcmp(data.name, requested) != 0) {
            continue;
        }
        result = dsd_open_resolved_local_entry(&data, out, out_size, &saved_errno);
        break;
    } while (_findnext(handle, &data) == 0);

    if (_findclose(handle) != 0 && result) {
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
