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

#include <complex>
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
fll_be_update_gains(fll_state_t* st, float loop_bw, float sps) {
    if (!st) {
        return;
    }
    /* Match GNU Radio fll_band_edge_cc: alpha = 0, beta = 4*loop_bw/sps (loop_bw already in rad). */
    st->be_loop_bw = loop_bw;
    st->be_alpha = 0.0f;
    st->be_beta = (4.0f * loop_bw) / sps;
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
    fll_be_update_gains(st, kTwoPiF / (sps * 250.0f), sps); /* OP25 default */
}

/* Very small integrator leakage to avoid long-term windup/drift.
 * Chosen as 1/4096 per update (>>12), small enough to be inaudible
 * for FM and gentle for digital modes while providing slow decay. */
static const int kFllIntLeakShift = 12; /* leak = x - (x >> 12) */

/* High-quality trig path */
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
    state->freq = 0.0f;
    state->phase = 0.0f;
    state->prev_r = 0.0f;
    state->prev_j = 0.0f;
    state->integrator = 0.0f;
    state->prev_hist_len = 0;
    for (int i = 0; i < 64; i++) {
        state->prev_hist_r[i] = 0.0f;
        state->prev_hist_j[i] = 0.0f;
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
 * @brief Mix I/Q by an NCO and advance phase by freq per sample (GNU Radio style).
 *
 * Phase and frequency are in radians. Phase wraps at ±2π.
 * Uses high-quality sin/cos from the math library for rotation.
 *
 * @param config FLL configuration.
 * @param state  FLL state (updates phase).
 * @param x      Input/output interleaved I/Q buffer (modified in-place).
 * @param N      Length of buffer in samples (must be even).
 */
void
fll_mix_and_update(const fll_config_t* config, fll_state_t* state, float* x, int N) {
    if (!config->enabled) {
        return;
    }

    float phase = state->phase;
    const float freq = state->freq;

    for (int i = 0; i + 1 < N; i += 2) {
        float c = cosf(phase);
        float s = sinf(phase);
        float xr = x[i];
        float xj = x[i + 1];
        float yr = xr * c + xj * s;
        float yj = xj * c - xr * s;
        x[i] = yr;
        x[i + 1] = yj;
        phase += freq;
        /* Wrap phase to [-2π, 2π] to avoid float precision loss */
        while (phase > kTwoPiF) {
            phase -= kTwoPiF;
        }
        while (phase < -kTwoPiF) {
            phase += kTwoPiF;
        }
    }
    state->phase = phase;
}

/**
 * @brief Estimate frequency error and update FLL control (GNU Radio-style native float PI).
 *
 * Uses a phase-difference discriminator to compute average error.
 * Applies proportional and integral actions to adjust the NCO frequency.
 *
 * @param config FLL configuration (native float gains).
 * @param state  FLL state (updates freq and integrator).
 * @param x      Input interleaved I/Q buffer.
 * @param N      Length of buffer in samples (must be even).
 */
void
fll_update_error(const fll_config_t* config, fll_state_t* state, const float* x, int N) {
    if (!config->enabled) {
        return;
    }

    const float alpha = config->alpha;
    const float beta = config->beta;
    float prev_r = state->prev_r;
    float prev_j = state->prev_j;
    double err_acc = 0.0; /* accumulate in double for stability */
    int count = 0;

    for (int i = 0; i + 1 < N; i += 2) {
        float r = x[i];
        float j = x[i + 1];
        if (i > 0 || (prev_r != 0.0f || prev_j != 0.0f)) {
            /* phase delta */
            double re = (double)r * (double)prev_r + (double)j * (double)prev_j;
            double im = (double)j * (double)prev_r - (double)r * (double)prev_j;
            double e = atan2(im, re); /* radians */
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

    float err_rad = (float)(err_acc / (double)count); /* radians */

    /* Integrator leakage: small exponential decay to avoid long-term drift.
     * Decay factor = 1 - 1/4096 ≈ 0.99976 per update. */
    const float kIntLeakFactor = 1.0f - (1.0f / 4096.0f);
    /* Frequency clamp in rad/sample: ~±0.8 rad/sample (generous for digital modes) */
    const float kFreqClamp = 0.8f;

    float i_base = state->integrator * kIntLeakFactor;
    i_base = clampf(i_base, -kFreqClamp, kFreqClamp);

    /* Deadband: ignore tiny phase errors to avoid audible low-frequency ramps.
       Keep leaked integrator so it slowly returns toward zero. */
    if (fabsf(err_rad) < config->deadband) {
        state->integrator = i_base;
        return;
    }

    /* True PI controller: u = Kp*e + I, where I accumulates beta*e */
    float p = alpha * err_rad;
    float i_term = beta * err_rad;

    /* Integrator update with anti-windup bound */
    float i_next = clampf(i_base + i_term, -kFreqClamp, kFreqClamp);

    /* Positional controller output */
    float u = p + i_next;

    /* Apply slew limit to change in frequency per update */
    float df = u - state->freq;
    df = clampf(df, -config->slew_max, config->slew_max);
    float f_new = clampf(state->freq + df, -kFreqClamp, kFreqClamp);

    state->freq = f_new;
    state->integrator = i_next;
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
fll_update_error_qpsk(const fll_config_t* config, fll_state_t* state, float* x, int N, int sps) {
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
    /* Incremental NCO to avoid per-sample sinf/cosf (mirror GNURadio control_loop). */
    std::complex<float> nco = std::exp(std::complex<float>(0.0f, phase));
    float last_r = 0.0f;
    float last_j = 0.0f;

    for (int n = 0; n < pairs; n++) {
        float ir = x[(size_t)(n << 1)];
        float iq = x[(size_t)(n << 1) + 1];
        /* Apply NCO rotation via incremental phasor */
        std::complex<float> z(ir, iq);
        std::complex<float> y = z * nco;
        x[(size_t)(n << 1)] = y.real();
        x[(size_t)(n << 1) + 1] = y.imag();
        last_r = y.real();
        last_j = y.imag();

        /* Push into ring buffer (with mirror to simplify dot product like GNU Radio). */
        buf_r[buf_idx] = y.real();
        buf_i[buf_idx] = y.imag();
        buf_r[buf_idx + taps_len] = y.real();
        buf_i[buf_idx + taps_len] = y.imag();
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
        /* Align sign with GNU Radio fll_band_edge_cc: error = lower - upper */
        float error = e_lower - e_upper;

        freq = freq + beta * error;
        phase = phase + freq + alpha * error;
        phase = fll_be_wrap_phase(phase);
        freq = clampf(freq, fmin, fmax);
        /* Advance incremental NCO (rebuild occasionally to bound drift) */
        std::complex<float> step = std::exp(std::complex<float>(0.0f, freq));
        nco *= step;
        if ((n & 63) == 0) {
            nco = std::exp(std::complex<float>(0.0f, phase));
        }
    }

    state->be_phase = phase;
    state->be_freq = freq;
    state->be_buf_idx = buf_idx;

    /* Clamp frequency to avoid runaway on noisy input (~0.5 rad/sample max) */
    const float kFreqClamp = 0.5f;
    state->freq = clampf(freq, -kFreqClamp, kFreqClamp);
    state->phase = fll_be_wrap_phase(phase);
    state->integrator = state->freq; /* mirror loop state for outer helpers */
    state->prev_r = last_r;
    state->prev_j = last_j;
}
