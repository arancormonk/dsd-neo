// SPDX-License-Identifier: GPL-2.0-or-later
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
#include <math.h>
#include <stdlib.h>

/* Clamp helper */
static inline int
clamp_i(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
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
 * @brief Fast atan2 approximation for 64-bit inputs.
 *
 * Uses a piecewise linear approximation stable across quadrants.
 *
 * @param y Imaginary component.
 * @param x Real component.
 * @return Approximate angle in Q14 where pi = 1<<14.
 */
static int
fast_atan2_64(int64_t y, int64_t x) {
    int angle;
    int pi4 = (1 << 12), pi34 = 3 * (1 << 12); /* note: pi = 1<<14 */
    int64_t yabs;
    if (x == 0 && y == 0) {
        return 0;
    }
    yabs = (y < 0) ? -y : y;

    if (x >= 0) {
        /* Use stable form: pi/4 - pi/4 * (x - |y|) / (x + |y|) */
        int64_t denom = x + yabs; /* only zero when x==0 && y==0 handled above */
        if (denom == 0) {
            angle = 0;
        } else {
            angle = (int)(pi4 - ((int64_t)pi4 * (x - yabs)) / denom);
        }
    } else {
        /* Use stable form: 3pi/4 - pi/4 * (x + |y|) / (|y| - x) */
        int64_t denom = yabs - x; /* strictly > 0 for x < 0 */
        if (denom == 0) {
            angle = pi34; /* rare tie case; pick quadrant boundary */
        } else {
            angle = (int)(pi34 - ((int64_t)pi4 * (x + yabs)) / denom);
        }
    }
    if (y < 0) {
        return -angle;
    }
    return angle;
}

/**
 * @brief Polar discriminator using fast atan2 approximation.
 *
 * Computes the phase difference between two complex samples.
 *
 * @param ar Real of current sample.
 * @param aj Imag of current sample.
 * @param br Real of previous sample.
 * @param bj Imag of previous sample.
 * @return Phase error in Q14 (pi = 1<<14).
 */
static int
polar_disc_fast(int ar, int aj, int br, int bj) {
    int64_t cr, cj;
    multiply64(ar, aj, br, -bj, &cr, &cj);
    return fast_atan2_64(cj, cr);
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
            int e = polar_disc_fast(r, j, prev_r, prev_j); /* Q14 */
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
 * @brief QPSK-oriented FLL update using symbol-spaced phase differences.
 *
 * Estimates CFO by averaging the angle of s[k] * conj(s[k - sps]) across the
 * block, where sps is the nominal samples-per-symbol in complex samples. This
 * reduces modulation-induced phase noise compared to adjacent-sample methods
 * when operating on QPSK/CQPSK signals.
 *
 * @param config FLL configuration (gains, deadband, slew limit).
 * @param state  FLL state (updates freq_q15; phase untouched here).
 * @param x      Input interleaved I/Q buffer.
 * @param N      Length of the buffer in elements (must be even).
 * @param sps    Samples-per-symbol (complex samples per symbol). If < 2, this
 *               function falls back to adjacent-sample update semantics.
 */
void
fll_update_error_qpsk(const fll_config_t* config, fll_state_t* state, const int16_t* x, int N, int sps) {
    if (!config->enabled) {
        return;
    }
    if (N < 4) {
        return;
    }
    if (sps < 0) {
        sps = 0;
    }
    if (sps > 64) {
        sps = 64;
    }
    /* Convert sps in complex samples to element stride in the interleaved array */
    int stride_elems = (sps >= 2) ? (sps << 1) : 2; /* >= 4 elements, else fallback to 2 */

    int64_t err_acc = 0; /* wide accumulator to prevent overflow at high SPS/block sizes */
    int count = 0;
    const int pairs = N >> 1;

    /* Use trailing history from the previous block to form the first few symbol-spaced
       errors so the estimator spans block boundaries. History length is clamped to
       min(sps,64) at the end of each call. */
    int hist_len = state->prev_hist_len;
    if (sps >= 2 && hist_len == sps) {
        int limit = (pairs < sps) ? pairs : sps;
        for (int n = 0; n < limit; n++) {
            int r = x[(size_t)(n << 1)];
            int j = x[(size_t)(n << 1) + 1];
            int br = state->prev_hist_r[n];
            int bj = state->prev_hist_j[n];
            int e = polar_disc_fast(r, j, br, bj); /* Q14 */
            err_acc += e;
            count++;
        }
    }

    /* Start at the first index that has a valid s[k - sps] */
    for (int i = stride_elems; i + 1 < N; i += 2) {
        int r = x[i];
        int j = x[i + 1];
        int br = x[i - stride_elems];
        int bj = x[i - stride_elems + 1];
        int e = polar_disc_fast(r, j, br, bj); /* Q14 */
        err_acc += e;
        count++;
    }

    if (count == 0) {
        return;
    }

    int32_t err = (int32_t)(err_acc / count); /* Q14 */

    /* Pre-apply small integrator leakage each update (even in deadband).
     * See comment in fll_update_error for rationale and scale.
     */
    const int32_t F_CLAMP = 4096;
    int32_t i_base = state->int_q15 - (state->int_q15 >> kFllIntLeakShift);
    i_base = clamp_i(i_base, -F_CLAMP, F_CLAMP);

    /* Deadband to avoid audible low-frequency sweeps or chattering.
       Keep leaked integrator so it slowly returns toward zero. Also refresh
       history even when we skip the PI update to preserve cross-block
       continuity. */
    if (err < config->deadband_q14 && err > -config->deadband_q14) {
        state->int_q15 = (int)i_base;
    } else {
        /* True PI (symbol-spaced detector): u = Kp*e + I; slew limit delta(u) */
        int32_t p = ((int64_t)config->alpha_q15 * err) >> 14;     /* -> Q15 */
        int32_t i_term = ((int64_t)config->beta_q15 * err) >> 14; /* -> Q15 */

        int32_t i_next = i_base + i_term;
        i_next = clamp_i(i_next, -F_CLAMP, F_CLAMP);

        int32_t u = p + i_next;           /* Q15 */
        int32_t df = u - state->freq_q15; /* desired delta */
        df = clamp_i(df, -config->slew_max_q15, config->slew_max_q15);
        int32_t f_new = state->freq_q15 + df;
        f_new = clamp_i(f_new, -F_CLAMP, F_CLAMP);

        state->freq_q15 = (int)f_new;
        state->int_q15 = (int)i_next;
    }

    /* Capture trailing samples for cross-block continuity (store oldest->newest). */
    int store = sps;
    if (store > pairs) {
        store = pairs;
    }
    if (store > 64) {
        store = 64;
    }
    if (store > 0) {
        int start_pair = pairs - store;
        for (int n = 0; n < store; n++) {
            int idx = start_pair + n;
            state->prev_hist_r[n] = x[(size_t)(idx << 1)];
            state->prev_hist_j[n] = x[(size_t)(idx << 1) + 1];
        }
    }
    state->prev_hist_len = store;
    /* Preserve last sample for adjacent-sample fallback callers. */
    if (pairs > 0) {
        state->prev_r = x[(size_t)((pairs - 1) << 1)];
        state->prev_j = x[(size_t)(((pairs - 1) << 1) + 1)];
    }
}
