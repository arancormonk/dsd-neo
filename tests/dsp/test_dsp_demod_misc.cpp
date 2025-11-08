// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for remaining demod helpers: deemph_filter, low_pass_real, low_pass,
   fifth_order, generic_fir, and dsd_fm_demod plumbing. */

#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <string.h>

// Provide globals expected by demod_pipeline.cpp to avoid linking RTL front-end
int use_halfband_decimator = 0;

static int
approx_eq(int a, int b, int tol) {
    int d = a - b;
    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

static int
monotonic_nondecreasing(const int16_t* x, int n) {
    for (int i = 1; i < n; i++) {
        if (x[i] < x[i - 1]) {
            return 0;
        }
    }
    return 1;
}

// Fake discriminator for dsd_fm_demod
static int
fake_disc(int ar, int aj, int br, int bj) {
    (void)ar;
    (void)aj;
    (void)br;
    (void)bj;
    return 123;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    memset(s, 0, sizeof(*s));

    // deemph_filter: step response
    {
        const int N = 64;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 2000;
        }
        s->deemph_a = 8192; // Q15 ~ 0.25
        s->deemph_avg = 0;
        deemph_filter(s);
        if (!monotonic_nondecreasing(s->result, N)) {
            fprintf(stderr, "deemph_filter: non-monotonic step response\n");
            free(s);
            return 1;
        }
        if (!approx_eq(s->result[N - 1], 2000, 150)) {
            fprintf(stderr, "deemph_filter: final=%d not near 2000\n", s->result[N - 1]);
            free(s);
            return 1;
        }
    }

    // low_pass_real: average 2:1 from 48k to 24k on constant signal
    {
        const int N = 32;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 1000;
        }
        s->rate_in = 48000;
        s->rate_out2 = 24000;
        s->now_lpr = 0;
        s->prev_lpr_index = 0;
        low_pass_real(s);
        if (s->result_len != N / 2) {
            fprintf(stderr, "low_pass_real: result_len=%d want %d\n", s->result_len, N / 2);
            free(s);
            return 1;
        }
        for (int i = 0; i < s->result_len; i++) {
            if (!approx_eq(s->result[i], 1000, 1)) {
                fprintf(stderr, "low_pass_real: out[%d]=%d not ~1000\n", i, s->result[i]);
                free(s);
                return 1;
            }
        }
    }

    // low_pass: 2:1 decimation on complex pairs by summing
    {
        s->lowpassed = s->hb_workbuf; // use internal storage
        for (int i = 0; i < 8; i++) {
            s->lowpassed[i] = 100;
        }
        s->lp_len = 8; // 4 pairs
        s->downsample = 2;
        s->now_r = s->now_j = s->prev_index = 0;
        low_pass(s);
        if (s->lp_len != 4) {
            fprintf(stderr, "low_pass: lp_len=%d want 4\n", s->lp_len);
            free(s);
            return 1;
        }
        if (!(s->lowpassed[0] == 200 && s->lowpassed[1] == 200 && s->lowpassed[2] == 200 && s->lowpassed[3] == 200)) {
            fprintf(stderr, "low_pass: summed outputs mismatch\n");
            free(s);
            return 1;
        }
    }

    // fifth_order: constant input, prefilled history -> output doubles DC
    {
        int16_t hist[6];
        for (int i = 0; i < 6; i++) {
            hist[i] = 500;
        }
        int16_t data[16];
        for (int i = 0; i < 16; i++) {
            data[i] = 500;
        }
        fifth_order(data, 16, hist);
        if (!(data[0] == 1000 && data[2] == 1000 && data[4] == 1000 && data[6] == 1000)) {
            fprintf(stderr, "fifth_order: expected doubled DC on decimated indices\n");
            free(s);
            return 1;
        }
    }

    // generic_fir: center-tap passthrough via hist
    {
        int16_t hist[9] = {0};
        hist[4] = 1234;
        int fir[6] = {0, 0, 0, 0, 0, (1 << 15)}; // only center contributes
        int16_t buf[2] = {555, 0};               // only even indices processed
        generic_fir(buf, 2, fir, hist);
        if (buf[0] != 1234) {
            fprintf(stderr, "generic_fir: expected center hist passthrough, got %d\n", buf[0]);
            free(s);
            return 1;
        }
    }
    // generic_fir: saturation when sum exceeds 16-bit range
    {
        int16_t hist[9];
        for (int i = 0; i < 9; i++) {
            hist[i] = 30000;
        }
        int fir[6] = {0, 4096, 4096, 4096, 4096, 4096};
        int16_t buf[2] = {0, 0};
        generic_fir(buf, 2, fir, hist);
        if (buf[0] != 32767) {
            fprintf(stderr, "generic_fir: expected saturation to 32767, got %d\n", buf[0]);
            free(s);
            return 1;
        }
    }

    // dsd_fm_demod: fake discriminator + FLL offset
    {
        int16_t iq[6] = {10, 20, 30, 40, 50, 60};
        s->lowpassed = iq;
        s->lp_len = 6; // 3 complex samples
        s->discriminator = &fake_disc;
        s->fll_enabled = 1;
        s->fll_freq_q15 = 100; // contributes +50 in Q14 domain
        s->pre_r = 0;
        s->pre_j = 0;
        dsd_fm_demod(s);
        if (s->result_len != 3) {
            fprintf(stderr, "dsd_fm_demod: result_len=%d want 3\n", s->result_len);
            free(s);
            return 1;
        }
        for (int i = 0; i < s->result_len; i++) {
            if (s->result[i] != 173) { // 123 + 50
                fprintf(stderr, "dsd_fm_demod: result[%d]=%d want 173\n", i, s->result[i]);
                free(s);
                return 1;
            }
        }
    }

    free(s);
    return 0;
}
