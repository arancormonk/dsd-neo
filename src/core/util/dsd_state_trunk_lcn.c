// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Scan-list heap tail (slots past the embedded trunk_lcn_freq[]).
 *
 * Kept out of dsd_init.c so test targets that link individual core sources
 * (menu services, UI snapshot, trunk scan) can attach the real implementation
 * without dragging in the full initState/freeState dependency chain.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

int
dsd_state_trunk_lcn_reserve(dsd_state* state, size_t count_needed) {
    if (!state) {
        return -1;
    }
    if (count_needed <= (size_t)DSD_TRUNK_LCN_EMBEDDED) {
        return 0;
    }
    const size_t ext_needed = count_needed - (size_t)DSD_TRUNK_LCN_EMBEDDED;
    if (ext_needed <= state->trunk_lcn_freq_ext_capacity) {
        return 0;
    }
    size_t capacity = state->trunk_lcn_freq_ext_capacity > 0 ? state->trunk_lcn_freq_ext_capacity : 16;
    while (capacity < ext_needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = ext_needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(long int)) {
        return -1;
    }
    long int* ext = (long int*)realloc(state->trunk_lcn_freq_ext, capacity * sizeof *ext);
    if (!ext) {
        return -1;
    }
    DSD_MEMSET(ext + state->trunk_lcn_freq_ext_capacity, 0,
               (capacity - state->trunk_lcn_freq_ext_capacity) * sizeof *ext);
    state->trunk_lcn_freq_ext = ext;
    state->trunk_lcn_freq_ext_capacity = capacity;
    return 0;
}

void
dsd_state_trunk_lcn_free(dsd_state* state) {
    if (!state) {
        return;
    }
    free(state->trunk_lcn_freq_ext);
    state->trunk_lcn_freq_ext = NULL;
    state->trunk_lcn_freq_ext_capacity = 0;
}

int
dsd_state_trunk_lcn_user_list_present(const dsd_opts* opts, const dsd_state* state) {
    if (opts && opts->chan_in_file[0] != '\0') {
        return 1;
    }
    if (!opts || !state || opts->trunk_scan_enabled != 1) {
        return 0;
    }
    return (state->lcn_freq_count > 1) ? 1 : 0;
}
