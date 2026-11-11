// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Small mixing primitives for the short (int16) voice mixers.
 *
 * These helpers encapsulate common slot→stereo/mono mixing patterns so
 * that the higher-level mixers in dsd_audio2.c can delegate the inner
 * loops here.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/dsp/speech_agc.h>

#include <math.h>
#include <stddef.h>

void
audio_mix_interleave_stereo_s16(const short* left, const short* right, size_t n, int encL, int encR,
                                short* stereo_out) {
    if (!stereo_out || !left || !right) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        stereo_out[i * 2 + 0] = encL ? 0 : left[i];
        stereo_out[i * 2 + 1] = encR ? 0 : right[i];
    }
}

/* The short voice mixers' frames: 8 kHz, at most this many samples go through the limiter at once. */
enum { MIX_MONO_BLOCK = 160 };

static short
mix_to_s16(float v) {
    if (v >= 32767.0f) {
        return 32767;
    }
    if (v <= -32768.0f) {
        return -32768;
    }
    return (short)lrintf(v);
}

void
audio_mix_mono_sum_s16(const short* left, const short* right, size_t n, int l_real, int r_real,
                       dsd_peak_limiter* limiter, short* out) {
    if (!left || !right || !out) {
        return;
    }
    float mix[MIX_MONO_BLOCK];
    for (size_t at = 0; at < n; at += MIX_MONO_BLOCK) {
        const size_t m = (n - at < MIX_MONO_BLOCK) ? n - at : MIX_MONO_BLOCK;
        for (size_t i = 0; i < m; i++) {
            const int l = l_real ? left[at + i] : 0;
            const int r = r_real ? right[at + i] : 0;
            mix[i] = (float)(l + r);
        }
        // Every block goes through the limiter, so its gain releases on time whichever slots play.
        dsd_peak_limiter_process(limiter, mix, m);
        for (size_t i = 0; i < m; i++) {
            out[at + i] = mix_to_s16(mix[i]);
        }
    }
}
