// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/utf16.h>
#include <dsd-neo/platform/file_compat.h>

#if DSD_PLATFORM_WIN_NATIVE

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <wchar.h>
#include <windows.h>

#include "win32_temp_name_internal.h"
#include "write_frames_internal.h"

/*
 * The CRT treats a negative descriptor as a caller bug: it raises the invalid-parameter handler, which ends the
 * process. POSIX reports EBADF instead, and -1 is the usual "no descriptor" value, so the descriptor wrappers answer it
 * the POSIX way before the CRT sees it.
 */
static int
dsd_fd_is_negative(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return 1;
    }
    return 0;
}

int
dsd_fileno(FILE* fp) {
    return fp ? _fileno(fp) : -1;
}

int
dsd_isatty(int fd) {
    if (dsd_fd_is_negative(fd)) {
        return 0;
    }
    return _isatty(fd);
}

int
dsd_set_binary_mode(int fd) {
    if (dsd_fd_is_negative(fd)) {
        return -1;
    }
    return _setmode(fd, _O_BINARY) < 0 ? -1 : 0;
}

int
dsd_dup(int oldfd) {
    if (dsd_fd_is_negative(oldfd)) {
        return -1;
    }
    return _dup(oldfd);
}

int
dsd_dup2(int oldfd, int newfd) {
    if (dsd_fd_is_negative(oldfd) || dsd_fd_is_negative(newfd)) {
        return -1;
    }
    /* _dup2 returns 0 on success; POSIX dup2 returns the new descriptor. */
    if (_dup2(oldfd, newfd) != 0) {
        return -1;
    }
    return newfd;
}

int
dsd_close(int fd) {
    if (dsd_fd_is_negative(fd)) {
        return -1;
    }
    return _close(fd);
}

int
dsd_fsync(int fd) {
    if (dsd_fd_is_negative(fd)) {
        return -1;
    }
    /* _commit is Windows equivalent of fsync */
    return _commit(fd);
}

int
dsd_fstat(int fd, dsd_stat_t* st) {
    if (dsd_fd_is_negative(fd)) {
        return -1;
    }
    return _fstat64(fd, st);
}

int
dsd_stat_path(const char* path, dsd_stat_t* st) {
    /* A UTF-8 path (the app's own files, named in any script) goes through the wide call, whatever the ANSI code page;
       one that is not UTF-8, or one the wide call does not find, through the ANSI call as before, so a command line's
       path in the ANSI code page still works. The other path helpers here choose the same way. */
    wchar_t* wide = dsd_win32_utf8_path_to_wide(path);
    if (wide) {
        const int rc = _wstat64(wide, st);
        const int saved_errno = errno;
        free(wide);
        if (rc == 0 || saved_errno != ENOENT) {
            errno = saved_errno;
            return rc;
        }
    }
    return _stat64(path, st);
}

/* The POSIX errno for a GetDiskFreeSpaceExW() failure, as statvfs() would set it for the same path. */
static int
dsd_free_space_errno(DWORD err) {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return ENOENT;
        case ERROR_ACCESS_DENIED: return EACCES;
        case ERROR_DIRECTORY: return ENOTDIR;
        case ERROR_INVALID_NAME: return EINVAL;
        default: return EIO;
    }
}

wchar_t*
dsd_win32_utf8_path_to_wide(const char* path) {
    if (!dsd_utf8_is_valid(path)) {
        return NULL;
    }
    const int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wide_len <= 0) {
        return NULL;
    }
    wchar_t* wide = (wchar_t*)calloc((size_t)wide_len, sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, wide_len) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

int
dsd_remove_file(const char* path) {
    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    wchar_t* wide = dsd_win32_utf8_path_to_wide(path);
    if (wide) {
        const int rc = _wremove(wide);
        const int saved_errno = errno;
        free(wide);
        if (rc == 0 || saved_errno != ENOENT) {
            errno = saved_errno;
            return rc;
        }
    }
    return remove(path);
}

int64_t
dsd_path_free_bytes(const char* path) {
    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    wchar_t* wide = dsd_win32_utf8_path_to_wide(path);
    if (!wide) {
        errno = EINVAL;
        return -1;
    }
    ULARGE_INTEGER available;
    available.QuadPart = 0;
    /* GetDiskFreeSpaceExW() takes a folder: a file's path fails with ERROR_DIRECTORY (ENOTDIR). */
    const BOOL ok = GetDiskFreeSpaceExW(wide, &available, NULL, NULL);
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    free(wide);
    if (!ok) {
        errno = dsd_free_space_errno(err);
        return -1;
    }
    return available.QuadPart > (ULONGLONG)INT64_MAX ? INT64_MAX : (int64_t)available.QuadPart;
}

