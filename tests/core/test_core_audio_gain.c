// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float_close(const char* label, float got, float want, float tol) {
    if (fabsf(got - want) > tol) {
        fprintf(stderr, "FAIL: %s (got %.6f want %.6f tol %.6f)\n", label, got, want, tol);
        return 1;
    }
    return 0;
}

static int
test_agsm_applies_gain_to_entire_block(void) {
    dsd_opts opts = {0};
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!state) {
        fprintf(stderr, "FAIL: alloc state\n");
        return 1;
    }

    short in[64];
    for (int i = 0; i < 64; i++) {
        in[i] = 1000;
    }

    agsm(&opts, state, in, 64);

    int rc = 0;
    /* nom/max = 4.8, clamped to 3.0 -> all samples should scale to 3000 */
    for (int i = 0; i < 64; i++) {
        char label[64];
        snprintf(label, sizeof label, "agsm scales sample %d", i);
        rc |= expect_int_eq(label, in[i], 3000);
    }
    rc |= expect_float_close("agsm stores applied gain", state->aout_gainA, 3.0f, 1e-6f);

    free(state);
    return rc;
}

static int
test_agsm_handles_silence_without_invalid_values(void) {
    dsd_opts opts = {0};
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!state) {
        fprintf(stderr, "FAIL: alloc state\n");
        return 1;
    }

    short in[32] = {0};
    agsm(&opts, state, in, 32);

    int rc = 0;
    for (int i = 0; i < 32; i++) {
        char label[64];
        snprintf(label, sizeof label, "agsm keeps silent sample %d", i);
        rc |= expect_int_eq(label, in[i], 0);
    }
    rc |= expect_true("agsm gain state is finite", isfinite(state->aout_gainA) ? 1 : 0);

    free(state);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_agsm_applies_gain_to_entire_block();
    rc |= test_agsm_handles_silence_without_invalid_values();
    return rc;
}
