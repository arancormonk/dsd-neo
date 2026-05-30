// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "frame_sync_level.h"

#include <stddef.h>

void
dsd_frame_sync_estimate_sorted_window_levels(const float* sorted_levels, int count, float* out_min, float* out_max) {
    if (out_min == NULL || out_max == NULL) {
        return;
    }
    if (sorted_levels == NULL || count <= 0) {
        *out_min = 0.0f;
        *out_max = 0.0f;
        return;
    }
    if (count < 3) {
        float sum = 0.0f;
        for (int i = 0; i < count; i++) {
            sum += sorted_levels[i];
        }
        float avg = sum / (float)count;
        *out_min = avg;
        *out_max = avg;
        return;
    }

    int min_idx = 0;
    int max_idx = count - 3;
    if (count >= 13) {
        min_idx = 2;
        max_idx = count - 5;
    }
    if (max_idx + 2 >= count) {
        max_idx = count - 3;
    }

    *out_min = (sorted_levels[min_idx] + sorted_levels[min_idx + 1] + sorted_levels[min_idx + 2]) / 3.0f;
    *out_max = (sorted_levels[max_idx] + sorted_levels[max_idx + 1] + sorted_levels[max_idx + 2]) / 3.0f;
}
