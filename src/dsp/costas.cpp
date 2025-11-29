// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright 2006,2010-2012 Free Software Foundation, Inc.
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Costas loop carrier recovery (GNU Radio port).
 *
 * Port of GNU Radio's costas_loop_cc implementation. The default loop bandwidth
 * is reduced (2*pi/800 rad/sample) to match the narrower DSP bandwidth run by
 * dsd-neo compared to typical GNU Radio flows.
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>

#include <cmath>
#include <complex>

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

/*
 * Standard GNU Radio QPSK phase detector for diagonal constellation symbols.
 *
 * CQPSK differential outputs sit at ±45° and ±135° (diagonals).
 * This detector produces zero error when symbols are on the diagonals,
 * and proportional error when they deviate.
 *
 * This is the standard GNU Radio phase_detector_4 formula from costas_loop_cc_impl.cc:
 *   return ((sample.real() > 0 ? 1.0 : -1.0) * sample.imag() -
 *           (sample.imag() > 0 ? 1.0 : -1.0) * sample.real());
 *
 * For a symbol at 45°+ε, this produces error ∝ +ε (positive error for positive offset).
 */
static inline float
phase_detector_4(float real, float imag) {
    float sign_r = (real > 0.0f) ? 1.0f : -1.0f;
    float sign_i = (imag > 0.0f) ? 1.0f : -1.0f;
    return sign_r * imag - sign_i * real;
}

static inline float
branchless_clip(float x, float clip) {
    return 0.5f * (std::abs(x + clip) - std::abs(x - clip));
}

static void
update_gains(dsd_costas_loop_state_t* c) {
    float bw = c->loop_bw;
    if (bw < 0.0f) {
        bw = 0.0f;
    }
    const float denom = 1.0f + 2.0f * c->damping * bw + bw * bw;
    c->alpha = (4.0f * c->damping * bw) / denom;
    c->beta = (4.0f * bw * bw) / denom;
}

static inline void
advance_loop(dsd_costas_loop_state_t* c, float err) {
    c->freq += c->beta * err;
    c->phase += c->freq + c->alpha * err;
}

static inline void
phase_wrap(dsd_costas_loop_state_t* c) {
    while (c->phase > kTwoPi) {
        c->phase -= kTwoPi;
    }
    while (c->phase < -kTwoPi) {
        c->phase += kTwoPi;
    }
}

static inline void
frequency_limit(dsd_costas_loop_state_t* c) {
    if (c->freq > c->max_freq) {
        c->freq = c->max_freq;
    } else if (c->freq < c->min_freq) {
        c->freq = c->min_freq;
    }
}

static void
prepare_costas(dsd_costas_loop_state_t* c, const demod_state* d) {
    if (c->loop_bw <= 0.0f) {
        c->loop_bw = dsd_neo_costas_default_loop_bw();
    }
    if (c->damping <= 0.0f) {
        c->damping = dsd_neo_costas_default_damping();
    }
    if (c->max_freq == 0.0f && c->min_freq == 0.0f) {
        c->max_freq = 1.0f;
        c->min_freq = -1.0f;
    }
    if (c->max_freq < c->min_freq) {
        float tmp = c->max_freq;
        c->max_freq = c->min_freq;
        c->min_freq = tmp;
    }
    update_gains(c);

    if (!c->initialized && d) {
        if (d->cqpsk_fll_rot_applied) {
            /* FLL already rotated the block; start Costas from zero to avoid double rotation. */
            c->phase = 0.0f;
            c->freq = 0.0f;
        } else {
            c->phase = d->fll_phase;
            c->freq = d->fll_freq;
        }
        c->initialized = 1;
    }
}

} // namespace

/*
 * OP25-style CQPSK Costas loop with per-sample feedback.
 *
 * OP25's gardner_costas_cc processes each sample through a tight loop:
 *   1. NCO rotation on raw sample
 *   2. Differential decode with previous sample
 *   3. Phase error detection on diff output
 *   4. Loop state update (phase, freq)
 *   5. Next sample sees updated NCO
 *
 * This per-sample feedback is essential for the PLL to track carrier offsets.
 * Splitting into separate block-wise passes breaks the feedback loop.
 */

/*
 * OP25-compatible CQPSK demodulation: differential decode + Costas loop.
 *
 * OP25's p25_demodulator.py CQPSK chain (line 504):
 *   agc -> fll -> clock (gardner_cc) -> diffdec -> costas -> to_float -> rescale
 *
 * Key insight: In OP25, diff_phasor_cc comes BEFORE costas_loop_cc.
 * The Costas loop operates on the already-differentiated signal.
 *
 * Signal flow per sample:
 *   1. diff_phasor: out[n] = in[n] * conj(in[n-1])  -- removes data modulation
 *   2. costas: rotated = diff * exp(-j*phase)       -- carrier phase correction
 *   3. phase_detector_4 on rotated signal           -- error detection
 *   4. loop update                                  -- track phase
 */
