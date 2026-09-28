// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_two_level_symbol_reliability() turns a two-level symbol into the confidence the vocoder's soft
 * FEC gives its hard bit (issue #588): 255 at either class mean, 0 on the decision threshold, and
 * 255 -- the hard path's own confidence -- when the thresholds carry no usable spacing.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/config.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* dsd_dibit.c is compiled into this test on its own; the symbol reader, clock and runtime config it reaches are
 * not used by the function under test. */
float
// NOLINTNEXTLINE(misc-use-internal-linkage)
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) {
    (void)opts;
    (void)state;
    (void)have_sync;
    return 0.0f;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ns(void) {
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ns(uint64_t ns) {
    (void)ns;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_config_init(void) {}

const dsdneoRuntimeConfig*
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_neo_get_config(void) {
    static dsdneoRuntimeConfig cfg;
    return &cfg;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

static int
expect_reliability(const char* label, const dsd_state* state, float symbol, int expected) {
    int got = dsd_two_level_symbol_reliability(symbol, state);
    if (got != expected) {
        DSD_FPRINTF(stderr, "FAIL: %s: symbol %.4f -> %d, expected %d\n", label, (double)symbol, got, expected);
        return 1;
    }
    return 0;
}

static void
set_thresholds(dsd_state* state, float min, float center, float max) {
    state->min = min;
    state->center = center;
    state->max = max;
}

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (state == NULL) {
        DSD_FPRINTF(stderr, "FAIL: state allocation\n");
        return 1;
    }
    int rc = 0;

    /* After a ProVoice sync the warm start puts min and max on the two class means and center
     * between them; the reliability is the distance from center over half the spacing. */
    set_thresholds(state, -1.0f, 0.0f, 1.0f);
    rc |= expect_reliability("upper class mean", state, 1.0f, 255);
    rc |= expect_reliability("lower class mean", state, -1.0f, 255);
    rc |= expect_reliability("on the threshold", state, 0.0f, 0);
    rc |= expect_reliability("halfway above", state, 0.5f, 128);
    rc |= expect_reliability("halfway below", state, -0.5f, 128);
    rc |= expect_reliability("quarter below", state, -0.25f, 64);
    rc |= expect_reliability("past the upper mean saturates", state, 2.5f, 255);
    rc |= expect_reliability("past the lower mean saturates", state, -4.0f, 255);

    /* Off-centre and scaled thresholds: only the distance from center relative to the spacing counts. */
    set_thresholds(state, 10000.0f, 12000.0f, 14000.0f);
    rc |= expect_reliability("scaled upper mean", state, 14000.0f, 255);
    rc |= expect_reliability("scaled threshold", state, 12000.0f, 0);
    rc |= expect_reliability("scaled halfway above", state, 13000.0f, 128);
    rc |= expect_reliability("scaled halfway below", state, 11000.0f, 128);

    /* Thresholds with no usable spacing say nothing about confidence: give the hard path's. */
    set_thresholds(state, 0.0f, 0.0f, 0.0f);
    rc |= expect_reliability("collapsed thresholds", state, 0.3f, 255);
    set_thresholds(state, 1.0f, 0.0f, -1.0f);
    rc |= expect_reliability("inverted thresholds", state, 0.3f, 255);
    set_thresholds(state, -1.0f, 2.0f, 1.0f);
    rc |= expect_reliability("center outside the means", state, 0.3f, 255);

    /* A symbol that is not a number carries no evidence at all. */
    set_thresholds(state, -1.0f, 0.0f, 1.0f);
    rc |= expect_reliability("NaN symbol", state, NAN, 0);
    rc |= expect_reliability("infinite symbol", state, INFINITY, 0);
    rc |= expect_reliability("negative infinite symbol", state, -INFINITY, 0);
    if (dsd_two_level_symbol_reliability(1.0f, NULL) != 0) {
        DSD_FPRINTF(stderr, "FAIL: NULL state\n");
        rc = 1;
    }

    free(state);
    if (rc == 0) {
        printf("DIBIT_TWO_LEVEL_RELIABILITY: OK\n");
    }
    return rc;
}
