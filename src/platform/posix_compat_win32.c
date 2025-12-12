// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/posix_compat.h>

#if DSD_PLATFORM_WIN_NATIVE

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

int
dsd_setenv(const char* name, const char* value, int overwrite) {
    if (name == NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite) {
        /* Check if variable already exists */
        size_t required = 0;
        if (getenv_s(&required, NULL, 0, name) == 0 && required > 0) {
            return 0; /* Already exists, don't overwrite */
        }
    }
    if (_putenv_s(name, value) != 0) {
        return -1;
    }
    return 0;
}

int
dsd_unsetenv(const char* name) {
    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* Setting to empty string removes the variable */
    if (_putenv_s(name, "") != 0) {
        return -1;
    }
    return 0;
}

int
dsd_mkdir(const char* path, int mode) {
    (void)mode; /* Windows ignores mode */
    return _mkdir(path);
}

void*
dsd_aligned_alloc(size_t alignment, size_t size) {
    return _aligned_malloc(size, alignment);
}

void
dsd_aligned_free(void* ptr) {
    _aligned_free(ptr);
}

int
dsd_mkstemp(char* tmpl) {
    if (tmpl == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(tmpl);
    if (len < 6) {
        errno = EINVAL;
        return -1;
    }

    /* Verify template ends with XXXXXX */
    if (strcmp(tmpl + len - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }

    /* _mktemp_s modifies the template in place */
    if (_mktemp_s(tmpl, len + 1) != 0) {
        return -1;
    }

    /* Open the file with exclusive access */
    int fd = _open(tmpl, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    return fd;
}

char*
dsd_mkdtemp(char* tmpl) {
    if (tmpl == NULL) {
        errno = EINVAL;
        return NULL;
    }

    size_t len = strlen(tmpl);
    if (len < 6) {
        errno = EINVAL;
        return NULL;
    }

    /* Verify template ends with XXXXXX */
    if (strcmp(tmpl + len - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return NULL;
    }

    /* _mktemp_s modifies the template in place */
    if (_mktemp_s(tmpl, len + 1) != 0) {
        return NULL;
    }

    /* Create the directory */
    if (_mkdir(tmpl) != 0) {
        return NULL;
    }

    return tmpl;
}

int
dsd_gettimeofday(struct dsd_timeval* tv, void* tz) {
    (void)tz;
    if (tv == NULL) {
        return -1;
    }

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    /* FILETIME is 100-nanosecond intervals since Jan 1, 1601 */
    /* Convert to Unix epoch (Jan 1, 1970) */
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    /* 116444736000000000 is the number of 100-ns intervals between
     * Jan 1, 1601 and Jan 1, 1970 */
    uli.QuadPart -= 116444736000000000ULL;

    tv->tv_sec = (long)(uli.QuadPart / 10000000ULL);
    tv->tv_usec = (long)((uli.QuadPart % 10000000ULL) / 10);

    return 0;
}

#endif /* DSD_PLATFORM_WIN_NATIVE */
