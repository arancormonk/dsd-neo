// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #888: the per-call WAV size probe carries a 64-bit size. A recording past 2 GiB must measure as its real size
 * (on Windows `long` is 32 bits), so the close neither mistakes it for the 44-byte empty header nor skips its export.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <stdint.h>
#include <stdio.h>
#include "core/file/wav_size_test_support.h"
#include "sparse_file.h"
#include "test_support.h"

/* The probe's answer is a 64-bit integer, whatever the platform's long. */
_Static_assert(sizeof(dsd_wav_file_size_or_negative("x")) == 8, "the WAV size probe must return a 64-bit size");

#define SKIP_CODE   77
#define SPARSE_SIZE (((int64_t)1 << 31) + 1000)

static int
test_small_and_missing(const char* dir) {
    char path[512];
    if (dsd_test_path_join(path, sizeof path, dir, "small.wav") != 0) {
        return 1;
    }
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "cannot create %s\n", path);
        return 1;
    }
    const unsigned char hdr[44] = {0};
    (void)fwrite(hdr, 1, sizeof hdr, fp);
    fclose(fp);
    int rc = 0;
    if (dsd_wav_file_size_or_negative(path) != 44) {
        DSD_FPRINTF(stderr, "44-byte file measured wrong\n");
        rc = 1;
    }
    (void)dsd_remove_file(path);
    if (dsd_wav_file_size_or_negative(path) != -1 || dsd_wav_file_size_or_negative("") != -1
        || dsd_wav_file_size_or_negative(NULL) != -1) {
        DSD_FPRINTF(stderr, "missing file not reported as -1\n");
        rc = 1;
    }
    return rc;
}

/* A temp file grown to @p size as a sparse file by the test support (dsd_test_grow_sparse()), independently of the
   seek the probe uses, its path in @p path: 0, or -1 (removed) when the filesystem cannot hold one. */
static int
make_sparse(char* path, size_t path_size, int64_t size) {
    const int fd = dsd_test_mkstemp(path, path_size, "dsdneo_wavsize");
    if (fd < 0) {
        return -1;
    }
    const int grown = dsd_test_grow_sparse(fd, size);
    const int closed = dsd_close(fd);
    if (grown != 0 || closed != 0) {
        (void)dsd_remove_file(path);
        return -1;
    }
    return 0;
}

/* 0 pass, 1 fail, SKIP_CODE when no sparse file past 2 GiB can be made. A small hole first: a filesystem that would
   allocate the gap (no sparse files, such as exFAT) is found before it is asked for 2 GiB of real space. */
static int
test_over_2gib(void) {
    char path[DSD_TEST_PATH_MAX];
    if (make_sparse(path, sizeof path, (int64_t)1 << 20) != 0) {
        printf("SKIP: the filesystem cannot hold a sparse file\n");
        return SKIP_CODE;
    }
    (void)dsd_remove_file(path);
    if (make_sparse(path, sizeof path, SPARSE_SIZE) != 0) {
        printf("SKIP: the filesystem cannot hold a sparse file past 2 GiB\n");
        return SKIP_CODE;
    }
    int rc = 0;
    dsd_stat_t st;
    if (dsd_stat_path(path, &st) != 0 || (int64_t)st.st_size != SPARSE_SIZE) {
        DSD_FPRINTF(stderr, "the sparse fixture is not %lld bytes\n", (long long)SPARSE_SIZE);
        rc = 1;
    }
    const int64_t got = dsd_wav_file_size_or_negative(path);
    if (got != SPARSE_SIZE) {
        DSD_FPRINTF(stderr, "over-2GiB file measured %lld, want %lld\n", (long long)got, (long long)SPARSE_SIZE);
        rc = 1;
    }
    (void)dsd_remove_file(path);
    return rc;
}

int
main(void) {
    char dir[512];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "wavsize")) {
        DSD_FPRINTF(stderr, "no temp dir\n");
        return 1;
    }
    int rc = test_small_and_missing(dir);
    const int big = test_over_2gib();
    (void)dsd_test_rmdir(dir);
    if (rc != 0 || (big != 0 && big != SKIP_CODE)) {
        return 1;
    }
    return big;
}
