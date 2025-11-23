// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frequency-Locked Loop (FLL) helpers for residual carrier correction.
 *
 * Provides NCO-based mixing and loop update routines for FM demodulation.
 * Uses high-quality sin/cos from the math library for NCO rotation.
*/

#include <dsd-neo/dsp/fll.h>
#include <dsd-neo/dsp/math_utils.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const float kTwoPiF = 6.28318530717958647692f;
static const float kBeDamping = 0.70710678f; /* sqrt(2)/2 */
static const int kBeMaxTaps = DSP_FLL_BE_MAX_TAPS;

/* Clamp helper */
static inline int
clamp_i(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline float
clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline float
fll_be_wrap_phase(float p) {
    while (p > kTwoPiF) {
        p -= kTwoPiF;
    }
    while (p < -kTwoPiF) {
        p += kTwoPiF;
    }
    return p;
}

static inline void
fll_be_update_gains(fll_state_t* st, float loop_bw) {
    if (!st) {
        return;
    }
    st->be_loop_bw = loop_bw;
    float denom = 1.0f + 2.0f * kBeDamping * loop_bw + loop_bw * loop_bw;
    st->be_alpha = (4.0f * kBeDamping * loop_bw) / denom;
    st->be_beta = (4.0f * loop_bw * loop_bw) / denom;
}

static void
fll_be_design_filters(fll_state_t* st, float sps, float rolloff, int filter_size) {
    if (!st) {
        return;
    }
    if (filter_size < 3) {
        filter_size = 3;
    }
    if (filter_size > kBeMaxTaps) {
        filter_size = kBeMaxTaps;
    }
    /* Baseband taps */
    double power = 0.0;
    int M = (int)lrint((double)filter_size / (double)sps);
    double bb[kBeMaxTaps];
    for (int i = 0; i < filter_size; i++) {
        double k = -((double)M) + ((double)i * 2.0 / (double)sps);
        double tap = dsd_neo_sinc(rolloff * k - 0.5) + dsd_neo_sinc(rolloff * k + 0.5);
        bb[i] = tap;
        power += tap;
    }
    if (fabs(power) < 1e-9) {
        power = 1.0;
    }
    int N = (filter_size - 1) / 2;
    for (int i = 0; i < filter_size; i++) {
        double tap = bb[i] / power;
        double k = ((double)(-N + i)) / (2.0 * (double)sps);
        double ang_lower = -kTwoPiF * (1.0 + rolloff) * k;
        double ang_upper = kTwoPiF * (1.0 + rolloff) * k;
        int idx = filter_size - i - 1; /* mirror to match GNU Radio design_filter */
        st->be_taps_lower_r[idx] = (float)(tap * cos(ang_lower));
        st->be_taps_lower_i[idx] = (float)(tap * sin(ang_lower));
        st->be_taps_upper_r[idx] = (float)(tap * cos(ang_upper));
        st->be_taps_upper_i[idx] = (float)(tap * sin(ang_upper));
    }
    st->be_taps_len = filter_size;
    st->be_rolloff = rolloff;
    st->be_sps = sps;
    st->be_max_freq = kTwoPiF * (2.0f / sps);
    st->be_min_freq = -st->be_max_freq;
    st->be_buf_idx = 0;
    int buf_len = filter_size * 2;
    for (int i = 0; i < buf_len; i++) {
        st->be_buf_r[i] = 0.0f;
        st->be_buf_i[i] = 0.0f;
    }
    fll_be_update_gains(st, kTwoPiF / (sps * 250.0f)); /* OP25 default */
}

/* Very small integrator leakage to avoid long-term windup/drift.
 * Chosen as 1/4096 per update (>>12), small enough to be inaudible
 * for FM and gentle for digital modes while providing slow decay. */
static const int kFllIntLeakShift = 12; /* leak = x - (x >> 12) */

/* High-quality trig path */
/**
 * @brief Compute Q15 cosine/sine from Q15 phase using high-quality trig.
 *
 * Phase is Q15 where 2*pi == 1<<15. Outputs are Q15 in [-32767, 32767].
 */
static inline void
fll_sin_cos_q15_from_phase_trig(int phase_q15, int16_t* c_out, int16_t* s_out) {
    const double kQ15ToRad = (2.0 * M_PI) / 32768.0; /* map [0..32768) -> [0..2*pi) */
    int p = phase_q15 & 0x7FFF;                      /* wrap */
    double th = (double)p * kQ15ToRad;
    double cd = cos(th);
    double sd = sin(th);
    long ci = lrint(cd * 32767.0);
    long si = lrint(sd * 32767.0);
    if (ci > 32767) {
        ci = 32767;
    }
    if (ci < -32767) {
        ci = -32767;
    }
    if (si > 32767) {
        si = 32767;
    }
    if (si < -32767) {
        si = -32767;
    }
    *c_out = (int16_t)ci;
    *s_out = (int16_t)si;
}

/**
 * @brief 64-bit complex multiply (a * conj(b)) helper.
 *
 * @param ar Real part of a.
 * @param aj Imaginary part of a.
 * @param br Real part of b.
 * @param bj Imaginary part of b.
 * @param cr [out] Real part result accumulator (64-bit).
 * @param cj [out] Imag part result accumulator (64-bit).
 */
static inline void
multiply64(int ar, int aj, int br, int bj, int64_t* cr, int64_t* cj) {
    *cr = (int64_t)ar * (int64_t)br - (int64_t)aj * (int64_t)bj;
    *cj = (int64_t)aj * (int64_t)br + (int64_t)ar * (int64_t)bj;
}

/**
 * @brief Initialize FLL state with default values.
 *
 * @param state FLL state to initialize.
 */
void
fll_init_state(fll_state_t* state) {
    state->freq_q15 = 0;
    state->phase_q15 = 0;
    state->prev_r = 0;
    state->prev_j = 0;
    state->int_q15 = 0;
    state->prev_hist_len = 0;
    for (int i = 0; i < 64; i++) {
        state->prev_hist_r[i] = 0;
        state->prev_hist_j[i] = 0;
    }
    state->be_phase = 0.0f;
    state->be_freq = 0.0f;
    state->be_alpha = 0.0f;
    state->be_beta = 0.0f;
    state->be_max_freq = 0.0f;
    state->be_min_freq = 0.0f;
    state->be_loop_bw = 0.0f;
    state->be_sps = 0.0f;
    state->be_rolloff = 0.0f;
    state->be_taps_len = 0;
    state->be_buf_idx = 0;
    for (int i = 0; i < kBeMaxTaps; i++) {
        state->be_taps_lower_r[i] = 0.0f;
        state->be_taps_lower_i[i] = 0.0f;
        state->be_taps_upper_r[i] = 0.0f;
        state->be_taps_upper_i[i] = 0.0f;
    }
    for (int i = 0; i < kBeMaxTaps * 2; i++) {
        state->be_buf_r[i] = 0.0f;
        state->be_buf_i[i] = 0.0f;
    }
}

/**
 * @brief Mix I/Q by an NCO and advance phase by freq_q15 per sample.
 *
 * Phase and frequency are Q15 where a full turn (2*pi) maps to 1<<15.
 * Uses high-quality sin/cos from the math library for rotation.
 *
 * @param config FLL configuration.
 * @param state  FLL state (updates phase_q15).
 * @param x      Input/output interleaved I/Q buffer (modified in-place).
 * @param N      Length of buffer in samples (must be even).
 */
void
fll_mix_and_update(const fll_config_t* config, fll_state_t* state, int16_t* x, int N) {
    if (!config->enabled) {
        return;
    }

    int phase = state->phase_q15;     /* Q15 wraps at 1<<15 ~ 2*pi */
    const int freq = state->freq_q15; /* Q15 increment per sample */

    /* High-quality trig-based rotator */
    for (int i = 0; i + 1 < N; i += 2) {
        int16_t c, s;
        fll_sin_cos_q15_from_phase_trig(phase, &c, &s);
        int xr = x[i];
        int xj = x[i + 1];
        int32_t yr = ((int32_t)xr * c + (int32_t)xj * s) >> 15;
        int32_t yj = ((int32_t)xj * c - (int32_t)xr * s) >> 15;
        x[i] = (int16_t)yr;
        x[i + 1] = (int16_t)yj;
        phase += freq;
    }
    state->phase_q15 = phase & 0x7FFF;
}

/**
 * @brief Estimate frequency error and update FLL control (PI in Q15).
 *
 * Uses a phase-difference discriminator to compute average error.
 * Applies proportional and integral actions to adjust the NCO frequency.
 *
 * @param config FLL configuration.
 * @param state  FLL state (updates freq_q15 and may advance phase_q15).
 * @param x      Input interleaved I/Q buffer.
 * @param N      Length of buffer in samples (must be even).
 */
void
fll_update_error(const fll_config_t* config, fll_state_t* state, const int16_t* x, int N) {
    if (!config->enabled) {
        return;
    }

    int alpha = config->alpha_q15; /* Q15 */
    int beta = config->beta_q15;   /* Q15 */
    int prev_r = state->prev_r;
    int prev_j = state->prev_j;
    int64_t err_acc = 0; /* use wide accumulator to avoid overflow on large blocks */
    int count = 0;

    for (int i = 0; i + 1 < N; i += 2) {
        int r = x[i];
        int j = x[i + 1];
        if (i > 0 || (prev_r != 0 || prev_j != 0)) {
            /* z_n * conj(z_{n-1}) phase delta (Q14) */
            int64_t re = (int64_t)r * (int64_t)prev_r + (int64_t)j * (int64_t)prev_j;
            int64_t im = (int64_t)j * (int64_t)prev_r - (int64_t)r * (int64_t)prev_j;
            int e = dsd_neo_fast_atan2(im, re);
            err_acc += e;
            count++;
        }
        prev_r = r;
        prev_j = j;
    }

    state->prev_r = prev_r;
    state->prev_j = prev_j;

    if (count == 0) {
        return;
    }

    int32_t err = (int32_t)(err_acc / count); /* Q14 */

    /* Pre-apply small integrator leakage each update (even in deadband).
     *
     * Absolute clamp on integrator/frequency in Q15. Historically this was
     * 2048 (~±3 kHz @48k). To give digital paths (e.g., CQPSK at 12 kHz) more
     * pull-in range while remaining conservative for analog FM, use a slightly
     * higher bound.
     */
    const int32_t F_CLAMP = 4096; /* ~±6 kHz @48k, ~±1.5 kHz @12k */
    int32_t i_base = state->int_q15 - (state->int_q15 >> kFllIntLeakShift);
    i_base = clamp_i(i_base, -F_CLAMP, F_CLAMP);

    /* Deadband: ignore tiny phase errors to avoid audible low-frequency ramps.
       Keep leaked integrator so it slowly returns toward zero. */
    if (err < config->deadband_q14 && err > -config->deadband_q14) {
        state->int_q15 = (int)i_base;
        return;
    }

    /* True PI: I[z] accumulates error; control u = Kp*e + I. Apply slew on delta(u).
       Everything is in Q15 except err (Q14). */

    int32_t p = ((int64_t)alpha * err) >> 14;     /* -> Q15 */
    int32_t i_term = ((int64_t)beta * err) >> 14; /* -> Q15 */

    /* Integrator update with simple anti-windup bound */
    int32_t i_next = i_base + i_term;
    i_next = clamp_i(i_next, -F_CLAMP, F_CLAMP);

    /* Positional controller output */
    int32_t u = p + i_next; /* Q15 */

    /* Apply slew limit to change in frequency per update */
    int32_t df = u - state->freq_q15; /* desired delta */
    df = clamp_i(df, -config->slew_max_q15, config->slew_max_q15);
    int32_t f_new = state->freq_q15 + df;

    /* Clamp absolute frequency range */
    f_new = clamp_i(f_new, -F_CLAMP, F_CLAMP);

    state->freq_q15 = (int)f_new;
    state->int_q15 = (int)i_next;
}

/**
 * @brief Band-edge FLL for CQPSK (mirrors GNU Radio fll_band_edge_cc).
 *
 * Rotates the input in-place, runs the band-edge filters, and updates the
 * control loop using OP25 defaults:
 *   - rolloff: 0.2
 *   - taps: 2*sps + 1
 *   - loop_bw: 2*pi/sps/250
 *   - freq limits: +/-2*pi*(2/sps)
 *
 * @param config FLL configuration (enable flag only).
 * @param state  FLL state (updates freq_q15/phase_q15 and band-edge state).
 * @param x      Input/output interleaved I/Q buffer.
 * @param N      Length of the buffer in elements (must be even).
 * @param sps    Samples-per-symbol (complex samples per symbol).
 */
void
fll_update_error_qpsk(const fll_config_t* config, fll_state_t* state, int16_t* x, int N, int sps) {
    if (!config || !state || !config->enabled || !x || N < 2) {
        return;
    }
    if (sps < 1) {
        sps = 1;
    }
    if (sps > 64) {
        sps = 64;
    }
    float sps_f = (float)sps;
    int filter_size = (2 * sps) + 1;
    if (filter_size < 3) {
        filter_size = 3;
    }
    if (filter_size > kBeMaxTaps) {
        filter_size = kBeMaxTaps;
    }
    const float rolloff = 0.2f; /* OP25 default */
    /* Redesign taps when sps/rolloff change or not yet initialized. */
    if (state->be_taps_len != filter_size || fabsf(state->be_sps - sps_f) > 1e-6f
        || fabsf(state->be_rolloff - rolloff) > 1e-6f) {
        fll_be_design_filters(state, sps_f, rolloff, filter_size);
    }

    float phase = state->be_phase;
    float freq = state->be_freq;
    float alpha = state->be_alpha;
    float beta = state->be_beta;
    float fmax = state->be_max_freq;
    float fmin = state->be_min_freq;
    int taps_len = state->be_taps_len;
    int buf_idx = state->be_buf_idx;
    const float* tlr = state->be_taps_lower_r;
    const float* tli = state->be_taps_lower_i;
    const float* tur = state->be_taps_upper_r;
    const float* tui = state->be_taps_upper_i;
    float* buf_r = state->be_buf_r;
    float* buf_i = state->be_buf_i;
    const int pairs = N >> 1;
    const float q15_per_rad = 32768.0f / kTwoPiF;
    int last_r = 0;
    int last_j = 0;

    for (int n = 0; n < pairs; n++) {
        float ir = (float)x[(size_t)(n << 1)];
        float iq = (float)x[(size_t)(n << 1) + 1];
        float c = cosf(phase);
        float s = sinf(phase);
        float zr = ir * c - iq * s;
        float zi = ir * s + iq * c;
        int32_t zr_i = (int32_t)lrintf(zr);
        int32_t zi_i = (int32_t)lrintf(zi);
        x[(size_t)(n << 1)] = sat16(zr_i);
        x[(size_t)(n << 1) + 1] = sat16(zi_i);
        last_r = zr_i;
        last_j = zi_i;

        /* Push into ring buffer (with mirror to simplify dot product like GNU Radio). */
        buf_r[buf_idx] = zr;
        buf_i[buf_idx] = zi;
        buf_r[buf_idx + taps_len] = zr;
        buf_i[buf_idx + taps_len] = zi;
        buf_idx++;
        if (buf_idx >= taps_len) {
            buf_idx = 0;
        }

        float acc_lr = 0.0f, acc_li = 0.0f, acc_ur = 0.0f, acc_ui = 0.0f;
        int base = buf_idx;
        for (int k = 0; k < taps_len; k++) {
            int pos = base + k;
            float br = buf_r[pos];
            float bi = buf_i[pos];
            acc_lr += br * tlr[k] - bi * tli[k];
            acc_li += br * tli[k] + bi * tlr[k];
            acc_ur += br * tur[k] - bi * tui[k];
            acc_ui += br * tui[k] + bi * tur[k];
        }
        float e_upper = acc_ur * acc_ur + acc_ui * acc_ui;
        float e_lower = acc_lr * acc_lr + acc_li * acc_li;
        float error = e_upper - e_lower;

        freq = freq + beta * error;
        phase = phase + freq + alpha * error;
        phase = fll_be_wrap_phase(phase);
        freq = clampf(freq, fmin, fmax);
    }

    state->be_phase = phase;
    state->be_freq = freq;
    state->be_buf_idx = buf_idx;

    int fq15 = (int)lrintf(freq * q15_per_rad);
    int pq15 = ((int)lrintf(phase * q15_per_rad)) & 0x7FFF;
    int f_clamp_q15 = (int)lrintf(fmax * q15_per_rad);
    state->freq_q15 = clamp_i(fq15, -f_clamp_q15, f_clamp_q15);
    state->phase_q15 = pq15;
    state->int_q15 = state->freq_q15; /* mirror loop state for outer helpers */
    state->prev_r = last_r;
    state->prev_j = last_j;
}
