// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/dynlib.h>
#include <dsd-neo/platform/platform.h>
#include <stddef.h>

#if !DSD_PLATFORM_WIN_NATIVE

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

struct dsd_dynlib {
    void* handle;
};

static void
dynlib_set_error(char* err, size_t err_size, const char* reason) {
    if (err && err_size > 0) {
        DSD_SNPRINTF(err, err_size, "%s", reason ? reason : "unknown error");
    }
}

dsd_dynlib*
dsd_dynlib_open(const char* path, char* err, size_t err_size) {
    dynlib_set_error(err, err_size, "");
    if (!path || path[0] == '\0') {
        dynlib_set_error(err, err_size, "no library path");
        return NULL;
    }
    /* dlopen() looks a name without a slash up on the loader's search path; "./" keeps it in the working directory. */
    const size_t len = strlen(path);
    char* open_path = (char*)malloc(len + 3U);
    if (!open_path) {
        dynlib_set_error(err, err_size, "out of memory");
        return NULL;
    }
    if (strchr(path, '/') != NULL) {
        DSD_MEMCPY(open_path, path, len + 1U);
    } else {
        DSD_SNPRINTF(open_path, len + 3U, "./%s", path);
    }
    (void)dlerror();
    void* handle = dlopen(open_path, RTLD_NOW | RTLD_LOCAL);
    free(open_path);
    if (!handle) {
        dynlib_set_error(err, err_size, dlerror());
        return NULL;
    }
    dsd_dynlib* lib = (dsd_dynlib*)calloc(1, sizeof(*lib));
    if (!lib) {
        (void)dlclose(handle);
        dynlib_set_error(err, err_size, "out of memory");
        return NULL;
    }
    lib->handle = handle;
    return lib;
}

dsd_dynlib_fn
dsd_dynlib_symbol(dsd_dynlib* lib, const char* name) {
    if (!lib || !name || name[0] == '\0') {
        return NULL;
    }
    void* symbol = dlsym(lib->handle, name);
    if (!symbol) {
        return NULL;
    }
    /* POSIX guarantees a function's dlsym() address converts to a function pointer; ISO C has no cast for it, so the
       representation is copied instead. */
    _Static_assert(sizeof(dsd_dynlib_fn) == sizeof(void*), "function and object pointers differ in size");
    dsd_dynlib_fn fn = NULL;
    DSD_MEMCPY((void*)&fn, (const void*)&symbol, sizeof(fn));
    return fn;
}

void
dsd_dynlib_close(dsd_dynlib* lib) {
    if (!lib) {
        return;
    }
    (void)dlclose(lib->handle);
    free(lib);
}

#endif /* !DSD_PLATFORM_WIN_NATIVE */
