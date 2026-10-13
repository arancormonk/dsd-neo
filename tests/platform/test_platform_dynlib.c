// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The run-time library loader (dsd-neo/platform/dynlib.h) over the TETRA test codec library: functions resolve and
 * run, a missing one is NULL, a missing file fails with a reason, a bare file name means the working directory and
 * never the loader's search path, and a UTF-8 path with non-ASCII characters loads.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/dynlib.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/tetra/tetra_codec_abi.h>
#include <stdint.h>
#include <stdio.h>

#include "test_support.h"

#if !defined(TETRA_TEST_CODEC_GOOD) || !defined(TETRA_TEST_CODEC_MISSING_SYMBOL)
#error the test codec libraries are required
#endif

/* The suffix of a shared library, and a system library the loader's search path would find by its bare name. */
#if defined(_WIN32)
#include <windows.h>
#define LIB_SUFFIX     ".dll"
#define SYSTEM_LIBRARY "kernel32.dll"
#elif defined(__APPLE__)
#define LIB_SUFFIX     ".dylib"
#define SYSTEM_LIBRARY "libSystem.B.dylib"
#elif defined(__linux__)
#define LIB_SUFFIX     ".so"
#define SYSTEM_LIBRARY "libc.so.6"
#else
#define LIB_SUFFIX ".so"
#endif

static int g_fail = 0;

static void
expect(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* The library's ABI version through the loader, or 0 when it cannot be called. */
static uint32_t
abi_version_of(dsd_dynlib* lib) {
    const dsd_tetra_codec_abi_version_fn fn =
        (dsd_tetra_codec_abi_version_fn)dsd_dynlib_symbol(lib, DSD_TETRA_CODEC_SYM_ABI_VERSION);
    return fn ? fn() : 0U;
}

#if !defined(_WIN32)
static int
copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    FILE* out = dsd_fopen_private(to, "wb");
    int rc = (in && out) ? 0 : -1;
    char buf[4096];
    while (rc == 0) {
        const size_t n = fread(buf, 1, sizeof(buf), in);
        if (n > 0 && fwrite(buf, 1, n, out) != n) {
            rc = -1;
        }
        if (n < sizeof(buf)) {
            rc = ferror(in) ? -1 : rc;
            break;
        }
    }
    if (in) {
        (void)fclose(in);
    }
    if (out && fclose(out) != 0) {
        rc = -1;
    }
    return rc;
}
#endif

static void
test_symbols(void) {
    char err[256];
    dsd_dynlib* lib = dsd_dynlib_open(TETRA_TEST_CODEC_GOOD, err, sizeof(err));
    expect(lib != NULL, "the test codec loads");
    expect(abi_version_of(lib) == DSD_TETRA_CODEC_ABI_VERSION, "its version function runs");
    expect(dsd_dynlib_symbol(lib, "dsd_neo_no_such_function") == NULL, "an absent function is NULL");
    expect(dsd_dynlib_symbol(lib, "") == NULL && dsd_dynlib_symbol(NULL, DSD_TETRA_CODEC_SYM_CREATE) == NULL,
           "no name or no library resolves nothing");
    dsd_dynlib_close(lib);

    lib = dsd_dynlib_open(TETRA_TEST_CODEC_MISSING_SYMBOL, err, sizeof(err));
    expect(lib != NULL && dsd_dynlib_symbol(lib, DSD_TETRA_CODEC_SYM_DESTROY) == NULL
               && dsd_dynlib_symbol(lib, DSD_TETRA_CODEC_SYM_CREATE) != NULL,
           "a library without one function lacks exactly that one");
    dsd_dynlib_close(lib);
    dsd_dynlib_close(NULL);
}

static void
test_failures(void) {
    char err[256];
    err[0] = '\0';
    expect(dsd_dynlib_open("dsd_neo_no_such_library" LIB_SUFFIX, err, sizeof(err)) == NULL && err[0] != '\0',
           "a missing library fails with a reason");
    err[0] = '\0';
    expect(dsd_dynlib_open("", err, sizeof(err)) == NULL && err[0] != '\0', "an empty path fails with a reason");
    expect(dsd_dynlib_open(NULL, NULL, 0) == NULL, "no path and no error buffer fail quietly");
#ifdef SYSTEM_LIBRARY
    dsd_dynlib* lib = dsd_dynlib_open(SYSTEM_LIBRARY, err, sizeof(err));
    expect(lib == NULL, "a bare name is not looked up on the loader's search path");
    dsd_dynlib_close(lib);
#endif
}

/* Copy the test codec into the working directory as @p leaf (UTF-8). The C runtime's narrow file names are not UTF-8
   on Windows, so there the copy is made under the wide name. */
static int
copy_codec_to(const char* leaf) {
#if defined(_WIN32)
    wchar_t from[DSD_TEST_PATH_MAX];
    wchar_t to[DSD_TEST_PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, TETRA_TEST_CODEC_GOOD, -1, from, DSD_TEST_PATH_MAX) <= 0
        || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, leaf, -1, to, DSD_TEST_PATH_MAX) <= 0) {
        return -1;
    }
    return CopyFileW(from, to, TRUE) ? 0 : -1;
#else
    return copy_file(TETRA_TEST_CODEC_GOOD, leaf);
#endif
}

static void
remove_copy(const char* leaf) {
#if defined(_WIN32)
    wchar_t to[DSD_TEST_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, leaf, -1, to, DSD_TEST_PATH_MAX) > 0) {
        (void)DeleteFileW(to);
    }
#else
    (void)remove(leaf);
#endif
}

/* A copy of the test codec in a fresh working directory under a non-ASCII name loads by its bare name. */
static void
test_working_directory_and_utf8(void) {
    static const char leaf[] = "codec_\xc3\xa9t\xc3\xa9" LIB_SUFFIX; /* "codec_été" */
    dsd_test_temp_cwd cwd;
    if (dsd_test_temp_cwd_enter(&cwd, "dynlib") != 0) {
        expect(0, "temp working directory");
        return;
    }
    expect(copy_codec_to(leaf) == 0, "copy the test codec");
    char err[256];
    dsd_dynlib* lib = dsd_dynlib_open(leaf, err, sizeof(err));
    if (!lib) {
        DSD_FPRINTF(stderr, "dynlib: %s\n", err);
    }
    expect(lib != NULL && abi_version_of(lib) == DSD_TETRA_CODEC_ABI_VERSION,
           "a bare non-ASCII name loads from the working directory");
    dsd_dynlib_close(lib);
    remove_copy(leaf);
    expect(dsd_test_temp_cwd_leave(&cwd) == 0, "leave the temp working directory");
}

int
main(void) {
    test_symbols();
    test_failures();
    test_working_directory_and_utf8();
    if (g_fail) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("PASS test_platform_dynlib");
    return 0;
}
