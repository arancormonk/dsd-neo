// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Shared gain and autogain helpers for audio paths.
 *
 * This module centralizes gain logic so that the core mixers in
 * dsd_audio2.c can act as thin orchestrators that delegate to these
 * helpers instead of inlining gain math.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <math.h>
#include <stddef.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static inline int
audio_is_all_zero_f(const float* buf, size_t n) {
    if (!buf) {
        return 1;
    }
    const float eps = 1e-12f;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] > eps || buf[i] < -eps) {
            return 0;
        }
    }
    return 1;
}

void
audio_apply_gain_f32(float* buf, size_t n, float gain) {
    if (!buf) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] *= gain;
    }
}

static inline float
agf_effective_gain(const dsd_opts* opts, const dsd_state* state) {
    float gain = 1.0f;

    if (state->payload_algid == 0x21 || state->payload_algidR == 0x21) {
        gain = 1.75f;
    }
    if (opts->audio_gain != 0) {
        gain = opts->audio_gain / 25.0f;
    }
    return gain;
}

static inline float
agf_slot_decimation(const dsd_state* state, int slot, float fallback) {
    if (slot == 0) {
        return 384.0f * (50.0f - state->aout_gain);
    }
    if (slot == 1) {
        return 384.0f * (50.0f - state->aout_gainR);
    }
    return fallback;
}

static inline float
agf_clip_sample(float v, float lo, float hi) {
    if (v > hi) {
        return hi;
    }
    if (v < lo) {
        return lo;
    }
    return v;
}

static void
agf_process_20_sample_block(float samp[160], int block_idx, float df, float gain, float mmin, float mmax, float* aavg) {
    for (int i = 0; i < 20; i++) {
        int idx = (block_idx * 20) + i;
        samp[idx] = samp[idx] / df;
        samp[idx] = agf_clip_sample(samp[idx], mmin, mmax);

        // Preserve established averaging behavior (first 20 entries each block).
        *aavg += fabsf(samp[i]);
        samp[idx] *= gain * 0.8f;
    }

    *aavg /= 20.0f;
}

static void
agf_update_slot_gain(dsd_state* state, int slot, float aavg) {
    if (slot == 0) {
        if (aavg < 0.075f && state->aout_gain < 46.0f) {
            state->aout_gain += 0.5f;
        }
        if (aavg >= 0.075f && state->aout_gain > 1.0f) {
            state->aout_gain -= 0.5f;
        }
    }

    if (slot == 1) {
        if (aavg < 0.075f && state->aout_gainR < 46.0f) {
            state->aout_gainR += 0.5f;
        }
        if (aavg >= 0.075f && state->aout_gainR > 1.0f) {
            state->aout_gainR -= 0.5f;
        }
    }
}

// Float-path autogain used by DMR/P25 mixers.
void
agf(const dsd_opts* opts, dsd_state* state, float samp[160], int slot) {
    float mmax = 0.90f;
    float mmin = -0.90f;
    float aavg = 0.0f;  //average of the absolute value
    float df = 3277.0f; //test value
    float gain = agf_effective_gain(opts, state);

    // Skip silent blocks.
    if (audio_is_all_zero_f(samp, 160)) {
        return;
    }

    for (int j = 0; j < 8; j++) {
        df = agf_slot_decimation(state, slot, df);
        agf_process_20_sample_block(samp, j, df, gain, mmin, mmax, &aavg);

        agf_update_slot_gain(state, slot, aavg);
        aavg = 0.0f; //reset
    }
}

// Manual analog gain for the M17 encoder's microphone input (-n N, N > 0): 0..100% maps to 0x..5x. The analog monitor's
// gain stage, fixed or automatic, is dsd_analog_audio_process_f().
void
analog_gain(const dsd_opts* opts, dsd_state* state, short* input, int len) {

    int i;
    UNUSED(state);

    /* 0% to 100% maps to 0x to 5x. One multiply by a literal, so fast-math has no division to turn into an inexact
       reciprocal; 0.05f rounds up, so a whole-number setting never lands a step low under the truncating cast. */
    float gain = opts->audio_gainA * 0.05f;

    /* Saturate to int16: above 1x a loud sample leaves the range, and converting it to short would wrap it to the
       opposite polarity. */
    for (i = 0; i < len; i++) {
        float scaled = (float)input[i] * gain;
        if (scaled > 32767.0f) {
            scaled = 32767.0f;
        } else if (scaled < -32768.0f) {
            scaled = -32768.0f;
        }
        input[i] = (short)scaled;
    }
}
