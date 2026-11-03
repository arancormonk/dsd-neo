// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * CQPSK chain EVM regression test.
 *
 * Synthesizes a raised-cosine-shaped pi/4-DQPSK stream (the TIA-102 CAI
 * transmit Nyquist filter model: flat to 0.4*Rs, cosine rolloff to 0.6*Rs) at
 * a whole capture rate, runs it through full_demod() (the half-band cascade,
 * the live I/Q rate step, then the CQPSK chain: channel LPF -> AGC -> FLL ->
 * Gardner -> diff_phasor -> Costas -> phase slicer), and measures symbol EVM
 * against the ideal {-3,-1,+1,+3} grid. The sliced symbols must also equal the
 * transmitted ones, aligned, over the whole measured span.
 *
 * Both CQPSK rate profiles are pinned: P25 Phase 1 LSM (4800 baud) and P25
 * Phase 2 H-DQPSK (6000 baud) share this chain and differ only in ted_sps.
 *
 * - At 48 kHz straight in (10 and 8 samples/symbol, no cascade, no step).
 * - At the rates a device-forced capture leaves the half-band cascade (issue
 *   #806): 78,125 and 93,750 Hz (2.5 and 3 MS/s after five passes; here 312.5
 *   and 375 kS/s after two) stepped to 48 kHz, and 39,062.5 and 46,875 Hz
 *   (after three) stepped to 24 kHz. Without the step the chain runs at those
 *   rates with a rounded ted_sps, which the Gardner's period clamp cannot
 *   follow: EVM near 20 % and symbols slipped. With it, the chain sees the
 *   whole-rate stream it is built for.
 * - At 24 kHz straight in (5 and 4 samples/symbol), the floor the 24 kHz
 *   legs are held to.
 *
 * The clean bound is the chain's floor at the target: 2.5% at 48 kHz and 3.5%
 * at 24 kHz, where 5 and 4 samples/symbol floor it higher. Straight in at
 * 24 kHz the chain measures 2.2-2.5% whatever the timing phase, through the
 * step 2.5-3.3%, and an RTL-shaped 192 kS/s capture through the three-pass
 * cascade alone (no step) 3.9% (P1) and 5.4% (P2).
 *
 * The clean-channel bound is the load-bearing assertion: it pins the
 * implementation floor of the whole timing/carrier chain. A time-reversed
 * MMSE interpolator (the bug this test was first written against) inverts the
 * Gardner loop's fractional feedback, which then limit-cycles across a whole
 * sample and floors EVM near 8% at sps=10 and 11% at sps=8 no matter how
 * clean the input is. The symbol comparison catches a chain that slips or
 * repeats symbols, which a +-2% count alone misses (a fractional rate drifts
 * the count by only +1.0 % at 78,125 Hz). The Costas loop runs on the
 * differential phasor and locks with a quarter-turn ambiguity (frame sync
 * resolves it, dsd_qpsk_sync_hamming_with_remaps()), so the comparison allows
 * one rotation of the levels, fixed for the whole span. The AWGN bound checks
 * the chain degrades gracefully instead of unlocking.
 */

#include <cmath>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/live_iq_step.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/runtime/mem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "dsd-neo/core/safe_api.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAlpha = 0.2;
constexpr int kTxSpanSymbols = 16; /* TX pulse span (symbols) */
constexpr int kTxHalfSpanSymbols = kTxSpanSymbols / 2;
constexpr int kNumSymbols = 12000;
constexpr int kWarmupSymbols = 2000;
/* The symbol comparison: the first matches that fix the alignment, how far the alignment may sit from the transmitted
   index, the last transmitted symbols left out (their pulse is cut off at the end of the waveform), and the measured
   span it must cover at least. */
constexpr int kAlignSymbols = 64;
constexpr int kMaxLagSymbols = 64;
constexpr int kTailSymbols = kTxSpanSymbols;
constexpr int kMinComparedSymbols = ((kNumSymbols - kWarmupSymbols) * 97) / 100;

/* Full raised-cosine impulse response (TIA-102 TX Nyquist filter model),
   t in samples, symbol period T = sps samples. */
double
rc_impulse(double t, double sps) {
    const double T = sps;
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
    float* iq;           /* interleaved I/Q */
    int pairs;           /* complex sample count */
    std::vector<int> tx; /* the transmitted symbols as the slicer reads them: -3, -1, +1, +3 */
};

/* Build a raised-cosine-shaped pi/4-DQPSK baseband waveform at sps samples/symbol (not necessarily whole), optionally
   with complex AWGN of the given sigma. */
