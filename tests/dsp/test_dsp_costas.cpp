// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit tests for the GNU Radio-derived Costas loop implementation.
 *
 * These tests focus on basic behaviors:
 *   - Identity rotation when phase/frequency are zero.
 *   - Positive CFO drives a positive frequency estimate.
 *   - Initial phase is seeded from the FLL state.
 */

#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_state.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
fill_qpsk_diag_pattern(int16_t* iq, int pairs, int16_t a) {
    for (int k = 0; k < pairs; k++) {
        int m = k & 3;
        int16_t i = (m == 0 || m == 3) ? a : (int16_t)-a;
        int16_t q = (m == 0 || m == 1) ? a : (int16_t)-a;
        iq[2 * k + 0] = i;
        iq[2 * k + 1] = q;
    }
}

static void
fill_cfo_sequence(int16_t* iq, int pairs, double r, double dtheta) {
    double ph = 0.0;
    for (int k = 0; k < pairs; k++) {
        iq[2 * k + 0] = (int16_t)lrint(r * cos(ph));
        iq[2 * k + 1] = (int16_t)lrint(r * sin(ph));
        ph += dtheta;
    }
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

static int
test_identity_rotation(void) {
    const int pairs = 8;
    int16_t buf[pairs * 2];
    int16_t ref[pairs * 2];
    fill_qpsk_diag_pattern(buf, pairs, 12000);
    memcpy(ref, buf, sizeof(buf));

    demod_state* s = alloc_state();
    if (!s) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    cqpsk_costas_mix_and_update(s);

    if (!arrays_close(buf, ref, pairs * 2, 1)) {
        fprintf(stderr, "IDENTITY: rotation distorted samples\n");
        free(s);
        return 1;
    }
    if (s->fll_freq_q15 < -16 || s->fll_freq_q15 > 16) {
        fprintf(stderr, "IDENTITY: expected near-zero freq, got %d\n", s->fll_freq_q15);
        free(s);
        return 1;
    }
    free(s);
    return 0;
}

static int
test_positive_cfo_pushes_freq(void) {
    const int pairs = 128;
    int16_t buf[pairs * 2];
    fill_cfo_sequence(buf, pairs, 12000.0, (2.0 * M_PI) / 400.0);

    demod_state* s = alloc_state();
    if (!s) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    cqpsk_costas_mix_and_update(s);

    if (s->fll_freq_q15 <= 0) {
        fprintf(stderr, "CFO: expected positive freq correction, got %d\n", s->fll_freq_q15);
        free(s);
        return 1;
    }
    if (s->costas_err_avg_q14 <= 0) {
        fprintf(stderr, "CFO: costas_err_avg_q14 not updated (%d)\n", s->costas_err_avg_q14);
        free(s);
        return 1;
    }
    free(s);
    return 0;
}

static int
test_phase_seed_from_fll(void) {
    int16_t buf[2];
    buf[0] = 14000;
    buf[1] = 0;

    demod_state* s = alloc_state();
    if (!s) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    s->cqpsk_enable = 1;
    s->lowpassed = buf;
    s->lp_len = 2;
    s->fll_phase_q15 = 8192; /* ~pi/2 to seed initial rotation */
    cqpsk_costas_mix_and_update(s);

    if (buf[0] > 100 || buf[1] > -100) {
        fprintf(stderr, "SEED: rotation not applied as expected (I=%d Q=%d)\n", buf[0], buf[1]);
        free(s);
        return 1;
    }
    if (!s->costas_state.initialized) {
        fprintf(stderr, "SEED: Costas loop not initialized\n");
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

int
main(void) {
    if (test_identity_rotation() != 0) {
        return 1;
    }
    if (test_positive_cfo_pushes_freq() != 0) {
        return 1;
    }
    if (test_phase_seed_from_fll() != 0) {
        return 1;
    }
    return 0;
}
