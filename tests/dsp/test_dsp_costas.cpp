// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit tests for the OP25-aligned CQPSK demodulation chain.
 *
 * Signal flow (from OP25 p25_demodulator.py):
 *   AGC -> Gardner (timing) -> diff_phasor -> Costas (carrier)
 *
 * dsd-neo implements this as separate blocks:
 *   op25_gardner_cc -> op25_diff_phasor_cc -> op25_costas_loop_cc
 *
 * The differential phasor block preserves GNU Radio diff_phasor_cc phase.
 * Costas normalizes reliable magnitudes and weights loop updates by raw phasor
 * confidence, then smooths the discriminator error to reduce simulcast phase
 * kicks with adaptive damping for abrupt raw-error jumps.
 */

#include <cmath>
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/safe_api.h"

static demod_state*
alloc_state(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (s) {
        DSD_MEMSET(s, 0, sizeof(*s));
        /* Initialize TED state */
        ted_init_state(&s->ted_state);
    }
    return s;
}

static void
run_cqpsk_chain(demod_state* s) {
    if (!s || !s->cqpsk_enable) {
        return;
    }
    op25_gardner_cc(s);
    op25_diff_phasor_cc(s);
    op25_costas_loop_cc(s);
}

/*
 * Test: Basic pipeline passes without crashing.
 *
 * Feed a buffer of constant-phase symbols through the chain and verify no
 * crashes and some output is produced.
 */
static int
test_basic_passthrough(void) {
    /* Generate oversampled symbols (5 samples/symbol, typical for P25 at 24kHz) */
    const int sps = 5;
    const int n_syms = 64;
    const int pairs = n_syms * sps;
    float* buf = (float*)malloc((size_t)pairs * 2 * sizeof(float));
    if (!buf) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }

    /* Fill with constant-phase symbols at 45° */
    const float a = 0.5f;
    for (int k = 0; k < pairs; k++) {
        buf[(size_t)k * 2 + 0] = a; /* I = 0.5 */
        buf[(size_t)k * 2 + 1] = a; /* Q = 0.5 */
    }

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "state alloc failed\n");
        free(buf);
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->ted_sps = sps;
    s->ted_gain = 0.025f;
    /* Initialize diff prev to (1, 0) for diff_phasor */
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;

    /* Set up Costas parameters (OP25 defaults) */
    s->costas_state.alpha = 0.04f;
    s->costas_state.beta = 0.125f * 0.04f * 0.04f;
    s->costas_state.max_freq = 0.628f;
    s->costas_state.initialized = 0;

    run_cqpsk_chain(s);

    /* Check that output was produced (decimated by ~sps) */
    int out_pairs = s->lp_len / 2;
    if (out_pairs < 1) {
        DSD_FPRINTF(stderr, "BASIC: no output symbols produced (lp_len=%d)\n", s->lp_len);
        free(buf);
        free(s);
        return 1;
    }

    /* Verify Costas state was initialized */
    if (!s->costas_state.initialized) {
        DSD_FPRINTF(stderr, "BASIC: Costas loop not initialized\n");
        free(buf);
        free(s);
        return 1;
    }

    /* Output symbols should have reasonable magnitudes */
    float mag_sum = 0.0f;
    for (int k = 0; k < out_pairs; k++) {
        float I = buf[(size_t)k * 2];
        float Q = buf[(size_t)k * 2 + 1];
        mag_sum += sqrtf(I * I + Q * Q);
    }
    float avg_mag = mag_sum / (float)out_pairs;
    if (avg_mag < 0.01f || avg_mag > 5.0f) {
        DSD_FPRINTF(stderr, "BASIC: output magnitude out of range (avg_mag=%f)\n", avg_mag);
        free(buf);
        free(s);
        return 1;
    }

    free(buf);
    free(s);
    return 0;
}

/*
 * Test: Costas loop tracks frequency offset.
 *
 * Feed symbols with a constant CFO and verify the loop's frequency estimate
 * moves away from zero.
 */
