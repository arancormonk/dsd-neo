// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include "test_env.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <io.h>
#define dsd_fdopen _fdopen
#else
#define dsd_fdopen fdopen
#endif

static void
clear_dmr_t3_env(void) {
    (void)dsd_test_unsetenv("DSD_NEO_DMR_T3_STEP_HZ");
    (void)dsd_test_unsetenv("DSD_NEO_DMR_T3_CC_FREQ");
    (void)dsd_test_unsetenv("DSD_NEO_DMR_T3_CC_LCN");
    (void)dsd_test_unsetenv("DSD_NEO_DMR_T3_START_LCN");
}

static void
write_temp_csv(char path[DSD_TEST_PATH_MAX], const char* body) {
    int fd;
    FILE* fp;

    fd = dsd_test_mkstemp(path, DSD_TEST_PATH_MAX, "dsdneo_t3_lcn");
    assert(fd >= 0);

    fp = dsd_fdopen(fd, "wb"); /* the bytes as given: a row-length test counts its line ending */
    assert(fp != NULL);
    assert(fputs(body, fp) >= 0);
    assert(fclose(fp) == 0);
}

/* Run the calculator with stdout redirected to a file, so a test asserts what a caller would see on stdout. */
static int
run_calc(const char* csv, char* out, size_t out_size) {
    char csv_path[DSD_TEST_PATH_MAX];
    char out_path[DSD_TEST_PATH_MAX];

    write_temp_csv(csv_path, csv);
    int fd = dsd_test_mkstemp(out_path, sizeof out_path, "dsdneo_t3_out");
    assert(fd >= 0);
    (void)fflush(stdout);
    int saved = dsd_dup(DSD_STDOUT_FILENO);
    assert(saved >= 0);
    assert(dsd_dup2(fd, DSD_STDOUT_FILENO) >= 0);
    dsd_close(fd);

    const int rc = dsd_cli_calc_dmr_t3_lcn_from_csv(csv_path);

    (void)fflush(stdout);
    assert(dsd_dup2(saved, DSD_STDOUT_FILENO) >= 0);
    dsd_close(saved);
    remove(csv_path);

    FILE* f = fopen(out_path, "rb");
    assert(f != NULL);
    DSD_MEMSET(out, 0, out_size);
    const size_t got = fread(out, 1U, out_size - 1U, f);
    assert(!ferror(f));
    out[got] = '\0';
    (void)fclose(f);
    remove(out_path);
    return rc;
}

static void
set_anchor(const char* freq, const char* lcn, const char* step) {
    clear_dmr_t3_env();
    if (freq) {
        (void)dsd_test_setenv("DSD_NEO_DMR_T3_CC_FREQ", freq, 1);
    }
    if (lcn) {
        (void)dsd_test_setenv("DSD_NEO_DMR_T3_CC_LCN", lcn, 1);
    }
    if (step) {
        (void)dsd_test_setenv("DSD_NEO_DMR_T3_STEP_HZ", step, 1);
    }
    dsd_neo_config_init();
}

static void
expect_error(const char* csv) {
    char out[4096];
    const int rc = run_calc(csv, out, sizeof out);
    assert(rc != 0);
    assert(out[0] == '\0');
}

static void
expect_ok(const char* csv, const char* want) {
    char out[4096];
    assert(run_calc(csv, out, sizeof out) == 0);
    if (strcmp(out, want) != 0) {
        DSD_FPRINTF(stderr, "got:\n%s\nwant:\n%s\n", out, want);
    }
    assert(strcmp(out, want) == 0);
}

static void
test_one_row_applies_anchor_and_step(void) {
    set_anchor("851.025", "10", "12500");
    expect_ok("f\n851025000\n", "lcn,freq\n10,851025000\n");
    expect_ok("f\n851037500\n", "lcn,freq\n11,851037500\n");
    expect_ok("f\n851012500\n", "lcn,freq\n9,851012500\n");
    /* off the grid, or no step to place it with */
    expect_error("f\n851030000\n");
    set_anchor("851.025", "10", NULL);
    expect_ok("f\n851025000\n", "lcn,freq\n10,851025000\n");
    expect_error("f\n851037500\n");
    clear_dmr_t3_env();
}

static void
test_results_outside_range_or_off_grid_print_nothing(void) {
    set_anchor("851.025", "1", "12500");
    expect_error("f\n851012500\n851025000\n"); /* LCN 0 */
    set_anchor("851.025", "4094", "12500");
    expect_error("f\n851025000\n851037500\n"); /* LCN 4095 */
    /* inferred step, anchor half a step off the evenly spaced inputs */
    set_anchor("851.01875", "10", NULL);
    expect_error("f\n851012500\n851025000\n851037500\n");
    /* explicit step, one input off the grid */
    set_anchor("851.025", "10", "12500");
    expect_error("f\n851012500\n851025000\n851040000\n");
    clear_dmr_t3_env();
}

