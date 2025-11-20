// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit test: FM/C4FM CMA smoother/equalizer reduces envelope variance on a
 * synthetic constant-envelope C4FM-like waveform with injected short-delay
 * multipath.
 *
 * We synthesize a complex baseband sequence with constant modulus (FM/FSK-like
 * rotation) and apply a small echo. With CMA disabled, the envelope exhibits
 * clear ripple. With the FM CMA 3-tap symmetric smoother enabled, we expect
 * the envelope variance to decrease by a conservative margin.
 */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <string.h>

// Provide global expected by demod_pipeline.cpp
int use_halfband_decimator = 0;

static void
make_c4fm_like_complex(int16_t* dst, int pairs, double amp, unsigned* seed) {
    unsigned s = *seed;
    const int sps = 10; /* samples per symbol (approximate) */
    int sym_rem = 0;
    int freq_idx = 1;
    double phase = 0.0;
    for (int n = 0; n < pairs; n++) {
        if (sym_rem <= 0) {
            sym_rem = sps;
            /* Simple dibit source -> 4-level index {-3,-1,+1,+3} */
            s = s * 1103515245u + 12345u;
            int dibit = (int)((s >> 30) & 3);
            static const int levels[4] = {-3, -1, +1, +3};
            freq_idx = levels[dibit];
        }
        sym_rem--;
        /* Map index to a modest phase increment to keep rotation slow but nonzero */
        double w = 0.04 * (double)freq_idx; /* rad/sample */
        phase += w;
        double I = amp * cos(phase);
        double Q = amp * sin(phase);
        if (I > 32767.0) {
            I = 32767.0;
        }
        if (I < -32768.0) {
            I = -32768.0;
        }
        if (Q > 32767.0) {
            Q = 32767.0;
        }
        if (Q < -32768.0) {
            Q = -32768.0;
        }
        dst[(size_t)(n << 1) + 0] = (int16_t)lrint(I);
        dst[(size_t)(n << 1) + 1] = (int16_t)lrint(Q);
    }
    *seed = s;
}

static void
apply_short_multipath(const int16_t* in, int16_t* out, int pairs, int delay, double alpha) {
    for (int n = 0; n < pairs; n++) {
        double I0 = in[(size_t)(n << 1) + 0];
        double Q0 = in[(size_t)(n << 1) + 1];
        double I1 = 0.0;
        double Q1 = 0.0;
        if (n - delay >= 0) {
            int idx = n - delay;
            I1 = in[(size_t)(idx << 1) + 0];
            Q1 = in[(size_t)(idx << 1) + 1];
        }
        double I = I0 + alpha * I1;
        double Q = Q0 + alpha * Q1;
        if (I > 32767.0) {
            I = 32767.0;
        }
        if (I < -32768.0) {
            I = -32768.0;
        }
        if (Q > 32767.0) {
            Q = 32767.0;
        }
        if (Q < -32768.0) {
            Q = -32768.0;
        }
        out[(size_t)(n << 1) + 0] = (int16_t)lrint(I);
        out[(size_t)(n << 1) + 1] = (int16_t)lrint(Q);
    }
}

/* Standard deviation of |z|^2 over complex pairs (envelope ripple proxy). */
static double
env_var_std(const int16_t* iq, int pairs) {
    if (pairs <= 0) {
        return 0.0;
    }
    long double acc = 0.0;
    long double acc2 = 0.0;
    for (int n = 0; n < pairs; n++) {
        long double I = iq[(size_t)(n << 1) + 0];
        long double Q = iq[(size_t)(n << 1) + 1];
        long double m2 = I * I + Q * Q;
        acc += m2;
        acc2 += m2 * m2;
    }
    long double mean = acc / (long double)pairs;
    long double var = acc2 / (long double)pairs - mean * mean;
    if (var < 0.0L) {
        var = 0.0L;
    }
    return std::sqrt((double)var);
}

int
main(void) {
    const int pairs = 4000;
    static int16_t base[(size_t)pairs * 2];
    static int16_t in[(size_t)pairs * 2];

    unsigned seed = 0xC4F0C4F0u;
    make_c4fm_like_complex(base, pairs, 12000.0, &seed);
    /* Inject stronger short-delay multipath: echo at 1 sample, ~-1 dB. */
    apply_short_multipath(base, in, pairs, 1, 0.9);

    /* Baseline: CMA disabled, raw_demod to expose complex baseband. */
    demod_state* s0 = (demod_state*)malloc(sizeof(demod_state));
    if (!s0) {
        return 1;
    }
    memset(s0, 0, sizeof(*s0));
    static int16_t buf0[(size_t)pairs * 2];
    memcpy(buf0, in, sizeof(buf0));
    s0->lowpassed = buf0;
    s0->lp_len = pairs * 2;
    s0->mode_demod = &raw_demod;
    s0->cqpsk_enable = 0;
    s0->fm_cma_enable = 0;
    s0->fm_agc_enable = 0;
    s0->fm_limiter_enable = 0;
    s0->iqbal_enable = 0;
    s0->fll_enabled = 0;
    s0->ted_enabled = 0;
    s0->iq_dc_block_enable = 0;
    s0->squelch_level = 0;
    s0->squelch_gate_open = 1;
    s0->squelch_env_q15 = 32768;

    full_demod(s0);
    int out_pairs0 = s0->result_len / 2;
    double std0 = env_var_std(s0->result, out_pairs0);

    /* CMA path: 3-tap symmetric smoother on complex envelope. */
    demod_state* s1 = (demod_state*)malloc(sizeof(demod_state));
    if (!s1) {
        free(s0);
        return 1;
    }
    memset(s1, 0, sizeof(*s1));
    static int16_t buf1[(size_t)pairs * 2];
    memcpy(buf1, in, sizeof(buf1));
    s1->lowpassed = buf1;
    s1->lp_len = pairs * 2;
    s1->mode_demod = &raw_demod;
    s1->cqpsk_enable = 0;
    s1->fm_cma_enable = 1;
    s1->fm_cma_taps = 3;     /* enable 3-tap symmetric smoother */
    s1->fm_cma_strength = 2; /* strong smoothing ([1,6,1]/8) */
    s1->fm_agc_enable = 0;
    s1->fm_limiter_enable = 0;
    s1->iqbal_enable = 0;
    s1->fll_enabled = 0;
    s1->ted_enabled = 0;
    s1->iq_dc_block_enable = 0;
    s1->squelch_level = 0;
    s1->squelch_gate_open = 1;
    s1->squelch_env_q15 = 32768;

    full_demod(s1);
    int out_pairs1 = s1->result_len / 2;
    double std1 = env_var_std(s1->result, out_pairs1);

    if (out_pairs0 <= 0 || out_pairs1 != out_pairs0) {
        fprintf(stderr, "FM CMA: unexpected output lengths base=%d cma=%d\n", out_pairs0, out_pairs1);
        free(s0);
        free(s1);
        return 1;
    }
    if (std0 <= 0.0 || std1 <= 0.0) {
        fprintf(stderr, "FM CMA: degenerate envelope stddev base=%.3f cma=%.3f\n", std0, std1);
        free(s0);
        free(s1);
        return 1;
    }
    /* Expect 3-tap smoother to reduce envelope variance by a conservative margin. */
    if (!(std1 < 0.98 * std0)) {
        fprintf(stderr, "FM CMA: envelope variance not reduced enough (base=%.3f cma=%.3f)\n", std0, std1);
        free(s0);
        free(s1);
        return 1;
    }

    free(s0);
    free(s1);
    return 0;
}
