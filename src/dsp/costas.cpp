// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright 2006,2010-2012 Free Software Foundation, Inc.
 * Copyright 2010-2015 KA1RBI (OP25 gardner_costas_cc)
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * OP25-compatible Gardner + Costas combined block.
 *
 * This is a direct port of OP25's gardner_costas_cc_impl.cc signal flow.
 * The algorithm:
 *   1. Per input sample: advance NCO phase, rotate sample, push to delay line
 *   2. Per output symbol (when mu < 1):
 *      a. MMSE interpolate at symbol and mid-symbol points
 *      b. Compute Gardner timing error: (last - current) * mid
 *      c. Compute diffdec for Costas: interp_samp * conj(last_sample)
 *      d. Update Costas phase tracking: phase_error_tracking(diffdec * PT_45)
 *      e. Output the RAW NCO-corrected interpolated symbol (NOT diffdec)
 *   3. External diff_phasor_cc is applied after this block
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>

#include <cmath>
#include <complex>

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kPi = 3.14159265358979323846f;

/* OP25 PT_45: rotation by +45 degrees = exp(j*pi/4) */
constexpr float kPT45_r = 0.70710678118654752440f; /* cos(pi/4) */
constexpr float kPT45_j = 0.70710678118654752440f; /* sin(pi/4) */

/* MMSE interpolator parameters - match OP25/GNU Radio */
#define MMSE_NTAPS  8
#define MMSE_NSTEPS 16

/* GNU Radio MMSE 8-tap polyphase interpolator coefficients (subset at 1/16 steps) */
static const float mmse_taps[MMSE_NSTEPS + 1][MMSE_NTAPS] = {
    /* Row 0/128 (mu=0): output = sample[4] */
    {0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f},
    /* Row 8/128 (mu=0.0625) */
    {-1.23337e-03f, 6.84261e-03f, -2.24178e-02f, 6.57852e-02f, 9.83392e-01f, -4.04519e-02f, 9.56876e-03f,
     -1.54221e-03f},
    /* Row 16/128 (mu=0.125) */
    {-2.43121e-03f, 1.35716e-02f, -4.49929e-02f, 1.36968e-01f, 9.55956e-01f, -7.43154e-02f, 1.80759e-02f,
     -2.94361e-03f},
    /* Row 24/128 (mu=0.1875) */
    {-3.55283e-03f, 1.99599e-02f, -6.70018e-02f, 2.12443e-01f, 9.18329e-01f, -1.01501e-01f, 2.53295e-02f,
     -4.16581e-03f},
    /* Row 32/128 (mu=0.25) */
    {-4.55932e-03f, 2.57844e-02f, -8.77011e-02f, 2.91006e-01f, 8.71305e-01f, -1.22047e-01f, 3.11866e-02f,
     -5.17776e-03f},
    /* Row 40/128 (mu=0.3125) */
    {-5.41467e-03f, 3.08323e-02f, -1.06342e-01f, 3.71376e-01f, 8.15826e-01f, -1.36111e-01f, 3.55525e-02f,
     -5.95620e-03f},
    /* Row 48/128 (mu=0.375) */
    {-6.08674e-03f, 3.49066e-02f, -1.22185e-01f, 4.52218e-01f, 7.52958e-01f, -1.43968e-01f, 3.83800e-02f,
     -6.48585e-03f},
    /* Row 56/128 (mu=0.4375) */
    {-6.54823e-03f, 3.78315e-02f, -1.34515e-01f, 5.32164e-01f, 6.83875e-01f, -1.45993e-01f, 3.96678e-02f,
     -6.75943e-03f},
    /* Row 64/128 (mu=0.5): midpoint */
    {-6.77751e-03f, 3.94578e-02f, -1.42658e-01f, 6.09836e-01f, 6.09836e-01f, -1.42658e-01f, 3.94578e-02f,
     -6.77751e-03f},
    /* Row 72/128 (mu=0.5625) */
    {-6.73929e-03f, 3.95900e-02f, -1.46043e-01f, 6.92808e-01f, 5.22267e-01f, -1.33190e-01f, 3.75341e-02f,
     -6.50285e-03f},
    /* Row 80/128 (mu=0.625) */
    {-6.48585e-03f, 3.83800e-02f, -1.43968e-01f, 7.52958e-01f, 4.52218e-01f, -1.22185e-01f, 3.49066e-02f,
     -6.08674e-03f},
    /* Row 88/128 (mu=0.6875) */
    {-5.95620e-03f, 3.55525e-02f, -1.36111e-01f, 8.15826e-01f, 3.71376e-01f, -1.06342e-01f, 3.08323e-02f,
     -5.41467e-03f},
    /* Row 96/128 (mu=0.75) */
    {-5.17776e-03f, 3.11866e-02f, -1.22047e-01f, 8.71305e-01f, 2.91006e-01f, -8.77011e-02f, 2.57844e-02f,
     -4.55932e-03f},
    /* Row 104/128 (mu=0.8125) */
    {-4.16581e-03f, 2.53295e-02f, -1.01501e-01f, 9.18329e-01f, 2.12443e-01f, -6.70018e-02f, 1.99599e-02f,
     -3.55283e-03f},
    /* Row 112/128 (mu=0.875) */
    {-2.94361e-03f, 1.80759e-02f, -7.43154e-02f, 9.55956e-01f, 1.36968e-01f, -4.49929e-02f, 1.35716e-02f,
     -2.43121e-03f},
    /* Row 120/128 (mu=0.9375) */
    {-1.54221e-03f, 9.56876e-03f, -4.04519e-02f, 9.83392e-01f, 6.57852e-02f, -2.24178e-02f, 6.84261e-03f,
     -1.23337e-03f},
    /* Row 128/128 (mu=1.0): output = sample[3] */
    {0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 1.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f, 0.00000e+00f},
};

