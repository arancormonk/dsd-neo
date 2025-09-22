// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for polyphase rational resampler (L/M). */

#include <cstdlib>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/runtime/mem.h>
#include <stdio.h>
#include <string.h>

static int
approx_eq(int a, int b, int tol) {
    int d = a - b;
    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

static int
expected_out_len_for_block(int in_len, int L, int M) {
    int phase = 0;
    int out_len = 0;
    for (int n = 0; n < in_len; n++) {
        int local = phase;
        while (local < L) {
            out_len++;
            local += M;
        }
        phase = local - L;
    }
    return out_len;
}

int
main(void) {
    // demod_state is large; allocate on heap to avoid stack overflow
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        fprintf(stderr, "alloc demod_state failed\n");
        return 1;
    }
    memset(s, 0, sizeof(*s));
    const int L = 3, M = 2;
    s->resamp_enabled = 1;

    resamp_design(s, L, M);
    if (!s->resamp_taps || !s->resamp_hist || s->resamp_taps_per_phase <= 0) {
        fprintf(stderr, "resamp_design failed to allocate/initialize\n");
        return 1;
    }

    const int N = 96;
    int16_t in[N];
    for (int i = 0; i < N; i++) {
        in[i] = 1000; // DC input
    }
    int16_t out[N * 4];
    int out_len = resamp_process_block(s, in, N, out);

    int exp_len = expected_out_len_for_block(N, L, M);
    if (out_len != exp_len) {
        fprintf(stderr, "RESAMP: out_len=%d expected=%d\n", out_len, exp_len);
        return 1;
    }
    // DC gain near unity after initial warm-up (history filled)
    int warm = s->resamp_taps_per_phase * 2;
    if (warm < 0) {
        warm = 0;
    }
    if (warm > out_len) {
        warm = out_len;
    }
    for (int i = warm; i < out_len; i++) {
        if (!approx_eq(out[i], 1000, 5)) {
            fprintf(stderr, "RESAMP: out[%d]=%d not within tol of 1000\n", i, (int)out[i]);
            return 1;
        }
    }

    // Clean up
    if (s->resamp_taps) {
        dsd_neo_aligned_free(s->resamp_taps);
    }
    if (s->resamp_hist) {
        dsd_neo_aligned_free(s->resamp_hist);
    }
    free(s);
    return 0;
}
