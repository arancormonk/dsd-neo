// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Shared gain and autogain helpers for float and short audio paths.
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
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
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

void
audio_apply_gain_s16(short* buf, size_t n, float gain) {
    if (!buf) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = (short)(buf[i] * gain);
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

        // Preserve legacy averaging behavior (first 20 entries each block).
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

// Older float-path autogain used by DMR/P25 mixers.
// Behavior is intentionally preserved while splitting logic for readability.
void
agf(const dsd_opts* opts, dsd_state* state, float samp[160], int slot) {
    float empty[160];
    DSD_MEMSET(empty, 0.0f, sizeof(empty));

    float mmax = 0.90f;
    float mmin = -0.90f;
    float aavg = 0.0f;  //average of the absolute value
    float df = 3277.0f; //test value
    float gain = agf_effective_gain(opts, state);

    // Determine whether or not to run gain on 'empty' floating samples
    if (audio_is_all_zero_f(samp, 160)) {
        return;
    }

    for (int j = 0; j < 8; j++) {
        df = agf_slot_decimation(state, slot, df);
        agf_process_20_sample_block(samp, j, df, gain, mmin, mmax, &aavg);

        //debug
        // DSD_FPRINTF(stderr, "\nS%d - DF = %f AAVG = %f", slot, df, aavg);
        agf_update_slot_gain(state, slot, aavg);
        aavg = 0.0f; //reset
    }
}

// Automatic gain for short mono paths (analog and some digital mono).
// Kept identical to the original implementation in dsd_audio2.c.
void
agsm(dsd_opts* opts, dsd_state* state, short* input, int len) {
    UNUSED(opts);

    if (!input || len <= 0 || !state) {
        return;
    }

    /* Legacy target level for analog monitor path (PCM16 scale). */
    const float nom = 4800.0f;
    float max_abs = 0.0f;
    for (int i = 0; i < len; i++) {
        float v = fabsf((float)input[i]);
        if (v > max_abs) {
            max_abs = v;
        }
    }

    /* Avoid divide-by-zero on silence and keep behavior stable. */
    if (max_abs < 1e-6f) {
        max_abs = 1e-6f;
    }

    float coeff = fabsf(nom / max_abs);

    /* Keep coefficient in a conservative range to limit pumping/noise lift. */
    if (coeff > 3.0f) {
        coeff = 3.0f;
    }

    /* Apply gain over the full block with explicit int16 saturation. */
    for (int i = 0; i < len; i++) {
        float scaled = (float)input[i] * coeff;
        if (scaled > 32767.0f) {
            scaled = 32767.0f;
        } else if (scaled < -32768.0f) {
            scaled = -32768.0f;
        }
        input[i] = (short)scaled;
    }

    state->aout_gainA = coeff; //store for internal use
}

// Manual analog gain control; uses a simple scalar derived from opts.
void
analog_gain(const dsd_opts* opts, dsd_state* state, short* input, int len) {

    int i;
    UNUSED(state);

    float gain = (opts->audio_gainA / 100.0f) * 5.0f; //scale 0x - 5x

    for (i = 0; i < len; i++) {
        input[i] = (short)(input[i] * gain);
    }
}

// Automatic gain for float mono paths (analog monitor).
// Native float version avoids repeated short<->float conversions.
// Input is expected to be normalized ~[-1, 1] from the RTL demodulator.
// Output should be scaled to int16 range for PulseAudio playback.
void
agsm_f(dsd_opts* opts, dsd_state* state, float* input, int len) {
    int i;

    UNUSED(opts);

    float coeff = 0.0f;  //gain coefficient
    float max = 0.0f;    //the highest sample value
    float nom = 4800.0f; //target output level (int16 scale)

    // Find max absolute value
    for (i = 0; i < len; i++) {
        if (fabsf(input[i]) > max) {
            max = fabsf(input[i]);
        }
    }

    // Avoid division by zero
    if (max < 1e-6f) {
        max = 1e-6f;
    }

    coeff = fabsf(nom / max);

    // For normalized float input ~[-1,1], we need higher gain to reach int16 levels.
    // Cap at 6000 to prevent extreme amplification on very quiet signals.
    if (coeff > 6000.0f) {
        coeff = 6000.0f;
    }

    // Apply the coefficient to bring the max value to our desired maximum value
    for (i = 0; i < len; i++) {
        input[i] *= coeff;
    }

    state->aout_gainA = coeff; //store for internal use
}

// Manual analog gain control for float paths.
// Input may be normalized ~[-1, 1] (RTL) or PCM16-scale (WAV/other inputs).
// Uses audio_in_type to determine if base scaling is needed.
void
analog_gain_f(const dsd_opts* opts, dsd_state* state, float* input, int len) {

    int i;
    UNUSED(state);

    // RTL input (type 3) produces normalized [-1,1] samples and needs base scaling.
    // All other input types (WAV, Pulse, TCP, etc.) produce PCM16-scale samples.
    float base_scale = (opts->audio_in_type == AUDIO_IN_RTL) ? 4800.0f : 1.0f;

    // User gain: 0% to 100% maps to 0x to 5x
    float user_gain = (opts->audio_gainA / 100.0f) * 5.0f;
    float gain = base_scale * user_gain;

    for (i = 0; i < len; i++) {
        input[i] *= gain;
    }
}
