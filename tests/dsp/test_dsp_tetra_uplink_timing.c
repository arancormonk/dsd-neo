// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The TETRA uplink burst timing (src/dsp/tetra_uplink_timing.c): a control and a normal uplink burst, root-raised-cosine
 * shaped at 4 samples a symbol, placed at every quarter sample of two symbol periods and turned by a carrier offset. The
 * look-ahead must return each sample its length later, find the burst's training on its last symbol to within half a
 * sample (an eighth of a symbol), tell a control burst from the normal training sequence inside its extended one and a
 * normal burst from payload that resembles the rest of the extended one, give the carrier rotation, and move the
 * Gardner's strobe onto the burst's grid once, two symbols before its first symbol. Noise alone and a burst without its
 * training find no burst scoring like a training.
 */

#include <assert.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/dsp/tetra_uplink_timing.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_PI   3.14159265358979323846
#define SPS       4
#define LEAD      300 /* symbols before the burst */
#define TAIL      300 /* symbols after it */
#define MAX_SYMS  (LEAD + 231 + TAIL)
#define MAX_SAMPS (MAX_SYMS * SPS + 64)
#define RRC_HALF  (8 * SPS)

static float g_re[MAX_SAMPS];
static float g_im[MAX_SAMPS];

/* A deterministic generator for payload dibits and the noise. */
static uint32_t g_lcg = 1u;

static uint32_t
lcg_next(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg;
}

static double
lcg_uniform(void) {
    return ((double)(lcg_next() >> 8) + 0.5) / 16777216.0;
}

/* The burst's fraction of a sample, on top of its whole-sample offset. */
static double g_frac = 0.0;

static double
rrc_tap(int t) {
    const double alpha = 0.35;
    const double x = ((double)t - g_frac) / SPS;
    if (fabs(x) < 1e-9) {
        return 1.0 - alpha + 4.0 * alpha / TEST_PI;
    }
    if (fabs(fabs(4.0 * alpha * x) - 1.0) < 1e-9) {
        return alpha / sqrt(2.0)
               * ((1.0 + 2.0 / TEST_PI) * sin(TEST_PI / (4.0 * alpha))
                  + (1.0 - 2.0 / TEST_PI) * cos(TEST_PI / (4.0 * alpha)));
    }
    return (sin(TEST_PI * x * (1.0 - alpha)) + 4.0 * alpha * x * cos(TEST_PI * x * (1.0 + alpha)))
           / (TEST_PI * x * (1.0 - (4.0 * alpha * x) * (4.0 * alpha * x)));
}

static double
dibit_step(int dibit) {
    static const double steps[4] = {TEST_PI / 4.0, 3.0 * TEST_PI / 4.0, -TEST_PI / 4.0, -3.0 * TEST_PI / 4.0};
    return steps[dibit & 3];
}

/* A burst of @p kind ('c' control, 'n' normal; 'x' a normal burst whose training is payload) whose first symbol is at
   sample LEAD * SPS + @p offset, turned by @p rot_per_symbol radians a symbol, under noise of @p noise rms a component.
   Returns the sample count; *last_training is the sample of the training's last symbol. */
static int
make_burst(char kind, int offset, double rot_per_symbol, double noise, int* last_training, int* symbols) {
    int dibits[231];
    int n = 0;
    const char* training = kind == 'c' ? TETRA_CB_SYNC : TETRA_NDB_NTS_SYNC;
    const int half = kind == 'c' ? 42 : 108;
    dibits[n++] = 3;
    dibits[n++] = 0;
    for (int i = 0; i < half; i++) {
        dibits[n++] = (int)(lcg_next() >> 30);
    }
    for (const char* c = training; *c; c++) {
        dibits[n++] = kind == 'x' ? (int)(lcg_next() >> 30) : *c - '0';
    }
    *last_training = (LEAD + n - 1) * SPS + offset;
    for (int i = 0; i < half; i++) {
        dibits[n++] = (int)(lcg_next() >> 30);
    }
    dibits[n++] = 3;
    dibits[n++] = 0;
    *symbols = n;

    const int count = MAX_SYMS * SPS;
    for (int i = 0; i < count; i++) {
        g_re[i] = 0.0f;
        g_im[i] = 0.0f;
    }
    double phase = 0.3;
    for (int k = 0; k < n; k++) {
        phase += dibit_step(dibits[k]) + rot_per_symbol;
        const int at = (LEAD + k) * SPS + offset;
        for (int t = -RRC_HALF; t <= RRC_HALF; t++) {
            const double tap = rrc_tap(t) * 0.5;
            g_re[at + t] += (float)(tap * cos(phase));
            g_im[at + t] += (float)(tap * sin(phase));
        }
    }
    for (int i = 0; i < count; i++) {
        /* Box-Muller from the generator. */
        const double u1 = lcg_uniform();
        const double u2 = lcg_uniform();
        const double r = sqrt(-2.0 * log(u1)) * noise;
        g_re[i] += (float)(r * cos(2.0 * TEST_PI * u2));
        g_im[i] += (float)(r * sin(2.0 * TEST_PI * u2));
    }
    return count;
}

