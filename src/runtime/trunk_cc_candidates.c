// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stdint.h>
#include <stdlib.h>

#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"

static void
dsd_trunk_cc_candidates_clear(dsd_trunk_cc_candidates* cc) {
    cc->count = 0;
    cc->idx = 0;
    for (int k = 0; k < DSD_TRUNK_CC_CANDIDATES_MAX; k++) {
        cc->candidates[k] = 0;
        cc->flags[k] = 0;
        cc->cool_until[k] = 0.0;
    }
}

static int
dsd_trunk_cc_candidates_has_valid_count(dsd_trunk_cc_candidates* cc) {
    if (cc->count >= 0 && cc->count <= DSD_TRUNK_CC_CANDIDATES_MAX) {
        return 1;
    }
    dsd_trunk_cc_candidates_clear(cc);
    return 0;
}

static void
dsd_trunk_cc_candidates_drop_oldest(dsd_trunk_cc_candidates* cc) {
    for (int k = 1; k < DSD_TRUNK_CC_CANDIDATES_MAX; k++) {
        cc->candidates[k - 1] = cc->candidates[k];
        cc->flags[k - 1] = cc->flags[k];
        cc->cool_until[k - 1] = cc->cool_until[k];
    }
    if (cc->idx > 0) {
        cc->idx--;
    }
}

static int
dsd_trunk_cc_candidate_is_allowed(const dsd_trunk_cc_candidates* cc, int idx, long skip_cc_freq, double now_monotonic_s,
                                  uint8_t required_flags, long* out_freq_hz) {
    const uint8_t flags = cc->flags[idx];
    const long freq_hz = cc->candidates[idx];

    if (required_flags != 0 && (uint8_t)(flags & required_flags) != required_flags) {
        return 0;
    }
    if (freq_hz == 0 || freq_hz == skip_cc_freq) {
        return 0;
    }
    if (cc->cool_until[idx] > 0.0 && now_monotonic_s < cc->cool_until[idx]) {
        return 0;
    }

    *out_freq_hz = freq_hz;
    return 1;
}

dsd_trunk_cc_candidates*
dsd_trunk_cc_candidates_get(dsd_state* state) {
    if (!state) {
        return NULL;
    }

    dsd_trunk_cc_candidates* cc =
        DSD_STATE_EXT_GET_AS(dsd_trunk_cc_candidates, state, DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES);
    if (cc) {
        return cc;
    }

    cc = (dsd_trunk_cc_candidates*)calloc(1, sizeof(*cc));
    if (!cc) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES, cc, free) != 0) {
        free(cc);
        return NULL;
    }
    return cc;
}

const dsd_trunk_cc_candidates*
dsd_trunk_cc_candidates_peek(const dsd_state* state) {
    if (!state) {
        return NULL;
    }

    return DSD_STATE_EXT_GET_AS(const dsd_trunk_cc_candidates, (dsd_state*)state,
                                DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES);
}

int
dsd_trunk_cc_candidates_add(dsd_state* state, long freq_hz, int bump_added, uint8_t flags) {
    if (!state || freq_hz == 0) {
        return 0;
    }

    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(state);
    if (!cc) {
        return 0;
    }

    if (!dsd_trunk_cc_candidates_has_valid_count(cc)) {
        return 0;
    }

    for (int k = 0; k < cc->count; k++) {
        if (cc->candidates[k] == freq_hz) {
            cc->flags[k] |= flags;
            return 0;
        }
    }

    if (cc->count < DSD_TRUNK_CC_CANDIDATES_MAX) {
        cc->candidates[cc->count] = freq_hz;
        cc->flags[cc->count] = flags;
        cc->cool_until[cc->count] = 0.0;
        cc->count++;
    } else {
        dsd_trunk_cc_candidates_drop_oldest(cc);
        cc->candidates[DSD_TRUNK_CC_CANDIDATES_MAX - 1] = freq_hz;
        cc->flags[DSD_TRUNK_CC_CANDIDATES_MAX - 1] = flags;
        cc->cool_until[DSD_TRUNK_CC_CANDIDATES_MAX - 1] = 0.0;
    }

    if (bump_added) {
        cc->added++;
    }
    return 1;
}

int
dsd_trunk_cc_candidates_next(dsd_state* state, double now_monotonic_s, uint8_t required_flags, long* out_freq_hz) {
    if (!state || !out_freq_hz) {
        return 0;
    }

    dsd_trunk_cc_candidates* cc =
        DSD_STATE_EXT_GET_AS(dsd_trunk_cc_candidates, state, DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES);
    if (!cc) {
        return 0;
    }

    if (!dsd_trunk_cc_candidates_has_valid_count(cc)) {
        return 0;
    }

    const long skip_cc_freq = (state->trunk_cc_freq != 0) ? state->trunk_cc_freq : state->p25_cc_freq;

    for (int tries = 0; tries < cc->count; tries++) {
        if (cc->idx >= cc->count) {
            cc->idx = 0;
        }
        const int idx = cc->idx++;
        if (dsd_trunk_cc_candidate_is_allowed(cc, idx, skip_cc_freq, now_monotonic_s, required_flags, out_freq_hz)) {
            cc->used++;
            return 1;
        }
    }
    return 0;
}

void
dsd_trunk_cc_candidates_set_cooldown(const dsd_state* state, long freq_hz, double until_monotonic_s) {
    if (!state || freq_hz == 0) {
        return;
    }

    dsd_trunk_cc_candidates* cc =
        DSD_STATE_EXT_GET_AS(dsd_trunk_cc_candidates, state, DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES);
    if (!cc) {
        return;
    }

    for (int i = 0; i < cc->count && i < DSD_TRUNK_CC_CANDIDATES_MAX; i++) {
        if (cc->candidates[i] == freq_hz) {
            cc->cool_until[i] = until_monotonic_s;
            return;
        }
    }
}
