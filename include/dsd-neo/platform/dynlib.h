// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_DYNLIB_H
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_DYNLIB_H

/**
 * @file
 * @brief Loading a user-supplied shared library at run time: dlopen() on POSIX, LoadLibraryExW() on Windows.
 *
 * For optional components the project cannot ship, such as a TETRA speech codec (`--tetra-codec`). The library is
 * the file the path names, relative to the working directory when the path is not absolute; a bare file name never
 * goes through the loader's search path, so it cannot pick up a same-named library from somewhere else. POSIX opens
 * it with RTLD_NOW | RTLD_LOCAL, so a library with an unresolved symbol fails here rather than at its first call and
 * its symbols never mix with anyone else's. Windows resolves the library's own dependencies from its directory and
 * the system directories only, never from the working directory or PATH.
 *
 * A handle belongs to the thread that uses it; the module keeps no state of its own.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** An open shared library. */
typedef struct dsd_dynlib dsd_dynlib;

/** What dsd_dynlib_symbol() returns: a generic function pointer, to be cast to the function's real type. */
typedef void (*dsd_dynlib_fn)(void);

/**
 * @brief Open the shared library at @p path.
 *
 * On Windows @p path is UTF-8, or, when it is not valid UTF-8, the ANSI code page of a C runtime command line.
 *
 * @param path     The library file.
 * @param err      Receives a NUL-terminated reason when the open fails; may be NULL.
 * @param err_size Capacity of @p err.
 * @return The library, or NULL when it could not be loaded.
 */
dsd_dynlib* dsd_dynlib_open(const char* path, char* err, size_t err_size);

/** @brief The function @p lib exports as @p name, or NULL when it exports none by that name. */
dsd_dynlib_fn dsd_dynlib_symbol(dsd_dynlib* lib, const char* name);

/** @brief Unload @p lib (NULL does nothing). No function resolved from it may be called afterwards. */
void dsd_dynlib_close(dsd_dynlib* lib);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_DYNLIB_H */
