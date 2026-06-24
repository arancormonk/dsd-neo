// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <math.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/dsp/resampler.h"
#include "pcm_input_staging.h"

static void
expect_close(const char* label, float actual, float expected, float tol) {
    if (fabsf(actual - expected) > tol) {
        DSD_FPRINTF(stderr, "%s: expected %.3f got %.3f\n", label, expected, actual);
        assert(0);
    }
}

static void
test_public_guards_and_disabled_factor(void) {
    assert(dsd_pcm_input_uses_staged_resampler(NULL) == 0);
    dsd_pcm_input_stage_resample(NULL, 1200.0f);
    assert(dsd_pcm_input_begin_resampler_tail(NULL) == 0);
    assert(dsd_pcm_input_stage_resampler_tail(NULL) == 0);

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.wav_sample_rate = 8000;
    assert(dsd_opts_input_upsample_factor(&opts) == 6);
    assert(dsd_pcm_input_uses_staged_resampler(&opts) == 0);

    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.input_upsample_len = 3;
    opts.input_upsample_pos = 2;
    opts.input_upsample_tail_blocks = 1;
    opts.input_upsample_prev = 42.0f;
    opts.input_upsample_prev_valid = 1;
    dsd_pcm_input_stage_resample(&opts, -120.0f);
    assert(opts.input_upsample_len == 3);
    assert(opts.input_upsample_pos == 2);
    assert(opts.input_upsample_tail_blocks == 1);
    assert(opts.input_upsample_prev == 42.0f);
    assert(opts.input_upsample_prev_valid == 1);
}

static void
test_staged_resampler_lifecycle(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
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
        DSD_SNPRINTF(label, sizeof(label), "first staged sample %d", i);
        expect_close(label, opts.input_upsample_buf[i], 1200.0f, 0.01f);
    }

    dsd_pcm_input_stage_resample(&opts, -850.0f);
    assert(opts.input_upsample_prev_valid == 1);
    assert(opts.input_resampler.taps_per_phase > 1);
    assert(dsd_pcm_input_begin_resampler_tail(&opts) == 1);

    int expected_tail_blocks = opts.input_resampler.taps_per_phase - 1;
    assert(opts.input_upsample_tail_blocks == expected_tail_blocks);
    assert(dsd_pcm_input_begin_resampler_tail(&opts) == 1);
    assert(opts.input_upsample_tail_blocks == expected_tail_blocks);

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
        DSD_SNPRINTF(label, sizeof(label), "reprimed staged sample %d", i);
        expect_close(label, opts.input_upsample_buf[i], -850.0f, 0.01f);
    }

    dsd_resampler_reset(&opts.input_resampler);
}

static void
test_stage_falls_back_when_resampler_state_rejects_block(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.wav_sample_rate = 8000;

    const int factor = dsd_opts_input_upsample_factor(&opts);
    dsd_pcm_input_stage_resample(&opts, 300.0f);
    assert(opts.input_resampler.enabled == 1);
    opts.input_resampler.phase = factor + 1;

    dsd_pcm_input_stage_resample(&opts, -125.0f);
    assert(opts.input_upsample_len == factor);
    assert(opts.input_upsample_pos == 0);
    assert(opts.input_upsample_prev_valid == 1);
    assert(opts.input_upsample_prev == -125.0f);
    for (int i = 0; i < factor; i++) {
        char label[64];
        DSD_SNPRINTF(label, sizeof(label), "fallback staged sample %d", i);
        expect_close(label, opts.input_upsample_buf[i], -125.0f, 0.01f);
    }

    dsd_resampler_reset(&opts.input_resampler);
}

static void
test_tail_flush_clears_state_when_resampler_rejects_block(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.wav_sample_rate = 8000;

    const int factor = dsd_opts_input_upsample_factor(&opts);
    dsd_pcm_input_stage_resample(&opts, 50.0f);
    dsd_pcm_input_stage_resample(&opts, 75.0f);
    assert(dsd_pcm_input_begin_resampler_tail(&opts) == 1);

    opts.input_resampler.phase = factor + 1;
    assert(dsd_pcm_input_stage_resampler_tail(&opts) == 0);
    assert(opts.input_upsample_tail_blocks == 0);
    assert(opts.input_resampler.phase == 0);
    assert(opts.input_resampler.hist_head == 0);
    assert(opts.input_upsample_prev == 0.0f);
    assert(opts.input_upsample_prev_valid == 0);

    for (int i = 0; i < opts.input_resampler.taps_per_phase * 2; i++) {
        char label[64];
        DSD_SNPRINTF(label, sizeof(label), "cleared history sample %d", i);
        expect_close(label, opts.input_resampler.hist[i], 0.0f, 0.0001f);
    }

    dsd_resampler_reset(&opts.input_resampler);
}

int
main(void) {
    test_public_guards_and_disabled_factor();
    test_staged_resampler_lifecycle();
    test_stage_falls_back_when_resampler_state_rejects_block();
    test_tail_flush_clears_state_when_resampler_rejects_block();
    return 0;
}