void
cqpsk_costas_diff_and_update(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    if (!d->cqpsk_enable) {
        return;
    }

    dsd_costas_loop_state_t* c = &d->costas_state;
    prepare_costas(c, d);

    int pairs = d->lp_len >> 1;
    float* iq = d->lowpassed;
    float err_acc = 0.0f;

    /* Previous raw sample for differential decoding (persistent across blocks) */
    float prev_r = d->cqpsk_diff_prev_r;
    float prev_j = d->cqpsk_diff_prev_j;

    for (int i = 0; i < pairs; i++) {
        float raw_r = iq[(i << 1) + 0];
        float raw_j = iq[(i << 1) + 1];

        /*
         * Step 1: Differential decode FIRST (like OP25's diff_phasor_cc).
         *
         * diff = raw * conj(prev) = raw * (prev_r - j*prev_j)
         * This removes the data modulation, leaving only carrier phase error.
         */
        float diff_r = raw_r * prev_r + raw_j * prev_j;
        float diff_j = raw_j * prev_r - raw_r * prev_j;

        /* Save current raw sample as prev for next iteration */
        prev_r = raw_r;
        prev_j = raw_j;

        /*
         * Step 2: Costas NCO rotation (like OP25's costas_loop_cc).
         *
         * OP25 uses: optr[i] = iptr[i] * gr_expj(-d_phase)
         * Note the NEGATIVE phase - this is critical!
         */
        float cos_phase = cosf(-c->phase);
        float sin_phase = sinf(-c->phase);
        float rotated_r = diff_r * cos_phase - diff_j * sin_phase;
        float rotated_j = diff_r * sin_phase + diff_j * cos_phase;

        /*
         * Step 3: Phase error detection (standard GNU Radio detector).
         *
         * CQPSK differential outputs sit at ±45° and ±135° (diagonals).
         * The standard phase_detector_4 produces zero error when symbols are
         * on the diagonals, which is exactly what we need.
         *
         * NOTE: We do NOT apply PT_45 rotation here. The downstream slicer
         * (qpsk_differential_demod) expects diagonal symbols for its 4/π
         * scaling to produce the correct {-3, -1, +1, +3} symbol levels.
         */
        float err_raw = phase_detector_4(rotated_r, rotated_j);

        /*
         * Step 4: Magnitude-weighted loop update (matches OP25 exactly).
         *
         * OP25 costas_loop_cc (lines 389-390):
         *   d_freq += d_beta * phase_error * abs(sample);
         *   d_phase += d_alpha * phase_error * abs(sample);
         *
         * The magnitude weighting is critical: it de-weights samples with low
         * amplitude (noise, fades, limiter hits) so they don't drive the loop,
         * and properly scales the effective loop gain relative to signal energy.
         * Without this, the PLL random-walks during signal dropouts.
         */
        float mag = sqrtf(rotated_r * rotated_r + rotated_j * rotated_j);
        float err_scaled = err_raw * mag;
        float err_loop = branchless_clip(err_scaled, 1.0f);

        /* Diagnostic: use unscaled error for display */
        float err_diag = branchless_clip(err_raw, 1.0f);
        c->error = err_diag;
        err_acc += fabsf(err_diag);

        advance_loop(c, err_loop);
        phase_wrap(c);
        frequency_limit(c);

        /* Write the carrier-corrected differential output back (diagonal symbols
         * at ±45°/±135° for downstream qpsk_differential_demod) */
        iq[(i << 1) + 0] = rotated_r;
        iq[(i << 1) + 1] = rotated_j;
    }

    /* Persist state for next block */
    d->cqpsk_diff_prev_r = prev_r;
    d->cqpsk_diff_prev_j = prev_j;
    d->fll_freq = c->freq;
    d->fll_phase = c->phase;

    if (pairs > 0) {
        float avg_err = err_acc / static_cast<float>(pairs);
        int avg_q14 = static_cast<int>(lrintf(avg_err * 16384.0f));
        if (avg_q14 < 0) {
            avg_q14 = 0;
        }
        if (avg_q14 > 32767) {
            avg_q14 = 32767;
        }
        d->costas_err_avg_q14 = avg_q14;
    } else {
        d->costas_err_avg_q14 = 0;
    }
}
