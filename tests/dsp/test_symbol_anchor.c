// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The symbol grid's sync anchor (src/dsp/symbol_anchor.c, issue #729): where a known sync word sits against the grid
 * that decided it, and the move the grid takes from that.
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "symbol_anchor_internal.h"

#define TEST_PI   3.14159265358979323846
#define SPS       10
#define SYNC_LEN  24
#define N_SYMBOLS 40

static const char* k_sync = "131111333113313313113313"; /* DMR BS voice sync */

static double
rc_pulse(double t) {
    const double a = 0.2;
    if (fabs(t) < 1e-9) {
        return 1.0;
    }
    const double s = sin(TEST_PI * t) / (TEST_PI * t);
    const double den = 1.0 - (2.0 * a * t) * (2.0 * a * t);
    if (fabs(den) < 1e-9) {
        return (TEST_PI / 4.0) * s;
    }
    return s * cos(TEST_PI * a * t) / den;
}

static double
level_for(char c) {
    switch (c) {
        case '0': return 1.0;
        case '1': return 3.0;
        case '2': return -1.0;
        default: return -3.0;
    }
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static double
noise(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return ((double)(g_rng >> 11) / 9007199254740992.0) - 0.5;
}

/* N_SYMBOLS symbols of raised-cosine 4FSK whose true centres sit @p offset samples after the grid's centre index
   (k * SPS + SPS/2), the sync word starting at symbol 8, random symbols elsewhere. */
static void
make_signal(double offset, double polarity, double noise_amp, float* samples, int* centres, char* dibits) {
    for (int k = 0; k < N_SYMBOLS; k++) {
        dibits[k] = (k >= 8 && k < 8 + SYNC_LEN) ? k_sync[k - 8] : (char)('0' + (int)((noise() + 0.5) * 3.999));
    }
    for (int n = 0; n < N_SYMBOLS * SPS; n++) {
        double v = 0.0;
        for (int k = 0; k < N_SYMBOLS; k++) {
            const double centre = (double)(k * SPS) + (0.5 * (double)SPS) + offset;
            v += level_for(dibits[k]) * rc_pulse(((double)n - centre) / SPS);
        }
        samples[n] = (float)(polarity * v + noise_amp * noise());
    }
    for (int k = 0; k < N_SYMBOLS; k++) {
        centres[k] = k * SPS + SPS / 2;
    }
}

static void
test_measures_whole_and_fractional_offsets(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    const double offsets[] = {-3.0, -2.0, -1.0, -0.4, 0.0, 0.4, 1.0, 2.0, 3.0, 1.5};
    for (size_t c = 0; c < sizeof(offsets) / sizeof(offsets[0]); c++) {
        make_signal(offsets[c], 1.0, 0.0, samples, centres, dibits);
        double measured = 99.0;
        assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, SPS,
                                                &measured)
               == 1);
        assert(fabs(measured - offsets[c]) < 0.5);
    }
}

static void
test_inverted_signal_measures_the_same(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(2.0, -1.0, 0.0, samples, centres, dibits);
    double measured = 0.0;
    assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, SPS, &measured)
           == 1);
    assert(fabs(measured - 2.0) < 0.5);
}

static void
test_noise_does_not_move_a_centred_grid(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    for (int trial = 0; trial < 20; trial++) {
        make_signal(0.0, 1.0, 1.0, samples, centres, dibits); /* noise up to +/-0.5, against +/-3 symbols */
        double measured = 99.0;
        assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, SPS,
                                                &measured)
               == 1);
        assert(fabs(measured) <= 0.5);
    }
}

static void
test_declines(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(0.0, 1.0, 0.0, samples, centres, dibits);
    double measured = 0.0;
    /* one sample per symbol: no sub-symbol structure */
    assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, 1, &measured)
           == 0);
    /* centres too near the end for the shifts to read */
    assert(dsd_symbol_anchor_measure_offset(samples, centres[8 + SYNC_LEN - 1] + 2, &centres[8], &dibits[8], SYNC_LEN,
                                            SPS, &measured)
           == 0);
    /* a flat trace */
    static float flat[N_SYMBOLS * SPS];
    DSD_MEMSET(flat, 0, sizeof(flat));
    assert(dsd_symbol_anchor_measure_offset(flat, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, SPS, &measured)
           == 0);
    /* not a dibit */
    char bad[SYNC_LEN];
    DSD_MEMCPY(bad, &dibits[8], SYNC_LEN);
    bad[3] = 'x';
    assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], bad, SYNC_LEN, SPS, &measured) == 0);
}

/* Through the grid's side: push the samples symbol by symbol, name the sync word, and read the move. */
static int
run_ring(double offset) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(offset, 1.0, 0.0, samples, centres, dibits);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    dsd_symbol_arm_sync_anchor(state);                    /* allocates and starts the recording */
    dsd_symbol_note_sync_anchor(state, k_sync, SYNC_LEN); /* nothing recorded yet, so not measured */
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    assert(anchor != NULL);
    assert(dsd_symbol_anchor_take_slip(anchor) == 0);
    int slip = 0;
    for (int k = 0; k < N_SYMBOLS; k++) {
        for (int i = 0; i < SPS; i++) {
            dsd_symbol_anchor_push_sample(anchor, samples[k * SPS + i], i == SPS / 2);
        }
        dsd_symbol_anchor_end_symbol(anchor, SPS);
        if (k == 8 + SYNC_LEN - 1) {
            dsd_symbol_note_sync_anchor(state, k_sync, SYNC_LEN);
        }
        if (k == 8 + SYNC_LEN) {
            slip = dsd_symbol_anchor_take_slip(anchor);
        }
    }
    dsd_state_ext_free_all(state);
    free(state);
    return slip;
}

static void
test_ring_queues_the_move_toward_the_symbols(void) {
    assert(run_ring(0.0) == 0);
    assert(run_ring(0.3) == 0);
    assert(run_ring(1.0) == 1);   /* symbols later than the grid: take a sample more */
    assert(run_ring(-1.0) == -1); /* earlier: one fewer */
    assert(run_ring(2.0) == 2);   /* at most two at once */
    assert(run_ring(-3.0) == -2);
}

static void
test_reset_drops_the_pending_move(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(1.0, 1.0, 0.0, samples, centres, dibits);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    dsd_symbol_arm_sync_anchor(state);
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    for (int k = 0; k <= 8 + SYNC_LEN; k++) {
        for (int i = 0; i < SPS; i++) {
            dsd_symbol_anchor_push_sample(anchor, samples[k * SPS + i], i == SPS / 2);
        }
        dsd_symbol_anchor_end_symbol(anchor, SPS);
        if (k == 8 + SYNC_LEN - 1) {
            dsd_symbol_note_sync_anchor(state, k_sync, SYNC_LEN);
        }
    }
    dsd_symbol_anchor_reset(state); /* the stream the samples came from ended */
    assert(dsd_symbol_anchor_take_slip(anchor) == 0);
    dsd_state_ext_free_all(state);
    free(state);
}

int
main(void) {
    test_measures_whole_and_fractional_offsets();
    test_inverted_signal_measures_the_same();
    test_noise_does_not_move_a_centred_grid();
    test_declines();
    test_ring_queues_the_move_toward_the_symbols();
    test_reset_drops_the_pending_move();
    printf("symbol anchor: OK\n");
    return 0;
}
