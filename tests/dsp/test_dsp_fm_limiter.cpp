// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: fm_constant_envelope_limiter scales samples outside [0.5, 2.0] of target
   magnitude to near the target magnitude, and leaves in-range samples unchanged. */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <string.h>

// Provide globals expected by demod_pipeline.cpp
int use_halfband_decimator = 0;

static double
rms_mag_iq(const int16_t* iq, int pairs) {
    double acc = 0.0;
    for (int n = 0; n < pairs; n++) {
        double I = iq[(size_t)(2 * n) + 0];
        double Q = iq[(size_t)(2 * n) + 1];
        acc += I * I + Q * Q;
    }
    return (pairs > 0) ? std::sqrt(acc / pairs) : 0.0;
}

static int
approx_eq(int a, int b, int tol) {
    int d = a - b;
    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    memset(s, 0, sizeof(*s));

    const int pairs = 256;
    static int16_t buf[(size_t)pairs * 2];

    // Build two-level magnitude block on I axis (Q=0): low=2000, high=22000 (>2x target to trigger clamp)
    for (int n = 0; n < pairs; n++) {
        int16_t amp = (n < pairs / 2) ? 2000 : 22000;
        buf[(size_t)(2 * n) + 0] = amp;
        buf[(size_t)(2 * n) + 1] = 0;
    }

    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->mode_demod = &raw_demod; // copy lowpassed -> result, then return
    s->fm_agc_enable = 0;       // isolate limiter behavior
    s->fm_limiter_enable = 1;
    s->fm_cma_enable = 0;
    s->cqpsk_enable = 0;
    s->iqbal_enable = 0;
    s->squelch_level = 0;
    s->fll_enabled = 0;
    s->ted_enabled = 0;
    s->fm_agc_target_rms = 10000; // default target

    double pre_rms = rms_mag_iq(buf, pairs);
    (void)pre_rms; // informational

    full_demod(s);

    // Verify both halves near the target magnitude on I, Q remains ~0
    for (int n = 0; n < pairs; n++) {
        int I = s->result[(size_t)(2 * n) + 0];
        int Q = s->result[(size_t)(2 * n) + 1];
        if (!approx_eq(I, 10000, 300)) {
            fprintf(stderr, "Limiter: sample %d I=%d not near 10000\n", n, I);
            free(s);
            return 1;
        }
        if (!approx_eq(Q, 0, 100)) {
            fprintf(stderr, "Limiter: sample %d Q=%d deviates from 0\n", n, Q);
            free(s);
            return 1;
        }
    }

    // Second scenario: in-range magnitude should remain unchanged (~7000)
    for (int n = 0; n < pairs; n++) {
        buf[(size_t)(2 * n) + 0] = 7000;
        buf[(size_t)(2 * n) + 1] = 0;
    }
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    for (int n = 0; n < pairs; n++) {
        int I = s->result[(size_t)(2 * n) + 0];
        int Q = s->result[(size_t)(2 * n) + 1];
        if (!approx_eq(I, 7000, 200)) {
            fprintf(stderr, "Limiter in-range: sample %d I=%d changed too much\n", n, I);
            free(s);
            return 1;
        }
        if (!approx_eq(Q, 0, 50)) {
            fprintf(stderr, "Limiter in-range: sample %d Q=%d deviates\n", n, Q);
            free(s);
            return 1;
        }
    }

    // Third scenario: mixed-phase tones with magnitudes outside the band should be normalized to target
    for (int n = 0; n < pairs; n++) {
        double th = (2.0 * M_PI * n) / pairs;
        double A = (n < pairs / 2) ? 3000.0 : 25000.0; // below 0.5x and above 2x target
        buf[(size_t)(2 * n) + 0] = (int16_t)lrint(A * cos(th));
        buf[(size_t)(2 * n) + 1] = (int16_t)lrint(A * sin(th));
    }
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    for (int n = 0; n < pairs; n++) {
        int I = s->result[(size_t)(2 * n) + 0];
        int Q = s->result[(size_t)(2 * n) + 1];
        double mag = std::sqrt((double)I * I + (double)Q * Q);
        if (!(mag > 9600.0 && mag < 10400.0)) {
            fprintf(stderr, "Limiter mixed-phase: sample %d |z|=%.1f not near 10000\n", n, mag);
            free(s);
            return 1;
        }
    }

    // Fourth scenario: near-boundary behavior — just inside band unchanged; just outside clamps to target
    auto gen_rot = [&](double A) {
        for (int n = 0; n < pairs; n++) {
            double th = (2.0 * M_PI * n) / (pairs - 1);
            buf[(size_t)(2 * n) + 0] = (int16_t)lrint(A * cos(th));
            buf[(size_t)(2 * n) + 1] = (int16_t)lrint(A * sin(th));
        }
    };

    // Inside low boundary: 0.51x target
    gen_rot(0.51 * 10000.0);
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    for (int n = 0; n < pairs; n++) {
        double preI = 0.51 * 10000.0 * cos((2.0 * M_PI * n) / (pairs - 1));
        double preQ = 0.51 * 10000.0 * sin((2.0 * M_PI * n) / (pairs - 1));
        int I = s->result[(size_t)(2 * n) + 0];
        int Q = s->result[(size_t)(2 * n) + 1];
        if (!approx_eq(I, (int)lrint(preI), 150) || !approx_eq(Q, (int)lrint(preQ), 150)) {
            fprintf(stderr, "Limiter boundary in-low: sample %d changed too much\n", n);
            free(s);
            return 1;
        }
    }

    // Outside low boundary: 0.49x target → clamp
    gen_rot(0.49 * 10000.0);
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    for (int n = 0; n < pairs; n++) {
        int I = s->result[(size_t)(2 * n) + 0];
        int Q = s->result[(size_t)(2 * n) + 1];
        double mag = std::sqrt((double)I * I + (double)Q * Q);
        if (!(mag > 9600.0 && mag < 10400.0)) {
            fprintf(stderr, "Limiter boundary out-low: sample %d |z|=%.1f not clamped\n", n, mag);
            free(s);
            return 1;
        }
    }

    // Inside high boundary: 1.99x target
    gen_rot(1.99 * 10000.0);
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    for (int n = 0; n < pairs; n++) {
        double preI = 1.99 * 10000.0 * cos((2.0 * M_PI * n) / (pairs - 1));
        double preQ = 1.99 * 10000.0 * sin((2.0 * M_PI * n) / (pairs - 1));
        int I = s->result[(size_t)(2 * n) + 0];
        int Q = s->result[(size_t)(2 * n) + 1];
        if (!approx_eq(I, (int)lrint(preI), 300) || !approx_eq(Q, (int)lrint(preQ), 300)) {
            fprintf(stderr, "Limiter boundary in-high: sample %d changed too much\n", n);
            free(s);
            return 1;
        }
    }

    // Outside high boundary: 2.01x target → clamp
    gen_rot(2.01 * 10000.0);
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    for (int n = 0; n < pairs; n++) {
        int I = s->result[(size_t)(2 * n) + 0];
        int Q = s->result[(size_t)(2 * n) + 1];
        double mag = std::sqrt((double)I * I + (double)Q * Q);
        if (!(mag > 9600.0 && mag < 10400.0)) {
            fprintf(stderr, "Limiter boundary out-high: sample %d |z|=%.1f not clamped\n", n, mag);
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}
