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

#include <cstdio>
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/math_utils.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

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

/* Optional runtime inversion of Costas rotation sign for debugging.
   Enable via DSD_NEO_CQPSK_ROT_INVERT=1 to flip e^{-jphi} <-> e^{+jphi}.
   Defaults to normal e^{-jphi}. */
static inline int
costas_rot_invert_enabled() {
    static int inited = 0;
    static int enabled = 0;
    if (!inited) {
        const char* v = getenv("DSD_NEO_CQPSK_ROT_INVERT");
        enabled = (v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T')) ? 1 : 0;
        inited = 1;
    }
    return enabled;
}

void
cqpsk_costas_mix_and_update(struct demod_state* d) {
    if (!d || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    if (!d->cqpsk_enable) {
        return;
    }

    /* Defaults if not preconfigured.
     * The default alpha/beta values are chosen to be more aggressive than the
     * FLL defaults to ensure the Costas loop can pull in larger frequency
     * offsets typical of SDRs. A wider loop bandwidth is necessary for
     * robust carrier acquisition.
     *
     * Costas tuning is independent of the FLL env knobs: use
     * demod_state->costas_alpha_q15/beta_q15 when set, otherwise fall back
     * to internal defaults (400/40). */
    int alpha_q15 = (d->costas_alpha_q15 > 0) ? d->costas_alpha_q15 : 400;
    int beta_q15 = (d->costas_beta_q15 > 0) ? d->costas_beta_q15 : 40;
    int deadband_q14 = (d->costas_deadband_q14 > 0) ? d->costas_deadband_q14 : 32;
    int slew_max_q15 = (d->costas_slew_max_q15 > 0) ? d->costas_slew_max_q15 : 64;

    int phase = d->fll_phase_q15; /* reuse FLL storage for NCO phase/freq to avoid expanding state */
    int freq = d->fll_freq_q15;

    int16_t* x = d->lowpassed;
    int N = d->lp_len;

    const int invert = costas_rot_invert_enabled();
    const char* dbg = getenv("DSD_NEO_DBG_COSTAS");
    int dbg_once = (dbg && dbg[0] == '1') ? 1 : 0;
    int64_t err_abs_acc = 0;
    int err_count = 0;
    for (int i = 0; i + 1 < N; i += 2) {
        int16_t c, s;
        sin_cos_q15_from_phase_trig(phase, &c, &s);

        int xr = x[i];
        int xj = x[i + 1];
        /* Rotate input by current NCO. Default is e^{-jphi}. When
           DSD_NEO_CQPSK_ROT_INVERT=1, flip to e^{+jphi} to aid diagnosis. */
        int32_t yr, yj;
        if (!invert) {
            /* y = x * e^{-jphi} */
            yr = ((int32_t)xr * c + (int32_t)xj * s) >> 15;
            yj = ((int32_t)xj * c - (int32_t)xr * s) >> 15;
        } else {
            /* y = x * e^{+jphi} */
            yr = ((int32_t)xr * c - (int32_t)xj * s) >> 15;
            yj = ((int32_t)xr * s + (int32_t)xj * c) >> 15;
        }
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
        /* Phase error ~ arg(z^4)/4. Unwrap arg(z^4) against last value for continuity. */
        int e4_now = dsd_neo_fast_atan2(im4, re4); /* Q14 in [-pi..pi] */
        if (d->costas_e4_prev_set) {
            int diff = e4_now - d->costas_e4_prev_q14;
            const int TWO_PI_Q14 = (1 << 15);
            if (diff > (1 << 14)) {
                e4_now -= TWO_PI_Q14;
            } else if (diff < -(1 << 14)) {
                e4_now += TWO_PI_Q14;
            }
        }
        d->costas_e4_prev_q14 = e4_now;
        d->costas_e4_prev_set = 1;
        int err_q14 = (e4_now >> 2);
        if (dbg_once) {
            fprintf(stderr,
                    "DBG_COSTAS: i=%d xr=%d xj=%d c=%d s=%d yr=%d yj=%d re4=%lld im4=%lld e4=%d err=%d ph=%d fq=%d\n",
                    i, xr, xj, c, s, (int)yr, (int)yj, (long long)re4, (long long)im4, e4_now, err_q14, phase, freq);
        }
        /* Accumulate absolute error for diagnostics */
        int ea = (err_q14 >= 0) ? err_q14 : -err_q14;
        err_abs_acc += ea;
        err_count++;

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
        /* Apply correction with standard sign (phase += freq, freq += df) */
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
    if (err_count > 0) {
        int avg = (int)(err_abs_acc / err_count);
        if (avg < 0) {
            avg = 0;
        }
        d->costas_err_avg_q14 = avg;
    }
    if (dbg_once) {
        fprintf(stderr, "DBG_COSTAS: final freq=%d phase=%d avg|err|=%d\n", d->fll_freq_q15, d->fll_phase_q15,
                d->costas_err_avg_q14);
    }
}
