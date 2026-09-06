// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "dsd-neo/core/safe_api.h"

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

/*
 * A read error partway through a file is the one way a validator can fail after it has
 * already counted rows, and the import pickers show whatever counts come back. Faulting
 * the stream is the only way to reach it: every real regular file the parser accepts
 * reads to EOF. The build arms this only where --wrap and fopencookie() both exist, and
 * the fault itself stays off unless a case turns it on, so every other case here opens
 * its file for real.
 */
#if defined(DSD_TEST_FAULT_READS)
// IWYU pragma: no_include <bits/types/cookie_io_functions_t.h>

static int g_fault_reads;
static int g_fault_hits;
static char g_fault_content[1400];
static size_t g_fault_size;
static size_t g_fault_offset;

// GNU ld --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
FILE* __real_dsd_path_fopen_user_read_file(const char* requested, char* out, size_t out_size);
FILE* __wrap_dsd_path_fopen_user_read_file(const char* requested, char* out, size_t out_size);

static ssize_t
fault_read(void* cookie, char* buf, size_t size) {
    (void)cookie;
    if (g_fault_offset >= g_fault_size) {
        g_fault_hits++;
        errno = EIO;
        return -1;
    }
    size_t n = g_fault_size - g_fault_offset;
    if (n > size) {
        n = size;
    }
    DSD_MEMCPY(buf, g_fault_content + g_fault_offset, n);
    g_fault_offset += n;
    return (ssize_t)n;
}