static size_t
fill_rows(char* buf, size_t size, int rows, int stepped) {
    size_t n = 0;
    for (int i = 0; i < rows; i++) {
        n += (size_t)DSD_SNPRINTF(buf + n, size - n, "%d\n", 800000000 + (stepped ? i * 12500 : 0));
    }
    return n;
}

static void
test_row_cap_is_an_error(void) {
    static char big[4100 * 16];

    set_anchor(NULL, NULL, "12500");
    /* duplicates collapse to one row, so only the raw row cap rejects this */
    (void)fill_rows(big, sizeof big, 4097, 0);
    expect_error(big);
    (void)fill_rows(big, sizeof big, 4097, 1);
    expect_error(big);
    /* 4096 rows fit the cap but their LCNs pass 4094 */
    (void)fill_rows(big, sizeof big, 4096, 1);
    expect_error(big);
    /* and 4096 identical rows are accepted: one unique frequency */
    (void)fill_rows(big, sizeof big, 4096, 0);
    expect_ok(big, "lcn,freq\n1,800000000\n");
    clear_dmr_t3_env();
}

static void
test_overlong_row_and_half_anchor_are_errors(void) {
    static char big[4096];
    size_t n = (size_t)DSD_SNPRINTF(big, sizeof big, "f\n851025000\n851,");
    for (int i = 0; i < 1100; i++) {
        big[n++] = '9';
    }
    big[n++] = '\n';
    big[n] = '\0';
    set_anchor(NULL, NULL, "12500");
    expect_error(big);
    /* the same long line as the last line, with no newline */
    big[n - 1] = '\0';
    expect_error(big);

    set_anchor("851.025", NULL, "12500");
    expect_error("f\n851025000\n851037500\n");
    set_anchor(NULL, "10", "12500");
    expect_error("f\n851025000\n851037500\n");
    clear_dmr_t3_env();
}

/* "f\n851025000\n" then one row of @p chars characters (a frequency padded with a note) and @p ending. */
static void
fill_long_row(char* buf, size_t size, size_t chars, const char* ending) {
    size_t n = (size_t)DSD_SNPRINTF(buf, size, "f\n851025000\n851037500,");
    while (n < 12U + chars) {
        buf[n++] = 'x';
    }
    (void)DSD_SNPRINTF(buf + n, size - n, "%s", ending);
}

/* A row may be 1023 bytes counting its line ending: 1022 characters and a newline, 1021 and CR LF, or 1023 with no
   line ending at the end of the file. One byte more is an error. */
static void
test_row_limit_counts_the_line_ending(void) {
    static char big[2048];
    static const char* const want = "lcn,freq\n1,851025000\n2,851037500\n";
    set_anchor(NULL, NULL, "12500");
    fill_long_row(big, sizeof big, 1022U, "\n");
    expect_ok(big, want);
    fill_long_row(big, sizeof big, 1021U, "\r\n");
    expect_ok(big, want);
    fill_long_row(big, sizeof big, 1023U, "");
    expect_ok(big, want);
    fill_long_row(big, sizeof big, 1023U, "\n");
    expect_error(big);
    fill_long_row(big, sizeof big, 1022U, "\r\n");
    expect_error(big);
    fill_long_row(big, sizeof big, 1024U, "");
    expect_error(big);
    clear_dmr_t3_env();
}

static void
test_missing_file_fails(void) {
    char path[DSD_TEST_PATH_MAX];

    clear_dmr_t3_env();
    dsd_neo_config_init();
    assert(dsd_test_make_temp_template(path, sizeof path, "dsdneo_missing_t3_lcn") == 0);
    (void)remove(path);
    assert(dsd_cli_calc_dmr_t3_lcn_from_csv(path) == 1);
}

static void
test_empty_csv_fails(void) {
    char path[DSD_TEST_PATH_MAX];

    clear_dmr_t3_env();
    dsd_neo_config_init();
    write_temp_csv(path, "header,value\nno frequency here\n");
    assert(dsd_cli_calc_dmr_t3_lcn_from_csv(path) == 2);
    remove(path);
}

static void
test_invalid_signed_numeric_prefixes_are_skipped(void) {
    char path[DSD_TEST_PATH_MAX];

    clear_dmr_t3_env();
    dsd_neo_config_init();
    write_temp_csv(path, "site,freq\nA,+not-a-number7\nB,-0\nC,0\n");
    assert(dsd_cli_calc_dmr_t3_lcn_from_csv(path) == 2);
    remove(path);
}

