// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The band-pass bank both noise squelches run on discriminator output (src/dsp/nfm_noise_squelch.c for radio input,
 * src/dsp/pcm_noise_squelch.c for audio input): Butterworth band-passes of prototype order 4 as four biquads, and the
 * quieting a window's band-pass powers read against a reference, Q = max(Q_sum, Q_max - guard).
 */

#ifndef DSD_NEO_SRC_DSP_NOISE_SQUELCH_BANK_H_
#define DSD_NEO_SRC_DSP_NOISE_SQUELCH_BANK_H_

#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A power at or below this holds no signal; Q never reads beyond +-DSD_NOISE_SQUELCH_BANK_Q_CAP_DB. */
#define DSD_NOISE_SQUELCH_BANK_MIN_POWER 1e-30
#define DSD_NOISE_SQUELCH_BANK_Q_CAP_DB  200.0

/* The DSD_NOISE_SQUELCH_SECTIONS biquads of a Butterworth band-pass of prototype order 4 over [f1, f2] at fs, each with
   unit gain at the band's centre, into out. */
void dsd_noise_squelch_bank_design_band_pass(double f1, double f2, double fs, dsd_noise_squelch_biquad* out);

/* 10 log10(ref / power), held to +-DSD_NOISE_SQUELCH_BANK_Q_CAP_DB; the cap when power holds no signal. */
double dsd_noise_squelch_bank_ratio_db(double ref, double power);

/* A window's quieting from its band-pass powers p against the references ref: Q_sum from the first sub_bands (their
   references summing to ref_sum) and Q_max the best of all bands, Q = max(Q_sum, Q_max - guard_db). With part non-NULL
   only the band-passes it marks take part (ref_sum then sums the marked sub-bands' references); 0 when no sub-band
   does. */
double dsd_noise_squelch_bank_quieting_db(const double* ref, double ref_sum, const double* p, const unsigned char* part,
                                          int sub_bands, int bands, double guard_db);

/* One input through one biquad (transposed direct form II). */
static inline double
dsd_noise_squelch_bank_biquad(const dsd_noise_squelch_biquad* q, double* s1, double* s2, double x) {
    const double y = (q->gain * x) + *s1;
    *s1 = *s2 - (q->a1 * y);
    *s2 = (-q->gain * x) - (q->a2 * y);
    return y;
}

/* One input through a band-pass's DSD_NOISE_SQUELCH_SECTIONS biquads, s1 and s2 their states. */
static inline double
dsd_noise_squelch_bank_run(const dsd_noise_squelch_biquad* sections, double* s1, double* s2, double x) {
    double y = x;
    for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
        y = dsd_noise_squelch_bank_biquad(&sections[s], &s1[s], &s2[s], y);
    }
    return y;
}

/* Pack one band's coefficients after clearing the bank on plan/reset. */
static inline void
dsd_noise_squelch_bank_prepare_band(dsd_noise_squelch_band_group* bank, int band,
                                    const dsd_noise_squelch_biquad* sections) {
    const int width = DSD_NOISE_SQUELCH_GROUP_BANDS;
    for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
        bank[band / width].gain[s][band % width] = sections[s].gain;
        bank[band / width].a1[s][band % width] = sections[s].a1;
        bank[band / width].a2[s][band % width] = sections[s].a2;
    }
}

/* SIMD lanes are independent bands, never successive samples or cascade stages.
 * Keep every band's multiply/add order and sample-by-sample energy accumulation. */
static inline void
dsd_noise_squelch_bank_energy(dsd_noise_squelch_band_group* bank, int bands, double x, double* energy) {
    const int width = DSD_NOISE_SQUELCH_GROUP_BANDS;
    for (int k = 0; k < bands; k += width) {
        dsd_noise_squelch_band_group* g = &bank[k / width];
        double y[DSD_NOISE_SQUELCH_GROUP_BANDS] = {x, x, x, x};
        for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
            for (int lane = 0; lane < width; lane++) {
                const double input = y[lane];
                y[lane] = (g->gain[s][lane] * input) + g->s1[s][lane];
                g->s1[s][lane] = g->s2[s][lane] - (g->a1[s][lane] * y[lane]);
                g->s2[s][lane] = (-g->gain[s][lane] * input) - (g->a2[s][lane] * y[lane]);
            }
        }
        for (int lane = 0; lane < width && k + lane < bands; lane++) {
            energy[k + lane] += y[lane] * y[lane];
        }
    }
}

static inline void
dsd_noise_squelch_bank_flush(dsd_noise_squelch_band_group* bank, int bands, double threshold) {
    const int width = DSD_NOISE_SQUELCH_GROUP_BANDS;
    for (int k = 0; k < bands; k++) {
        for (int s = 0; s < DSD_NOISE_SQUELCH_SECTIONS; s++) {
            double* s1 = &bank[k / width].s1[s][k % width];
            double* s2 = &bank[k / width].s2[s][k % width];
            if (fabs(*s1) < threshold) {
                *s1 = 0.0;
            }
            if (fabs(*s2) < threshold) {
                *s2 = 0.0;
            }
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_DSP_NOISE_SQUELCH_BANK_H_ */