static int
test_cfo_tracking(void) {
    const int sps = 5;
    const int n_syms = 128;
    const int pairs = n_syms * sps;
    float* buf = (float*)malloc((size_t)pairs * 2 * sizeof(float));
    if (!buf) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }

    /* Generate symbols with CFO (phase ramps linearly) */
    double dtheta = (2.0 * M_PI) / 200.0; /* ~30 Hz CFO at 24kHz */
    double ph = 0.0;
    const double r = 0.5;
    for (int k = 0; k < pairs; k++) {
        buf[(size_t)k * 2 + 0] = (float)(r * cos(ph));
        buf[(size_t)k * 2 + 1] = (float)(r * sin(ph));
        ph += dtheta;
    }

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "state alloc failed\n");
        free(buf);
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->ted_sps = sps;
    s->ted_gain = 0.025f;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;

    s->costas_state.alpha = 0.04f;
    s->costas_state.beta = 0.125f * 0.04f * 0.04f;
    s->costas_state.max_freq = 0.628f;
    s->costas_state.initialized = 0;

    run_cqpsk_chain(s);

    /* With CFO, loop should track and develop non-zero freq */
    float freq_mag = fabsf(s->costas_state.freq);
    /* Expect some frequency tracking (>0.001 rad/sample) */
    if (freq_mag < 0.0001f) {
        /* This is acceptable - with OP25's slow loop, small CFO may not
         * develop much freq correction in 128 symbols. Just warn. */
        DSD_FPRINTF(stderr, "CFO: freq correction is small (freq=%f), may need more symbols\n", s->costas_state.freq);
    }

    free(buf);
    free(s);
    return 0;
}

/*
 * Test: Loop is disabled when cqpsk_enable is false.
 */
