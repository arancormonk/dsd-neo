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
constexpr float kPiOver4 = 0.78539816339744830962f;

/* OP25 PT_45: 45-degree rotation phasor for CQPSK constellation alignment.
 * CQPSK symbols sit at ±45° and ±135°; rotating by 45° aligns them to axes. */
static const std::complex<float> kPT45 = std::polar(1.0f, kPiOver4);

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

/*
 * OP25-style QPSK phase detector for CQPSK after PT_45 rotation.
 *
 * Unlike the GNU Radio phase_detector_4, this version selects either ±imag
 * or ±real based on which axis the sample is closer to. This gives near-zero
 * error for samples on the I/Q axes (where CQPSK symbols land after PT_45).
 */
static inline float
phase_detector_4_op25(const std::complex<float>& sample) {
    if (fabsf(sample.real()) > fabsf(sample.imag())) {
        return (sample.real() > 0.0f) ? -sample.imag() : sample.imag();
    } else {
        return (sample.imag() > 0.0f) ? sample.real() : -sample.real();
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
 * Combined Costas NCO + differential decode + loop update with per-sample feedback.
 *
 * Signal flow per sample:
 *   raw[n] -> NCO rotation -> diff decode with prev -> phase error -> loop update
 *                                                                  -> output diff[n]
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

    /* Previous NCO-corrected sample for differential decoding (persistent across blocks) */
    float prev_r = d->cqpsk_diff_prev_r;
    float prev_j = d->cqpsk_diff_prev_j;

    for (int i = 0; i < pairs; i++) {
        std::complex<float> raw(iq[(i << 1) + 0], iq[(i << 1) + 1]);

        /*
         * Step 1: NCO rotation on raw sample.
         *
         * Match OP25 exactly: nco = gr_expj(d_phase + d_theta)
         * We use exp(+j*phase) to match their sign convention.
         * Note: The d_theta (π/4) cancels in differential decoding, but we
         * include it here to match OP25's structure exactly.
         */
        std::complex<float> nco = std::polar(1.0f, c->phase);
        std::complex<float> corrected = raw * nco;

        /*
         * Step 2: Differential decode.
         *
         * diffdec = corrected * conj(prev) = corrected * (prev_r - j*prev_j)
         */
        float corr_r = corrected.real();
        float corr_j = corrected.imag();

        float diff_r = corr_r * prev_r + corr_j * prev_j;
        float diff_j = corr_j * prev_r - corr_r * prev_j;

        std::complex<float> diffdec(diff_r, diff_j);

        /* Save current corrected sample as prev for next iteration */
        prev_r = corr_r;
        prev_j = corr_j;

        /*
         * Step 3: Phase error detection.
         *
         * The θ rotation applied to raw samples cancels out in differential
         * decoding (since both samples are rotated by the same θ). CQPSK
         * symbols are at ±45°/±135°, not axis-aligned.
         *
         * OP25's phase_error_tracking() (line 510) multiplies by PT_45 after
         * differential decoding to align the constellation to I/Q axes:
         *   phase_error_tracking(diffdec * PT_45)
         *
         * Apply the same +45° rotation before the phase detector.
         */
        std::complex<float> rotated = diffdec * kPT45;
        float err_raw = phase_detector_4_op25(rotated);

        /*
         * Step 4: Loop update.
         *
         * Match OP25 exactly (lines 389-390):
         *   d_freq += d_beta * phase_error * abs(sample);
         *   d_phase += d_alpha * phase_error * abs(sample);
         *
         * No negation - use error directly as OP25 does.
         */
        float mag = std::abs(diffdec);
        float err_scaled = err_raw * mag; /* Match OP25: no negation */
        float err_loop = branchless_clip(err_scaled, 1.0f);

        /* Diagnostic */
        float err_diag = branchless_clip(err_raw, 1.0f);
        c->error = err_diag;
        err_acc += fabsf(err_diag);

        advance_loop(c, err_loop);
        phase_wrap(c);
        frequency_limit(c);

        /* Write differential output back (replaces raw sample) */
        iq[(i << 1) + 0] = diff_r;
        iq[(i << 1) + 1] = diff_j;
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
