// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Shared gain helpers for audio paths. The digital voice gain, the same for both output formats, is the voice chain's
 * (dsd_voice_chain_run(), <dsd-neo/core/voice_chain.h>, issue #653).
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

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