/* A row is a trunk frequency, so it must fit 1..2147483647 Hz (DSD_TRUNK_FREQ_MAX_HZ). Infinities, NaN, a value
 * strtod() overflows on, and 3000 (MHz, so 3 GHz) are each skipped, leaving no frequency; the ceiling itself, in Hz,
 * still makes a one-row map. Each row carries an LCN column: a row with no digit at all is skipped before its
 * frequency is parsed, so without one the infinity and NaN rows would never reach the range check. */
static void
test_rows_outside_the_trunk_range_are_skipped(void) {
    static const char* const kSkipped[] = {
        "site,freq,lcn\nA,+inf,1\n",       "site,freq,lcn\nA,-inf,1\n", "site,freq,lcn\nA,+nan,1\n",
        "site,freq,lcn\nA,1e400,1\n",      "site,freq,lcn\nA,3000,1\n", "site,freq,lcn\nA,5000000000,1\n",
        "site,freq,lcn\nA,2147483648,1\n",
    };
    char path[DSD_TEST_PATH_MAX];
    int failures = 0;

    for (size_t i = 0; i < sizeof kSkipped / sizeof kSkipped[0]; i++) {
        clear_dmr_t3_env();
        dsd_neo_config_init();
        write_temp_csv(path, kSkipped[i]);
        const int rc = dsd_cli_calc_dmr_t3_lcn_from_csv(path);
        remove(path);
        if (rc != 2) {
            DSD_FPRINTF(stderr, "FAIL: row %zu (%s) gave %d, want 2 (no frequency)\n", i, kSkipped[i], rc);
            failures++;
        }
    }
    assert(failures == 0);

    clear_dmr_t3_env();
    dsd_neo_config_init();
    write_temp_csv(path, "site,freq,lcn\nA,2147483647,1\n");
    assert(dsd_cli_calc_dmr_t3_lcn_from_csv(path) == 0);
    remove(path);
}

static void
test_single_frequency_uses_default_start_lcn(void) {
    char path[DSD_TEST_PATH_MAX];

    clear_dmr_t3_env();
    dsd_neo_config_init();
    write_temp_csv(path, "site,freq\nA,851.0125\n");
    assert(dsd_cli_calc_dmr_t3_lcn_from_csv(path) == 0);
    remove(path);
}

static void
test_unsorted_duplicates_infer_step(void) {
    char path[DSD_TEST_PATH_MAX];

    clear_dmr_t3_env();
    dsd_neo_config_init();
    write_temp_csv(path, "site,freq\nB,851.025\nA,851.0125\nC,851.025\nD,851.0375\n");
    assert(dsd_cli_calc_dmr_t3_lcn_from_csv(path) == 0);
    remove(path);
}

static void
test_too_small_spacing_without_configured_step_fails(void) {
    char path[DSD_TEST_PATH_MAX];

    clear_dmr_t3_env();
    dsd_neo_config_init();
    write_temp_csv(path, "site,freq\nA,851000000\nB,851000050\n");
    assert(dsd_cli_calc_dmr_t3_lcn_from_csv(path) == 3);
    remove(path);
}

static void
test_configured_step_and_anchor_are_accepted(void) {
    clear_dmr_t3_env();
    (void)dsd_test_setenv("DSD_NEO_DMR_T3_STEP_HZ", "12500", 1);
    (void)dsd_test_setenv("DSD_NEO_DMR_T3_CC_FREQ", "851.025", 1);
    (void)dsd_test_setenv("DSD_NEO_DMR_T3_CC_LCN", "10", 1);
    (void)dsd_test_setenv("DSD_NEO_DMR_T3_START_LCN", "3", 1);
    dsd_neo_config_init();

    expect_ok("site,freq\nA,851012500\nB,851025000\nC,851037500\n",
              "lcn,freq\n9,851012500\n10,851025000\n11,851037500\n");
    clear_dmr_t3_env();
}

int
main(void) {
    test_missing_file_fails();
    test_empty_csv_fails();
    test_invalid_signed_numeric_prefixes_are_skipped();
    test_rows_outside_the_trunk_range_are_skipped();
    test_single_frequency_uses_default_start_lcn();
    test_unsorted_duplicates_infer_step();
    test_too_small_spacing_without_configured_step_fails();
    test_configured_step_and_anchor_are_accepted();
    test_one_row_applies_anchor_and_step();
    test_results_outside_range_or_off_grid_print_nothing();
    test_row_cap_is_an_error();
    test_overlong_row_and_half_anchor_are_errors();
    test_row_limit_counts_the_line_ending();
    printf("RUNTIME_DMR_T3_LCN_CALC: OK\n");
    return 0;
}
