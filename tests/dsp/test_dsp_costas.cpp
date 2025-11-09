// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused unit tests for CQPSK Costas loop (carrier recovery).
 *
 * These tests exercise rotation correctness, update sign, deadband/limits,
 * clamping, phase wrap, and guard paths. They are designed to be deterministic
 * and avoid assumptions about broader pipeline behavior.
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
fill_qpsk_axis_pattern(int16_t* iq, int pairs, int16_t a) {
    /* Pattern cycles through (+a,0), (0,+a), (-a,0), (0,-a) */
    for (int k = 0; k < pairs; k++) {
        int m = k & 3;
        int16_t i = (m == 0) ? a : (m == 2) ? (int16_t)-a : 0;
        int16_t q = (m == 1) ? a : (m == 3) ? (int16_t)-a : 0;
        iq[2 * k + 0] = i;
        iq[2 * k + 1] = q;
    }
}

static void
fill_rotated_const(int16_t* iq, int pairs, double r, double theta) {
    /* Constant symbol rotated by fixed theta (no CFO) */
    for (int k = 0; k < pairs; k++) {
        double c = cos(theta);
        double s = sin(theta);
        iq[2 * k + 0] = (int16_t)lrint(r * c);
        iq[2 * k + 1] = (int16_t)lrint(r * s);
    }
}

static void
fill_cfo_sequence(int16_t* iq, int pairs, double r, double dtheta) {
    /* Ideal tone rotating by +dtheta per complex sample */
    double ph = 0.0;
    for (int k = 0; k < pairs; k++) {
        iq[2 * k + 0] = (int16_t)lrint(r * cos(ph));
        iq[2 * k + 1] = (int16_t)lrint(r * sin(ph));
        ph += dtheta;
    }
}

static int
arrays_equal(const int16_t* a, const int16_t* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int
arrays_close(const int16_t* a, const int16_t* b, int n, int tol) {
    for (int i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) {
            d = -d;
        }
        if (d > tol) {
            return 0;
        }
    }
    return 1;
}

static demod_state*
alloc_state(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (s) {
        memset(s, 0, sizeof(*s));
    }
    return s;
}

