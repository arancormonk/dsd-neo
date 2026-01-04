// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/cli.h>

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#define DSD_NEO_MAIN
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#undef DSD_NEO_MAIN

#include <stdio.h>

void
noCarrier(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
test_redirect_stdout_to_null(void) {
#if defined(_WIN32)
    (void)freopen("NUL", "w", stdout);
#else
    (void)freopen("/dev/null", "w", stdout);
#endif
}

static int
test_help_returns_one_shot_and_does_not_exit(void) {
    dsd_opts opts;
    dsd_state state;
    initOpts(&opts);
    initState(&state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-h";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;

    test_redirect_stdout_to_null();
    int rc = dsd_parse_args(2, argv, &opts, &state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ONE_SHOT) {
        fprintf(stderr, "expected rc=%d, got %d\n", DSD_PARSE_ONE_SHOT, rc);
        return 1;
    }
    if (exit_rc != 0) {
        fprintf(stderr, "expected exit_rc=0, got %d\n", exit_rc);
        return 1;
    }
    return 0;
}

static int
test_invalid_option_returns_error_and_does_not_exit(void) {
    dsd_opts opts;
    dsd_state state;
    initOpts(&opts);
    initState(&state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-H";
    char arg2[] = "ZZ";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = 0;

    int rc = dsd_parse_args(3, argv, &opts, &state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR) {
        fprintf(stderr, "expected rc=%d, got %d\n", DSD_PARSE_ERROR, rc);
        return 1;
    }
    if (exit_rc != 1) {
        fprintf(stderr, "expected exit_rc=1, got %d\n", exit_rc);
        return 1;
    }
    return 0;
}

static int
test_unknown_option_returns_error_and_does_not_exit(void) {
    dsd_opts opts;
    dsd_state state;
    initOpts(&opts);
    initState(&state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-?";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = 0;

    test_redirect_stdout_to_null();
    int rc = dsd_parse_args(2, argv, &opts, &state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR) {
        fprintf(stderr, "expected rc=%d, got %d\n", DSD_PARSE_ERROR, rc);
        return 1;
    }
    if (exit_rc != 1) {
        fprintf(stderr, "expected exit_rc=1, got %d\n", exit_rc);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_help_returns_one_shot_and_does_not_exit();
    rc |= test_invalid_option_returns_error_and_does_not_exit();
    rc |= test_unknown_option_returns_error_and_does_not_exit();
    return rc;
}