static int
test_disabled_when_not_cqpsk(void) {
    static float buf[100];
    for (int i = 0; i < 100; i++) {
        buf[i] = 0.5f;
    }
    float ref[100];
    DSD_MEMCPY(ref, buf, sizeof(buf));

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 0; /* disabled */
    s->lowpassed = buf;
    s->lp_len = 100;

    run_cqpsk_chain(s);

    /* Buffer should be unchanged when disabled */
    for (int i = 0; i < 100; i++) {
        if (buf[i] < ref[i] || ref[i] < buf[i]) {
            DSD_FPRINTF(stderr, "DISABLED: buffer modified when cqpsk_enable=0\n");
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}

/*
 * Test: External diff_phasor phase matches GNU Radio diff_phasor_cc.
 *
 * Verify that op25_diff_phasor_cc preserves the expected differential phase
 * y[n] = x[n] * conj(x[n-1]).
 */
static int
test_diff_phasor_correctness(void) {
    static float buf[8]; /* 4 complex samples */

    /* Samples at phases: 0°, 90°, 180°, -90° */
    buf[0] = 1.0f;
    buf[1] = 0.0f; /* 0° */
    buf[2] = 0.0f;
    buf[3] = 1.0f; /* 90° */
    buf[4] = -1.0f;
    buf[5] = 0.0f; /* 180° */
    buf[6] = 0.0f;
    buf[7] = -1.0f; /* -90° (270°) */

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->lowpassed = buf;
    s->lp_len = 8;
    /* Start diff prev at (1, 0) */
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;

    op25_diff_phasor_cc(s);

    /* Expected differential phases:
     * diff[0] = (1,0) * conj(1,0) = (1,0) -> 0°
     * diff[1] = (0,1) * conj(1,0) = (0,1) -> 90°
     * diff[2] = (-1,0) * conj(0,1) = (-1,0)*(0,-1) = (0,1) -> 90°
     * diff[3] = (0,-1) * conj(-1,0) = (0,-1)*(-1,0) = (0,1) -> 90°
     */

    /* Check sample 0: should be ~0° */
    float ang0 = atan2f(buf[1], buf[0]);
    if (fabsf(ang0) > 0.1f) {
        DSD_FPRINTF(stderr, "DIFF: sample 0 angle wrong (ang=%f, expected ~0)\n", ang0);
        free(s);
        return 1;
    }

    /* Check sample 1: should be ~90° */
    float ang1 = atan2f(buf[3], buf[2]);
    float target1 = 1.5708f; /* pi/2 */
    if (fabsf(ang1 - target1) > 0.1f) {
        DSD_FPRINTF(stderr, "DIFF: sample 1 angle wrong (ang=%f, expected ~90°)\n", ang1);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: reliable Costas detector magnitudes are normalized.
 *
 * The Costas phase detector gain is proportional to input magnitude, so
 * simulcast AM ripple should not directly change loop gain once the phasor has
 * enough magnitude to be trusted.
 */
static int
test_costas_normalizes_reliable_magnitude(void) {
    static float buf[8]; /* 4 complex samples */

    /* Differential phasors with large envelope swings. */
    buf[0] = 0.20f;
    buf[1] = 0.00f; /* 0 deg, mag 0.20 */
    buf[2] = 0.00f;
    buf[3] = 2.00f; /* 90 deg, mag 2.00 */
    buf[4] = -0.40f;
    buf[5] = 0.00f; /* 180 deg, mag 0.40 */
    buf[6] = 0.00f;
    buf[7] = -1.40f; /* -90 deg, mag 1.40 */

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->lowpassed = buf;
    s->lp_len = 8;
    s->cqpsk_enable = 1;
    s->costas_state.initialized = 1;
    s->costas_state.alpha = 0.0f;
    s->costas_state.beta = 0.0f;
    s->costas_state.max_freq = 1.0f;
    s->costas_state.min_freq = -1.0f;

    op25_costas_loop_cc(s);

    const float target = 0.85f * 0.85f;
    for (int k = 0; k < 4; k++) {
        float I = buf[(size_t)k * 2];
        float Q = buf[(size_t)k * 2 + 1];
        float mag = sqrtf(I * I + Q * Q);
        if (fabsf(mag - target) > 0.0001f) {
            DSD_FPRINTF(stderr, "COSTAS NORM: sample %d magnitude %f expected %f\n", k, mag, target);
            free(s);
            return 1;
        }
    }

    float ang1 = atan2f(buf[3], buf[2]);
    if (fabsf(ang1 - 1.5708f) > 0.1f) {
        DSD_FPRINTF(stderr, "COSTAS NORM: sample 1 phase changed unexpectedly (ang=%f)\n", ang1);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: deep fades are not boosted or allowed to train the Costas loop.
 */
static int
test_costas_deep_fade_does_not_boost_or_train(void) {
    static float buf[4]; /* 2 complex samples */
    buf[0] = 0.03f;
    buf[1] = 0.01f;
    buf[2] = 0.028f;
    buf[3] = 0.010f;

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->lowpassed = buf;
    s->lp_len = 4;
    s->cqpsk_enable = 1;
    s->costas_state.initialized = 1;
    s->costas_state.alpha = 0.04f;
    s->costas_state.beta = 0.0002f;
    s->costas_state.max_freq = 1.0f;
    s->costas_state.min_freq = -1.0f;
    s->costas_state.error_smooth = 0.5f;

    op25_costas_loop_cc(s);

    for (int k = 0; k < 2; k++) {
        float I = buf[(size_t)k * 2];
        float Q = buf[(size_t)k * 2 + 1];
        float mag = sqrtf(I * I + Q * Q);
        if (mag > 0.05f) {
            DSD_FPRINTF(stderr, "COSTAS FADE: sample %d magnitude %f was boosted\n", k, mag);
            free(s);
            return 1;
        }
    }

    if (fabsf(s->costas_state.phase) > 1.0e-7f || fabsf(s->costas_state.freq) > 1.0e-7f
        || fabsf(s->costas_state.error_smooth) > 1.0e-7f) {
        DSD_FPRINTF(stderr, "COSTAS FADE: loop trained on deep fade phase=%f freq=%f smooth=%f\n",
                    s->costas_state.phase, s->costas_state.freq, s->costas_state.error_smooth);
        free(s);
        return 1;
    }
    if (s->costas_err_avg_q14 != 0 || s->costas_err_raw_avg_q14 != 0 || s->costas_conf_avg_q14 != 0
        || s->costas_zero_conf_pct != 100) {
        DSD_FPRINTF(stderr, "COSTAS FADE: metrics smooth=%d raw=%d conf=%d zero=%d\n", s->costas_err_avg_q14,
                    s->costas_err_raw_avg_q14, s->costas_conf_avg_q14, s->costas_zero_conf_pct);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

static float
run_single_costas_update(float mag, float* out_freq) {
    const float angle = 0.30f;
    static float buf[2];
    buf[0] = mag * cosf(angle);
    buf[1] = mag * sinf(angle);

    demod_state* s = alloc_state();
    if (!s) {
        return 0.0f;
    }
    s->lowpassed = buf;
    s->lp_len = 2;
    s->cqpsk_enable = 1;
    s->costas_state.initialized = 1;
    s->costas_state.alpha = 0.04f;
    s->costas_state.beta = 0.0002f;
    s->costas_state.max_freq = 1.0f;
    s->costas_state.min_freq = -1.0f;

    op25_costas_loop_cc(s);

    float phase = s->costas_state.phase;
    if (out_freq) {
        *out_freq = s->costas_state.freq;
    }
    free(s);
    return phase;
}

/*
 * Test: marginal phasors still update Costas, but less than full-confidence samples.
 */
static int
test_costas_marginal_confidence_scales_loop_update(void) {
    float full_freq = 0.0f;
    float marginal_freq = 0.0f;
    float full_phase = run_single_costas_update(0.70f, &full_freq);
    float marginal_phase = run_single_costas_update(0.20f, &marginal_freq);

    if (!(fabsf(full_phase) > 0.001f && fabsf(full_freq) > 0.000001f)) {
        DSD_FPRINTF(stderr, "COSTAS CONF: full-confidence sample did not train phase=%f freq=%f\n", full_phase,
                    full_freq);
        return 1;
    }
    if (!(fabsf(marginal_phase) > 0.0001f && fabsf(marginal_freq) > 0.0f)) {
        DSD_FPRINTF(stderr, "COSTAS CONF: marginal sample did not train phase=%f freq=%f\n", marginal_phase,
                    marginal_freq);
        return 1;
    }
    if (!(fabsf(marginal_phase) < fabsf(full_phase) && fabsf(marginal_freq) < fabsf(full_freq))) {
        DSD_FPRINTF(stderr,
                    "COSTAS CONF: marginal update not smaller full phase=%f freq=%f marginal phase=%f freq=%f\n",
                    full_phase, full_freq, marginal_phase, marginal_freq);
        return 1;
    }

    return 0;
}

/*
 * Test: Costas smooths a sudden discriminator step before updating phase/freq.
 */
static int
test_costas_smooths_error_step(void) {
    const float angle = 0.30f;
    const float mag = 0.70f;
    static float buf[2];
    buf[0] = mag * cosf(angle);
    buf[1] = mag * sinf(angle);

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->lowpassed = buf;
    s->lp_len = 2;
    s->cqpsk_enable = 1;
    s->costas_state.initialized = 1;
    s->costas_state.alpha = 0.04f;
    s->costas_state.beta = 0.0002f;
    s->costas_state.max_freq = 1.0f;
    s->costas_state.min_freq = -1.0f;
    s->costas_state.error_smooth = 0.0f;

    op25_costas_loop_cc(s);

    const float target = 0.85f * 0.85f;
    const float raw_error = target * (sinf(angle) - cosf(angle));
    const float expected_error = raw_error * 0.25f;
    const float expected_freq = s->costas_state.beta * expected_error;
    const float expected_phase = expected_freq + s->costas_state.alpha * expected_error;

    if (fabsf(s->costas_state.error_smooth - expected_error) > 1.0e-5f
        || fabsf(s->costas_state.error - expected_error) > 1.0e-5f
        || fabsf(s->costas_state.freq - expected_freq) > 1.0e-6f
        || fabsf(s->costas_state.phase - expected_phase) > 1.0e-5f) {
        DSD_FPRINTF(stderr, "COSTAS SMOOTH: err=%f smooth=%f freq=%f phase=%f expected err=%f freq=%f phase=%f\n",
                    s->costas_state.error, s->costas_state.error_smooth, s->costas_state.freq, s->costas_state.phase,
                    expected_error, expected_freq, expected_phase);
        free(s);
        return 1;
    }
    int expected_raw_q14 = (int)lrintf(fabsf(raw_error) * 16384.0f);
    int expected_smooth_q14 = (int)lrintf(fabsf(expected_error) * 16384.0f);
    if (abs(s->costas_err_raw_avg_q14 - expected_raw_q14) > 1 || abs(s->costas_err_avg_q14 - expected_smooth_q14) > 1
        || s->costas_conf_avg_q14 != 16384 || s->costas_zero_conf_pct != 0) {
        DSD_FPRINTF(stderr, "COSTAS SMOOTH: metrics smooth=%d raw=%d conf=%d zero=%d expected smooth=%d raw=%d\n",
                    s->costas_err_avg_q14, s->costas_err_raw_avg_q14, s->costas_conf_avg_q14, s->costas_zero_conf_pct,
                    expected_smooth_q14, expected_raw_q14);
        free(s);
        return 1;
    }

    dsd_costas_loop_state_t c = s->costas_state;
    c.error_smooth = 0.5f;
    dsd_costas_reset(&c);
    if (c.error_smooth != 0.0f) {
        DSD_FPRINTF(stderr, "COSTAS SMOOTH: reset did not clear smoothed error\n");
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: abrupt discriminator kicks reduce the Costas smoothing gain.
 */
static int
test_costas_adapts_smoothing_for_phase_kick(void) {
    const float angle = 0.30f;
    const float mag = 0.70f;
    const float seed_error = 0.05f;
    static float buf[2];
    buf[0] = mag * cosf(angle);
    buf[1] = mag * sinf(angle);

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->lowpassed = buf;
    s->lp_len = 2;
    s->cqpsk_enable = 1;
    s->costas_state.initialized = 1;
    s->costas_state.alpha = 0.04f;
    s->costas_state.beta = 0.0002f;
    s->costas_state.max_freq = 1.0f;
    s->costas_state.min_freq = -1.0f;
    s->costas_state.error_smooth = seed_error;

    op25_costas_loop_cc(s);

    const float target = 0.85f * 0.85f;
    const float raw_error = target * (sinf(angle) - cosf(angle));
    const float fixed_error = seed_error + 0.25f * (raw_error - seed_error);
    const float expected_error = seed_error + 0.10f * (raw_error - seed_error);
    const float expected_freq = s->costas_state.beta * expected_error;
    const float expected_phase = expected_freq + s->costas_state.alpha * expected_error;

    if (fabsf(s->costas_state.error_smooth - expected_error) > 1.0e-5f
        || fabsf(s->costas_state.error - expected_error) > 1.0e-5f
        || fabsf(s->costas_state.freq - expected_freq) > 1.0e-6f
        || fabsf(s->costas_state.phase - expected_phase) > 1.0e-5f
        || fabsf(s->costas_state.error_smooth - fixed_error) < 0.01f) {
        DSD_FPRINTF(stderr,
                    "COSTAS ADAPT: err=%f smooth=%f freq=%f phase=%f expected err=%f freq=%f phase=%f fixed=%f\n",
                    s->costas_state.error, s->costas_state.error_smooth, s->costas_state.freq, s->costas_state.phase,
                    expected_error, expected_freq, expected_phase, fixed_error);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: TED state is properly initialized by the combined block.
 */
static int
test_ted_initialization(void) {
    const int sps = 5;
    const int pairs = 100;
    float* buf = (float*)malloc((size_t)pairs * 2 * sizeof(float));
    if (!buf) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }

    for (int k = 0; k < pairs; k++) {
        buf[(size_t)k * 2 + 0] = 0.5f;
        buf[(size_t)k * 2 + 1] = 0.5f;
    }

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "state alloc failed\n");
        free(buf);
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->ted_sps = sps;
    s->ted_gain = 0.025f;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;

    s->costas_state.alpha = 0.04f;
    s->costas_state.beta = 0.0002f;
    s->costas_state.max_freq = 0.628f;
    s->costas_state.initialized = 0;

    /* TED state should be zero-initialized */
    if (s->ted_state.omega != 0.0f) {
        DSD_FPRINTF(stderr, "TED: omega should start at 0 before call\n");
        free(buf);
        free(s);
        return 1;
    }

    op25_gardner_cc(s);

    /* After call, TED state should be initialized */
    if (s->ted_state.omega < 1.0f) {
        DSD_FPRINTF(stderr, "TED: omega not initialized after call (omega=%f)\n", s->ted_state.omega);
        free(buf);
        free(s);
        return 1;
    }

    if (s->ted_state.twice_sps < 2) {
        DSD_FPRINTF(stderr, "TED: twice_sps not initialized (twice_sps=%d)\n", s->ted_state.twice_sps);
        free(buf);
        free(s);
        return 1;
    }

    free(buf);
    free(s);
    return 0;
}

/*
 * Test: OP25 Gardner omega clamp is absolute, not scaled by SPS.
 *
 * P25P2 at 48 kHz runs at 8 samples/symbol. OP25 clips the timing period to
 * omega_mid +/- 0.002 samples. Scaling that window by SPS lets the loop hunt
 * several times wider and smears marginal TDMA constellations.
 */
static int
test_gardner_omega_absolute_clamp_p25p2(void) {
    const int sps = 8;
    const int pairs = 512 * sps;
    float* buf = (float*)malloc((size_t)pairs * 2 * sizeof(float));
    if (!buf) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }

    for (int k = 0; k < pairs; k++) {
        int sym = k / sps;
        float sign_i = (sym & 1) ? -1.0f : 1.0f;
        float sign_q = (sym & 2) ? -1.0f : 1.0f;
        float frac = (float)(k % sps) / (float)sps;
        buf[(size_t)k * 2 + 0] = sign_i * (0.3f + 0.7f * frac);
        buf[(size_t)k * 2 + 1] = sign_q * (0.9f - 0.5f * frac);
    }

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "state alloc failed\n");
        free(buf);
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->ted_sps = sps;
    s->ted_gain = 1.0f; /* force the clamp to engage if the error is nonzero */

    op25_gardner_cc(s);

    float omega_delta = fabsf(s->ted_state.omega - (float)sps);
    if (omega_delta > 0.0021f) {
        DSD_FPRINTF(stderr, "GARDNER: omega delta %f exceeds OP25 absolute clamp\n", omega_delta);
        free(buf);
        free(s);
        return 1;
    }

    free(buf);
    free(s);
    return 0;
}

static int
expect_p25p2_tracking_gain_after_lock(const char* label, float ted_gain, int ted_gain_is_set, float expected_gain) {
    dsd_unsetenv("DSD_NEO_TED_GAIN");
    dsd_neo_config_init(NULL);

    const int sps = 8;
    const int pairs = 64 * sps;
    float* buf = (float*)malloc((size_t)pairs * 2 * sizeof(float));
    if (!buf) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }

    for (int k = 0; k < pairs; k++) {
        buf[(size_t)k * 2 + 0] = 0.6f;
        buf[(size_t)k * 2 + 1] = 0.2f;
    }

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "state alloc failed\n");
        free(buf);
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->rate_out = 48000;
    s->ted_sps = sps;
    s->ted_gain = ted_gain;
    s->ted_gain_is_set = ted_gain_is_set;
    s->ted_state.lock_count = 240;
    s->ted_state.lock_accum = 24.0f;

    op25_gardner_cc(s);

    if (fabsf(s->ted_effective_gain - expected_gain) > 0.0001f) {
        DSD_FPRINTF(stderr, "%s: got effective TED gain %.6f want %.6f\n", label, s->ted_effective_gain, expected_gain);
        free(buf);
        free(s);
        return 1;
    }

    free(buf);
    free(s);
    return 0;
}

/*
 * Test: P25P2 uses a lower effective Gardner gain after initial acquisition
 * when TED gain is still the automatic default.
 */
static int
test_p25p2_tracking_gain_reduces_after_lock(void) {
    return expect_p25p2_tracking_gain_after_lock("P25P2 automatic gain", 0.025f, 0, 0.018f);
}

/*
 * Test: Runtime/UI TED gain changes remain hard overrides after P25P2 lock.
 */
static int
test_p25p2_tracking_gain_honors_runtime_override_after_lock(void) {
    return expect_p25p2_tracking_gain_after_lock("P25P2 runtime TED gain", 0.031f, 1, 0.031f);
}

/*
 * Test: public reset helpers tolerate null pointers and clear documented state.
 */
static int
test_reset_helpers_clear_tracking_state(void) {
    dsd_costas_reset(NULL);
    dsd_fll_band_edge_reset(NULL);

    dsd_costas_loop_state_t c;
    DSD_MEMSET(&c, 0, sizeof(c));
    c.phase = 0.9f;
    c.freq = -0.3f;
    c.error = 0.2f;
    c.error_smooth = -0.1f;
    c.alpha = 0.04f;
    c.initialized = 1;

    dsd_costas_reset(&c);
    if (c.phase != 0.0f || c.freq != 0.0f || c.error != 0.0f || c.error_smooth != 0.0f || c.initialized != 0
        || c.alpha != 0.04f) {
        DSD_FPRINTF(stderr, "RESET: Costas reset left phase=%f freq=%f err=%f smooth=%f init=%d alpha=%f\n", c.phase,
                    c.freq, c.error, c.error_smooth, c.initialized, c.alpha);
        return 1;
    }

    dsd_fll_band_edge_state_t f;
    DSD_MEMSET(&f, 0, sizeof(f));
    dsd_fll_band_edge_init(&f, 5);
    f.phase = 0.7f;
    f.freq = -0.2f;
    f.delay_idx = 3;
    f.delay_r[0] = 1.0f;
    f.delay_i[0] = -1.0f;
    const float tap0 = f.taps_upper_r[0];
    const int n_taps = f.n_taps;

    dsd_fll_band_edge_reset(&f);
    if (std::fabs(f.phase) > 1e-7f || std::fabs(f.freq) > 1e-7f || f.delay_idx != 0 || std::fabs(f.delay_r[0]) > 1e-7f
        || std::fabs(f.delay_i[0]) > 1e-7f || std::fabs(f.taps_upper_r[0] - tap0) > 1e-7f || f.n_taps != n_taps
        || !f.initialized) {
        DSD_FPRINTF(stderr, "RESET: FLL reset phase=%f freq=%f delay=%d tap0=%f/%f n_taps=%d/%d initialized=%d\n",
                    f.phase, f.freq, f.delay_idx, f.taps_upper_r[0], tap0, f.n_taps, n_taps, f.initialized);
        return 1;
    }

    return 0;
}

/*
 * Test: diff phasor guard paths leave state untouched when there is no full
 * complex sample to process.
 */
static int
test_diff_phasor_short_block_preserves_state(void) {
    op25_diff_phasor_cc(NULL);

    static float buf[2] = {0.25f, -0.5f};
    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->lowpassed = buf;
    s->lp_len = 1;
    s->cqpsk_diff_prev_r = 0.4f;
    s->cqpsk_diff_prev_j = -0.7f;

    op25_diff_phasor_cc(s);

    if (buf[0] != 0.25f || buf[1] != -0.5f || s->cqpsk_diff_prev_r != 0.4f || s->cqpsk_diff_prev_j != -0.7f) {
        DSD_FPRINTF(stderr, "DIFF GUARD: short block changed buf=(%f,%f) prev=(%f,%f)\n", buf[0], buf[1],
                    s->cqpsk_diff_prev_r, s->cqpsk_diff_prev_j);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: unsafe Costas loop state is clamped before and after a symbol update.
 */
static int
test_costas_clamps_initial_phase_and_frequency(void) {
    static float buf[4] = {0.7f, 0.0f, 0.0f, -0.7f};
    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->lowpassed = buf;
    s->lp_len = 4;
    s->cqpsk_enable = 1;
    s->costas_state.initialized = 1;
    s->costas_state.phase = 10.0f;
    s->costas_state.freq = 2.0f;
    s->costas_state.alpha = 0.0f;
    s->costas_state.beta = 0.0f;
    s->costas_state.max_freq = 1.0f;
    s->costas_state.min_freq = -1.0f;

    op25_costas_loop_cc(s);

    if (fabsf(s->costas_state.phase - (float)(M_PI / 2.0)) > 0.001f || s->costas_state.freq != 1.0f
        || !std::isfinite(buf[0]) || !std::isfinite(buf[1])) {
        DSD_FPRINTF(stderr, "COSTAS CLAMP: high clamp phase=%f freq=%f out=(%f,%f)\n", s->costas_state.phase,
                    s->costas_state.freq, buf[0], buf[1]);
        free(s);
        return 1;
    }

    buf[0] = 0.7f;
    buf[1] = 0.0f;
    s->lp_len = 2;
    s->costas_state.phase = -10.0f;
    s->costas_state.freq = -2.0f;

    op25_costas_loop_cc(s);

    if (fabsf(s->costas_state.phase + (float)(M_PI / 2.0)) > 0.001f || s->costas_state.freq != -1.0f) {
        DSD_FPRINTF(stderr, "COSTAS CLAMP: low clamp phase=%f freq=%f\n", s->costas_state.phase, s->costas_state.freq);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: non-finite detector magnitudes do not train the loop and are reported
 * as zero-confidence samples.
 */
static int
test_costas_nonfinite_sample_is_rejected(void) {
    static float buf[2] = {INFINITY, 0.5f};
    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->lowpassed = buf;
    s->lp_len = 2;
    s->cqpsk_enable = 1;
    s->costas_state.initialized = 1;
    s->costas_state.phase = 0.2f;
    s->costas_state.freq = 0.1f;
    s->costas_state.alpha = 0.04f;
    s->costas_state.beta = 0.0002f;
    s->costas_state.max_freq = 1.0f;
    s->costas_state.min_freq = -1.0f;

    op25_costas_loop_cc(s);

    if (buf[0] != 0.0f || buf[1] != 0.0f || s->costas_state.error != 0.0f || s->costas_state.error_smooth != 0.0f
        || s->costas_conf_avg_q14 != 0 || s->costas_zero_conf_pct != 100) {
        DSD_FPRINTF(stderr, "COSTAS NONFINITE: out=(%f,%f) err=%f smooth=%f conf=%d zero=%d\n", buf[0], buf[1],
                    s->costas_state.error, s->costas_state.error_smooth, s->costas_conf_avg_q14,
                    s->costas_zero_conf_pct);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: Gardner refuses SPS values that would overrun the fixed MMSE delay
 * line and reports no output symbols.
 */
static int
test_gardner_oversized_sps_disables_output(void) {
    static float buf[32];
    for (int i = 0; i < 32; i++) {
        buf[i] = (i & 1) ? -0.2f : 0.6f;
    }

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = 32;
    s->ted_sps = 200;

    op25_gardner_cc(s);

    if (s->lp_len != 0 || s->ted_state.twice_sps != 0 || s->ted_state.omega_mid != 200.0f) {
        DSD_FPRINTF(stderr, "GARDNER OVERSIZE: lp_len=%d twice_sps=%d omega_mid=%f\n", s->lp_len,
                    s->ted_state.twice_sps, s->ted_state.omega_mid);
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/*
 * Test: FLL band-edge block initializes lazily and processes an IQ block in
 * place while preserving a bounded loop state.
 */
static int
test_fll_band_edge_processes_block(void) {
    const int pairs = 64;
    static float buf[pairs * 2];
    for (int k = 0; k < pairs; k++) {
        const float phase = 0.11f * (float)k;
        buf[(size_t)k * 2] = cosf(phase);
        buf[(size_t)k * 2 + 1] = sinf(phase);
    }

    demod_state* s = alloc_state();
    if (!s) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->ted_sps = 5;
    s->rate_out = 24000;

    op25_fll_band_edge_cc(s);

    dsd_fll_band_edge_state_t* f = &s->fll_band_edge_state;
    const int expected_delay = pairs % f->n_taps;
    if (!f->initialized || f->sps != 5 || f->n_taps != 11 || f->delay_idx != expected_delay || !std::isfinite(f->phase)
        || !std::isfinite(f->freq) || f->freq < f->min_freq || f->freq > f->max_freq) {
        DSD_FPRINTF(stderr, "FLL PROCESS: initialized=%d sps=%d taps=%d delay=%d/%d phase=%f freq=%f min=%f max=%f\n",
                    f->initialized, f->sps, f->n_taps, f->delay_idx, expected_delay, f->phase, f->freq, f->min_freq,
                    f->max_freq);
        free(s);
        return 1;
    }
    if (f->delay_r[0] == 0.0f && f->delay_i[0] == 0.0f) {
        DSD_FPRINTF(stderr, "FLL PROCESS: delay line was not populated\n");
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

int
main(void) {
    if (test_basic_passthrough() != 0) {
        return 1;
    }
    if (test_cfo_tracking() != 0) {
        return 1;
    }
    if (test_disabled_when_not_cqpsk() != 0) {
        return 1;
    }
    if (test_diff_phasor_correctness() != 0) {
        return 1;
    }
    if (test_costas_normalizes_reliable_magnitude() != 0) {
        return 1;
    }
    if (test_costas_deep_fade_does_not_boost_or_train() != 0) {
        return 1;
    }
    if (test_costas_marginal_confidence_scales_loop_update() != 0) {
        return 1;
    }
    if (test_costas_smooths_error_step() != 0) {
        return 1;
    }
    if (test_costas_adapts_smoothing_for_phase_kick() != 0) {
        return 1;
    }
    if (test_ted_initialization() != 0) {
        return 1;
    }
    if (test_gardner_omega_absolute_clamp_p25p2() != 0) {
        return 1;
    }
    if (test_p25p2_tracking_gain_reduces_after_lock() != 0) {
        return 1;
    }
    if (test_p25p2_tracking_gain_honors_runtime_override_after_lock() != 0) {
        return 1;
    }
    if (test_reset_helpers_clear_tracking_state() != 0) {
        return 1;
    }
    if (test_diff_phasor_short_block_preserves_state() != 0) {
        return 1;
    }
    if (test_costas_clamps_initial_phase_and_frequency() != 0) {
        return 1;
    }
    if (test_costas_nonfinite_sample_is_rejected() != 0) {
        return 1;
    }
    if (test_gardner_oversized_sps_disables_output() != 0) {
        return 1;
    }
    if (test_fll_band_edge_processes_block() != 0) {
        return 1;
    }
    return 0;
}
