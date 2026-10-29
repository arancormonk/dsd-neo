// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Test temp files go to the temp directory, never the working directory, when no temp variable is set: a test run
 * from a checkout that aborted before its cleanup used to leave dsdneo_* files in the repository root. */

#include <assert.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/platform/platform.h"
#include "test_support.h"

int
main(void) {
    assert(dsd_test_unsetenv("DSD_NEO_TEST_TMPDIR") == 0);

#if DSD_PLATFORM_WIN_NATIVE
    assert(dsd_test_setenv("TEMP", "C:\\dsdneo-temp", 1) == 0);
    assert(strcmp(dsd_test_tmpdir(), "C:\\dsdneo-temp") == 0);
#else
    assert(dsd_test_unsetenv("TMPDIR") == 0);
    assert(strcmp(dsd_test_tmpdir(), "/tmp") == 0);

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsdneo_test_tmpdir");
    assert(fd >= 0);
    assert(strncmp(path, "/tmp/dsdneo_test_tmpdir_", strlen("/tmp/dsdneo_test_tmpdir_")) == 0);
    assert(dsd_close(fd) == 0);
    assert(remove(path) == 0);

    assert(dsd_test_setenv("TMPDIR", "/var/dsdneo-tmp", 1) == 0);
    assert(strcmp(dsd_test_tmpdir(), "/var/dsdneo-tmp") == 0);
#endif

    assert(dsd_test_setenv("DSD_NEO_TEST_TMPDIR", "dsdneo-override", 1) == 0);
    assert(strcmp(dsd_test_tmpdir(), "dsdneo-override") == 0);

    printf("PLATFORM_TEST_TMPDIR: OK\n");
    return 0;
}