/* 8-tap MMSE FIR interpolation */
static inline float
mmse_interp_8tap(const float* s, float mu) {
    float idx_f = mu * (float)MMSE_NSTEPS;
    int idx_lo = (int)idx_f;
    float frac = idx_f - (float)idx_lo;

    if (idx_lo < 0) {
        idx_lo = 0;
        frac = 0.0f;
    }
    if (idx_lo >= MMSE_NSTEPS) {
        idx_lo = MMSE_NSTEPS - 1;
        frac = 1.0f;
    }
    int idx_hi = idx_lo + 1;

    const float* taps_lo = mmse_taps[idx_lo];
    const float* taps_hi = mmse_taps[idx_hi];
    float one_minus_frac = 1.0f - frac;

    float acc = 0.0f;
    for (int k = 0; k < MMSE_NTAPS; k++) {
        float tap = one_minus_frac * taps_lo[k] + frac * taps_hi[k];
        acc += tap * s[k];
    }
    return acc;
}

/* Complex MMSE interpolation */
static inline void
mmse_interp_cc(const float* dl, float mu, float* out_r, float* out_j) {
    float sr[MMSE_NTAPS];
    float sj[MMSE_NTAPS];

    for (int k = 0; k < MMSE_NTAPS; k++) {
        sr[k] = dl[k * 2];
        sj[k] = dl[k * 2 + 1];
    }

    *out_r = mmse_interp_8tap(sr, mu);
    *out_j = mmse_interp_8tap(sj, mu);
}

/* Branchless clip (matches GNU Radio) */
static inline float
branchless_clip(float x, float limit) {
    float a = x + limit;
    float b = x - limit;
    if (a < 0.0f) {
        a = 0.0f;
    }
    if (b > 0.0f) {
        b = 0.0f;
    }
    return 0.5f * (a + b);
}

/*
 * OP25 QPSK phase error detector.
 *
 * From gardner_costas_cc_impl.cc phase_error_detector_qpsk():
 *   if(fabsf(sample.real()) > fabsf(sample.imag())) {
 *     if(sample.real() > 0)
 *       phase_error = -sample.imag();
 *     else
 *       phase_error = sample.imag();
 *   } else {
 *     if(sample.imag() > 0)
 *       phase_error = sample.real();
 *     else
 *       phase_error = -sample.real();
 *   }
 *
 * This expects symbols rotated to axis-aligned positions (0°, 90°, 180°, 270°).
 */
static inline float
phase_error_detector_qpsk(float real, float imag) {
    float phase_error = 0.0f;
    if (fabsf(real) > fabsf(imag)) {
        if (real > 0.0f) {
            phase_error = -imag;
        } else {
            phase_error = imag;
        }
    } else {
        if (imag > 0.0f) {
            phase_error = real;
        } else {
            phase_error = -real;
        }
    }
    return phase_error;
}

