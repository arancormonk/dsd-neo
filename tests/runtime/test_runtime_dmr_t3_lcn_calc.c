// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdio.h>

#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <io.h>
#define dsd_fdopen _fdopen
#else
#define dsd_fdopen fdopen
#endif

static void
clear_dmr_t3_env(void) {
    (void)dsd_unsetenv("DSD_NEO_DMR_T3_STEP_HZ");
    (void)dsd_unsetenv("DSD_NEO_DMR_T3_CC_FREQ");
    (void)dsd_unsetenv("DSD_NEO_DMR_T3_CC_LCN");
    (void)dsd_unsetenv("DSD_NEO_DMR_T3_START_LCN");
}

static void
write_temp_csv(char path[DSD_TEST_PATH_MAX], const char* body) {
    int fd;
    FILE* fp;

    fd = dsd_test_mkstemp(path, DSD_TEST_PATH_MAX, "dsdneo_t3_lcn");
    assert(fd >= 0);

    fp = dsd_fdopen(fd, "w");
    assert(fp != NULL);
    assert(fputs(body, fp) >= 0);
    assert(fclose(fp) == 0);
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
    char path[DSD_TEST_PATH_MAX];

    clear_dmr_t3_env();
    (void)dsd_setenv("DSD_NEO_DMR_T3_STEP_HZ", "12500", 1);
    (void)dsd_setenv("DSD_NEO_DMR_T3_CC_FREQ", "851.025M", 1);
    (void)dsd_setenv("DSD_NEO_DMR_T3_CC_LCN", "10", 1);
    (void)dsd_setenv("DSD_NEO_DMR_T3_START_LCN", "3", 1);
    dsd_neo_config_init();

    write_temp_csv(path, "site,freq\nA,851012500\nB,851025000\nC,851037500\n");
    assert(dsd_cli_calc_dmr_t3_lcn_from_csv(path) == 0);
    remove(path);
    clear_dmr_t3_env();
}

int
main(void) {
    test_missing_file_fails();
    test_empty_csv_fails();
    test_invalid_signed_numeric_prefixes_are_skipped();
    test_single_frequency_uses_default_start_lcn();
    test_unsorted_duplicates_infer_step();
    test_too_small_spacing_without_configured_step_fails();
    test_configured_step_and_anchor_are_accepted();
    printf("RUNTIME_DMR_T3_LCN_CALC: OK\n");
    return 0;
}
