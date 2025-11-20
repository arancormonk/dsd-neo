// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: audio_lpf_filter and dc_block_filter behavior. */

#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <string.h>

// Provide globals expected by demod_pipeline.cpp to avoid linking RTL front-end
int use_halfband_decimator = 0;

static int
monotonic_nondecreasing(const int16_t* x, int n) {
    for (int i = 1; i < n; i++) {
        if (x[i] < x[i - 1]) {
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    // Allocate demod_state on heap
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    memset(s, 0, sizeof(*s));

    // Test audio_lpf_filter on step input
    {
        const int N = 64;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 1000; // step from 0 (state) to 1000
        }
        s->audio_lpf_enable = 1;
        s->audio_lpf_alpha = 8192; // Q15 ~ 0.25
        s->audio_lpf_state = 0;
        audio_lpf_filter(s);
        if (!monotonic_nondecreasing(s->result, s->result_len)) {
            fprintf(stderr, "audio_lpf_filter: not monotonic nondecreasing on step\n");
            free(s);
            return 1;
        }
        // Final value should approach target (allow some residual)
        if (!(s->result[N - 1] >= 900 && s->result[N - 1] <= 1000)) {
            fprintf(stderr, "audio_lpf_filter: final=%d not near 1000\n", s->result[N - 1]);
            free(s);
            return 1;
        }
    }

    // Test dc_block_filter on DC input: output should trend down from initial value
    {
        const int N = 256;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 4096;
        }
        s->dc_avg = 0; // initial DC estimate
        dc_block_filter(s);
        // Should be non-increasing sequence and significantly reduced by end
        for (int i = 1; i < N; i++) {
            if (s->result[i] > s->result[i - 1]) {
                fprintf(stderr, "dc_block_filter: sequence increased at %d\n", i);
                free(s);
                return 1;
            }
        }
        int last = s->result[N - 1];
        if (last >= 4096) {
            fprintf(stderr, "dc_block_filter: insufficient reduction (last=%d)\n", last);
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}
