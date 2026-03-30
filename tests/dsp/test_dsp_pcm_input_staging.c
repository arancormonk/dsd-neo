// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/dsp/resampler.h"
#include "pcm_input_staging.h"

static void
expect_close(const char* label, float actual, float expected, float tol) {
    if (fabsf(actual - expected) > tol) {
        fprintf(stderr, "%s: expected %.3f got %.3f\n", label, expected, actual);
        assert(0);
    }
}

int
main(void) {
    dsd_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.wav_sample_rate = 8000;

    const int factor = dsd_opts_input_upsample_factor(&opts);
    assert(factor == 6);
    assert(dsd_pcm_input_uses_staged_resampler(&opts) == 1);

    dsd_pcm_input_stage_resample(&opts, 1200.0f);
    assert(opts.input_resampler.enabled == 1);
    assert(opts.input_resampler.taps != NULL);
    assert(opts.input_resampler.hist != NULL);
    assert(opts.input_upsample_len == factor);
    assert(opts.input_upsample_pos == 0);
    assert(opts.input_upsample_prev_valid == 1);
    for (int i = 0; i < factor; i++) {
        char label[64];
        snprintf(label, sizeof(label), "first staged sample %d", i);
        expect_close(label, opts.input_upsample_buf[i], 1200.0f, 0.01f);
    }

    dsd_pcm_input_stage_resample(&opts, -850.0f);
    assert(opts.input_upsample_prev_valid == 1);
    assert(opts.input_resampler.taps_per_phase > 1);
    assert(dsd_pcm_input_begin_resampler_tail(&opts) == 1);

    int expected_tail_blocks = opts.input_resampler.taps_per_phase - 1;
    int flushed_tail_blocks = 0;
    int flushed_tail_samples = 0;
    while (dsd_pcm_input_stage_resampler_tail(&opts)) {
        flushed_tail_blocks++;
        flushed_tail_samples += opts.input_upsample_len;
        assert(opts.input_upsample_len == factor);
        assert(opts.input_upsample_pos == 0);
    }
    assert(flushed_tail_blocks == expected_tail_blocks);
    assert(flushed_tail_samples == expected_tail_blocks * factor);
    assert(opts.input_upsample_tail_blocks == 0);
    assert(opts.input_upsample_prev == 0.0f);
    assert(opts.input_upsample_prev_valid == 0);
    assert(dsd_pcm_input_stage_resampler_tail(&opts) == 0);

    dsd_opts_reset_input_upsample_state(&opts);
    dsd_pcm_input_stage_resample(&opts, -850.0f);
    assert(opts.input_upsample_len == factor);
    assert(opts.input_upsample_pos == 0);
    assert(opts.input_upsample_tail_blocks == 0);
    assert(opts.input_upsample_prev_valid == 1);
    for (int i = 0; i < factor; i++) {
        char label[64];
        snprintf(label, sizeof(label), "reprimed staged sample %d", i);
        expect_close(label, opts.input_upsample_buf[i], -850.0f, 0.01f);
    }

    dsd_resampler_reset(&opts.input_resampler);
    return 0;
}
