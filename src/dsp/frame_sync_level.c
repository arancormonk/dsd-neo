// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frame_sync_level.h"

#include <float.h> // IWYU pragma: keep
#include <stddef.h>

// IWYU pragma: no_include <__float_float.h>

// Ranks read from the retained endpoints: with lmn ascending and lmx
// descending, sums are accumulated in the same ascending value order the
// previous full sort produced, so results are bit-identical.

// Retain the five smallest values seen so far in lmn, ascending.
static void
frame_sync_level_track_low(float lmn[5], float x) {
    if (x < lmn[4]) {
        int j = 4;
        for (; j > 0 && x < lmn[j - 1]; j--) {
            lmn[j] = lmn[j - 1];
        }
        lmn[j] = x;
    }
}

// Retain the five largest values seen so far in lmx, descending.
static void
frame_sync_level_track_high(float lmx[5], float x) {
    if (x > lmx[4]) {
        int j = 4;
        for (; j > 0 && x > lmx[j - 1]; j--) {
            lmx[j] = lmx[j - 1];
        }
        lmx[j] = x;
    }
}

void
dsd_frame_sync_estimate_window_levels(const float* levels, int count, float* out_min, float* out_max) {
    if (out_min == NULL || out_max == NULL) {
        return;
    }
    if (levels == NULL || count <= 0) {
        *out_min = 0.0f;
        *out_max = 0.0f;
        return;
    }

    float lmn[5] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    float lmx[5] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int i = 0; i < count; i++) {
        const float x = levels[i];
        frame_sync_level_track_low(lmn, x);
        frame_sync_level_track_high(lmx, x);
    }

    if (count < 3) {
        float sum = (count == 1) ? lmn[0] : (lmn[0] + lmx[0]);
        float avg = sum / (float)count;
        *out_min = avg;
        *out_max = avg;
        return;
    }

    const int min_idx = (count >= 13) ? 2 : 0;
    *out_min = (lmn[min_idx] + lmn[min_idx + 1] + lmn[min_idx + 2]) / 3.0f;
    if (count >= 13) {
        *out_max = (lmx[4] + lmx[3] + lmx[2]) / 3.0f;
    } else {
        *out_max = (lmx[2] + lmx[1] + lmx[0]) / 3.0f;
    }
}
