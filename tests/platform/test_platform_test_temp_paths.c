// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The temp paths dsd_test_mkstemp() and dsd_test_mkdtemp() create are removed when a test leaves them behind, at exit
 * and when a failed assert() aborts it (test_support/temp_paths.c). Each run makes its paths under DSD_NEO_TEST_TMPDIR
 * in one mode; tests/cmake/TestTempPathsCheck.cmake runs every mode and checks that folder is empty afterwards.
 *
 *   remove   create, then remove everything itself: nothing is left, no cleanup line is printed, and it exits 0
 *   leave    create and return 0 without removing anything: the cleanup removes it all and the run fails
 *   abort    a file, an empty folder and a folder holding a file of its own, then abort() before any cleanup: the
 *            first two are removed, and the third, which a signal handler cannot empty, is named and left
 *   many     create and remove more files than the tracker holds at once, then leave one
 *   link     (POSIX) leave a folder holding a symbolic link to argv[2], which must survive the cleanup */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

#if !DSD_PLATFORM_WIN_NATIVE
#include <unistd.h>
#endif

static int
fail(const char* what) {
    DSD_FPRINTF(stderr, "TEST_TEMP_PATHS: %s\n", what);
    return 1;
}

static int
write_file(const char* path, const char* text) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    const size_t len = strlen(text);
    const int rc = fwrite(text, 1, len, fp) == len ? 0 : -1;
    return fclose(fp) == 0 ? rc : -1;
}

/* A temp file, and a temp folder holding a file and a subfolder with a file of its own, as code under test writes. */
typedef struct {
    char file[DSD_TEST_PATH_MAX];
    char dir[DSD_TEST_PATH_MAX];
    char sub[DSD_TEST_PATH_MAX];
    char inner[DSD_TEST_PATH_MAX];
    char deep[DSD_TEST_PATH_MAX];
} temp_set;

static int
make_set(temp_set* set) {
    const int fd = dsd_test_mkstemp(set->file, sizeof set->file, "dsdneo_temp_paths_file");
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);
    if (!dsd_test_mkdtemp(set->dir, sizeof set->dir, "dsdneo_temp_paths_dir")) {
        return -1;
    }
    if (dsd_test_path_join(set->inner, sizeof set->inner, set->dir, "inner.txt") != 0
        || write_file(set->inner, "inner\n") != 0) {
        return -1;
    }
    if (dsd_test_path_join(set->sub, sizeof set->sub, set->dir, "sub") != 0 || dsd_mkdir(set->sub, 0700) != 0) {
        return -1;
    }
    if (dsd_test_path_join(set->deep, sizeof set->deep, set->sub, "deep.txt") != 0
        || write_file(set->deep, "deep\n") != 0) {
        return -1;
    }
    return 0;
}

static int
remove_set(const temp_set* set) {
    int rc = remove(set->file) == 0 ? 0 : -1;
    rc |= remove(set->deep) == 0 ? 0 : -1;
    rc |= dsd_test_rmdir(set->sub) == 0 ? 0 : -1;
    rc |= remove(set->inner) == 0 ? 0 : -1;
    rc |= dsd_test_rmdir(set->dir) == 0 ? 0 : -1;
    return rc;
}

/* More files than the tracker holds at once, each removed before the next: the full table drops the removed ones and
   keeps tracking, so the last one, left behind, is still removed at exit. */
static int
run_many(void) {
    char path[DSD_TEST_PATH_MAX];
    for (int i = 0; i < 600; i++) {
        const int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_temp_paths_many");
        if (fd < 0) {
            return fail("mkstemp failed");
        }
        (void)dsd_close(fd);
        if (remove(path) != 0) {
            return fail("remove failed");
        }
    }
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_temp_paths_last");
    if (fd < 0) {
        return fail("last mkstemp failed");
    }
    (void)dsd_close(fd);
    return 0;
}

/* What the abort handler can remove, a file and an empty folder, beside a folder it cannot: one holding a file it
   was never told about. */
static int
run_abort(void) {
    char file[DSD_TEST_PATH_MAX];
    char empty[DSD_TEST_PATH_MAX];
    char full[DSD_TEST_PATH_MAX];
    char inner[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(file, sizeof file, "dsdneo_temp_paths_file");
    if (fd < 0) {
        return fail("mkstemp failed");
    }
    (void)dsd_close(fd);
    if (!dsd_test_mkdtemp(empty, sizeof empty, "dsdneo_temp_paths_empty")
        || !dsd_test_mkdtemp(full, sizeof full, "dsdneo_temp_paths_full")
        || dsd_test_path_join(inner, sizeof inner, full, "inner.txt") != 0 || write_file(inner, "inner\n") != 0) {
        return fail("abort setup failed");
    }
    abort();
}

#if !DSD_PLATFORM_WIN_NATIVE
/* A folder left behind that holds a link to @p target: the cleanup removes the link, never what it points to. */
static int
run_link(const char* target) {
    char dir[DSD_TEST_PATH_MAX];
    char link_path[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_temp_paths_link")
        || dsd_test_path_join(link_path, sizeof link_path, dir, "outside") != 0 || symlink(target, link_path) != 0) {
        return fail("link setup failed");
    }
    return 0;
}
#endif

int
main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
    temp_set set;
    if (strcmp(mode, "many") == 0) {
        return run_many();
    }
    if (strcmp(mode, "abort") == 0) {
        return run_abort();
    }
#if !DSD_PLATFORM_WIN_NATIVE
    if (strcmp(mode, "link") == 0) {
        return argc > 2 ? run_link(argv[2]) : fail("link needs a target");
    }
#endif
    if (make_set(&set) != 0) {
        return fail("setup failed");
    }
    if (strcmp(mode, "remove") == 0) {
        return remove_set(&set) == 0 ? 0 : fail("remove failed");
    }
    if (strcmp(mode, "leave") == 0) {
        return 0;
    }
    return fail("unknown mode");
}