static dsd_tetra_ul_timing g_t;

/* Bursts found with a training score of at least @p min_score; the best of them in @p out. Noise can reach the
   search's threshold now and then; a training scores well above it. */
static int
found_bursts(dsd_tetra_ul_timing* t, float min_score, dsd_tetra_ul_burst** out) {
    int count = 0;
    for (int i = 0; i < DSD_TETRA_UL_BURSTS; i++) {
        dsd_tetra_ul_burst* b = &t->bursts[i];
        if (b->end != 0 && b->score >= min_score) {
            if (count == 0 || b->score > (*out)->score) {
                *out = b;
            }
            count++;
        }
    }
    return count;
}

static double
wrap_quarter(double phase) {
    const double quarter = TEST_PI / 2.0;
    return phase - quarter * floor(phase / quarter + 0.5);
}

static void
test_burst(char kind, int offset, double rot) {
    int last = 0;
    int symbols = 0;
    const int count = make_burst(kind, offset, rot, 0.02, &last, &symbols);
    dsd_tetra_ul_timing_reset(&g_t, SPS);
    const int delay = DSD_TETRA_UL_LOOKAHEAD_SYMBOLS * SPS;
    for (int i = 0; i < count; i++) {
        float out_r = 0.0f;
        float out_j = 0.0f;
        dsd_tetra_ul_timing_push(&g_t, g_re[i], g_im[i], &out_r, &out_j);
        /* The look-ahead returns each sample its length later, 0 while it fills. */
        if (i >= delay) {
            assert(fabsf(out_r - g_re[i - delay]) < 1e-9f && fabsf(out_j - g_im[i - delay]) < 1e-9f);
        } else {
            assert(fabsf(out_r) < 1e-9f && fabsf(out_j) < 1e-9f);
        }
    }

    dsd_tetra_ul_burst* b = NULL;
    assert(found_bursts(&g_t, 0.95f, &b) == 1);
    /* On the training's last symbol, in Gardner input samples. */
    const double anchor = (double)b->anchor + (double)b->frac - (double)delay;
    assert(fabs(anchor - (double)last - g_frac) < 0.5);
    const int training_last = kind == 'c' ? 58 : 120;
    assert(b->anchor - b->start == (uint64_t)(training_last * SPS));
    assert(b->end - b->start == (uint64_t)(symbols * SPS));
    assert(b->score > 0.95f);
    assert(b->pending == 1);
    assert(fabs(wrap_quarter((double)b->carrier - wrap_quarter(rot))) < 0.1);

    /* The strobe, a fraction of a symbol off the grid, steps a symbol at a time towards the burst. Nothing moves until
       it reaches two symbols before the first symbol; then once, onto the grid. */
    const double first = (double)b->start + (double)b->frac;
    const double misalign = 1.7;
    double strobe = first - 40.0 * SPS + misalign;
    int moves = 0;
    g_t.symbols_out = 0;
    while (strobe < first + 20.0 * SPS) {
        float move = 0.0f;
        const int hit = dsd_tetra_ul_timing_align(&g_t, strobe - (double)g_t.pushed, (float)SPS, &move);
        if (hit) {
            moves++;
            assert(strobe >= first - 2.0 * SPS - 1e-9);
            assert(strobe < first - 1.0 * SPS);
            assert(fabs((double)move + misalign) < 1e-3);
            strobe += move;
            /* The carrier waits for the next symbol the carrier loop takes. */
            float phase = 0.0f;
            while (g_t.carrier_seen <= g_t.symbols_out) {
                assert(dsd_tetra_ul_timing_carrier(&g_t, &phase) == 0);
            }
            assert(dsd_tetra_ul_timing_carrier(&g_t, &phase) == 1);
            assert(dsd_tetra_ul_timing_carrier(&g_t, &phase) == 0);
            assert(fabsf(phase - b->carrier) < 1e-6f);
        } else {
            assert(fabsf(move) < 1e-9f);
        }
        g_t.symbols_out++;
        strobe += SPS;
    }
    assert(moves == 1);
    assert(b->pending == 0);
}