int
dsd_fseek64(FILE* fp, int64_t offset, int whence) {
    if (!fp) {
        errno = EINVAL;
        return -1;
    }
    return _fseeki64(fp, (__int64)offset, whence) == 0 ? 0 : -1;
}

int64_t
dsd_ftell64(FILE* fp) {
    if (!fp) {
        errno = EINVAL;
        return -1;
    }
    const __int64 pos = _ftelli64(fp);
    return pos < 0 ? -1 : (int64_t)pos;
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
    if (strchr(mode, 'x') != NULL) {
        /* C11's exclusive create: only a "w" mode creates, and an existing file is left untouched (EEXIST). */
        if (mode[0] != 'w') {
            errno = EINVAL;
            return -1;
        }
        flags |= _O_EXCL;
    }
    return flags;
}

/* The mode for the stream over an opened descriptor: _fdopen() has no use for 'x', the descriptor already exists. */
static int
dsd_private_stream_mode(const char* mode, char* out, size_t out_size) {
    size_t n = 0;
    for (const char* p = mode; *p != '\0'; ++p) {
        if (*p == 'x') {
            continue;
        }
        if (n + 1U >= out_size) {
            errno = EINVAL;
            return -1;
        }
        out[n++] = *p;
    }
    out[n] = '\0';
    return 0;
}

/* The POSIX errno for a Win32 error from a file call (create, move, find, attributes). */
static int
dsd_win32_file_errno(DWORD err) {
    static const struct {
        DWORD win_error;
        int posix_error;
    } errors[] = {
        {ERROR_FILE_EXISTS, EEXIST},         {ERROR_ALREADY_EXISTS, EEXIST},
        {ERROR_FILE_NOT_FOUND, ENOENT},      {ERROR_PATH_NOT_FOUND, ENOENT},
        {ERROR_INVALID_DRIVE, ENOENT},       {ERROR_ACCESS_DENIED, EACCES},
        {ERROR_SHARING_VIOLATION, EACCES},   {ERROR_LOCK_VIOLATION, EACCES},
        {ERROR_TOO_MANY_OPEN_FILES, EMFILE}, {ERROR_DISK_FULL, ENOSPC},
        {ERROR_HANDLE_DISK_FULL, ENOSPC},    {ERROR_NOT_ENOUGH_MEMORY, ENOMEM},
        {ERROR_OUTOFMEMORY, ENOMEM},         {ERROR_FILENAME_EXCED_RANGE, ENAMETOOLONG},
        {ERROR_INVALID_NAME, EINVAL},        {ERROR_INVALID_PARAMETER, EINVAL},
        {ERROR_DIRECTORY, ENOTDIR},          {ERROR_NOT_SAME_DEVICE, EXDEV},
    };

    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; ++i) {
        if (errors[i].win_error == err) {
            return errors[i].posix_error;
        }
    }
    return EIO;
}

/* CreateFile(CREATE_NEW) of the final name itself, never a link's target; wide or ANSI as dsd_stat_path() chooses. */
static HANDLE
dsd_private_create_new(const char* path, DWORD access) {
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    const DWORD attrs = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT;
    wchar_t* wide = dsd_win32_utf8_path_to_wide(path);
    if (wide) {
        HANDLE handle = CreateFileW(wide, access, share, NULL, CREATE_NEW, attrs, NULL);
        const DWORD err = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        free(wide);
        if (handle != INVALID_HANDLE_VALUE || (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)) {
            SetLastError(err);
            return handle;
        }
    }
    return CreateFileA(path, access, share, NULL, CREATE_NEW, attrs, NULL);
}

static int
dsd_private_create_exclusive(const char* path, int flags, int* out_opened) {
    DWORD access = GENERIC_WRITE;
    if ((flags & _O_RDWR) != 0) {
        access |= GENERIC_READ;
    }
    /* _open(_O_CREAT | _O_EXCL) follows dangling symlinks on Windows. Claim the final name itself atomically, as
       POSIX O_CREAT | O_EXCL does, instead of creating a missing link target. */
    HANDLE handle = dsd_private_create_new(path, access);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = dsd_win32_file_errno(GetLastError());
        return -1;
    }
    if (out_opened) {
        *out_opened = 1;
    }
    int fd = _open_osfhandle((intptr_t)handle, flags & (_O_WRONLY | _O_RDWR | _O_BINARY | _O_TEXT | _O_NOINHERIT));
    if (fd < 0) {
        int saved_errno = errno;
        CloseHandle(handle);
        errno = saved_errno;
    }
    return fd;
}