/* NaN check helper */
#define IS_NAN(x) ((x) != (x))

} // namespace

/*
 * Reset Costas loop state for fresh carrier acquisition on retune.
 *
 * IMPORTANT: We preserve c->freq (carrier frequency estimate) because the
 * local oscillator offset is a property of the RTL-SDR hardware, not the
 * channel. When retuning from CC to VC on the same system, the carrier
 * offset should be similar. Preserving freq allows faster re-acquisition.
 *
 * We DO reset phase because the phase relationship changes with frequency.
 * We also reset error accumulator and initialized flag to trigger fresh
 * gain initialization.
 */
extern "C" void
dsd_costas_reset(dsd_costas_loop_state_t* c) {
    if (!c) {
        return;
    }
    c->phase = 0.0f;
    /* Preserve c->freq - oscillator offset is hardware property, similar across channels */
    c->error = 0.0f;
    c->initialized = 0;
}

/*
 * OP25-compatible Gardner + Costas combined block.
 *
 * This is a direct port of OP25's gardner_costas_cc_impl::general_work().
 *
 * Signal flow:
 *   Input: AGC'd complex samples at sample rate
 *   Processing per sample:
 *     1. Advance NCO: phase += freq
 *     2. Rotate input: sample = input * exp(j*(phase+theta))
 *     3. Push rotated sample to delay line
 *   Processing per symbol (when mu accumulates past 1.0):
 *     4. MMSE interpolate at mid-symbol and symbol points
 *     5. Compute Gardner error: (last - current) * mid
 *     6. Compute diffdec for Costas: interp * conj(last)
 *     7. Costas phase error tracking on diffdec * PT_45
 *     8. Update omega and mu
 *     9. Output RAW interpolated symbol (NOT diffdec)
 *   Output: Symbol-rate NCO-corrected samples (external diff_phasor applied later)
 */
