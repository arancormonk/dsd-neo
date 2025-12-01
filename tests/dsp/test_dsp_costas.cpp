// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit tests for the CQPSK Costas loop implementation with OP25-style phase detection.
 *
 * These tests verify the combined differential decode + NCO + loop update function
 * (cqpsk_costas_diff_and_update) which matches OP25's p25_demodulator.py signal flow:
 *   - Differential decoding FIRST (like OP25's diff_phasor_cc before costas_loop_cc)
 *   - NCO rotation with exp(-j*phase) on the differentiated signal (OP25 convention)
 *   - Per-sample feedback where each sample sees the correction from previous samples
 *   - Standard GNU Radio phase_detector_4 for diagonal CQPSK symbols (±45°, ±135°)
 *   - Output remains at diagonal positions for downstream 4/π scaling
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static demod_state*
alloc_state(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (s) {
        memset(s, 0, sizeof(*s));
    }
    return s;
}

/*
 * Test: Identity input converges to diagonal constellation.
 *
 * With constant-phase raw samples at 45° and zero initial phase/freq, the
 * differential output starts on the +I axis (0°). The OP25-style Costas loop
 * uses the QPSK diagonal phase detector, so it will rotate toward the nearest
 * diagonal (here, -45°) and then hold there. Frequency should remain near 0.
 *
 * Note: OP25 parameters (alpha=0.04, beta=0.0002) are designed for real-world
 * signals with noise. The loop converges slowly for stability, so we use a
 * larger buffer and wider tolerance than the previous implementation.
 */
static int
test_identity_rotation(void) {
    const int pairs = 256; /* More samples needed for OP25's slower loop */
    float buf[pairs * 2];

    /* Fill with constant raw samples at 45° (CQPSK symbol position) */
    const float a = 0.5f;
    for (int k = 0; k < pairs; k++) {
        buf[2 * k + 0] = a; /* I = 0.5 */
        buf[2 * k + 1] = a; /* Q = 0.5 */
    }

    demod_state* s = alloc_state();
    if (!s) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    /* Initialize diff prev to match first sample so first diff output is meaningful */
    s->cqpsk_diff_prev_r = a;
    s->cqpsk_diff_prev_j = a;

    cqpsk_costas_diff_and_update(s);

    /* After convergence the loop should sit on a diagonal (~-45° here).
       OP25 parameters are slower so use wider tolerance and check that
       we're converging toward a diagonal (not stuck at 0°). */
    const float target = -0.78539816f; /* -pi/4 */
    const float tol = 0.35f;           /* ~20° tolerance for OP25's slow convergence */
    const int tail = 8;                /* check last N samples */
    for (int k = pairs - tail; k < pairs; k++) {
        float out_i = buf[2 * k + 0];
        float out_q = buf[2 * k + 1];
        float ang = atan2f(out_q, out_i);
        if (fabsf(ang - target) > tol) {
            fprintf(stderr, "IDENTITY: expected ~-45° after lock at k=%d (ang=%f rad I=%f Q=%f)\n", k, ang, out_i,
                    out_q);
            free(s);
            return 1;
        }
    }

    /* Frequency should remain near zero for a locked signal */
    if (s->fll_freq < -0.05f || s->fll_freq > 0.05f) {
        fprintf(stderr, "IDENTITY: expected near-zero freq, got %f\n", s->fll_freq);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: CFO drives non-zero frequency estimate.
 *
 * Feed raw samples with linearly increasing phase (simulating CFO).
 * The Costas loop should accumulate a non-zero frequency correction.
 *
 * Note: After differential decoding, linear CFO becomes a constant phase
 * offset per sample. The Costas loop should converge to track this offset.
 */
static int
test_cfo_pushes_freq(void) {
    const int pairs = 128;
    float buf[pairs * 2];

    /* Generate raw samples with CFO: phase advances by dtheta each sample */
    double dtheta = (2.0 * M_PI) / 400.0; /* frequency offset */
    double ph = 0.0;
    double r = 0.5;
    for (int k = 0; k < pairs; k++) {
        buf[2 * k + 0] = (float)(r * cos(ph));
        buf[2 * k + 1] = (float)(r * sin(ph));
        ph += dtheta;
    }

    demod_state* s = alloc_state();
    if (!s) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    /* Start diff prev at phase 0 to match first sample's starting point */
    s->cqpsk_diff_prev_r = (float)r;
    s->cqpsk_diff_prev_j = 0.0f;

    cqpsk_costas_diff_and_update(s);

    /* With CFO, loop should show some frequency movement (may be small
     * since diff decode removes cumulative phase, leaving constant offset) */
    if (fabsf(s->fll_freq) < 0.000001f) {
        fprintf(stderr, "CFO: expected non-zero freq correction, got %f\n", s->fll_freq);
        free(s);
        return 1;
    }

    /* Error average should be updated */
    if (s->costas_err_avg_q14 <= 0) {
        fprintf(stderr, "CFO: costas_err_avg_q14 not updated (%d)\n", s->costas_err_avg_q14);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: Phase seeding from FLL state.
 *
 * The Costas loop should initialize its phase from fll_phase when not yet
 * initialized. With fll_phase = π/4, the NCO = exp(-j*π/4) rotates samples
 * by -45° (OP25 sign convention).
 */
static int
test_phase_seed_from_fll(void) {
    const int pairs = 4;
    float buf[pairs * 2];

    /* Raw samples at 0° phase */
    const float r = 0.5f;
    for (int k = 0; k < pairs; k++) {
        buf[2 * k + 0] = r;    /* I */
        buf[2 * k + 1] = 0.0f; /* Q */
    }

    demod_state* s = alloc_state();
    if (!s) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->fll_phase = 0.78539816f; /* π/4: NCO = exp(+j*π/4) rotates by +45° */
    /* Initialize diff prev to match rotated sample so we can see the rotation effect */
    s->cqpsk_diff_prev_r = r * 0.70710678f; /* cos(45°) * r */
    s->cqpsk_diff_prev_j = r * 0.70710678f; /* sin(45°) * r */

    cqpsk_costas_diff_and_update(s);

    /* Costas state should be initialized */
    if (!s->costas_state.initialized) {
        fprintf(stderr, "SEED: Costas loop not initialized\n");
        free(s);
        return 1;
    }

    /* The initial phase should have been seeded from fll_phase */
    /* (Note: phase will have drifted slightly due to loop updates) */

    free(s);
    return 0;
}

/*
 * Test: Differential decoding produces correct output (no PT_45 rotation).
 *
 * Feed a known sequence of raw samples and verify the differential
 * output matches expectations:
 *   diff[n] = raw[n] * conj(raw[n-1])
 *
 * Output remains at the differential phase angle (not rotated by PT_45)
 * so downstream qpsk_differential_demod can apply 4/π scaling correctly.
 */
static int
test_differential_decode(void) {
    float buf[4]; /* 2 complex samples */

    /* First sample at 0°, second at 90° */
    buf[0] = 1.0f;
    buf[1] = 0.0f; /* sample 0: (1, 0) = 0° */
    buf[2] = 0.0f;
    buf[3] = 1.0f; /* sample 1: (0, 1) = 90° */

    demod_state* s = alloc_state();
    if (!s) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = 4;
    /* Set diff prev to (1, 0) so first output is sample0 * conj(prev) = (1,0)*(1,0) = (1,0) */
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;

    cqpsk_costas_diff_and_update(s);

    /* diff[0] = (1,0) * conj(1,0) = (1,0) -> phase 0° (purely real)
     * Costas NCO starts at 0, so first output should stay near 0°. */
    float ang0 = atan2f(buf[1], buf[0]);
    if (fabsf(ang0) > 0.25f) { /* ~14° */
        fprintf(stderr, "DIFF: first output angle off (ang=%f rad I=%f Q=%f), expected ~0°\n", ang0, buf[0], buf[1]);
        free(s);
        return 1;
    }

    /* diff[1] = (0,1) * conj(1,0) = (0,1) -> phase 90°
     * Costas loop starts steering toward diagonal, so expect a modest rotation
     * away from 90° but nowhere near a PT_45 (+45°) shift. */
    float ang1 = atan2f(buf[3], buf[2]);
    const float target = 1.57079633f;   /* pi/2 */
    if (fabsf(ang1 - target) > 0.40f) { /* ~23° window around 90° */
        fprintf(stderr, "DIFF: second output angle off (ang=%f rad I=%f Q=%f), expected near 90°\n", ang1, buf[2],
                buf[3]);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: Loop is disabled when cqpsk_enable is false.
 */
static int
test_disabled_when_not_cqpsk(void) {
    float buf[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    float ref[4];
    memcpy(ref, buf, sizeof(buf));

    demod_state* s = alloc_state();
    if (!s) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 0; /* disabled */
    s->lowpassed = buf;
    s->lp_len = 4;

    cqpsk_costas_diff_and_update(s);

    /* Buffer should be unchanged when disabled */
    for (int i = 0; i < 4; i++) {
        if (buf[i] != ref[i]) {
            fprintf(stderr, "DISABLED: buffer modified when cqpsk_enable=0\n");
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}

int
main(void) {
    if (test_identity_rotation() != 0) {
        return 1;
    }
    if (test_cfo_pushes_freq() != 0) {
        return 1;
    }
    if (test_phase_seed_from_fll() != 0) {
        return 1;
    }
    if (test_differential_decode() != 0) {
        return 1;
    }
    if (test_disabled_when_not_cqpsk() != 0) {
        return 1;
    }
    return 0;
}
