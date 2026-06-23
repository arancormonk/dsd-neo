// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/path_policy.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/file_compat.h"
#include "test_support.h"

static int
write_temp_file(char* out_path, size_t out_size) {
    int fd = dsd_test_mkstemp(out_path, out_size, "dsdneo_path_policy");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }

    static const char payload[] = "path policy\n";
    ssize_t written = dsd_write(fd, payload, sizeof(payload) - 1U);
    int saved_errno = errno;
    (void)dsd_close(fd);
    errno = saved_errno;
    if (written < 0 || (size_t)written != sizeof(payload) - 1U) {
        DSD_FPRINTF(stderr, "write_temp_file write failed: %s\n", strerror(errno));
        (void)remove(out_path);
        return 1;
    }
    return 0;
}

static int
expect_expand_fails(const char* requested, char* out, size_t out_size, int expected_errno) {
    errno = 0;
    if (dsd_path_expand_user(requested, out, out_size) == 0 || errno != expected_errno) {
        DSD_FPRINTF(stderr, "expand failure mismatch for \"%s\": errno=%d expected=%d\n",
                    requested ? requested : "(null)", errno, expected_errno);
        return 1;
    }
    return 0;
}

static int
test_expand_and_absolute_guards(void) {
    char out[DSD_TEST_PATH_MAX];
    int rc = 0;

    rc |= expect_expand_fails(NULL, out, sizeof(out), EINVAL);
    rc |= expect_expand_fails("", out, sizeof(out), EINVAL);
    rc |= expect_expand_fails("abc", NULL, sizeof(out), EINVAL);
    rc |= expect_expand_fails("abc", out, 0, EINVAL);
    rc |= expect_expand_fails("abcdef", out, 4, ENAMETOOLONG);

    if (dsd_path_expand_user("abc", out, sizeof(out)) != 0 || strcmp(out, "abc") != 0) {
        DSD_FPRINTF(stderr, "basic expand failed: rc errno=%d out=%s\n", errno, out);
        rc |= 1;
    }

    if (dsd_path_is_absolute(NULL) || dsd_path_is_absolute("") || dsd_path_is_absolute("relative")) {
        DSD_FPRINTF(stderr, "absolute-path guard mismatch\n");
        rc |= 1;
    }
    if (!dsd_path_is_absolute("/tmp/file") || !dsd_path_is_absolute("\\tmp\\file")) {
        DSD_FPRINTF(stderr, "slash absolute-path detection mismatch\n");
        rc |= 1;
    }
#if DSD_PLATFORM_WIN_NATIVE
    if (!dsd_path_is_absolute("C:\\tmp\\file") || dsd_path_is_absolute("C:relative")) {
        DSD_FPRINTF(stderr, "Windows drive absolute-path detection mismatch\n");
        rc |= 1;
    }
#endif

    return rc;
}

static int
test_relative_resolution(void) {
    char out[DSD_TEST_PATH_MAX];
    int rc = 0;

    if (dsd_path_resolve_relative_to_file(NULL, "leaf.txt", out, sizeof(out)) != 0 || strcmp(out, "leaf.txt") != 0) {
        DSD_FPRINTF(stderr, "NULL base relative resolution mismatch: %s\n", out);
        rc |= 1;
    }
    errno = 0;
    if (dsd_path_resolve_relative_to_file(NULL, "leaf.txt", NULL, sizeof(out)) == 0 || errno != EINVAL) {
        DSD_FPRINTF(stderr, "NULL output relative resolution guard mismatch: errno=%d\n", errno);
        rc |= 1;
    }
    if (dsd_path_resolve_relative_to_file("", "leaf.txt", out, sizeof(out)) != 0 || strcmp(out, "leaf.txt") != 0) {
        DSD_FPRINTF(stderr, "empty base relative resolution mismatch: %s\n", out);
        rc |= 1;
    }
    if (dsd_path_resolve_relative_to_file("<stdin>", "leaf.txt", out, sizeof(out)) != 0
        || strcmp(out, "leaf.txt") != 0) {
        DSD_FPRINTF(stderr, "pseudo base relative resolution mismatch: %s\n", out);
        rc |= 1;
    }
    if (dsd_path_resolve_relative_to_file("config.ini", "leaf.txt", out, sizeof(out)) != 0
        || strcmp(out, "leaf.txt") != 0) {
        DSD_FPRINTF(stderr, "base without separator resolution mismatch: %s\n", out);
        rc |= 1;
    }
    if (dsd_path_resolve_relative_to_file("dir/config.ini", "leaf.txt", out, sizeof(out)) != 0
        || strcmp(out, "dir/leaf.txt") != 0) {
        DSD_FPRINTF(stderr, "slash base relative resolution mismatch: %s\n", out);
        rc |= 1;
    }
    if (dsd_path_resolve_relative_to_file("dir\\config.ini", "leaf.txt", out, sizeof(out)) != 0
        || strcmp(out, "dir\\leaf.txt") != 0) {
        DSD_FPRINTF(stderr, "backslash base relative resolution mismatch: %s\n", out);
        rc |= 1;
    }
    if (dsd_path_resolve_relative_to_file("dir/sub\\config.ini", "leaf.txt", out, sizeof(out)) != 0
        || strcmp(out, "dir/sub\\leaf.txt") != 0) {
        DSD_FPRINTF(stderr, "mixed-separator base relative resolution mismatch: %s\n", out);
        rc |= 1;
    }

    errno = 0;
    if (dsd_path_resolve_relative_to_file("dir/config.ini", "leaf.txt", out, 5) == 0 || errno != ENAMETOOLONG) {
        DSD_FPRINTF(stderr, "short relative output buffer mismatch: errno=%d\n", errno);
        rc |= 1;
    }
    errno = 0;
    if (dsd_path_resolve_relative_to_file("dir/config.ini", "", out, sizeof(out)) == 0 || errno != EINVAL) {
        DSD_FPRINTF(stderr, "relative empty requested guard mismatch: errno=%d\n", errno);
        rc |= 1;
    }

    return rc;
}