FILE*
__wrap_dsd_path_fopen_user_read_file(const char* requested, char* out, size_t out_size) {
    if (!g_fault_reads) {
        return __real_dsd_path_fopen_user_read_file(requested, out, out_size);
    }
    if (out && out_size > 0) {
        DSD_SNPRINTF(out, out_size, "%s", requested ? requested : "");
    }
    g_fault_offset = 0;
    cookie_io_functions_t io = {0};
    io.read = fault_read;
    return fopencookie(NULL, "r", io);
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

/* Header, then a row too long to be a row (counted as skipped), then the fault. */
static int
test_p25_bandplan_read_error_reports_no_counts(void) {
    const char* header = "iden,base_hz,spacing_hz\n";
    const size_t header_len = strlen(header);
    DSD_MEMCPY(g_fault_content, header, header_len);
    DSD_MEMSET(g_fault_content + header_len, 'x', sizeof(g_fault_content) - header_len);
    g_fault_content[sizeof(g_fault_content) - 1] = '\n';
    g_fault_size = sizeof(g_fault_content);

    dsd_csv_validation v = {9U, 9U, 9U};
    g_fault_hits = 0;
    g_fault_reads = 1;
    const int rc = dsd_csv_validate_p25_bandplan_file("faulted-bandplan.csv", &v);
    g_fault_reads = 0;

    /* Without the wrap in place the opener would fail outright and the counts would
       be zero for the wrong reason, which would pass this case without testing it. */
    if (g_fault_hits == 0 || g_fault_offset != g_fault_size) {
        DSD_FPRINTF(stderr, "the faulted stream never served the file: hits=%d offset=%zu size=%zu\n", g_fault_hits,
                    g_fault_offset, g_fault_size);
        return 1;
    }

    if (rc == 0) {
        DSD_FPRINTF(stderr, "bandplan validate reported success on a read error\n");
        return 1;
    }
    if (v.accepted != 0U || v.skipped != 0U || v.total != 0U) {
        DSD_FPRINTF(stderr, "failed bandplan validate kept counts: accepted=%u skipped=%u total=%u\n", v.accepted,
                    v.skipped, v.total);
        return 1;
    }
    return 0;
}
#endif

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
    int (*validators[])(const char*, dsd_csv_validation*) = {
        dsd_csv_validate_src_file,     dsd_csv_validate_group_file,   dsd_csv_validate_chan_file,
        dsd_csv_validate_key_file_dec, dsd_csv_validate_key_file_hex, dsd_csv_validate_p25_bandplan_file,
    };
    const char* bad_paths[] = {NULL, "", "dsd-neo-test-validate-missing-dir/missing.csv"};
    for (size_t i = 0; i < sizeof(validators) / sizeof(validators[0]); ++i) {
        for (size_t j = 0; j < sizeof(bad_paths) / sizeof(bad_paths[0]); ++j) {
            dsd_csv_validation counts = {9, 9, 9};
            assert(validators[i](bad_paths[j], &counts) == -1);
            assert(counts.accepted == 0 && counts.skipped == 0 && counts.total == 0);
        }
    }
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
test_chan_direct_key_columns_validate(void) {
    char valid_tmpl[] = "dsd-neo-test-validate-chan-direct-XXXXXX";
    char conflict_tmpl[] = "dsd-neo-test-validate-chan-direct-mix-XXXXXX";
    char invalid_tmpl[] = "dsd-neo-test-validate-chan-direct-bad-XXXXXX";
    char skipped_conflict_tmpl[] = "dsd-neo-test-validate-chan-direct-skip-mix-XXXXXX";
    char skipped_invalid_tmpl[] = "dsd-neo-test-validate-chan-direct-skip-bad-XXXXXX";
    if (write_temp_csv(valid_tmpl, "channel,frequency_hz,single_key_dec,single_key_hex\n"
                                   "1,851000000,1,0123456789\n"
                                   "2,notafreq,2,00112233445566778899AABBCCDDEEFF\n"
                                   "3,852000000,0,\n")
        != 0) {
        return 1;
    }
    dsd_csv_validation v = {0U, 0U, 0U};
    int failed = 0;
    if (dsd_csv_validate_chan_file(valid_tmpl, &v) != 0 || v.accepted != 2U || v.skipped != 1U || v.total != 3U) {
        DSD_FPRINTF(stderr, "chan direct-key validation mismatch: accepted=%u skipped=%u total=%u\n", v.accepted,
                    v.skipped, v.total);
        failed = 1;
    }
    if (write_temp_csv(conflict_tmpl, "channel,frequency_hz,keys_hex_csv,single_key_dec\n"
                                      "1,851000000,keys.csv,1\n")
            != 0
        || dsd_csv_validate_chan_file(conflict_tmpl, &v) == 0) {
        DSD_FPRINTF(stderr, "chan validation accepted mixed direct/file key sources\n");
        failed = 1;
    }
    if (write_temp_csv(invalid_tmpl, "channel,frequency_hz,single_key_hex\n"
                                     "1,851000000,invalid-secret-value\n")
            != 0
        || dsd_csv_validate_chan_file(invalid_tmpl, &v) == 0) {
        DSD_FPRINTF(stderr, "chan validation accepted an invalid direct key\n");
        failed = 1;
    }
    if (write_temp_csv(skipped_invalid_tmpl, "channel,frequency_hz,single_key_hex\n"
                                             "bad,851000000,invalid-secret-value\n")
            != 0
        || dsd_csv_validate_chan_file(skipped_invalid_tmpl, &v) == 0) {
        DSD_FPRINTF(stderr, "chan validation skipped an invalid direct key on a no-slot row\n");
        failed = 1;
    }
    if (write_temp_csv(skipped_conflict_tmpl, "channel,frequency_hz,keys_hex_csv,single_key_dec\n"
                                              "bad,851000000,keys.csv,1\n")
            != 0
        || dsd_csv_validate_chan_file(skipped_conflict_tmpl, &v) == 0) {
        DSD_FPRINTF(stderr, "chan validation skipped mixed key sources on a no-slot row\n");
        failed = 1;
    }
    (void)remove(valid_tmpl);
    (void)remove(conflict_tmpl);
    (void)remove(invalid_tmpl);
    (void)remove(skipped_conflict_tmpl);
    (void)remove(skipped_invalid_tmpl);
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

static void
test_source_validation_example(void) {
    const char* path = "source-validate.csv";
    FILE* fp = dsd_fopen_private(path, "w");
    assert(fp);
    assert(fputs("id,name,tags\n1234567,Engine 21,Fire\n1234568,Ladder 4,Fire\n2000000-2000999,Dispatch consoles,Ops\n",
                 fp)
           >= 0);
    assert(fclose(fp) == 0);
    dsd_csv_validation v;
    assert(dsd_csv_validate_src_file(path, &v) == 0 && v.accepted == 3 && v.skipped == 0 && v.total == 3);
    assert(remove(path) == 0);
}

int
main(void) {
    test_source_validation_example();
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
    if (test_chan_direct_key_columns_validate() != 0) {
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
#if defined(DSD_TEST_FAULT_READS)
    if (test_p25_bandplan_read_error_reports_no_counts() != 0) {
        return 1;
    }
#endif
    return 0;
}