static void
test_no_training(void) {
    int last = 0;
    int symbols = 0;
    /* Noise alone. */
    make_burst('n', 0, 0.0, 0.5, &last, &symbols);
    for (int i = 0; i < MAX_SYMS * SPS; i++) {
        g_re[i] = (float)(sqrt(-2.0 * log(lcg_uniform())) * cos(2.0 * TEST_PI * lcg_uniform()) * 0.5);
        g_im[i] = (float)(sqrt(-2.0 * log(lcg_uniform())) * sin(2.0 * TEST_PI * lcg_uniform()) * 0.5);
    }
    dsd_tetra_ul_timing_reset(&g_t, SPS);
    float out_r = 0.0f;
    float out_j = 0.0f;
    for (int i = 0; i < MAX_SYMS * SPS; i++) {
        dsd_tetra_ul_timing_push(&g_t, g_re[i], g_im[i], &out_r, &out_j);
    }
    dsd_tetra_ul_burst* b = NULL;
    assert(found_bursts(&g_t, 0.95f, &b) == 0);

    /* A normal burst whose training is payload. */
    const int count = make_burst('x', 1, 0.0, 0.02, &last, &symbols);
    dsd_tetra_ul_timing_reset(&g_t, SPS);
    for (int i = 0; i < count; i++) {
        dsd_tetra_ul_timing_push(&g_t, g_re[i], g_im[i], &out_r, &out_j);
    }
    assert(found_bursts(&g_t, 0.95f, &b) == 0);
}

static void
test_reset_and_bounds(void) {
    dsd_tetra_ul_timing_reset(&g_t, 1);
    assert(g_t.sps == 0);
    float out_r = 0.0f;
    float out_j = 0.0f;
    /* Not set up: the sample passes straight through and nothing is found. */
    dsd_tetra_ul_timing_push(&g_t, 0.25f, -0.5f, &out_r, &out_j);
    assert(fabsf(out_r - 0.25f) < 1e-9f && fabsf(out_j + 0.5f) < 1e-9f);
    float move = 1.0f;
    assert(dsd_tetra_ul_timing_align(&g_t, 0.0, 4.0f, &move) == 0 && fabsf(move) < 1e-9f);
    dsd_tetra_ul_timing_reset(&g_t, DSD_TETRA_UL_MAX_SPS + 1);
    assert(g_t.sps == 0);
    dsd_tetra_ul_timing_reset(&g_t, DSD_TETRA_UL_MAX_SPS);
    assert(g_t.sps == DSD_TETRA_UL_MAX_SPS && g_t.delay == DSD_TETRA_UL_LOOKAHEAD_SYMBOLS * DSD_TETRA_UL_MAX_SPS);
}

int
main(void) {
    /* Every sample of two symbol periods, both kinds, with and without a carrier offset (0.1 rad a symbol is 290 Hz;
       0.6 rad needs the wrap into a quarter turn). */
    const double rots[3] = {0.0, 0.1, -0.6};
    for (int quarter = 0; quarter < 4; quarter++) {
        g_frac = 0.25 * quarter;
        for (int offset = 0; offset < 2 * SPS; offset++) {
            for (int r = 0; r < 3; r++) {
                test_burst('c', offset, rots[r]);
                test_burst('n', offset, rots[r]);
            }
        }
    }
    g_frac = 0.0;
    test_no_training();
    test_reset_and_bounds();
    printf("DSP_TETRA_UPLINK_TIMING: OK\n");
    return 0;
}
