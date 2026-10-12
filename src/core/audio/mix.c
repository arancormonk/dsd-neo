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

void
audio_mix_mono_from_slots_s16(const short* left, const short* right, size_t n, int l_on, int r_on, short* out) {
    if (!left || !right || !out) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (l_on && r_on) {
            out[i] = (short)(((int)left[i] + (int)right[i]) / 2);
        } else if (l_on) {
            out[i] = left[i];
        } else if (r_on) {
            out[i] = right[i];
        } else {
            out[i] = 0;
        }
    }
}