Waveform
synthesize_lsm(double sps, double noise_sigma) {
    static const double kPhaseStep[4] = {kPi / 4.0, 3.0 * kPi / 4.0, -kPi / 4.0, -3.0 * kPi / 4.0};
    static const int kLevel[4] = {1, 3, -1, -3}; /* the phase step x 4 / pi */

    const int half_span = (int)(kTxHalfSpanSymbols * sps);
    const int pairs = (int)(kNumSymbols * sps);

    Waveform wf;
    wf.iq = nullptr;
    wf.pairs = 0;
    double* phase = (double*)malloc((size_t)kNumSymbols * sizeof(double));
    float* iq = (float*)calloc((size_t)pairs * 2, sizeof(float));
    if (!phase || !iq) {
        free(phase);
        free(iq);
        return wf;
    }

    wf.tx.resize((size_t)kNumSymbols);
    uint32_t seed = 0x13572468u;
    double acc = 0.0;
    for (int k = 0; k < kNumSymbols; k++) {
        const uint32_t dibit = lcg_dibit(&seed);
        wf.tx[(size_t)k] = kLevel[dibit];
        acc += kPhaseStep[dibit];
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
        const double centerd = k * sps;
        const int center = (int)std::lround(centerd);
        int n0 = center - half_span;
        int n1 = center + half_span;
        if (n0 < 0) {
            n0 = 0;
        }
        if (n1 >= pairs) {
            n1 = pairs - 1;
        }
        for (int n = n0; n <= n1; n++) {
            const double p = rc_impulse((double)n - centerd, sps);
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
    wf.iq = iq;
    wf.pairs = pairs;
    return wf;
}

/* One leg: a capture at capture_hz carrying symbol_hz symbols, decoded on a target_hz DSP bandwidth. */
struct Leg {
    const char* label;
    int capture_hz;
    int symbol_hz;
    int target_hz;
};

/* The live rate plan for the leg, as a device-forced capture at capture_hz plans it, as the step takes it. */
int
plan_leg(const Leg& leg, dsd_live_iq_step_plan* out) {
    dsd_rate_plan_input in;
    DSD_MEMSET(&in, 0, sizeof(in));
    in.backend = DSD_RATE_PLAN_BACKEND_AIRSPY;
    in.request_hz = (uint32_t)leg.target_hz;
    in.programmed_hz = (uint32_t)leg.capture_hz;
    in.readback_hz = (uint32_t)leg.capture_hz;
    in.target_hz = (uint32_t)leg.target_hz;
    in.max_passes = 10;
    dsd_rate_plan rp;
    if (dsd_rate_plan_compute(&in, &rp) != DSD_RATE_PLAN_OK) {
        DSD_FPRINTF(stderr, "cqpsk evm %s: rate plan refused: %s\n", leg.label, rp.message);
        return 1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->enabled = rp.step.enabled;
    out->passes = rp.passes;
    out->hb_out_num = rp.hb_out_num;
    out->hb_out_shift = rp.hb_out_shift;
    out->L = rp.step.L;
    out->M = rp.step.M;
    out->target_hz = (int)rp.target_hz;
    out->protect_hz = rp.step.protect_hz;
    out->stop_hz = rp.step.stop_hz;
    return 0;
}

demod_state*
make_state(const Leg& leg, const dsd_live_iq_step_plan& plan) {
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return nullptr;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    ted_init_state(&s->ted_state);

    s->rate_in = leg.target_hz;
    s->rate_out = leg.target_hz;
    s->downsample_passes = plan.passes;
    s->cqpsk_enable = 1;
    s->ted_sps = leg.target_hz / leg.symbol_hz;
    s->sps_is_integer = 1;
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    s->symbol_rate_hz = leg.symbol_hz;
    s->symbol_levels = 4;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    s->mode_demod = &dsd_fm_demod; /* any non-raw demod; CQPSK path bypasses it */
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;
    s->cqpsk_agc_avg = 1.0f;
    s->squelch_gate_open = 1;
    if (plan.enabled) {
        int rc = dsd_demod_live_step_prepare(s, &plan);
        if (rc == DSD_LIVE_IQ_STEP_OK) {
            rc = dsd_live_iq_step_commit(&s->live_iq_step);
        }
        dsd_live_iq_step_finish(&s->live_iq_step);
        if (rc != DSD_LIVE_IQ_STEP_OK) {
            DSD_FPRINTF(stderr, "cqpsk evm %s: step refused (%d, design %d)\n", leg.label, rc,
                        s->live_iq_step.design_status);
            dsd_live_iq_step_disable(&s->live_iq_step);
            dsd_neo_aligned_free(s);
            return nullptr;
        }
    }
    return s;
}

void
free_state(demod_state* s) {
    dsd_live_iq_step_disable(&s->live_iq_step);
    dsd_neo_aligned_free(s);
}

int
slice(float v) {
    if (v > 2.0f) {
        return 3;
    }
    if (v > 0.0f) {
        return 1;
    }
    if (v > -2.0f) {
        return -1;
    }
    return -3;
}

struct ChainResult {
    double evm;
    int symbols;
    std::vector<int> sliced;
};

/* Run the waveform through full_demod in 40 ms capture blocks, returning EVM% over post-warmup symbols and every
   sliced symbol. */
int
run_chain_evm(const Waveform& wf, const Leg& leg, const dsd_live_iq_step_plan& plan, ChainResult* out) {
    demod_state* s = make_state(leg, plan);
    if (!s) {
        return 1;
    }

    const int block_pairs = leg.capture_hz / 25;
    double err_acc = 0.0;
    double ref_acc = 0.0;
    int n_meas = 0;
    out->sliced.clear();

    for (int off = 0; off + block_pairs <= wf.pairs; off += block_pairs) {
        DSD_MEMCPY(s->input_cb_buf, wf.iq + (size_t)off * 2, (size_t)block_pairs * 2 * sizeof(float));
        s->lowpassed = s->input_cb_buf;
        s->lp_len = block_pairs * 2;
        full_demod(s);

        for (int i = 0; i < s->result_len; i++) {
            const float v = s->result[i];
            const int ideal = slice(v);
            out->sliced.push_back(ideal);
            if ((int)out->sliced.size() <= kWarmupSymbols) {
                continue;
            }
            const double e = (double)v - (double)ideal;
            err_acc += e * e;
            ref_acc += (double)ideal * (double)ideal;
            n_meas++;
        }
    }

    free_state(s);
    if (n_meas <= 0 || ref_acc <= 0.0) {
        return 1;
    }
    out->evm = std::sqrt(err_acc / (double)n_meas) / std::sqrt(ref_acc / (double)n_meas) * 100.0;
    out->symbols = (int)out->sliced.size();
    return 0;
}

/* Transmitted level @p level turned by @p quarter quarter-turns, as the slicer reads it: +2 a quarter-turn, wrapped. */
int
rotated(int level, int quarter) {
    int v = level + 2 * quarter;
    while (v > 3) {
        v -= 8;
    }
    return v;
}

/* Whether sliced symbols kWarmupSymbols.. match the transmitted ones at lag @p lag, turned by @p quarter, for the first
   kAlignSymbols. */
int
aligned_at(const std::vector<int>& sliced, const std::vector<int>& tx, int lag, int quarter) {
    for (int j = 0; j < kAlignSymbols; j++) {
        const int i = kWarmupSymbols + j;
        const int t = i + lag;
        if (i >= (int)sliced.size() || t < 0 || t >= (int)tx.size()
            || sliced[(size_t)i] != rotated(tx[(size_t)t], quarter)) {
            return 0;
        }
    }
    return 1;
}

/* The lag and rotation that align the sliced symbols with the transmitted ones. Returns 0 when none does. */
int
find_alignment(const std::vector<int>& sliced, const std::vector<int>& tx, int* lag_out, int* quarter_out) {
    for (int quarter = 0; quarter < 4; quarter++) {
        for (int lag = -kMaxLagSymbols; lag <= kMaxLagSymbols; lag++) {
            if (aligned_at(sliced, tx, lag, quarter)) {
                *lag_out = lag;
                *quarter_out = quarter;
                return 1;
            }
        }
    }
    return 0;
}

/* The sliced symbols after warm-up, aligned with the transmitted ones: every one must match. Returns 0 on success. */
int
check_symbols(const char* label, const std::vector<int>& sliced, const std::vector<int>& tx) {
    int lag = 0;
    int quarter = 0;
    if (!find_alignment(sliced, tx, &lag, &quarter)) {
        DSD_FPRINTF(stderr, "cqpsk evm %s: sliced symbols never align with the transmitted ones\n", label);
        return 1;
    }
    int compared = 0;
    int mismatches = 0;
    int first_mismatch = -1;
    for (int i = kWarmupSymbols; i < (int)sliced.size(); i++) {
        const int t = i + lag;
        if (t >= kNumSymbols - kTailSymbols) {
            break;
        }
        compared++;
        if (sliced[(size_t)i] != rotated(tx[(size_t)t], quarter)) {
            mismatches++;
            if (first_mismatch < 0) {
                first_mismatch = i;
            }
        }
    }
    DSD_FPRINTF(stderr, "cqpsk evm %s: symbols aligned at lag %d, %d quarter-turn(s), %d compared, %d mismatched\n",
                label, lag, quarter, compared, mismatches);
    if (mismatches != 0) {
        DSD_FPRINTF(stderr, "cqpsk evm %s: %d of %d symbols differ from the transmitted ones (first at %d)\n", label,
                    mismatches, compared, first_mismatch);
        return 1;
    }
    if (compared < kMinComparedSymbols) {
        DSD_FPRINTF(stderr, "cqpsk evm %s: only %d symbols compared, %d needed\n", label, compared,
                    kMinComparedSymbols);
        return 1;
    }
    return 0;
}

int
run_leg(const Leg& leg, double noise_sigma, ChainResult* result, std::vector<int>* tx) {
    dsd_live_iq_step_plan plan;
    if (plan_leg(leg, &plan) != 0) {
        return 1;
    }
    Waveform wf = synthesize_lsm((double)leg.capture_hz / (double)leg.symbol_hz, noise_sigma);
    if (!wf.iq) {
        DSD_FPRINTF(stderr, "waveform synthesis failed\n");
        return 1;
    }
    int rc = run_chain_evm(wf, leg, plan, result);
    free(wf.iq);
    if (tx) {
        *tx = wf.tx;
    }
    return rc;
}

int
check_count(const char* label, const char* what, int symbols) {
    const int expected = kNumSymbols;
    if (symbols < (expected * 98) / 100 || symbols > (expected * 102) / 100) {
        DSD_FPRINTF(stderr, "cqpsk evm %s: %ssymbol count %d outside 2%% of %d\n", label, what, symbols, expected);
        return 1;
    }
    return 0;
}

/* Clean channel: implementation floor of the timing/carrier chain, and every symbol right. Measured ~1.3% (sps=10)
   and ~1.5% (sps=8) at 48 kHz straight in, and 1.3-1.6% through the step; a time-reversed interpolator limit-cycles
   to ~8-11%, and a fractional rate without the step floors near 20%. At 24 kHz the bound is the chain's floor there
   (see the file comment). */
int
check_clean(const Leg& leg) {
    const double bound = leg.target_hz >= 48000 ? 2.5 : 3.5;
    ChainResult clean;
    std::vector<int> tx;
    if (run_leg(leg, 0.0, &clean, &tx) != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "cqpsk evm %s: clean=%.2f%% symbols=%d\n", leg.label, clean.evm, clean.symbols);

    int rc = check_count(leg.label, "", clean.symbols);
    if (!(clean.evm < bound)) {
        DSD_FPRINTF(stderr, "cqpsk evm %s: clean-channel EVM %.2f%% exceeds %.1f%% floor bound\n", leg.label, clean.evm,
                    bound);
        rc = 1;
    }
    rc |= check_symbols(leg.label, clean.sliced, tx);
    return rc;
}

/* AWGN channel: the chain must degrade gracefully, not unlock. */
int
check_noisy(const Leg& leg) {
    ChainResult noisy;
    if (run_leg(leg, 0.05, &noisy, NULL) != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "cqpsk evm %s: noisy=%.2f%% symbols=%d\n", leg.label, noisy.evm, noisy.symbols);

    int rc = check_count(leg.label, "noisy ", noisy.symbols);
    if (!(noisy.evm > 3.0 && noisy.evm < 10.0)) {
        DSD_FPRINTF(stderr, "cqpsk evm %s: noisy EVM %.2f%% outside sane 3..10%% window\n", leg.label, noisy.evm);
        rc = 1;
    }
    return rc;
}

} // namespace

int
main(void) {
    /* 48 kHz straight in: no cascade, no step. */
    const Leg native[] = {
        {"p25p1 4800/sps10", 48000, 4800, 48000},
        {"p25p2 6000/sps8", 48000, 6000, 48000},
    };
    /* 24 kHz straight in: the floor the 24 kHz legs below are held to. */
    const Leg native_24k[] = {
        {"p25p1 4800/sps5", 24000, 4800, 24000},
        {"p25p2 6000/sps4", 24000, 6000, 24000},
    };
    /* Captures whose half-band output is a device-forced rate, stepped to the DSP bandwidth rate. */
    const Leg stepped[] = {
        {"p25p1 78125->48k", 312500, 4800, 48000},   {"p25p2 78125->48k", 312500, 6000, 48000},
        {"p25p1 93750->48k", 375000, 4800, 48000},   {"p25p2 93750->48k", 375000, 6000, 48000},
        {"p25p1 39062.5->24k", 312500, 4800, 24000}, {"p25p2 39062.5->24k", 312500, 6000, 24000},
        {"p25p1 46875->24k", 375000, 4800, 24000},   {"p25p2 46875->24k", 375000, 6000, 24000},
    };
    int rc = 0;
    for (const Leg& leg : native) {
        rc |= check_clean(leg);
        rc |= check_noisy(leg);
    }
    for (const Leg& leg : native_24k) {
        rc |= check_clean(leg);
    }
    for (const Leg& leg : stepped) {
        rc |= check_clean(leg);
    }
    return rc;
}
