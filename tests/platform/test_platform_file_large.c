// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * File sizes and offsets past 2 GiB and 4 GiB (issue #701): dsd_stat_path()/dsd_fstat() report the whole size, and
 * dsd_fseek64()/dsd_ftell64() reach and report offsets that a long (32 bits on Windows) cannot hold. The files are
 * sparse; where the filesystem cannot make one, the case is skipped.
 */

#include <dsd-neo/platform/file_compat.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"
#include "sparse_file.h"
#include "test_support.h"

#define GIB ((int64_t)1 << 30)

static int
expect_i64(const char* tag, const char* label, int64_t got, int64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: %s: got=%" PRId64 " want=%" PRId64 "\n", tag, label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s: %s\n", tag, label);
        return 1;
    }
    return 0;
}

/* Write @p text at @p offset through dsd_fseek64(). */
static int
write_at(FILE* fp, int64_t offset, const char* text) {
    if (dsd_fseek64(fp, offset, SEEK_SET) != 0) {
        return -1;
    }
    const size_t n = strlen(text);
    return fwrite(text, 1, n, fp) == n ? 0 : -1;
}

/* Read strlen(@p want) bytes at @p offset through dsd_fseek64() and compare. */
static int
read_matches_at(FILE* fp, int64_t offset, const char* want) {
    char buf[32];
    const size_t n = strlen(want);
    if (n > sizeof(buf) || dsd_fseek64(fp, offset, SEEK_SET) != 0) {
        return 0;
    }
    if (fread(buf, 1, n, fp) != n) {
        return 0;
    }
    return memcmp(buf, want, n) == 0;
}

/* One sparse file of @p size bytes with markers near its end and, when it reaches them, past 2 GiB and 4 GiB.
 * Returns 0 on pass or skip, 1 on failure; *ran is set when the case ran rather than skipped. */
static int
check_large_file(const char* tag, int64_t size, int* ran) {
    *ran = 0;
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsdneo_file_large");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: mkstemp: %s\n", tag, strerror(errno));
        return 1;
    }
    if (dsd_test_grow_sparse(fd, size) != 0) {
        DSD_FPRINTF(stderr, "SKIP: %s: cannot make a sparse file of %" PRId64 " bytes here: %s\n", tag, size,
                    strerror(errno));
        (void)dsd_close(fd);
        (void)remove(path);
        return 0;
    }
    *ran = 1;

    const int64_t past_2g = 2 * GIB + 16;
    const int64_t past_4g = 4 * GIB + 32;
    const int64_t tail = size - 5;
    int rc = 0;

    FILE* fp = dsd_test_fdopen(fd, "r+b");
    if (!fp) {
        DSD_FPRINTF(stderr, "FAIL: %s: fdopen: %s\n", tag, strerror(errno));
        (void)dsd_close(fd);
        (void)remove(path);
        return 1;
    }
    rc |= expect_true(tag, "write head", write_at(fp, 0, "HEAD") == 0);
    rc |= expect_true(tag, "write past 2 GiB", write_at(fp, past_2g, "MARK2") == 0);
    if (size > past_4g + 8) {
        rc |= expect_true(tag, "write past 4 GiB", write_at(fp, past_4g, "MARK4") == 0);
    }
    rc |= expect_true(tag, "write tail", write_at(fp, tail, "TAIL!") == 0);
    rc |= expect_true(tag, "close after writes", fclose(fp) == 0);

    dsd_stat_t st;
    rc |= expect_true(tag, "stat path", dsd_stat_path(path, &st) == 0);
    rc |= expect_i64(tag, "dsd_stat_path size", (int64_t)st.st_size, size);
    rc |= expect_true(tag, "stat is regular", dsd_stat_is_regular(&st));

    fp = dsd_fopen_existing_regular_file(path, "rb");
    if (!fp) {
        DSD_FPRINTF(stderr, "FAIL: %s: reopen: %s\n", tag, strerror(errno));
        (void)remove(path);
        return 1;
    }
    dsd_stat_t fst;
    rc |= expect_true(tag, "fstat", dsd_fstat(dsd_fileno(fp), &fst) == 0);
    rc |= expect_i64(tag, "dsd_fstat size", (int64_t)fst.st_size, size);

    rc |= expect_true(tag, "head reads back", read_matches_at(fp, 0, "HEAD"));
    rc |= expect_true(tag, "2 GiB marker reads back", read_matches_at(fp, past_2g, "MARK2"));
    rc |= expect_i64(tag, "tell after 2 GiB marker", dsd_ftell64(fp), past_2g + 5);
    if (size > past_4g + 8) {
        rc |= expect_true(tag, "4 GiB marker reads back", read_matches_at(fp, past_4g, "MARK4"));
        rc |= expect_i64(tag, "tell after 4 GiB marker", dsd_ftell64(fp), past_4g + 5);
    }
    rc |= expect_true(tag, "tail reads back", read_matches_at(fp, tail, "TAIL!"));

    rc |= expect_true(tag, "seek to end", dsd_fseek64(fp, 0, SEEK_END) == 0);
    rc |= expect_i64(tag, "tell at end", dsd_ftell64(fp), size);
    rc |= expect_true(tag, "seek back from end", dsd_fseek64(fp, -5, SEEK_END) == 0);
    rc |= expect_i64(tag, "tell 5 before end", dsd_ftell64(fp), tail);
    rc |= expect_true(tag, "seek to 1 GiB", dsd_fseek64(fp, GIB, SEEK_SET) == 0);
    rc |= expect_true(tag, "seek relative past 2 GiB", dsd_fseek64(fp, past_2g - GIB, SEEK_CUR) == 0);
    rc |= expect_i64(tag, "tell after relative seek", dsd_ftell64(fp), past_2g);
    char buf[5];
    rc |= expect_true(tag, "read after relative seek",
                      fread(buf, 1, sizeof(buf), fp) == sizeof(buf) && memcmp(buf, "MARK2", 5) == 0);
    /* A sparse hole reads back as zeros. */
    unsigned char hole[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    rc |= expect_true(tag, "seek into a hole", dsd_fseek64(fp, past_2g + 4096, SEEK_SET) == 0);
    rc |= expect_true(tag, "hole reads as zeros",
                      fread(hole, 1, sizeof(hole), fp) == sizeof(hole) && hole[0] == 0 && hole[3] == 0);

    (void)fclose(fp);
    rc |= expect_true(tag, "remove", remove(path) == 0);
    return rc;
}

static int
test_bad_arguments(void) {
    int rc = 0;
    errno = 0;
    rc |= expect_true("args", "fseek64 NULL stream", dsd_fseek64(NULL, 0, SEEK_SET) == -1 && errno == EINVAL);
    errno = 0;
    rc |= expect_true("args", "ftell64 NULL stream", dsd_ftell64(NULL) == -1 && errno == EINVAL);
    return rc;
}

int
main(void) {
    int rc = 0;
    int ran_2g = 0;
    int ran_4g = 0;
    rc |= test_bad_arguments();
    /* Between 2 GiB and 4 GiB: a signed 32-bit size or offset goes negative. */
    rc |= check_large_file("past 2 GiB", 3 * GIB + 7, &ran_2g);
    /* Past 4 GiB: an unsigned 32-bit size wraps. */
    rc |= check_large_file("past 4 GiB", 5 * GIB + 4103, &ran_4g);
    if (rc == 0) {
        DSD_FPRINTF(stdout, "PLATFORM_FILE_LARGE: OK (2 GiB case %s, 4 GiB case %s)\n", ran_2g ? "ran" : "skipped",
                    ran_4g ? "ran" : "skipped");
    }
    return rc;
}
