// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/dynlib.h>
#include <dsd-neo/platform/platform.h>
#include <stddef.h>

#if DSD_PLATFORM_WIN_NATIVE

#include <stdlib.h>
#include <windows.h>

/* LoadLibraryExW() search flags (KB2533623, built into Windows 8 and later); older SDK headers lack the names. */
#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif

struct dsd_dynlib {
    HMODULE module;
};

static void
dynlib_set_error(char* err, size_t err_size, const char* reason, DWORD code) {
    if (!err || err_size == 0) {
        return;
    }
    if (code == 0) {
        DSD_SNPRINTF(err, err_size, "%s", reason);
    } else {
        DSD_SNPRINTF(err, err_size, "%s (Windows error %lu)", reason, (unsigned long)code);
    }
}

/* @p text in code page @p cp as a NUL-terminated wide string the caller frees; NULL when it does not decode or memory
   runs out. */
static wchar_t*
dynlib_decode(const char* text, UINT cp, DWORD flags) {
    const int count = MultiByteToWideChar(cp, flags, text, -1, NULL, 0);
    if (count <= 0) {
        return NULL;
    }
    wchar_t* wide = (wchar_t*)calloc((size_t)count, sizeof(*wide));
    if (!wide) {
        return NULL;
    }
    if (MultiByteToWideChar(cp, flags, text, -1, wide, count) != count) {
        free(wide);
        return NULL;
    }
    return wide;
}

/* The path as UTF-8, which the frontends pass; a path that is not valid UTF-8 came from the ANSI command line of the
   C runtime and is read in the ANSI code page. */
static wchar_t*
dynlib_path_to_wide(const char* path) {
    wchar_t* wide = dynlib_decode(path, CP_UTF8, MB_ERR_INVALID_CHARS);
    return wide ? wide : dynlib_decode(path, CP_ACP, 0);
}

/* @p path made absolute against the working directory, which LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires; the caller
   frees it. NULL on failure. */
static wchar_t*
dynlib_full_path(const wchar_t* path) {
    const DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (needed == 0) {
        return NULL;
    }
    wchar_t* full = (wchar_t*)calloc((size_t)needed, sizeof(*full));
    if (!full) {
        return NULL;
    }
    const DWORD written = GetFullPathNameW(path, needed, full, NULL);
    if (written == 0 || written >= needed) {
        free(full);
        return NULL;
    }
    return full;
}

static const char*
dynlib_load_failure(DWORD code) {
    switch (code) {
        case ERROR_MOD_NOT_FOUND: return "the library or a library it depends on was not found";
        case ERROR_BAD_EXE_FORMAT: return "not a library for this architecture";
        case ERROR_PROC_NOT_FOUND: return "a library it depends on lacks a function it needs";
        default: return "the library could not be loaded";
    }
}

dsd_dynlib*
dsd_dynlib_open(const char* path, char* err, size_t err_size) {
    dynlib_set_error(err, err_size, "", 0);
    if (!path || path[0] == '\0') {
        dynlib_set_error(err, err_size, "no library path", 0);
        return NULL;
    }
    wchar_t* wide = dynlib_path_to_wide(path);
    if (!wide) {
        dynlib_set_error(err, err_size, "the path cannot be decoded", GetLastError());
        return NULL;
    }
    wchar_t* full = dynlib_full_path(wide);
    free(wide);
    if (!full) {
        dynlib_set_error(err, err_size, "the path cannot be resolved", GetLastError());
        return NULL;
    }
    HMODULE module = LoadLibraryExW(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const DWORD code = module ? 0 : GetLastError();
    free(full);
    if (!module) {
        dynlib_set_error(err, err_size, dynlib_load_failure(code), code);
        return NULL;
    }
    dsd_dynlib* lib = (dsd_dynlib*)calloc(1, sizeof(*lib));
    if (!lib) {
        (void)FreeLibrary(module);
        dynlib_set_error(err, err_size, "out of memory", 0);
        return NULL;
    }
    lib->module = module;
    return lib;
}

dsd_dynlib_fn
dsd_dynlib_symbol(dsd_dynlib* lib, const char* name) {
    if (!lib || !name || name[0] == '\0') {
        return NULL;
    }
    /* FARPROC is itself a function pointer; void (*)(void) is the generic one every function pointer converts to. */
    return (dsd_dynlib_fn)GetProcAddress(lib->module, name);
}

void
dsd_dynlib_close(dsd_dynlib* lib) {
    if (!lib) {
        return;
    }
    (void)FreeLibrary(lib->module);
    free(lib);
}

#endif /* DSD_PLATFORM_WIN_NATIVE */
