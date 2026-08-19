// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/platform/platform.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#define DSD_TEST_RMDIR _rmdir
#else
#include <unistd.h>
#define DSD_TEST_RMDIR rmdir
#endif

static int
make_paths(char* scratch, size_t scratch_sz, char* csv, size_t csv_sz, char* rr, size_t rr_sz, const char* prefix) {
    if (dsd_test_mkdtemp(scratch, scratch_sz, prefix) == NULL) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    if (dsd_test_path_join(csv, csv_sz, scratch, "SARA System group.csv") != 0) {
        return 1;
    }
    int n = DSD_SNPRINTF(rr, rr_sz, "%s.rr", csv);
    return n > 0 && (size_t)n < rr_sz ? 0 : 1;
}

static void
fill_sample(dsd_rr_provenance* p) {
    DSD_MEMSET(p, 0, sizeof *p);
    DSD_STRNCPY(p->kind, "group", sizeof p->kind - 1);
    p->sid = 12059;
    DSD_STRNCPY(p->site_ids, "4181,4182", sizeof p->site_ids - 1);
    p->partial_enc_as_de = 1;
    DSD_STRNCPY(p->system_name, "SARA System", sizeof p->system_name - 1);
    p->imported_at = 1755500000LL;
}

static int
write_sidecar_text(const char* rr_path, const char* body) {
    FILE* fp = dsd_fopen_private(rr_path, "w");
    if (!fp) {
        return 1;
    }
    int rc = fputs(body, fp) < 0 ? 1 : 0;
    rc |= fclose(fp) != 0 ? 1 : 0;
    return rc;
}

static int
expect_round_trip(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_prov") != 0) {
        return 1;
    }

    dsd_rr_provenance p;
    fill_sample(&p);

    int rc = dsd_rr_provenance_write(csv, &p) == 0 ? 0 : 1;

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= strcmp(got.kind, "group") == 0 ? 0 : 1;
    rc |= got.sid == 12059 ? 0 : 1;
    rc |= strcmp(got.site_ids, "4181,4182") == 0 ? 0 : 1;
    rc |= got.partial_enc_as_de == 1 ? 0 : 1;
    rc |= strcmp(got.system_name, "SARA System") == 0 ? 0 : 1;
    rc |= got.imported_at == 1755500000LL ? 0 : 1;

    /* Overwrite an existing sidecar: this is the Windows replace-existing path. */
    p.sid = 9340;
    rc |= dsd_rr_provenance_write(csv, &p) == 0 ? 0 : 1;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.sid == 9340 ? 0 : 1;

    /* No leftover temp file. */
    dsd_stat_t st;
    char tmp_glob[DSD_TEST_PATH_MAX];
    rc |= DSD_SNPRINTF(tmp_glob, sizeof tmp_glob, "%s.tmp.XXXXXX", rr) > 0 ? 0 : 1;
    rc |= dsd_stat_path(tmp_glob, &st) != 0 ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_zero_timestamp_is_stamped(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_stamp") != 0) {
        return 1;
    }

    dsd_rr_provenance p;
    fill_sample(&p);
    p.imported_at = 0;

    int rc = dsd_rr_provenance_write(csv, &p) == 0 ? 0 : 1;

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.imported_at > 1700000000LL ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_missing_sidecar_fails(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_missing") != 0) {
        return 1;
    }

    dsd_rr_provenance got;
    fill_sample(&got);
    int rc = dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;
    /* A failed read must not clobber the caller's struct. */
    rc |= got.sid == 12059 ? 0 : 1;

    rc |= dsd_rr_provenance_read(NULL, &got) == -1 ? 0 : 1;
    rc |= dsd_rr_provenance_read(csv, NULL) == -1 ? 0 : 1;
    rc |= dsd_rr_provenance_write(csv, NULL) == -1 ? 0 : 1;

    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_unknown_keys_are_ignored(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_unknown") != 0) {
        return 1;
    }

    int rc = write_sidecar_text(rr, "# dsd-neo RadioReference provenance. Regenerated on refresh; do not edit.\n"
                                    "kind = chan\n"
                                    "sid = 6673\n"
                                    "site_ids = 4181\n"
                                    "partial_enc_as_de = 0\n"
                                    "system_name = Example\n"
                                    "imported_at = 1755500000\n"
                                    "future_key = whatever\n"
                                    "\n"
                                    "a line with no separator\n");

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= strcmp(got.kind, "chan") == 0 ? 0 : 1;
    rc |= got.sid == 6673 ? 0 : 1;
    rc |= strcmp(got.site_ids, "4181") == 0 ? 0 : 1;
    rc |= got.partial_enc_as_de == 0 ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_malformed_values_fail(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_bad") != 0) {
        return 1;
    }

    dsd_rr_provenance got;
    int rc = write_sidecar_text(rr, "kind = group\nsid = junk\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;

    rc |= write_sidecar_text(rr, "kind = group\nsid = 1\npartial_enc_as_de = 2\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;

    rc |= write_sidecar_text(rr, "kind = group\nimported_at = -1\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= expect_round_trip();
    rc |= expect_zero_timestamp_is_stamped();
    rc |= expect_missing_sidecar_fails();
    rc |= expect_unknown_keys_are_ignored();
    rc |= expect_malformed_values_fail();
    return rc;
}