extern "C" void
op25_gardner_costas_cc(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    if (!d->cqpsk_enable) {
        return;
    }

    dsd_costas_loop_state_t* c = &d->costas_state;
    ted_state_t* ted = &d->ted_state;

    const int buf_len = d->lp_len;
    const int nc = buf_len >> 1; /* input complex samples */
    if (nc < 4) {
        return;
    }

    /* Initialize Costas loop parameters if not already set.
     *
     * IMPORTANT: We do NOT reset phase/freq here! OP25's set_omega() preserves
     * d_phase and d_freq across frequency changes, allowing the Costas loop to
     * continue tracking without re-acquisition slew.
     *
     * The phase/freq are initialized to 0 only at startup (in rtl_demod_config.cpp
     * or via dsd_costas_reset). On retunes, they should be preserved.
     */
    if (!c->initialized) {
        /* OP25 defaults: alpha=0.04, beta=0.125*alpha^2=0.0002 */
        if (c->alpha <= 0.0f) {
            c->alpha = 0.04f;
        }
        if (c->beta <= 0.0f) {
            c->beta = 0.125f * c->alpha * c->alpha;
        }
        if (c->max_freq <= 0.0f) {
            /* OP25: fmax = 2*pi * 2400 / if_rate; for 24kHz: ~0.628 rad/sample */
            c->max_freq = kTwoPi * 2400.0f / 24000.0f;
        }
        c->min_freq = -c->max_freq;
        /* DO NOT reset c->phase or c->freq here - preserve carrier recovery state */
        c->initialized = 1;
    }

    /* Get TED parameters */
    const int sps = d->ted_sps > 0 ? d->ted_sps : 5;
    float omega = ted->omega;

    /*
     * Reinitialize TED state when SPS changes (e.g., P25P1 CC 5 sps -> P25P2 VC 4 sps).
     *
     * This mirrors OP25's set_omega() behavior which is called from configure_tdma()
     * when TDMA state changes. Without this, the delay line contains stale samples
     * from the previous symbol rate and the omega bounds are wrong, causing the
     * Gardner loop to malfunction on P25P2 voice channels.
     */
    int need_reinit = (ted->omega_mid == 0.0f || ted->twice_sps < 2);
    if (!need_reinit && ted->sps > 0 && ted->sps != sps) {
        need_reinit = 1;
    }

    if (need_reinit) {
        /* Debug: log TED SPS change when DSD_NEO_DEBUG_CQPSK=1 */
        {
            static int debug_init = 0;
            static int debug_cqpsk = 0;
            if (!debug_init) {
                const char* env = getenv("DSD_NEO_DEBUG_CQPSK");
                debug_cqpsk = (env && *env == '1') ? 1 : 0;
                debug_init = 1;
            }
            if (debug_cqpsk) {
                /* Note: freq is preserved from previous channel to speed acquisition */
                float freq_hz = c->freq * (24000.0f / kTwoPi);
                fprintf(stderr,
                        "[COSTAS] TED reinit: sps=%d->%d old_omega=%.3f costas_phase=%.3f freq=%.1fHz (preserved)\n",
                        ted->sps, sps, ted->omega, c->phase, freq_hz);
            }
        }
        omega = (float)sps;
        ted->omega = omega;
        float omega_rel = 0.005f; /* OP25 default */
        ted->omega_mid = omega;
        ted->omega_min = omega * (1.0f - omega_rel);
        ted->omega_max = omega * (1.0f + omega_rel);

        int twice_sps_op25 = 2 * (int)ceilf(ted->omega_max);
        int twice_sps_mmse = (int)ceilf(ted->omega_max / 2.0f) + MMSE_NTAPS + 1;
        int twice_sps_required = (twice_sps_op25 > twice_sps_mmse) ? twice_sps_op25 : twice_sps_mmse;

        if (twice_sps_required > TED_DL_SIZE) {
            d->lp_len = 0;
            return;
        }
        ted->twice_sps = twice_sps_required;
        ted->dl_index = 0;
        ted->sps = sps;

        /* Clear delay line (OP25's set_omega does memset(d_dl, 0, ...)) */
        for (int i = 0; i < TED_DL_SIZE * 2 * 2; i++) {
            ted->dl[i] = 0.0f;
        }

        /* Reset TED timing state on SPS change.
         *
         * Unlike OP25's continuous sample flow where set_omega() preserves all state,
         * dsd-neo's SPS change happens during a retune with sample gap. The old timing
         * state is from a different symbol rate:
         *   - mu: fractional phase scaled to old SPS, not valid for new SPS
         *   - last_r/j: last symbol from old rate, will corrupt first Gardner error
         *
         * Reset to clean state for fresh acquisition on new symbol rate.
         * Costas phase/freq are preserved - carrier offset is similar between CC and VC.
         *
         * IMPORTANT: Reset last_r/j to (1,0) NOT (0,0). With (0,0):
         *   - First Gardner error = (0-sym)*mid = -sym*mid (large spurious error)
         *   - First diffdec = sym*conj(0) = 0 (no Costas phase correction!)
         * With (1,0):
         *   - First Gardner error = (1-sym)*mid (bounded, reasonable)
         *   - First diffdec = sym*conj(1) = sym (passthrough, like external diff_prev)
         * This matches the external cqpsk_diff_prev reset behavior.
         *
         * CRITICAL: Initialize mu to a value > twice_sps so the inner sample-consumption
         * loop runs first, filling the delay line with valid samples before any symbol
         * output. Without this, the first symbols are interpolated from the zeroed delay
         * line, producing garbage that corrupts the Costas loop.
         *
         * In OP25's continuous flow, the delay line is always populated with recent samples.
         * But dsd-neo has discrete retune events with sample gaps - we must explicitly
         * pre-fill the delay line before interpolating.
         */
        /* OP25: Does NOT touch d_mu or d_last_sample in set_omega().
         * We preserve mu (timing phase) and don't reset last_sample.
         * This matches OP25's continuous sample flow behavior. */
    }

    /* OP25 gains: gain_mu=0.025, gain_omega=0.1*gain_mu^2 */
    float gain_mu = d->ted_gain > 0.0f ? d->ted_gain : 0.025f;
    float gain_omega = 0.1f * gain_mu * gain_mu;

    /* Costas theta: OP25 uses M_PI/4.0 (45 degrees) */
    const float theta = kPi / 4.0f;

    float* iq_in = d->lowpassed;
    float* iq_out = d->timing_buf;

    float phase = c->phase;
    float freq = c->freq;
    float mu = ted->mu;
    int dl_index = ted->dl_index;
    int twice_sps = ted->twice_sps;
    float* dl = ted->dl;

    /* Last sample for Gardner and diffdec (OP25: d_last_sample) */
    float last_r = ted->last_r;
    float last_j = ted->last_j;

    int i = 0; /* input index */
    int o = 0; /* output index (interleaved floats) */

    /*
     * Main loop: OP25's general_work() structure
     * Note: OP25 has NO mute logic - samples are processed immediately.
     */
    while (o < buf_len && i < nc) {
        /*
         * Inner loop: consume samples while mu > 1.0, filling delay line
         * This is the per-sample NCO rotation from OP25 lines 428-464
         */
        while (mu > 1.0f && i < nc) {
            mu -= 1.0f;

            /* OP25: d_phase += d_freq */
            phase += freq;

            /* Keep phase in [-pi, pi] */
            while (phase > kPi) {
                phase -= kTwoPi;
            }
            while (phase < -kPi) {
                phase += kTwoPi;
            }

            /* OP25: nco = gr_expj(d_phase + d_theta) */
            float nco_angle = phase + theta;
            float nco_r = cosf(nco_angle);
            float nco_j = sinf(nco_angle);

            /* Get input sample */
            float in_r = iq_in[i * 2];
            float in_j = iq_in[i * 2 + 1];
            if (IS_NAN(in_r)) {
                in_r = 0.0f;
            }
            if (IS_NAN(in_j)) {
                in_j = 0.0f;
            }

            /* OP25: sample = nco * symbol (complex multiply) */
            float sample_r = nco_r * in_r - nco_j * in_j;
            float sample_j = nco_r * in_j + nco_j * in_r;

            /* OP25: push to delay line at both dl_index and dl_index + twice_sps */
            dl[dl_index * 2] = sample_r;
            dl[dl_index * 2 + 1] = sample_j;
            dl[(dl_index + twice_sps) * 2] = sample_r;
            dl[(dl_index + twice_sps) * 2 + 1] = sample_j;

            dl_index++;
            if (dl_index >= twice_sps) {
                dl_index = 0;
            }

            i++;
        }

        if (i >= nc) {
            break;
        }

        /*
         * Symbol output: OP25 lines 466-517
         * Interpolate, compute Gardner error, Costas tracking, output
         */

        /* Compute half-omega parameters (OP25 style) */
        float half_omega = omega / 2.0f;
        int half_sps = (int)floorf(half_omega);
        float half_mu = mu + half_omega - (float)half_sps;
        if (half_mu > 1.0f) {
            half_mu -= 1.0f;
            half_sps += 1;
        }
        if (half_sps < 0) {
            half_sps = 0;
        }

        /* Bounds check for MMSE reads */
        int max_mid_idx = dl_index + MMSE_NTAPS - 1;
        int max_sym_idx = dl_index + half_sps + MMSE_NTAPS - 1;
        if (max_mid_idx >= 2 * twice_sps || max_sym_idx >= 2 * twice_sps) {
            mu += omega;
            continue;
        }

        /* OP25: interp_samp_mid at dl_index (mid-symbol) */
        float mid_r, mid_j;
        mmse_interp_cc(&dl[dl_index * 2], mu, &mid_r, &mid_j);

        /* OP25: interp_samp at dl_index + half_sps (symbol point) */
        float sym_r, sym_j;
        mmse_interp_cc(&dl[(dl_index + half_sps) * 2], half_mu, &sym_r, &sym_j);

        /* OP25 Gardner error: (last - current) * mid
         * Note: OP25 has NO mute logic - always process samples. */
        float error_real = (last_r - sym_r) * mid_r;
        float error_imag = (last_j - sym_j) * mid_j;
        float symbol_error = error_real + error_imag;

        if (IS_NAN(symbol_error)) {
            symbol_error = 0.0f;
        }
        if (symbol_error < -1.0f) {
            symbol_error = -1.0f;
        }
        if (symbol_error > 1.0f) {
            symbol_error = 1.0f;
        }

        /* OP25 omega update: d_omega += d_gain_omega * symbol_error * abs(interp_samp) */
        float sym_mag = sqrtf(sym_r * sym_r + sym_j * sym_j);
        omega = omega + gain_omega * symbol_error * sym_mag;

        /* Clip omega to valid range */
        omega = ted->omega_mid + branchless_clip(omega - ted->omega_mid, ted->omega_max - ted->omega_mid);

        /*
         * OP25: diffdec = interp_samp * conj(d_last_sample)
         * This is used for Costas phase error (internal only, not output)
         */
        float diffdec_r = sym_r * last_r + sym_j * last_j;
        float diffdec_j = sym_j * last_r - sym_r * last_j;

        /* Save current symbol as last for next iteration */
        last_r = sym_r;
        last_j = sym_j;

        /* OP25 mu update: d_mu += d_omega + d_gain_mu * symbol_error */
        mu += omega + gain_mu * symbol_error;

        /*
         * OP25 Costas phase error tracking: phase_error_tracking(diffdec * PT_45)
         *
         * PT_45 rotates by +45 degrees to move diagonal constellation to axes.
         * diffdec * PT_45 = (diffdec_r + j*diffdec_j) * (kPT45_r + j*kPT45_j)
         */
        float rotated_r = diffdec_r * kPT45_r - diffdec_j * kPT45_j;
        float rotated_j = diffdec_r * kPT45_j + diffdec_j * kPT45_r;

        /* OP25 phase error detector for QPSK (axis-aligned after PT_45 rotation) */
        float phase_error = phase_error_detector_qpsk(rotated_r, rotated_j);

        /* OP25: d_freq += d_beta * phase_error * abs(sample)
         *       d_phase += d_alpha * phase_error * abs(sample)
         * Note: OP25 uses magnitude weighting */
        float rotated_mag = sqrtf(rotated_r * rotated_r + rotated_j * rotated_j);
        freq += c->beta * phase_error * rotated_mag;
        phase += c->alpha * phase_error * rotated_mag;

        /* Keep phase in [-pi, pi] */
        while (phase > kPi) {
            phase -= kTwoPi;
        }
        while (phase < -kPi) {
            phase += kTwoPi;
        }

        /* Limit frequency */
        freq = branchless_clip(freq, c->max_freq);

        /* Store error for diagnostics */
        c->error = phase_error;

        /* Output interpolated sample (OP25 has no mute - always output) */
        iq_out[o++] = sym_r;
        iq_out[o++] = sym_j;
    }

    /* Save state */
    c->phase = phase;
    c->freq = freq;
    ted->mu = mu;
    ted->omega = omega;
    ted->dl_index = dl_index;
    ted->last_r = last_r;
    ted->last_j = last_j;

    /* Copy output back to lowpassed buffer and update length */
    if (o >= 2) {
        for (int j = 0; j < o; j++) {
            d->lowpassed[j] = iq_out[j];
        }
        d->lp_len = o;
    } else {
        d->lp_len = 0;
    }

    /* Update diagnostic */
    d->fll_freq = freq;
    d->fll_phase = phase;
}