/* _open() of a write mode, wide or ANSI as dsd_stat_path() chooses. */
static int
dsd_private_open_path(const char* path, int flags) {
    wchar_t* wide = dsd_win32_utf8_path_to_wide(path);
    if (wide) {
        const int fd = _wopen(wide, flags, _S_IREAD | _S_IWRITE);
        const int saved_errno = errno;
        free(wide);
        if (fd >= 0 || saved_errno != ENOENT) {
            errno = saved_errno;
            return fd;
        }
    }
    return _open(path, flags, _S_IREAD | _S_IWRITE);
}

FILE*
dsd_fopen_private_ex(const char* path, const char* mode, int* out_opened) {
    if (out_opened) {
        *out_opened = 0;
    }
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }
    if (mode[0] == 'r') {
        FILE* fp = fopen(path, mode);
        if (out_opened) {
            *out_opened = fp != NULL;
        }
        return fp;
    }

    int flags = dsd_private_open_flags(mode);
    if (flags < 0) {
        return NULL;
    }
    char stream_mode[8];
    if (dsd_private_stream_mode(mode, stream_mode, sizeof stream_mode) != 0) {
        return NULL;
    }

    int fd = (flags & _O_EXCL) != 0 ? dsd_private_create_exclusive(path, flags, out_opened)
                                    : dsd_private_open_path(path, flags);
    if (fd < 0) {
        return NULL;
    }
    /* The file is created or emptied now, whatever becomes of the stream over it. */
    if (out_opened) {
        *out_opened = 1;
    }

    FILE* fp = _fdopen(fd, stream_mode);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        _close(fd);
        errno = saved_errno;
    }
    return fp;
}

