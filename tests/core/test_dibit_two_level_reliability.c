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
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
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
dsd_realtime_mono_ns(void) {
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
expect_reliability(const char* label, const dsd_opts* opts, const dsd_state* state, float symbol, int expected) {
    int got = dsd_two_level_symbol_reliability(opts, state, symbol);
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
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        DSD_FPRINTF(stderr, "FAIL: allocation\n");
        free(opts);
        free(state);
        return 1;
    }
    int rc = 0;

    /* After a ProVoice sync the warm start puts min and max on the two class means and center
     * between them; the reliability is the distance from center over half the spacing. */
    set_thresholds(state, -1.0f, 0.0f, 1.0f);
    rc |= expect_reliability("upper class mean", opts, state, 1.0f, 255);
    rc |= expect_reliability("lower class mean", opts, state, -1.0f, 255);
    rc |= expect_reliability("on the threshold", opts, state, 0.0f, 0);
    rc |= expect_reliability("halfway above", opts, state, 0.5f, 128);
    rc |= expect_reliability("halfway below", opts, state, -0.5f, 128);
    rc |= expect_reliability("quarter below", opts, state, -0.25f, 64);
    rc |= expect_reliability("past the upper mean saturates", opts, state, 2.5f, 255);
    rc |= expect_reliability("past the lower mean saturates", opts, state, -4.0f, 255);

    /* Off-centre and scaled thresholds: only the distance from center relative to the spacing counts. */
    set_thresholds(state, 10000.0f, 12000.0f, 14000.0f);
    rc |= expect_reliability("scaled upper mean", opts, state, 14000.0f, 255);
    rc |= expect_reliability("scaled threshold", opts, state, 12000.0f, 0);
    rc |= expect_reliability("scaled halfway above", opts, state, 13000.0f, 128);
    rc |= expect_reliability("scaled halfway below", opts, state, 11000.0f, 128);

    /* Thresholds with no usable spacing say nothing about confidence: give the hard path's. */
    set_thresholds(state, 0.0f, 0.0f, 0.0f);
    rc |= expect_reliability("collapsed thresholds", opts, state, 0.3f, 255);
    set_thresholds(state, 1.0f, 0.0f, -1.0f);
    rc |= expect_reliability("inverted thresholds", opts, state, 0.3f, 255);
    set_thresholds(state, -1.0f, 2.0f, 1.0f);
    rc |= expect_reliability("center outside the means", opts, state, 0.3f, 255);

    /* A symbol that is not a number carries no evidence at all. */
    set_thresholds(state, -1.0f, 0.0f, 1.0f);
    rc |= expect_reliability("NaN symbol", opts, state, NAN, 0);
    rc |= expect_reliability("infinite symbol", opts, state, INFINITY, 0);
    rc |= expect_reliability("negative infinite symbol", opts, state, -INFINITY, 0);
    if (dsd_two_level_symbol_reliability(opts, NULL, 1.0f) != 0) {
        DSD_FPRINTF(stderr, "FAIL: NULL state\n");
        rc = 1;
    }
    if (dsd_two_level_symbol_reliability(NULL, state, 1.0f) != 0) {
        DSD_FPRINTF(stderr, "FAIL: NULL opts\n");
        rc = 1;
    }

    /* A legacy symbol capture keeps only the decided bit, and replay turns it back into an ideal four-level
     * amplitude: +1 and +3 against thresholds the sync warm-started from those amplitudes. They are not
     * confidences, so every bit gets the hard decision's weight rather than 85 for one value and 255 for the
     * other, which biased the soft decode into miscorrections the hard one did not make. */
    set_thresholds(state, -3.0f, 0.0f, 3.0f);
    opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_LEGACY;
    rc |= expect_reliability("legacy capture, inner amplitude", opts, state, 1.0f, 255);
    rc |= expect_reliability("legacy capture, outer amplitude", opts, state, 3.0f, 255);

    /* The soft capture format replays the symbol that was measured, and so does a float symbol file. */
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_SOFT;
    rc |= expect_reliability("soft capture", opts, state, 1.0f, 85);
    opts->audio_in_type = AUDIO_IN_SYMBOL_FLT;
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN;
    rc |= expect_reliability("float symbol file", opts, state, 1.0f, 85);

    /* A replayed symbol whose stored amplitude was unusable (NaN, infinite or out of range) stands in as 0. Against
     * thresholds that do not centre on 0, 0 alone is a confident bit, so the reader marks the symbol and its bit is
     * an erasure: no confidence, whatever the thresholds. */
    set_thresholds(state, 10000.0f, 12000.0f, 14000.0f);
    opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_SOFT;
    state->symbol_replay_symbol_unusable = 0;
    rc |= expect_reliability("soft capture, a real 0 off-centre", opts, state, 0.0f, 255);
    state->symbol_replay_symbol_unusable = 1;
    rc |= expect_reliability("soft capture, an unusable symbol off-centre", opts, state, 0.0f, 0);
    /* A legacy capture's bits carry full confidence, but not the symbols after it ended mid-frame (issue #634). */
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_LEGACY;
    rc |= expect_reliability("legacy capture, an unusable symbol", opts, state, 0.0f, 0);
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_SOFT;
    set_thresholds(state, -1.0f, 1.0f, 3.0f);
    rc |= expect_reliability("soft capture, an unusable symbol between the thresholds", opts, state, 0.0f, 0);
    opts->audio_in_type = AUDIO_IN_SYMBOL_FLT;
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN;
    rc |= expect_reliability("float symbol file, an unusable symbol", opts, state, 0.0f, 0);
    state->symbol_replay_symbol_unusable = 0;
    rc |= expect_reliability("float symbol file, a real 0", opts, state, 0.0f, 128);

    free(opts);
    free(state);
    if (rc == 0) {
        printf("DIBIT_TWO_LEVEL_RELIABILITY: OK\n");
    }
    return rc;
}
