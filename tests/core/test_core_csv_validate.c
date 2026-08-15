// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

static int
write_temp_csv(char* tmpl, const char* contents) {
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);
    FILE* fp = dsd_fopen_private(tmpl, "w");
    if (!fp) {
        (void)remove(tmpl);
        return -1;
    }
    DSD_FPRINTF(fp, "%s", contents);
    fclose(fp);
    return 0;
}

static int
test_missing_file_fails(void) {
    dsd_csv_validation v = {9U, 9U, 9U};
    if (dsd_csv_validate_group_file("dsd-neo-test-validate-missing-dir/missing.csv", &v) == 0) {
        DSD_FPRINTF(stderr, "group validate accepted a missing file\n");
        return 1;
    }
    if (dsd_csv_validate_chan_file("dsd-neo-test-validate-missing-dir/missing.csv", &v) == 0) {
        DSD_FPRINTF(stderr, "chan validate accepted a missing file\n");
        return 1;
    }
    if (dsd_csv_validate_group_file(NULL, &v) == 0 || dsd_csv_validate_group_file("", &v) == 0) {
        DSD_FPRINTF(stderr, "group validate accepted an empty path\n");
        return 1;
    }
    return 0;
}

static int
test_group_header_only(void) {
    char tmpl[] = "dsd-neo-test-validate-group-hdr-XXXXXX";
    if (write_temp_csv(tmpl, "TG,Mode,Name\n") != 0) {
        return 1;
    }
    dsd_csv_validation v = {9U, 9U, 9U};
    int failed = 0;
    if (dsd_csv_validate_group_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "group validate failed on header-only file\n");
        failed = 1;
    }
    if (v.accepted != 0U || v.skipped != 0U || v.total != 0U) {
        DSD_FPRINTF(stderr, "group header-only counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

static int
test_group_counts_mixed_rows(void) {
    char tmpl[] = "dsd-neo-test-validate-group-mix-XXXXXX";
    if (write_temp_csv(tmpl, "TG,Mode,Name\n"
                             "101,D,Dispatch\n"
                             "201-210,D,Ops Range\n"
                             "bogus,D,Bad Id\n"
                             "301\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_group_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "group validate failed on mixed file\n");
        failed = 1;
    }
    if (v.accepted != 2U || v.skipped != 2U || v.total != 4U) {
        DSD_FPRINTF(stderr, "group mixed counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

static int
test_chan_counts_mixed_rows(void) {
    char tmpl[] = "dsd-neo-test-validate-chan-mix-XXXXXX";
    if (write_temp_csv(tmpl, "channel_number,frequency_hz\n"
                             "1,851000000\n"
                             "2,notafreq\n"
                             "notachan,852000000\n"
                             "3,852500000\n"
                             "4\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_chan_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "chan validate failed on mixed file\n");
        failed = 1;
    }
    if (v.accepted != 2U || v.skipped != 3U || v.total != 5U) {
        DSD_FPRINTF(stderr, "chan mixed counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

static int
test_key_dec_counts_mixed_rows(void) {
    char tmpl[] = "dsd-neo-test-validate-keydec-XXXXXX";
    if (write_temp_csv(tmpl, "key id (dec),key value (dec)\n"
                             "2,70\n"
                             "3,notanumber\n"
                             "5\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_key_file_dec(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "dec key validate failed on mixed file\n");
        failed = 1;
    }
    if (v.accepted != 1U || v.skipped != 2U || v.total != 3U) {
        DSD_FPRINTF(stderr, "dec key mixed counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

static int
test_key_hex_counts_mixed_rows(void) {
    char tmpl[] = "dsd-neo-test-validate-keyhex-XXXXXX";
    if (write_temp_csv(tmpl, "key id (hex),key value (hex)\n"
                             "C197,A753BC945DE5E0F1\n"
                             "C198,nothexatall\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_key_file_hex(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "hex key validate failed on mixed file\n");
        failed = 1;
    }
    if (v.accepted != 1U || v.skipped != 1U || v.total != 2U) {
        DSD_FPRINTF(stderr, "hex key mixed counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

int
main(void) {
    if (test_missing_file_fails() != 0) {
        return 1;
    }
    if (test_group_header_only() != 0) {
        return 1;
    }
    if (test_group_counts_mixed_rows() != 0) {
        return 1;
    }
    if (test_chan_counts_mixed_rows() != 0) {
        return 1;
    }
    if (test_key_dec_counts_mixed_rows() != 0) {
        return 1;
    }
    if (test_key_hex_counts_mixed_rows() != 0) {
        return 1;
    }
    return 0;
}
