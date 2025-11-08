// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * QPSK Costas loop (carrier recovery) implementation.
 *
 * Second-order loop using 4th-power phase detector for QPSK.
 * Rotates baseband by NCO (high-quality sin/cos) and updates freq/phase.
*/

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>
#include <math.h>
#include <stdint.h>

/* Fast atan2 approximation (64-bit inputs), returns Q14 where pi == 1<<14 */
static inline int
fast_atan2_64(int64_t y, int64_t x) {
    int angle;
    int pi4 = (1 << 12), pi34 = 3 * (1 << 12); /* pi = 1<<14 */
    if (x == 0 && y == 0) {
        return 0;
    }
    int64_t yabs = (y < 0) ? -y : y;
    if (x >= 0) {
        int64_t denom = x + yabs;
        if (denom == 0) {
            angle = 0;
        } else {
            angle = (int)(pi4 - ((int64_t)pi4 * (x - yabs)) / denom);
        }
    } else {
        int64_t denom = yabs - x;
        if (denom == 0) {
            angle = pi34;
        } else {
            angle = (int)(pi34 - ((int64_t)pi4 * (x + yabs)) / denom);
        }
    }
    return (y < 0) ? -angle : angle;
}

/* High-quality trig path: Q15 cos/sin from Q15 phase (2*pi == 1<<15) */
static inline void
sin_cos_q15_from_phase_trig(int phase_q15, int16_t* c_out, int16_t* s_out) {
    const double kQ15ToRad = (2.0 * M_PI) / 32768.0;
    int p = phase_q15 & 0x7FFF;
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

/* Clamp helper */
static inline int
clamp_i(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void
cqpsk_costas_mix_and_update(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    if (!d->cqpsk_enable) {
        return;
    }

    /* Defaults if not preconfigured */
    int alpha_q15 = (d->fll_alpha_q15 > 0) ? d->fll_alpha_q15 : 100; /* reuse FLL defaults if present */
    int beta_q15 = (d->fll_beta_q15 > 0) ? d->fll_beta_q15 : 10;
    int deadband_q14 = (d->fll_deadband_q14 > 0) ? d->fll_deadband_q14 : 32;
    int slew_max_q15 = (d->fll_slew_max_q15 > 0) ? d->fll_slew_max_q15 : 64;

    int phase = d->fll_phase_q15; /* reuse FLL storage for NCO phase/freq to avoid expanding state */
    int freq = d->fll_freq_q15;

    int16_t* x = d->lowpassed;
    int N = d->lp_len;

    for (int i = 0; i + 1 < N; i += 2) {
        int16_t c, s;
        sin_cos_q15_from_phase_trig(phase, &c, &s);

        int xr = x[i];
        int xj = x[i + 1];
        /* Rotate input by current NCO */
        int32_t yr = ((int32_t)xr * c + (int32_t)xj * s) >> 15;
        int32_t yj = ((int32_t)xj * c - (int32_t)xr * s) >> 15;
        x[i] = (int16_t)clamp_i(yr, -32768, 32767);
        x[i + 1] = (int16_t)clamp_i(yj, -32768, 32767);

        /* 4th-power phase detector for QPSK */
        int64_t r = yr;
        int64_t j = yj;
        /* z^2 = (r^2 - j^2) + j*(2*r*j) */
        int64_t a = r * r - j * j;
        int64_t b = (r + r) * j;
        /* z^4 = (a^2 - b^2) + j*(2ab) */
        int64_t re4 = a * a - b * b;
        int64_t im4 = (a + a) * b;
        /* Phase error ~ arg(z^4)/4, keep in Q14 */
        int err4_q14 = fast_atan2_64(im4, re4);
        int err_q14 = err4_q14 >> 2; /* divide by 4 */

        /* Deadband */
        if (err_q14 < deadband_q14 && err_q14 > -deadband_q14) {
            phase += freq; /* still advance NCO */
            continue;
        }

        /* PI update on frequency (Q15 domain) */
        int32_t p = ((int64_t)alpha_q15 * err_q14) >> 14;   /* -> Q15 */
        int32_t iacc = ((int64_t)beta_q15 * err_q14) >> 14; /* -> Q15 */
        int32_t df = p + iacc;
        if (df > slew_max_q15) {
            df = slew_max_q15;
        }
        if (df < -slew_max_q15) {
            df = -slew_max_q15;
        }
        freq += (int)df;
        /* Clamp NCO frequency */
        const int F_CLAMP = 4096; /* allow wider than FLL; ~±6 kHz @48k */
        if (freq > F_CLAMP) {
            freq = F_CLAMP;
        }
        if (freq < -F_CLAMP) {
            freq = -F_CLAMP;
        }

        /* Advance phase */
        phase += freq;
    }

    d->fll_phase_q15 = phase & 0x7FFF;
    d->fll_freq_q15 = freq;
}
