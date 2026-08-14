// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * CQPSK chain EVM regression test.
 *
 * Synthesizes a raised-cosine-shaped pi/4-DQPSK stream (the TIA-102 CAI
 * transmit Nyquist filter model: flat to 0.4*Rs, cosine rolloff to 0.6*Rs) at
 * 48 kHz / 10 samples per symbol, runs it through the full CQPSK chain
 * (channel LPF -> AGC -> FLL -> Gardner -> diff_phasor -> Costas -> phase
 * slicer) via full_demod(), and measures symbol EVM against the ideal
 * {-3,-1,+1,+3} grid.
 *
 * The clean-channel bound is the load-bearing assertion: it pins the
 * implementation floor of the whole timing/carrier chain. A time-reversed
 * MMSE interpolator (the bug this test was written against) inverts the
 * Gardner loop's fractional feedback, which then limit-cycles across a whole
 * sample and floors EVM near 8% no matter how clean the input is. The AWGN
 * bound checks the chain degrades gracefully instead of unlocking.
 */

#include <cmath>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/ted.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/safe_api.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSps = 10;
constexpr double kAlpha = 0.2;
constexpr int kTxSpanSymbols = 16; /* TX pulse span (symbols) */
constexpr int kNumSymbols = 12000;
constexpr int kWarmupSymbols = 2000;

/* Full raised-cosine impulse response (TIA-102 TX Nyquist filter model),
   t in samples, symbol period T = kSps. */
double
rc_impulse(double t) {
    const double T = (double)kSps;
    const double a = kAlpha;
    const double x = t / T;

    double sinc;
    if (std::fabs(x) < 1e-9) {
        sinc = 1.0;
    } else {
        sinc = std::sin(kPi * x) / (kPi * x);
    }

    const double den = 1.0 - (2.0 * a * x) * (2.0 * a * x);
    if (std::fabs(den) < 1e-7) {
        /* t = +/- T/(2a): limit of the cosine factor is (pi/4)*sinc(x). */
        return sinc * kPi / 4.0;
    }
    return sinc * std::cos(kPi * a * x) / den;
}

/* Deterministic helpers (test-only waveform synthesis). */
uint32_t
lcg_next_u32(uint32_t* state) {
    *state = (*state * 1103515245u) + 12345u;
    return *state;
}

uint32_t
lcg_dibit(uint32_t* state) {
    return (lcg_next_u32(state) >> 16) & 0x3u;
}

double
lcg_uniform(uint32_t* state) {
    return ((double)(lcg_next_u32(state) >> 8) + 0.5) / 16777216.0;
}

/* Box-Muller gaussian pair from the LCG. */
void
lcg_gauss_pair(uint32_t* state, double sigma, double* g0, double* g1) {
    const double u1 = lcg_uniform(state);
    const double u2 = lcg_uniform(state);
    const double mag = sigma * std::sqrt(-2.0 * std::log(u1));
    *g0 = mag * std::cos(2.0 * kPi * u2);
    *g1 = mag * std::sin(2.0 * kPi * u2);
}

struct Waveform {
    float* iq; /* interleaved I/Q */
    int pairs; /* complex sample count */
};

/* Build a raised-cosine-shaped pi/4-DQPSK baseband waveform at kSps
   samples/symbol, optionally with complex AWGN of the given sigma. */
Waveform
synthesize_lsm(double noise_sigma) {
    static const double kPhaseStep[4] = {kPi / 4.0, 3.0 * kPi / 4.0, -kPi / 4.0, -3.0 * kPi / 4.0};

    const int half_span = (kTxSpanSymbols / 2) * kSps;
    const int pairs = kNumSymbols * kSps;

    double* phase = (double*)malloc((size_t)kNumSymbols * sizeof(double));
    float* iq = (float*)calloc((size_t)pairs * 2, sizeof(float));
    if (!phase || !iq) {
        free(phase);
        free(iq);
        return Waveform{nullptr, 0};
    }

    uint32_t seed = 0x13572468u;
    double acc = 0.0;
    for (int k = 0; k < kNumSymbols; k++) {
        acc += kPhaseStep[lcg_dibit(&seed)];
        if (acc > kPi) {
            acc -= 2.0 * kPi;
        } else if (acc < -kPi) {
            acc += 2.0 * kPi;
        }
        phase[k] = acc;
    }

    const double amp = 0.35;
    for (int k = 0; k < kNumSymbols; k++) {
        const double si = std::cos(phase[k]) * amp;
        const double sq = std::sin(phase[k]) * amp;
        const int center = k * kSps;
        int n0 = center - half_span;
        int n1 = center + half_span;
        if (n0 < 0) {
            n0 = 0;
        }
        if (n1 >= pairs) {
            n1 = pairs - 1;
        }
        for (int n = n0; n <= n1; n++) {
            const double p = rc_impulse((double)(n - center));
            iq[(size_t)n * 2 + 0] += (float)(si * p);
            iq[(size_t)n * 2 + 1] += (float)(sq * p);
        }
    }

    if (noise_sigma > 0.0) {
        uint32_t nseed = 0x2468ACE1u;
        for (int n = 0; n < pairs; n++) {
            double gi = 0.0;
            double gq = 0.0;
            lcg_gauss_pair(&nseed, noise_sigma, &gi, &gq);
            iq[(size_t)n * 2 + 0] += (float)gi;
            iq[(size_t)n * 2 + 1] += (float)gq;
        }
    }

    free(phase);
    return Waveform{iq, pairs};
}