static int
test_open_regular_file_paths(void) {
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_file(path, sizeof(path)) != 0) {
        return 1;
    }

    int rc = 0;
    char opened[DSD_TEST_PATH_MAX];
    FILE* fp = dsd_path_fopen_user_read_file(path, opened, sizeof(opened));
    if (!fp || strcmp(opened, path) != 0) {
        DSD_FPRINTF(stderr, "fopen regular file failed: errno=%d opened=%s\n", errno, opened);
        rc |= 1;
    }
    if (fp) {
        (void)fclose(fp);
    }

    opened[0] = '\0';
    fp = dsd_path_fopen_user_read_file(path, NULL, 0);
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen regular file without output buffer failed: errno=%d\n", errno);
        rc |= 1;
    }
    if (fp) {
        (void)fclose(fp);
    }

    errno = 0;
    if (dsd_path_fopen_user_read_file(path, opened, 4) != NULL || errno != ENAMETOOLONG) {
        DSD_FPRINTF(stderr, "fopen short output buffer mismatch: errno=%d\n", errno);
        rc |= 1;
    }

    errno = 0;
    if (dsd_path_resolve_user_read_file(path, opened, sizeof(opened)) != 0 || strcmp(opened, path) != 0) {
        DSD_FPRINTF(stderr, "resolve regular file failed: errno=%d opened=%s\n", errno, opened);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_open_failure_paths(void) {
    char missing[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(missing, sizeof(missing), dsd_test_tmpdir(), "dsdneo_path_policy_missing_file") != 0) {
        DSD_FPRINTF(stderr, "missing path join failed: %s\n", strerror(errno));
        return 1;
    }
    (void)remove(missing);

    int rc = 0;
    char out[DSD_TEST_PATH_MAX];
    errno = 0;
    if (dsd_path_resolve_user_read_file(missing, out, sizeof(out)) == 0 || errno == 0) {
        DSD_FPRINTF(stderr, "missing file resolve unexpectedly succeeded or left errno unset\n");
        rc |= 1;
    }

    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_path_policy_dir")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    errno = 0;
    if (dsd_path_fopen_user_read_file(dir, out, sizeof(out)) != NULL || errno != EINVAL) {
        DSD_FPRINTF(stderr, "directory open rejection mismatch: errno=%d\n", errno);
        rc |= 1;
    }
#if DSD_PLATFORM_WIN_NATIVE
    (void)_rmdir(dir);
#else
    (void)rmdir(dir);
#endif

    errno = 0;
    if (dsd_path_fopen_user_read_file("", out, sizeof(out)) != NULL || errno != EINVAL) {
        DSD_FPRINTF(stderr, "empty fopen requested guard mismatch: errno=%d\n", errno);
        rc |= 1;
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_expand_and_absolute_guards();
    rc |= test_relative_resolution();
    rc |= test_open_regular_file_paths();
    rc |= test_open_failure_paths();
    if (rc != 0) {
        return 1;
    }

    DSD_FPRINTF(stderr, "runtime path policy tests OK\n");
    return 0;
}
