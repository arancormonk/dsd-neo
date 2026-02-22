// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Ensure BSD/Darwin extensions (mkdtemp) are declared on macOS. */
#if defined(__APPLE__) && defined(__MACH__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include <dsd-neo/platform/posix_compat.h>
#include <sys/types.h>

#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE

#include <stdlib.h>
#include <sys/stat.h>

#if defined(__APPLE__) && defined(__MACH__)
/*
 * Some Apple SDK feature-level combinations still hide mkdtemp from stdlib.h.
 * Provide the prototype explicitly so strict C99+ builds do not fail with an
 * implicit declaration.
 */
extern char* mkdtemp(char* tmpl);
#endif

int
dsd_setenv(const char* name, const char* value, int overwrite) {
    return setenv(name, value, overwrite);
}

int
dsd_unsetenv(const char* name) {
    return unsetenv(name);
}

int
dsd_mkdir(const char* path, int mode) {
    return mkdir(path, (mode_t)mode);
}

void*
dsd_aligned_alloc(size_t alignment, size_t size) {
    void* ptr = NULL;
#if defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
    /* C11 aligned_alloc requires size to be multiple of alignment */
    size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
    ptr = aligned_alloc(alignment, aligned_size);
#else
    if (posix_memalign(&ptr, alignment, size) != 0) {
        ptr = NULL;
    }
#endif
    return ptr;
}

void
dsd_aligned_free(void* ptr) {
    free(ptr);
}

int
dsd_mkstemp(char* tmpl) {
    return mkstemp(tmpl);
}

char*
dsd_mkdtemp(char* tmpl) {
    return mkdtemp(tmpl);
}

#endif /* !DSD_PLATFORM_WIN_NATIVE */