demod_state*
make_state(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return nullptr;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    ted_init_state(&s->ted_state);

    s->rate_in = 48000;
    s->rate_out = 48000;
    s->downsample_passes = 0;
    s->cqpsk_enable = 1;
    s->ted_sps = kSps;
    s->sps_is_integer = 1;
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    s->symbol_rate_hz = 4800;
    s->symbol_levels = 4;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    s->mode_demod = &dsd_fm_demod; /* any non-raw demod; CQPSK path bypasses it */
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;
    s->cqpsk_agc_avg = 1.0f;
    s->squelch_gate_open = 1;
    return s;
}

/* Run the waveform through full_demod, returning EVM% over post-warmup symbols. */
int
run_chain_evm(const Waveform& wf, double* evm_out, int* symbols_out) {
    demod_state* s = make_state();
    if (!s) {
        return 1;
    }

    const int block_pairs = 1920;
    double err_acc = 0.0;
    double ref_acc = 0.0;
    int n_meas = 0;
    int n_total = 0;

    for (int off = 0; off + block_pairs <= wf.pairs; off += block_pairs) {
        DSD_MEMCPY(s->hb_workbuf, wf.iq + (size_t)off * 2, (size_t)block_pairs * 2 * sizeof(float));
        s->lowpassed = s->hb_workbuf;
        s->lp_len = block_pairs * 2;
        full_demod(s);

        for (int i = 0; i < s->result_len; i++) {
            n_total++;
            if (n_total <= kWarmupSymbols) {
                continue;
            }
            const float v = s->result[i];
            float ideal;
            if (v > 2.0f) {
                ideal = 3.0f;
            } else if (v > 0.0f) {
                ideal = 1.0f;
            } else if (v > -2.0f) {
                ideal = -1.0f;
            } else {
                ideal = -3.0f;
            }
            const double e = (double)v - (double)ideal;
            err_acc += e * e;
            ref_acc += (double)ideal * (double)ideal;
            n_meas++;
        }
    }

    free(s);
    if (n_meas <= 0 || ref_acc <= 0.0) {
        return 1;
    }
    *evm_out = std::sqrt(err_acc / (double)n_meas) / std::sqrt(ref_acc / (double)n_meas) * 100.0;
    *symbols_out = n_total;
    return 0;
}

int
run_leg(double noise_sigma, double* evm, int* syms) {
    Waveform wf = synthesize_lsm(noise_sigma);
    if (!wf.iq) {
        DSD_FPRINTF(stderr, "waveform synthesis failed\n");
        return 1;
    }
    int rc = run_chain_evm(wf, evm, syms);
    free(wf.iq);
    return rc;
}

} // namespace

int
main(void) {
    int rc = 0;

    /* Clean channel: implementation floor of the timing/carrier chain.
       Measured ~1.3%; a time-reversed interpolator limit-cycles to ~8%. */
    double clean_evm = 0.0;
    int clean_syms = 0;
    if (run_leg(0.0, &clean_evm, &clean_syms) != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "cqpsk evm: clean=%.2f%% symbols=%d\n", clean_evm, clean_syms);

    const int expected = kNumSymbols;
    if (clean_syms < (expected * 98) / 100 || clean_syms > (expected * 102) / 100) {
        DSD_FPRINTF(stderr, "cqpsk evm: symbol count %d outside 2%% of %d\n", clean_syms, expected);
        rc = 1;
    }
    if (!(clean_evm < 2.5)) {
        DSD_FPRINTF(stderr, "cqpsk evm: clean-channel EVM %.2f%% exceeds 2.5%% floor bound\n", clean_evm);
        rc = 1;
    }

    /* AWGN channel: the chain must degrade gracefully, not unlock. */
    double noisy_evm = 0.0;
    int noisy_syms = 0;
    if (run_leg(0.05, &noisy_evm, &noisy_syms) != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "cqpsk evm: noisy=%.2f%% symbols=%d\n", noisy_evm, noisy_syms);

    if (noisy_syms < (expected * 98) / 100 || noisy_syms > (expected * 102) / 100) {
        DSD_FPRINTF(stderr, "cqpsk evm: noisy symbol count %d outside 2%% of %d\n", noisy_syms, expected);
        rc = 1;
    }
    if (!(noisy_evm > 3.0 && noisy_evm < 10.0)) {
        DSD_FPRINTF(stderr, "cqpsk evm: noisy EVM %.2f%% outside sane 3..10%% window\n", noisy_evm);
        rc = 1;
    }

    return rc;
}