/*
 * External differential phasor (matches GNU Radio digital.diff_phasor_cc).
 *
 * y[n] = x[n] * conj(x[n-1])
 *
 * This is applied AFTER op25_gardner_costas_cc to produce the final
 * differential output for phase extraction.
 */
extern "C" void
op25_diff_phasor_cc(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2) {
        return;
    }

    const int pairs = d->lp_len >> 1;
    float* iq = d->lowpassed;

    float prev_r = d->cqpsk_diff_prev_r;
    float prev_j = d->cqpsk_diff_prev_j;

    for (int n = 0; n < pairs; n++) {
        float cur_r = iq[n * 2];
        float cur_j = iq[n * 2 + 1];

        /* y = x * conj(prev) = (cur_r + j*cur_j) * (prev_r - j*prev_j) */
        float out_r = cur_r * prev_r + cur_j * prev_j;
        float out_j = cur_j * prev_r - cur_r * prev_j;

        iq[n * 2] = out_r;
        iq[n * 2 + 1] = out_j;

        prev_r = cur_r;
        prev_j = cur_j;
    }

    d->cqpsk_diff_prev_r = prev_r;
    d->cqpsk_diff_prev_j = prev_j;
}

/*
 * Legacy function - redirect to new implementation.
 * Kept for API compatibility during transition.
 */
extern "C" void
cqpsk_costas_diff_and_update(struct demod_state* d) {
    if (!d || !d->cqpsk_enable) {
        return;
    }
    /* Call the OP25-aligned combined block, then external diff_phasor */
    op25_gardner_costas_cc(d);
    op25_diff_phasor_cc(d);
}
