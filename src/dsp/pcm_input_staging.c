// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "pcm_input_staging.h"

#include <dsd-neo/core/opts.h>
#include <string.h>

static void
dsd_pcm_input_fill_staging(dsd_opts* opts, int factor, float invalue) {
    if (!opts || factor <= 0) {
        return;
    }
    for (int i = 0; i < factor; i++) {
        opts->input_upsample_buf[i] = invalue;
    }
    opts->input_upsample_len = factor;
}

static void
dsd_pcm_input_prime_resampler_history(dsd_opts* opts, float invalue) {
    if (!opts || !opts->input_resampler.hist || opts->input_resampler.taps_per_phase <= 0) {
        return;
    }

    int taps_per_phase = opts->input_resampler.taps_per_phase;
    for (int i = 0; i < taps_per_phase; i++) {
        opts->input_resampler.hist[i] = invalue;
        opts->input_resampler.hist[i + taps_per_phase] = invalue;
    }
    opts->input_resampler.hist_head = 0;
    opts->input_resampler.phase = 0;
}

static int
dsd_pcm_input_resampler_can_flush_tail(const dsd_opts* opts, int factor, size_t cap) {
    return opts && factor > 1 && factor <= (int)cap && opts->input_upsample_prev_valid && opts->input_resampler.enabled
           && opts->input_resampler.L == factor && opts->input_resampler.M == 1 && opts->input_resampler.taps != NULL
           && opts->input_resampler.hist != NULL && opts->input_resampler.taps_per_phase > 1;
}

int
dsd_pcm_input_uses_staged_resampler(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    switch (opts->audio_in_type) {
        case AUDIO_IN_STDIN:
        case AUDIO_IN_WAV:
        case AUDIO_IN_UDP:
        case AUDIO_IN_TCP: return dsd_opts_input_upsample_factor(opts) > 1;
        default: return 0;
    }
}

void
dsd_pcm_input_stage_resample(dsd_opts* opts, float invalue) {
    int factor = dsd_opts_input_upsample_factor(opts);
    size_t cap = sizeof(opts->input_upsample_buf) / sizeof(opts->input_upsample_buf[0]);
    if (!opts || factor <= 1) {
        return;
    }

    opts->input_upsample_len = 0;
    opts->input_upsample_pos = 0;
    opts->input_upsample_tail_blocks = 0;
    if (factor > (int)cap) {
        return;
    }

    if (!opts->input_resampler.enabled || opts->input_resampler.L != factor || opts->input_resampler.M != 1
        || opts->input_resampler.taps == NULL || opts->input_resampler.hist == NULL) {
        dsd_resampler_reset(&opts->input_resampler);
        if (!dsd_resampler_design(&opts->input_resampler, factor, 1)) {
            dsd_pcm_input_fill_staging(opts, factor, invalue);
            opts->input_upsample_prev = invalue;
            opts->input_upsample_prev_valid = 1;
            return;
        }
    }

    if (!opts->input_upsample_prev_valid) {
        dsd_pcm_input_prime_resampler_history(opts, invalue);
        dsd_pcm_input_fill_staging(opts, factor, invalue);
        opts->input_upsample_prev = invalue;
        opts->input_upsample_prev_valid = 1;
        return;
    }

    int wrote = dsd_resampler_process_block(&opts->input_resampler, &invalue, 1, opts->input_upsample_buf, factor);
    if (wrote <= 0) {
        dsd_pcm_input_fill_staging(opts, factor, invalue);
        wrote = factor;
    }
    opts->input_upsample_len = wrote;
    opts->input_upsample_prev = invalue;
    opts->input_upsample_prev_valid = 1;
}

int
dsd_pcm_input_begin_resampler_tail(dsd_opts* opts) {
    int factor = dsd_opts_input_upsample_factor(opts);
    size_t cap = sizeof(opts->input_upsample_buf) / sizeof(opts->input_upsample_buf[0]);

    if (!dsd_pcm_input_resampler_can_flush_tail(opts, factor, cap)) {
        return 0;
    }
    if (opts->input_upsample_tail_blocks > 0) {
        return 1;
    }

    opts->input_upsample_tail_blocks = opts->input_resampler.taps_per_phase - 1;
    return opts->input_upsample_tail_blocks > 0;
}

int
dsd_pcm_input_stage_resampler_tail(dsd_opts* opts) {
    int factor = dsd_opts_input_upsample_factor(opts);
    size_t cap = sizeof(opts->input_upsample_buf) / sizeof(opts->input_upsample_buf[0]);
    float pad = 0.0f;

    if (!dsd_pcm_input_resampler_can_flush_tail(opts, factor, cap) || opts->input_upsample_tail_blocks <= 0) {
        return 0;
    }

    opts->input_upsample_len = 0;
    opts->input_upsample_pos = 0;

    int wrote = dsd_resampler_process_block(&opts->input_resampler, &pad, 1, opts->input_upsample_buf, factor);
    if (wrote <= 0) {
        opts->input_upsample_tail_blocks = 0;
        dsd_resampler_clear_history(&opts->input_resampler);
        opts->input_upsample_prev = 0.0f;
        opts->input_upsample_prev_valid = 0;
        return 0;
    }

    opts->input_upsample_len = wrote;
    opts->input_upsample_tail_blocks--;
    if (opts->input_upsample_tail_blocks == 0) {
        dsd_resampler_clear_history(&opts->input_resampler);
        opts->input_upsample_prev = 0.0f;
        opts->input_upsample_prev_valid = 0;
    }
    return 1;
}