FILE*
dsd_fopen_private(const char* path, const char* mode) {
    return dsd_fopen_private_ex(path, mode, NULL);
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

/* @p attrs straight from GetFileAttributes(), whose GetLastError() this reads on a failure. */
static int
dsd_existing_regular_attrs_ok(DWORD attrs) {
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        /* Report a missing file as open() does on POSIX: callers tell "not there yet" (ENOENT) from "unusable". */
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            errno = ENOENT;
        } else if (err == ERROR_ACCESS_DENIED) {
            errno = EACCES;
        } else {
            errno = EINVAL;
        }
        return -1;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* The attribute check, then the open, wide or ANSI as dsd_stat_path() chooses. */
static int
dsd_open_existing_regular_path(const char* path, int flags) {
    wchar_t* wide = dsd_win32_utf8_path_to_wide(path);
    if (wide) {
        int fd = -1;
        if (dsd_existing_regular_attrs_ok(GetFileAttributesW(wide)) == 0) {
            fd = _wopen(wide, flags);
        }
        const int saved_errno = errno;
        free(wide);
        if (fd >= 0 || saved_errno != ENOENT) {
            errno = saved_errno;
            return fd;
        }
    }
    if (dsd_existing_regular_attrs_ok(GetFileAttributesA(path)) != 0) {
        return -1;
    }
    return _open(path, flags);
}

static int
dsd_open_existing_regular_fd_win32(const char* path, const char* mode) {
    if (!path || path[0] == '\0' || !mode || mode[0] != 'r' || strchr(mode, '+') != NULL) {
        errno = EINVAL;
        return -1;
    }

    int flags = 0;
    if (dsd_existing_regular_mode_flags(mode, &flags) != 0) {
        return -1;
    }
    int fd = dsd_open_existing_regular_path(path, flags);
    if (fd < 0) {
        return -1;
    }

    dsd_stat_t st;
    if (_fstat64(fd, &st) != 0) {
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

    int flags = dsd_private_open_flags(mode);
    if (flags < 0) {
        return NULL;
    }

    int fd = dsd_win32_temp_open(tmp_path, tmp_path + n - 6, flags);
    if (fd < 0) {
        return NULL;
    }

    FILE* fp = _fdopen(fd, mode);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        _close(fd);
        (void)dsd_remove_file(tmp_path);
        errno = saved_errno;
        return NULL;
    }
    return fp;
}

void
dsd_fsync_parent_dir(const char* path) {
    (void)path;
}

int
dsd_replace_file_with_temp(const char* tmp_path, const char* final_path) {
    if (!tmp_path || tmp_path[0] == '\0' || !final_path || final_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    /* Both names in UTF-8 (the app's own files, in any script) move through the wide call; a name in the ANSI code
       page, or one the wide call does not find, through the ANSI one, as before. */
    wchar_t* wide_tmp = dsd_win32_utf8_path_to_wide(tmp_path);
    wchar_t* wide_final = wide_tmp ? dsd_win32_utf8_path_to_wide(final_path) : NULL;
    BOOL moved = FALSE;
    DWORD err = ERROR_FILE_NOT_FOUND;
    if (wide_tmp && wide_final) {
        moved = MoveFileExW(wide_tmp, wide_final, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
        err = moved ? ERROR_SUCCESS : GetLastError();
    }
    free(wide_tmp);
    free(wide_final);
    if (!moved && (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)) {
        moved = MoveFileExA(tmp_path, final_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    }
    if (!moved) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

/* The ANSI form of dsd_rename_noreplace(), for a name that is not UTF-8 or that the wide call does not find. The CRT's
   rename() refuses an existing destination and keeps the CRT's own path conversion (a UTF-8 locale included), which
   MoveFileExA() would not. It reports that refusal as EACCES, the errno of every other denial too, so the native error
   the same call left tells them apart. */
static int
dsd_rename_noreplace_ansi(const char* from, const char* to) {
    _doserrno = 0;
    if (rename(from, to) == 0) {
        return 0;
    }
    const int saved = errno;
    const unsigned long native = (unsigned long)_doserrno;
    errno = (native == ERROR_ALREADY_EXISTS || native == ERROR_FILE_EXISTS) ? EEXIST : saved;
    return -1;
}

int
dsd_rename_noreplace(const char* from, const char* to, int* out_source_left) {
    if (out_source_left) {
        *out_source_left = 0;
    }
    if (!from || from[0] == '\0' || !to || to[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    /* MoveFileExW() without MOVEFILE_REPLACE_EXISTING refuses an existing destination (ERROR_ALREADY_EXISTS or
       ERROR_FILE_EXISTS, so EEXIST) and, within one volume, renames a link itself. MOVEFILE_COPY_ALLOWED keeps what the
       CRT's rename() did for a destination on another volume. Two UTF-8 names (the app's own files, in any script) move
       through it; otherwise, or when the wide call does not find a name, the ANSI form, as dsd_stat_path() chooses. */
    wchar_t* wide_from = dsd_win32_utf8_path_to_wide(from);
    wchar_t* wide_to = wide_from ? dsd_win32_utf8_path_to_wide(to) : NULL;
    DWORD err = ERROR_FILE_NOT_FOUND;
    if (wide_from && wide_to) {
        err = MoveFileExW(wide_from, wide_to, MOVEFILE_COPY_ALLOWED) ? ERROR_SUCCESS : GetLastError();
    }
    free(wide_from);
    free(wide_to);
    if (err == ERROR_SUCCESS) {
        return 0;
    }
    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
        errno = dsd_win32_file_errno(err);
        return -1;
    }
    return dsd_rename_noreplace_ansi(from, to);
}

/* dsd_path_exists_nofollow()'s answer from one GetFileAttributes() result, whose GetLastError() this reads on a
   failure. GetFileAttributes() describes a link or junction itself, never its target. */
static int
dsd_path_exists_answer(DWORD attrs) {
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return 1;
    }
    errno = dsd_win32_file_errno(GetLastError());
    return (errno == ENOENT || errno == ENOTDIR) ? 0 : -1;
}

int
dsd_path_exists_nofollow(const char* path) {
    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    wchar_t* wide = dsd_win32_utf8_path_to_wide(path);
    if (wide) {
        const int answer = dsd_path_exists_answer(GetFileAttributesW(wide));
        const int saved_errno = errno;
        free(wide);
        if (answer != 0) {
            errno = saved_errno;
            return answer;
        }
    }
    return dsd_path_exists_answer(GetFileAttributesA(path));
}

int
dsd_path_canonical(const char* path, char* out, size_t out_size) {
    if (!path || path[0] == '\0' || !out || out_size == 0) {
        errno = EINVAL;
        return -1;
    }
    char full[_MAX_PATH];
    if (_fullpath(full, path, sizeof full) == NULL) {
        errno = ENAMETOOLONG;
        return -1;
    }
    struct _stat64 st;
    if (_stat64(full, &st) != 0) {
        return -1;
    }
    const int n = DSD_SNPRINTF(out, out_size, "%s", full);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        errno = ENAMETOOLONG;
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

// Split out of dsd_dir_list() to keep it under the CCN ceiling tools/lizard.sh
// enforces, the same way dsd_dir_entry_is_regular_file() is split out on the
// POSIX side; it carries the per-entry rejection the walk applies.
//
// @return 0 to keep walking, non-zero when the callback asked to stop.
static int
dsd_dir_report_entry(const struct _finddata_t* data, dsd_dir_list_cb cb, void* user) {
    // _A_SUBDIR also covers "." and "..", so no separate name test is needed.
    if ((data->attrib & _A_SUBDIR) != 0) {
        return 0;
    }
    return cb(data->name, user);
}

// The wide walk's entry, reported by its UTF-8 name. A name with no UTF-8 form
// (an unpaired surrogate) is skipped: no UTF-8 path could open it.
static int
dsd_dir_report_wide_entry(const WIN32_FIND_DATAW* data, dsd_dir_list_cb cb, void* user) {
    // FILE_ATTRIBUTE_DIRECTORY also covers "." and "..".
    if ((data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return 0;
    }
    // cFileName holds MAX_PATH UTF-16 units, NUL included; each takes at most 3 UTF-8 bytes.
    char name[MAX_PATH * 3];
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data->cFileName, -1, name, (int)sizeof name, NULL, NULL)
        <= 0) {
        return 0;
    }
    return cb(name, user);
}

// dsd_dir_list()'s answer when the wide walk did not find the folder: the ANSI walk is tried next.
enum { DSD_DIR_LIST_WIDE_NOT_FOUND = 1 };

// The "<dir>\*" search pattern, wide; NULL when @p dir is not UTF-8 or memory runs out.
static wchar_t*
dsd_dir_list_wide_pattern(const char* dir) {
    wchar_t* wide = dsd_win32_utf8_path_to_wide(dir);
    if (!wide) {
        return NULL;
    }
    const size_t len = wcslen(wide);
    wchar_t* pattern = (wchar_t*)calloc(len + 3U, sizeof(wchar_t));
    if (pattern) {
        DSD_MEMCPY(pattern, wide, len * sizeof(wchar_t));
        pattern[len] = L'\\';
        pattern[len + 1U] = L'*';
    }
    free(wide);
    return pattern;
}

// A UTF-8 folder (the app's own, named in any script) is walked with the wide
// calls and its entries reported by UTF-8 names, whatever the ANSI code page.
//
// @return 0 or -1 as dsd_dir_list() answers, or DSD_DIR_LIST_WIDE_NOT_FOUND when
//         the folder is not UTF-8 or the wide call does not find it.
static int
dsd_dir_list_wide(const char* dir, dsd_dir_list_cb cb, void* user) {
    wchar_t* pattern = dsd_dir_list_wide_pattern(dir);
    if (!pattern) {
        return DSD_DIR_LIST_WIDE_NOT_FOUND;
    }
    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileW(pattern, &data);
    const DWORD first_err = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        if (first_err == ERROR_FILE_NOT_FOUND || first_err == ERROR_PATH_NOT_FOUND) {
            return DSD_DIR_LIST_WIDE_NOT_FOUND;
        }
        errno = dsd_win32_file_errno(first_err);
        return -1;
    }

    int list_errno = 0;
    for (;;) {
        if (dsd_dir_report_wide_entry(&data, cb, user) != 0) {
            break; // the callback stopped the walk; not an error
        }
        if (!FindNextFileW(handle, &data)) {
            // ERROR_NO_MORE_FILES is the clean end; anything else a read that failed part-way.
            const DWORD err = GetLastError();
            list_errno = err == ERROR_NO_MORE_FILES ? 0 : dsd_win32_file_errno(err);
            break;
        }
    }
    if (!FindClose(handle) && list_errno == 0) {
        list_errno = dsd_win32_file_errno(GetLastError());
    }
    errno = list_errno;
    return list_errno == 0 ? 0 : -1;
}

// A folder named in the ANSI code page (a command line's), or one the wide walk did not find.
static int
dsd_dir_list_ansi(const char* dir, dsd_dir_list_cb cb, void* user) {
    // The C source text is "%s\\*": two backslashes here produce one at runtime,
    // so _findfirst() receives <dir>\* .
    char pattern[1024];
    int n = DSD_SNPRINTF(pattern, sizeof pattern, "%s\\*", dir);
    if (n < 0 || (size_t)n >= sizeof pattern) {
        errno = ENAMETOOLONG;
        return -1;
    }

    struct _finddata_t data;
    intptr_t handle = _findfirst(pattern, &data);
    if (handle == -1) {
        return -1;
    }

    int list_errno = 0;
    for (;;) {
        if (dsd_dir_report_entry(&data, cb, user) != 0) {
            break; // the callback stopped the walk; not an error
        }
        // Cleared per iteration so a callback's own errno cannot be mistaken for a
        // _findnext() failure on the next pass, mirroring the readdir() loop in
        // file_compat_posix.c.
        errno = 0;
        if (_findnext(handle, &data) != 0) {
            // ENOENT is the clean end of the directory; anything else is a real
            // read failure and the header promises -1 for one ("reading it failed
            // part-way"). Treating them alike would report a truncated listing as
            // a complete one, which is how the Imported Systems browser decides
            // whether files are missing or simply absent.
            list_errno = (errno == ENOENT) ? 0 : errno;
            break;
        }
    }

    if (_findclose(handle) != 0 && list_errno == 0) {
        list_errno = errno ? errno : EINVAL;
    }
    errno = list_errno;
    return list_errno == 0 ? 0 : -1;
}

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
int
dsd_dir_list(const char* dir, dsd_dir_list_cb cb, void* user) {
    if (!dir || dir[0] == '\0' || !cb) {
        errno = EINVAL;
        return -1;
    }
    const int wide_rc = dsd_dir_list_wide(dir, cb, user);
    if (wide_rc != DSD_DIR_LIST_WIDE_NOT_FOUND) {
        return wide_rc;
    }
    return dsd_dir_list_ansi(dir, cb, user);
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

ssize_t
dsd_read(int fd, void* buf, size_t count) {
    if (dsd_fd_is_negative(fd)) {
        return -1;
    }
    return (ssize_t)_read(fd, buf, (unsigned int)count);
}

/* Windows has no wait on a CRT descriptor. An anonymous pipe (stdin from another program) reports the bytes it holds
   (PeekNamedPipe()), so it is polled in short sleeps up to the bound; a pipe whose writer has gone fails the peek and
   counts as ready, so the read reports its end. Any other handle (a console, a regular file) is ready at once, and the
   read that follows may block. */
enum { DSD_FD_WAIT_PIPE_SLICE_MS = 5 };

int
dsd_fd_wait_readable(int fd, unsigned int timeout_ms) {
    if (dsd_fd_is_negative(fd)) {
        return -1;
    }
    const HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (GetFileType(handle) != FILE_TYPE_PIPE) {
        return 1;
    }
    const ULONGLONG start_ms = GetTickCount64();
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL) || available > 0) {
            return 1;
        }
        const ULONGLONG waited_ms = GetTickCount64() - start_ms;
        if (waited_ms >= (ULONGLONG)timeout_ms) {
            return 0;
        }
        const ULONGLONG left_ms = (ULONGLONG)timeout_ms - waited_ms;
        Sleep(left_ms < DSD_FD_WAIT_PIPE_SLICE_MS ? (DWORD)left_ms : (DWORD)DSD_FD_WAIT_PIPE_SLICE_MS);
    }
}

ssize_t
dsd_write(int fd, const void* buf, size_t count) {
    if (dsd_fd_is_negative(fd)) {
        return -1;
    }
    return (ssize_t)_write(fd, buf, (unsigned int)count);
}

static ssize_t
// cppcheck-suppress constParameterCallback -- signature fixed by dsd_write_frames_ops
write_frames_win32_write(void* ctx, const void* buf, size_t count) {
    const int fd = *(const int*)ctx;
    if (dsd_fd_is_negative(fd)) {
        return -1;
    }
    if (count > (size_t)INT_MAX) {
        count = (size_t)INT_MAX;
    }
    return (ssize_t)_write(fd, buf, (unsigned int)count);
}

dsd_write_frames_status
dsd_write_frames(int fd, const void* buf, size_t bytes, size_t frame_bytes, size_t* written) {
    int fd_copy = fd;
    /* The CRT descriptors block, and there is no SIGPIPE: a closed pipe is a plain write error. */
    const dsd_write_frames_ops ops = {write_frames_win32_write, NULL, &fd_copy};
    return dsd_write_frames_loop(&ops, buf, bytes, frame_bytes, written);
}

const char*
dsd_null_device(void) {
    return "NUL";
}

#endif /* DSD_PLATFORM_WIN_NATIVE */
