// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/slicer_thresholds.h>
#include <stddef.h>

/* True when x can be converted to int: finite and inside [-2^31, 2^31). Both
 * bounds are exactly representable as float; NaN fails both comparisons. */
static int
slicer_level_fits_int(float x) {
    return x < 2147483648.0f && x >= -2147483648.0f;
}

void
dsd_engine_slicer_threshold_cache_init(dsd_engine_slicer_threshold_cache* cache) {
    if (cache == NULL) {
        return;
    }
    cache->last_max = 0;
    cache->last_min = 0;
    cache->primed = 0;
}

int
dsd_engine_slicer_thresholds_refresh(dsd_state* state, dsd_engine_slicer_threshold_cache* cache) {
    if (state == NULL || cache == NULL) {
        return -1;
    }
    if (!slicer_level_fits_int(state->max) || !slicer_level_fits_int(state->min)) {
        /* Converting a NaN, an infinity or an out-of-range float to int is
         * undefined; keep the last good thresholds until the extremes are sane
         * again. (This unit is built with -fno-fast-math so the check holds.) */
        return 0;
    }
    /* Truncation toward zero, exactly as the scanner loop has always done it;
     * see the header for why this is preserved rather than tightened. */
    const int current_max = (int)state->max;
    const int current_min = (int)state->min;
    if (cache->primed && current_max == cache->last_max && current_min == cache->last_min) {
        return 0;
    }
    state->center = (state->max + state->min) / 2;
    state->umid = ((state->max - state->center) * 5 / 8) + state->center;
    state->lmid = ((state->min - state->center) * 5 / 8) + state->center;
    cache->last_max = current_max;
    cache->last_min = current_min;
    cache->primed = 1;
    return 1;
}
