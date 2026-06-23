// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: audio_lpf_filter and dc_block_filter behavior. */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/sps_filters.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
monotonic_nondecreasing(const float* x, int n) {
    for (int i = 1; i < n; i++) {
        if (x[i] < x[i - 1]) {
            return 0;
        }
    }
    return 1;
}

typedef float (*sps_filter_fn)(float sample, int samples_per_symbol);

static int
expect_sps_filter_response(const char* name, sps_filter_fn filter, int base_sps, int redesign_sps) {
    init_rrc_filter_memory();
    if (filter(123.0f, 1) != 123.0f) {
        DSD_FPRINTF(stderr, "%s: sps<=1 bypass failed\n", name);
        return 1;
    }

    float base_sum = 0.0f;
    float base_abs = 0.0f;
    for (int i = 0; i < 160; i++) {
        float y = filter(i == 0 ? 1.0f : 0.0f, base_sps);
        if (!std::isfinite(y)) {
            DSD_FPRINTF(stderr, "%s: non-finite base response at %d\n", name, i);
            return 1;
        }
        base_sum += y;
        base_abs += std::fabs(y);
    }
    if (!(base_abs > 0.01f && base_sum > 0.80f && base_sum < 1.20f)) {
        DSD_FPRINTF(stderr, "%s: unexpected base response sum=%f abs=%f\n", name, base_sum, base_abs);
        return 1;
    }

    init_rrc_filter_memory();
    float redesign_sum = 0.0f;
    float redesign_abs = 0.0f;
    for (int i = 0; i < 220; i++) {
        float y = filter(i == 0 ? 1.0f : 0.0f, redesign_sps);
        if (!std::isfinite(y)) {
            DSD_FPRINTF(stderr, "%s: non-finite redesign response at %d\n", name, i);
            return 1;
        }
        redesign_sum += y;
        redesign_abs += std::fabs(y);
    }
    if (!(redesign_abs > 0.01f && redesign_sum > 0.80f && redesign_sum < 1.20f)) {
        DSD_FPRINTF(stderr, "%s: unexpected redesign response sum=%f abs=%f\n", name, redesign_sum, redesign_abs);
        return 1;
    }
    return 0;
}

static int
test_sps_filter_wrappers(void) {
    int rc = 0;
    rc |= expect_sps_filter_response("dmr", dmr_filter, 10, 8);
    rc |= expect_sps_filter_response("nxdn", nxdn_filter, 20, 10);
    rc |= expect_sps_filter_response("dpmr", dpmr_filter, 20, 12);
    rc |= expect_sps_filter_response("m17", m17_filter, 10, 8);
    rc |= expect_sps_filter_response("p25", p25_filter, 10, 8);
    return rc;
}

int
main(void) {
    // Allocate demod_state on heap
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    DSD_MEMSET(s, 0, sizeof(*s));

    // Test audio_lpf_filter on step input
    {
        const int N = 64;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 1.0f; // step from 0 (state) to 1.0
        }
        s->audio_lpf_enable = 1;
        s->audio_lpf_alpha = 0.25f;
        s->audio_lpf_state = 0.0f;
        audio_lpf_filter(s);
        if (!monotonic_nondecreasing(s->result, s->result_len)) {
            DSD_FPRINTF(stderr, "audio_lpf_filter: not monotonic nondecreasing on step\n");
            free(s);
            return 1;
        }
        // Final value should approach target (allow some residual)
        if (!(s->result[N - 1] >= 0.9f && s->result[N - 1] <= 1.0f)) {
            DSD_FPRINTF(stderr, "audio_lpf_filter: final=%f not near 1.0\n", s->result[N - 1]);
            free(s);
            return 1;
        }
    }

    // Test dc_block_filter on DC input: output should trend down from initial value
    {
        const int N = 256;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 0.5f;
        }
        s->dc_avg = 0.0f; // initial DC estimate
        dc_block_filter(s);
        // Should be non-increasing sequence and significantly reduced by end
        for (int i = 1; i < N; i++) {
            if (s->result[i] > s->result[i - 1]) {
                DSD_FPRINTF(stderr, "dc_block_filter: sequence increased at %d\n", i);
                free(s);
                return 1;
            }
        }
        float last = s->result[N - 1];
        if (last >= 0.5f) {
            DSD_FPRINTF(stderr, "dc_block_filter: insufficient reduction (last=%f)\n", last);
            free(s);
            return 1;
        }
    }

    if (test_sps_filter_wrappers() != 0) {
        free(s);
        return 1;
    }

    free(s);
    return 0;
}