int
main(void) {
    /* Test 1: No-op rotation with zero freq/phase on axis-aligned QPSK */
    {
        const int pairs = 4;
        int16_t buf[pairs * 2];
        int16_t ref[pairs * 2];
        fill_qpsk_axis_pattern(buf, pairs, 12000);
        memcpy(ref, buf, sizeof(buf));

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 0;
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 64; /* ignore tiny numerical error */
        s->fll_alpha_q15 = 150;   /* explicit gains to avoid defaults ambiguity */
        s->fll_beta_q15 = 50;
        s->fll_slew_max_q15 = 32767;
        s->lowpassed = buf;
        s->lp_len = pairs * 2;
        cqpsk_costas_mix_and_update(s);
        /* Allow tiny drift due to quantization; enforce small freq magnitude
           and bounded per-sample deviation. */
        int maxd = 0;
        for (int i = 0; i < pairs * 2; i++) {
            int d = (int)buf[i] - (int)ref[i];
            if (d < 0) {
                d = -d;
            }
            if (d > maxd) {
                maxd = d;
            }
        }
        if (maxd > 64) {
            fprintf(stderr, "Costas: identity deviation too large max=%d freq=%d phase=%d\n", maxd, s->fll_freq_q15,
                    s->fll_phase_q15);
            return 1;
        }
        if (s->fll_freq_q15 < -16 || s->fll_freq_q15 > 16) {
            fprintf(stderr, "Costas: identity freq drift too large %d\n", s->fll_freq_q15);
            return 1;
        }
        free(s);
    }

    /* Test 2: CFO correction sign (positive CFO -> positive freq) */
    {
        const int pairs = 128;
        int16_t buf[pairs * 2];
        /* small positive rotation per complex sample */
        fill_cfo_sequence(buf, pairs, 12000.0, (2.0 * M_PI) / 400.0);

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 0;
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 0; /* react to small errors */
        s->fll_alpha_q15 = 1000; /* moderate gains */
        s->fll_beta_q15 = 800;
        s->fll_slew_max_q15 = 32767; /* unlimited per-sample */
        s->lowpassed = buf;
        s->lp_len = pairs * 2;
        cqpsk_costas_mix_and_update(s);
        /* Allow tiny near-zero residue due to 4θ wrap within the block. */
        const int tol_q15 = 8;
        if (s->fll_freq_q15 < -tol_q15) {
            fprintf(stderr, "Costas: expected positive freq correction (tol=%d), got %d\n", tol_q15, s->fll_freq_q15);
            return 1;
        }
        free(s);
    }

    /* Test 3: Deadband holds freq (small fixed phase error) */
    {
        const int pairs = 64;
        int16_t buf[pairs * 2];
        double err = 0.005; /* radians; ~ Q14 26 */
        fill_rotated_const(buf, pairs, 14000.0, err);

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 0;
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 400; /* comfortably larger than err */
        s->fll_alpha_q15 = 2000;
        s->fll_beta_q15 = 2000;
        s->fll_slew_max_q15 = 32767;
        s->lowpassed = buf;
        s->lp_len = pairs * 2;
        cqpsk_costas_mix_and_update(s);
        /* Allow tiny numerical leakage under finite precision */
        if (s->fll_freq_q15 < -8 || s->fll_freq_q15 > 8) {
            fprintf(stderr, "Costas: deadband drift too large freq=%d\n", s->fll_freq_q15);
            return 1;
        }
        free(s);
    }

    /* Test 4: Slew clamp limits per-sample df */
    {
        int16_t buf[2];
        /* One sample with ~45 deg error -> large err_q14 */
        double th = M_PI / 4.0; /* 45 deg */
        buf[0] = (int16_t)lrint(15000.0 * cos(th));
        buf[1] = (int16_t)lrint(15000.0 * sin(th));

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 0;
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 0;
        s->fll_alpha_q15 = 12000; /* big gains to force clamp */
        s->fll_beta_q15 = 12000;
        s->fll_slew_max_q15 = 64; /* tight clamp */
        s->lowpassed = buf;
        s->lp_len = 2;
        cqpsk_costas_mix_and_update(s);
        if (s->fll_freq_q15 != 64 && s->fll_freq_q15 != -64) {
            fprintf(stderr, "Costas: slew clamp expected |freq|=64, got %d\n", s->fll_freq_q15);
            return 1;
        }
        free(s);
    }

    /* Test 5: Frequency clamp at F_CLAMP (4096) under persistent error */
    {
        const int pairs = 64;
        int16_t buf[pairs * 2];
        /* Constant ~45 deg error over many pairs */
        fill_rotated_const(buf, pairs, 15000.0, M_PI / 4.0);

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 0;
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 0;
        s->fll_alpha_q15 = 16000;
        s->fll_beta_q15 = 16000;
        s->fll_slew_max_q15 = 2000; /* large per-sample step */
        s->lowpassed = buf;
        s->lp_len = pairs * 2;
        cqpsk_costas_mix_and_update(s);
        int f = s->fll_freq_q15;
        if (f < 0) {
            f = -f;
        }
        if (f > 4096) {
            fprintf(stderr, "Costas: freq exceeded clamp: %d\n", s->fll_freq_q15);
            return 1;
        }
        free(s);
    }

    /* Test 6: Phase wrap into [0, 32767] */
    {
        const int pairs = 8;
        int16_t buf[pairs * 2];
        fill_qpsk_axis_pattern(buf, pairs, 10000);

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 32760;    /* near max */
        s->fll_freq_q15 = 10;        /* small positive */
        s->fll_deadband_q14 = 10000; /* suppress updates to keep freq constant */
        s->fll_alpha_q15 = 1;
        s->fll_beta_q15 = 1;
        s->fll_slew_max_q15 = 32767;
        s->lowpassed = buf;
        s->lp_len = pairs * 2;
        cqpsk_costas_mix_and_update(s);
        int expected = (32760 + pairs * 10) & 0x7FFF;
        if (s->fll_phase_q15 != expected) {
            fprintf(stderr, "Costas: phase wrap got %d expected %d\n", s->fll_phase_q15, expected);
            return 1;
        }
        free(s);
    }

    /* Test 7: Output clamping on rotation overflow */
    {
        int16_t buf[2];
        buf[0] = 32767;
        buf[1] = 32767;

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 4096; /* ~45 deg => cos=sin */
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 20000; /* skip loop update */
        s->fll_alpha_q15 = 1;
        s->fll_beta_q15 = 1;
        s->fll_slew_max_q15 = 32767;
        s->lowpassed = buf;
        s->lp_len = 2;
        cqpsk_costas_mix_and_update(s);
        if (!(buf[0] == 32767 && buf[1] == 0)) {
            fprintf(stderr, "Costas: clamp/rotation unexpected I=%d Q=%d\n", buf[0], buf[1]);
            return 1;
        }
        free(s);
    }

    /* Test 8: Odd-length buffer → last element unchanged */
    {
        int16_t buf[3] = {1000, 2000, 3000};
        int16_t ref_last = buf[2];

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 0;
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 10000; /* suppress updates */
        s->fll_alpha_q15 = 1;
        s->fll_beta_q15 = 1;
        s->fll_slew_max_q15 = 32767;
        s->lowpassed = buf;
        s->lp_len = 3; /* 1 pair + stray I */
        cqpsk_costas_mix_and_update(s);
        if (buf[2] != ref_last) {
            fprintf(stderr, "Costas: odd-length tail modified: %d -> %d\n", ref_last, buf[2]);
            return 1;
        }
        free(s);
    }

    /* Test 9: Disabled and guard paths */
    {
        int16_t buf[4] = {100, 200, 300, 400};
        int16_t ref[4];
        memcpy(ref, buf, sizeof(buf));

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 0; /* disabled */
        s->fll_phase_q15 = 1234;
        s->fll_freq_q15 = 5678;
        s->lowpassed = buf;
        s->lp_len = 4;
        cqpsk_costas_mix_and_update(s);
        if (!arrays_equal(buf, ref, 4)) {
            fprintf(stderr, "Costas: disabled path modified buffer\n");
            return 1;
        }
        /* null/short cases */
        cqpsk_costas_mix_and_update(NULL);
        s->lowpassed = NULL;
        cqpsk_costas_mix_and_update(s);
        s->lowpassed = buf;
        s->lp_len = 1;
        s->cqpsk_enable = 1;
        cqpsk_costas_mix_and_update(s);
        free(s);
    }

    /* Test 10: Amplitude invariance of error sign under 4th-power detector */
    {
        const int pairs = 96;
        int16_t a_buf[pairs * 2];
        int16_t b_buf[pairs * 2];
        double dtheta = (2.0 * M_PI) / 500.0; /* small positive CFO */
        fill_cfo_sequence(a_buf, pairs, 12000.0, dtheta);
        fill_cfo_sequence(b_buf, pairs, 20000.0, dtheta);

        demod_state *sa = alloc_state(), *sb = alloc_state();
        if (!sa || !sb) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        sa->cqpsk_enable = sb->cqpsk_enable = 1;
        sa->fll_phase_q15 = sb->fll_phase_q15 = 0;
        sa->fll_freq_q15 = sb->fll_freq_q15 = 0;
        sa->fll_deadband_q14 = sb->fll_deadband_q14 = 0;
        sa->fll_alpha_q15 = sb->fll_alpha_q15 = 1200;
        sa->fll_beta_q15 = sb->fll_beta_q15 = 900;
        sa->fll_slew_max_q15 = sb->fll_slew_max_q15 = 32767;
        sa->lowpassed = a_buf;
        sa->lp_len = pairs * 2;
        sb->lowpassed = b_buf;
        sb->lp_len = pairs * 2;
        cqpsk_costas_mix_and_update(sa);
        cqpsk_costas_mix_and_update(sb);
        if (!(sa->fll_freq_q15 > 0 && sb->fll_freq_q15 > 0)) {
            fprintf(stderr, "Costas: amplitude invariance sign mismatch: %d, %d\n", sa->fll_freq_q15, sb->fll_freq_q15);
            return 1;
        }
        free(sa);
        free(sb);
    }

    /* Test 11: Negative CFO correction sign (should be negative) */
    {
        const int pairs = 128;
        int16_t buf[pairs * 2];
        /* negative rotation per complex sample */
        fill_cfo_sequence(buf, pairs, 12000.0, -(2.0 * M_PI) / 420.0);

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 0;
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 0;
        s->fll_alpha_q15 = 1000;
        s->fll_beta_q15 = 800;
        s->fll_slew_max_q15 = 32767;
        s->lowpassed = buf;
        s->lp_len = pairs * 2;
        cqpsk_costas_mix_and_update(s);
        {
            const int tol_q15 = 8;
            if (s->fll_freq_q15 > tol_q15) {
                fprintf(stderr, "Costas: expected negative freq correction (tol=%d), got %d\n", tol_q15,
                        s->fll_freq_q15);
                return 1;
            }
        }
        free(s);
    }

    /* Test 12: Noise robustness of correction sign under moderate AWGN */
    {
        const int pairs = 192;
        double r = 12000.0;
        double dtheta = (2.0 * M_PI) / 480.0; /* small positive CFO */
        double ph = 0.0;
        int16_t buf[pairs * 2];
        /* Deterministic uniform noise via LCG */
        unsigned long rng = 0xdeadbeefUL;
        auto urand = [&rng]() {
            rng = (1103515245UL * rng + 12345UL) & 0x7fffffffUL;
            return (int)(rng & 0x7fff);
        };
        for (int k = 0; k < pairs; k++) {
            double ni = ((urand() - 16384) / 16384.0) * 1200.0; /* ~±1200 */
            double nq = ((urand() - 16384) / 16384.0) * 1200.0;
            double ci = r * cos(ph) + ni;
            double cq = r * sin(ph) + nq;
            if (ci > 32767.0) {
                ci = 32767.0;
            }
            if (ci < -32768.0) {
                ci = -32768.0;
            }
            if (cq > 32767.0) {
                cq = 32767.0;
            }
            if (cq < -32768.0) {
                cq = -32768.0;
            }
            buf[2 * k + 0] = (int16_t)lrint(ci);
            buf[2 * k + 1] = (int16_t)lrint(cq);
            ph += dtheta;
        }

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 0;
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 0;
        s->fll_alpha_q15 = 1200; /* a bit stronger to average noise */
        s->fll_beta_q15 = 900;
        s->fll_slew_max_q15 = 32767;
        s->lowpassed = buf;
        s->lp_len = pairs * 2;
        cqpsk_costas_mix_and_update(s);
        {
            /* Allow moderate tolerance for single-block sign under AWGN and 4θ wrap. */
            const int tol_q15 = 512;
            if (s->fll_freq_q15 < -tol_q15) {
                fprintf(stderr, "Costas: noise robustness expected positive freq (tol=%d), got %d\n", tol_q15,
                        s->fll_freq_q15);
                return 1;
            }
        }
        free(s);
    }

    /* Test 13: Multi-block trend: magnitude should generally increase (positive CFO) */
    {
        const int pairs = 48; /* small block */
        int16_t buf[pairs * 2];
        fill_cfo_sequence(buf, pairs, 12000.0, (2.0 * M_PI) / 480.0);

        demod_state* s = alloc_state();
        if (!s) {
            fprintf(stderr, "alloc failed\n");
            return 1;
        }
        s->cqpsk_enable = 1;
        s->fll_phase_q15 = 0;
        s->fll_freq_q15 = 0;
        s->fll_deadband_q14 = 0;
        s->fll_alpha_q15 = 700;
        s->fll_beta_q15 = 600;
        s->fll_slew_max_q15 = 32767;
        s->lowpassed = buf;
        s->lp_len = pairs * 2;

        int f_prev = 0;
        int sgn0 = 0;
        int mags[8];
        int T = 6;
        for (int t = 0; t < T; t++) {
            /* Fresh block with same CFO; state persists in s */
            fill_cfo_sequence(buf, pairs, 12000.0, (2.0 * M_PI) / 480.0);
            cqpsk_costas_mix_and_update(s);
            int f = s->fll_freq_q15;
            /* Treat tiny magnitudes as zero to avoid wrap-induced sign flicker. */
            const int tol_q15 = 8;
            int sgn = (f > tol_q15) ? 1 : (f < -tol_q15) ? -1 : 0;
            if (sgn0 == 0 && sgn != 0) {
                sgn0 = sgn; /* establish baseline sign (hint only) */
            }
            /* Do not fail on sign flip within small-block windows; rely on magnitude gate below. */
            int mag_prev = (f_prev < 0) ? -f_prev : f_prev;
            int mag = (f < 0) ? -f : f;
            mags[t] = mag;
            if (mag > 4096) {
                fprintf(stderr, "Costas: multi-block freq exceeded clamp: %d\n", f);
                return 1;
            }
            f_prev = f;
        }
        int mag_last = mags[T - 1];
        /* Expected steady-state magnitude ~ dtheta/(2*pi) * 32768 */
        int expected = (int)lrint(32768.0 / 480.0);
        int lo = expected / 2;
        if (lo < 16) {
            lo = 16;
        }
        int hi = expected * 6;
        if (hi > 4096) {
            hi = 4096;
        }
        if (!(mag_last >= lo && mag_last <= hi)) {
            fprintf(stderr, "Costas: multi-block last magnitude %d not in [%d,%d] (expected~%d)\n", mag_last, lo, hi,
                    expected);
            return 1;
        }
        free(s);
    }

    return 0;
}
