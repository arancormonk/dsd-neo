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
#include <string.h>
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

/*
 * A trailing empty line is filler, not a malformed row — editors and several
 * exporters leave one behind, and counting it would tell the user a clean file
 * had rows it could not read.
 */
static int
test_blank_lines_are_not_rows(void) {
    char tmpl[] = "dsd-neo-test-validate-blank-XXXXXX";
    if (write_temp_csv(tmpl, "TG,Mode,Name\n"
                             "101,D,Dispatch\n"
                             "\n"
                             "102,D,Fire\n"
                             "   \n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_group_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "group validate failed on file with blank lines\n");
        failed = 1;
    }
    if (v.accepted != 2U || v.skipped != 0U || v.total != 2U) {
        DSD_FPRINTF(stderr, "blank-line counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

/*
 * A key id that is not decimal normalizes to 0, so every such row would "store"
 * onto slot 0 together. Reporting them as loaded is how a hex-id file passed off
 * as decimal validates as "N keys" while exactly one key exists.
 */
static int
test_key_dec_bad_id_is_skipped(void) {
    char tmpl[] = "dsd-neo-test-validate-keyid-XXXXXX";
    if (write_temp_csv(tmpl, "key id (dec),key value (dec)\n"
                             "C197,1234\n"
                             "C198,5678\n"
                             "7,9012\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_key_file_dec(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "dec key validate failed on bad-id file\n");
        failed = 1;
    }
    if (v.accepted != 1U || v.skipped != 2U || v.total != 3U) {
        DSD_FPRINTF(stderr, "dec key bad-id counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

/*
 * A channel map row and a decimal key row are both `number,number`, and the
 * header line is free text, so nothing in the file says which it is. Picking a
 * key list as a channel map used to validate as "N channels" and load its
 * values as frequencies -- the doc's own example row `2,70` became a channel at
 * 70 Hz, which the trunking SM would then try to tune. The frequency column
 * being a plausible RF frequency is what tells them apart.
 */
static int
test_chan_rejects_a_key_list(void) {
    char tmpl[] = "dsd-neo-test-validate-chan-keys-XXXXXX";
    /* docs/csv-formats.md's own decimal key example, verbatim. */
    if (write_temp_csv(tmpl, "key id or tg id (dec),key number or value (dec)\n"
                             "2,70\n"
                             "12,48713912656\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_chan_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "chan validate failed to open a key list\n");
        failed = 1;
    }
    /* Nothing usable, which is what the imports screen reports as "no usable
     * rows" and what svc_import_channel_map() refuses to adopt. */
    if (v.accepted != 0U || v.skipped != 2U || v.total != 2U) {
        DSD_FPRINTF(stderr, "key list read as a channel map: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

/* The bounds are generous on purpose -- they reject numbers that cannot be
 * radio frequencies at all, not frequencies outside a band plan. */
static int
test_chan_frequency_bounds(void) {
    char tmpl[] = "dsd-neo-test-validate-chan-bounds-XXXXXX";
    if (write_temp_csv(tmpl, "channel_number,frequency_hz\n"
                             "1,0\n"          /* a blank column parses to this */
                             "2,99999\n"      /* just under the floor */
                             "3,100000\n"     /* the floor itself: HF, kept */
                             "4,6000000000\n" /* the ceiling: 6 GHz, kept */
                             "5,6000000001\n" /* past any front end's reach */
                             "6,851000000\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_chan_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "chan validate failed on the bounds file\n");
        failed = 1;
    }
    if (v.accepted != 3U || v.skipped != 3U || v.total != 6U) {
        DSD_FPRINTF(stderr, "chan bounds counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

/* The optional name column is a label, not a row the counters treat differently:
 * a named row still stands or falls on its frequency. */
static int
test_chan_counts_with_name_column(void) {
    char tmpl[] = "dsd-neo-test-validate-chan-name-XXXXXX";
    if (write_temp_csv(tmpl, "channel,frequency_hz,name\n"
                             "1,851000000,Dispatch\n"
                             "2,notafreq,Ops\n"
                             "3,852000000,Fireground\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_chan_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "chan validate failed on a named file\n");
        failed = 1;
    }
    if (v.accepted != 2U || v.skipped != 1U || v.total != 3U) {
        DSD_FPRINTF(stderr, "chan name-column counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

/* Empty fields are kept rather than collapsed, so column 2 of `1,,851000000` is
 * the (blank) frequency and the row is skipped. Collapsing them used to promote
 * the third column into the frequency's place and silently load a shifted row. */
static int
test_chan_empty_frequency_field_is_skipped(void) {
    char tmpl[] = "dsd-neo-test-validate-chan-empty-XXXXXX";
    if (write_temp_csv(tmpl, "channel_number,frequency_hz\n"
                             "1,,851000000\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_chan_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "chan validate failed on an empty-field file\n");
        failed = 1;
    }
    if (v.accepted != 0U || v.skipped != 1U || v.total != 1U) {
        DSD_FPRINTF(stderr, "chan empty-field counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

/* Key columns ride along without moving the counters: a keyed row still stands
 * or falls on its frequency, and validation opens the key files, so an
 * unloadable key path fails validation the way it fails the import. */
static int
test_chan_counts_with_key_columns(void) {
    char key_tmpl[] = "dsd-neo-test-validate-rowkey-XXXXXX";
    char tmpl[] = "dsd-neo-test-validate-chan-keys-XXXXXX";
    char body[1024];
    if (write_temp_csv(key_tmpl, "key id(hex),key value (hex)\n0010,AAAAAAAAAAAAAAAA\n") != 0) {
        return 1;
    }
    body[0] = '\0';
    (void)DSD_SNPRINTF(body + strlen(body), sizeof(body) - strlen(body),
                       "channel,frequency_hz,keys_hex_csv\n"
                       "1,851000000,%s\n"
                       "2,notafreq,%s\n"
                       "3,852000000,\n",
                       key_tmpl, key_tmpl);
    if (write_temp_csv(tmpl, body) != 0) {
        (void)remove(key_tmpl);
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_chan_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "chan validate failed on a keyed file\n");
        failed = 1;
    }
    if (v.accepted != 2U || v.skipped != 1U || v.total != 3U) {
        DSD_FPRINTF(stderr, "chan key-column counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(key_tmpl);
    (void)remove(tmpl);
    return failed;
}

static int
test_chan_key_column_bad_path_fails(void) {
    char tmpl[] = "dsd-neo-test-validate-chan-badkey-XXXXXX";
    if (write_temp_csv(tmpl, "channel,frequency_hz,keys_hex_csv\n"
                             "1,851000000,dsd-neo-test-validate-missing-dir/missing.csv\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_chan_file(tmpl, &v) == 0) {
        DSD_FPRINTF(stderr, "chan validate accepted an unloadable key path\n");
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

static int
test_p25_bandplan_counts_mixed_rows(void) {
    char tmpl[] = "dsd-neo-test-validate-bandplan-XXXXXX";
    if (write_temp_csv(tmpl, "iden,base_hz,spacing_hz,type,tx_offset_hz,bandwidth_hz,wacn,sysid\n"
                             "0,851006250,6250,1,-45000000,12500,,\n"
                             "1,851006251,6250,1,,,,\n"
                             "\n"
                             "2,762006250,6250,3,,,BEE00,3A1\n"
                             "16,851006250,6250,,,,,\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_p25_bandplan_file(tmpl, &v) != 0) {
        DSD_FPRINTF(stderr, "bandplan validate failed on mixed file\n");
        failed = 1;
    }
    if (v.accepted != 2U || v.skipped != 2U || v.total != 4U) {
        DSD_FPRINTF(stderr, "bandplan mixed counts wrong: accepted=%u skipped=%u total=%u\n", v.accepted, v.skipped,
                    v.total);
        failed = 1;
    }
    (void)remove(tmpl);
    return failed;
}

static int
test_p25_bandplan_rejects_other_kinds(void) {
    // The Android library checks the kind the user picked by content: a channel map or a key
    // list yields zero accepted band-plan rows (too few columns, or a base that is not a
    // multiple of 5 Hz).
    char chan[] = "dsd-neo-test-validate-bandplan-chan-XXXXXX";
    char plan[] = "dsd-neo-test-validate-bandplan-plan-XXXXXX";
    if (write_temp_csv(chan, "channel,freq\n"
                             "1,851000000\n"
                             "2,70\n")
        != 0) {
        return 1;
    }
    if (write_temp_csv(plan, "iden,base_hz,spacing_hz\n"
                             "0,851006250,6250\n")
        != 0) {
        (void)remove(chan);
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_p25_bandplan_file(chan, &v) != 0 || v.accepted != 0U || v.total != 2U) {
        DSD_FPRINTF(stderr, "bandplan validate accepted a channel map: accepted=%u total=%u\n", v.accepted, v.total);
        failed = 1;
    }
    if (dsd_csv_validate_p25_bandplan_file("dsd-neo-test-validate-missing-dir/missing.csv", &v) == 0) {
        DSD_FPRINTF(stderr, "bandplan validate accepted a missing file\n");
        failed = 1;
    }
    (void)remove(chan);
    (void)remove(plan);
    return failed;
}

int
main(void) {
    if (test_missing_file_fails() != 0) {
        return 1;
    }
    if (test_chan_rejects_a_key_list() != 0) {
        return 1;
    }
    if (test_chan_frequency_bounds() != 0) {
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
    if (test_chan_counts_with_name_column() != 0) {
        return 1;
    }
    if (test_chan_empty_frequency_field_is_skipped() != 0) {
        return 1;
    }
    if (test_chan_counts_with_key_columns() != 0) {
        return 1;
    }
    if (test_chan_key_column_bad_path_fails() != 0) {
        return 1;
    }
    if (test_key_dec_counts_mixed_rows() != 0) {
        return 1;
    }
    if (test_key_hex_counts_mixed_rows() != 0) {
        return 1;
    }
    if (test_blank_lines_are_not_rows() != 0) {
        return 1;
    }
    if (test_key_dec_bad_id_is_skipped() != 0) {
        return 1;
    }
    if (test_p25_bandplan_counts_mixed_rows() != 0) {
        return 1;
    }
    if (test_p25_bandplan_rejects_other_kinds() != 0) {
        return 1;
    }
    return 0;
}
