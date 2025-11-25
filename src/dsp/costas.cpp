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

void
cqpsk_costas_mix_and_update(struct demod_state* d) {
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

    /*
     * CQPSK Costas loop with OP25-style phase detection:
     *
     * At this point, iq[] contains differential phasors from cqpsk_diff_phasor():
     *   diffdec[n] = sample[n] * conj(sample[n-1])
     *
     * For CQPSK, symbols sit at ±45° and ±135°. OP25's phase_error_tracking()
     * rotates by PT_45 (45°) to align the constellation to I/Q axes before
     * running the standard QPSK phase detector.
     *
     * Key operations:
     * 1. Apply NCO rotation to correct residual CFO/phase offset
     * 2. Compute phase error from (rotated_diffdec * PT_45) for CQPSK alignment
     * 3. Update loop state (phase, frequency)
     * 4. Write NCO-corrected phasors back for downstream phase extraction
     *
     * Note on differential domain: A constant frequency offset ω in the raw
     * samples appears as a constant phase offset in the differential domain.
     * Thus, NCO rotation of differential phasors effectively corrects CFO.
     */
    for (int i = 0; i < pairs; i++) {
        std::complex<float> diffdec(iq[(i << 1) + 0], iq[(i << 1) + 1]);

        /* Apply NCO rotation to correct carrier offset */
        std::complex<float> nco = std::polar(1.0f, -c->phase);
        std::complex<float> corrected = diffdec * nco;

        /* Write corrected phasor back for downstream phase extraction */
        iq[(i << 1) + 0] = corrected.real();
        iq[(i << 1) + 1] = corrected.imag();

        /* OP25-style phase detection: rotate by PT_45 to align CQPSK to axes */
        std::complex<float> rotated = corrected * kPT45;

        /* Use OP25's phase detector which handles axis-aligned samples correctly */
        float err_raw = phase_detector_4_op25(rotated);
        float err_loop = branchless_clip(err_raw, 1.0f);

        /* Diagnostic: normalize the raw detector output (pre-clipping) by magnitude. */
        float mag_r = fabsf(rotated.real());
        float mag_i = fabsf(rotated.imag());
        float mag = (mag_i > mag_r) ? mag_i : mag_r;
        float err_diag_raw = err_raw;
        if (mag > 1.0f) {
            err_diag_raw /= mag;
        }
        float err_diag = branchless_clip(err_diag_raw, 1.0f);
        c->error = err_diag;
        err_acc += fabsf(err_diag);

        advance_loop(c, err_loop);
        phase_wrap(c);
        frequency_limit(c);
    }

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
