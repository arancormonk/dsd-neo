// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/state.h>

#define DSD_NEO_MAIN
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>

#undef DSD_NEO_MAIN

#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/state_fwd.h"

int
main(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    // Ensure initState explicitly resets fields even if caller pre-seeded them.
    state->rc2_context = state;
    state->nid_corrections_total = 12;
    state->nid_failures_total = 34;
    state->nid_parity_overrides = 56;
    state->trunk_chan_map[0x0123] = 851000000L;
    state->trunk_chan_map_used[0] = 0x0123U;
    state->trunk_chan_map_used_count = 1U;
    state->trunk_chan_map_seq = 99U;
    state->rtl_symbol_cache[0] = 1234.0f;
    state->rtl_symbol_cache[DSD_RTL_SYMBOL_CACHE_CAP - 1] = 5678.0f;
    state->rtl_symbol_cache_pos = 2;
    state->rtl_symbol_cache_len = 4;
    state->rtl_symbol_cache_output_kind = 3;
    state->rtl_symbol_cache_channel_profile = 5;
    state->rtl_symbol_cache_symbol_rate_hz = 6000;
    state->rtl_symbol_cache_levels = 4;
    state->rtl_symbol_cache_generation = 42U;
    state->rtl_symbol_cache_published_pending = 2;
    state->ess_b[0][95] = 1;
    state->ess_b_llr[1][95] = 123;
    state->fourv_counter[0] = 2;
    state->p25_p1_soft_hamming_ok = 77U;
    initState(state);

    if (state->rc2_context != NULL) {
        fprintf(stderr, "expected rc2_context to be NULL after initState\n");
        // Avoid invalid free if initialization regresses.
        state->rc2_context = NULL;
        freeState(state);
        free(state);
        return 2;
    }
    if (!state->dmr_reliab_buf || state->dmr_reliab_p != state->dmr_reliab_buf + 200 || !state->dmr_soft_buf
        || state->dmr_soft_p != state->dmr_soft_buf + 200) {
        fprintf(stderr, "initState did not allocate/reset dibit soft-decision buffers\n");
        freeState(state);
        free(state);
        return 9;
    }
    for (int i = 0; i < 200; i++) {
        if (state->dmr_reliab_buf[i] != 0 || state->dmr_soft_buf[i].reliability != 0
            || state->dmr_soft_buf[i].llr[0] != 0 || state->dmr_soft_buf[i].llr[1] != 0) {
            fprintf(stderr, "initState did not clear dibit soft-decision buffer prefix\n");
            freeState(state);
            free(state);
            return 10;
        }
    }
    if (state->nid_corrections_total != 0 || state->nid_failures_total != 0 || state->nid_parity_overrides != 0) {
        fprintf(stderr, "expected NID counters to be reset after initState\n");
        freeState(state);
        free(state);
        return 3;
    }
    if (state->ess_b[0][95] != 0 || state->ess_b_llr[1][95] != 0 || state->fourv_counter[0] != 0
        || state->fourv_counter[1] != 0) {
        fprintf(stderr, "initState did not clear P25P2 ESS fragment state\n");
        freeState(state);
        free(state);
        return 11;
    }
    if (state->p25_p1_soft_hamming_ok != 0U) {
        fprintf(stderr, "initState did not clear P25P1 soft Hamming telemetry\n");
        freeState(state);
        free(state);
        return 12;
    }

    if (state->trunk_chan_map_used_count != 0U || state->trunk_chan_map[0x0123] != 0
        || state->trunk_chan_map_seq != 0U) {
        fprintf(stderr, "initState did not clear trunk channel-map sparse state\n");
        freeState(state);
        free(state);
        return 4;
    }

    if (state->rtl_symbol_cache_pos != 0 || state->rtl_symbol_cache_len != 0 || state->rtl_symbol_cache_output_kind != 0
        || state->rtl_symbol_cache_channel_profile != 0 || state->rtl_symbol_cache_symbol_rate_hz != 0
        || state->rtl_symbol_cache_levels != 0 || state->rtl_symbol_cache_generation != 0U
        || state->rtl_symbol_cache_published_pending != 0 || state->rtl_symbol_cache[0] != 0.0f
        || state->rtl_symbol_cache[DSD_RTL_SYMBOL_CACHE_CAP - 1] != 0.0f) {
        fprintf(stderr, "initState did not clear RTL symbol cache state\n");
        freeState(state);
        free(state);
        return 5;
    }

    for (int i = 0; i < 4; i++) {
        state->minbuf[i] = (float)(-10 - i);
        state->maxbuf[i] = (float)(10 + i);
    }
    state->midx = 1;
    dsd_state_invalidate_minmax_sums(state);
    dsd_state_push_minmax_window(state, 4, -20.0f, 20.0f);
    if (state->midx != 2 || state->minmax_sum_window != 4 || state->min != -13.75f || state->max != 13.75f) {
        fprintf(stderr, "min/max rolling window mismatch: midx=%d window=%d min=%.2f max=%.2f\n", state->midx,
                state->minmax_sum_window, (double)state->min, (double)state->max);
        freeState(state);
        free(state);
        return 6;
    }

    dsd_state_set_trunk_chan_freq(state, 0x1234U, 851500000L);
    dsd_state_set_trunk_chan_freq(state, 0x0123U, 851000000L);
    dsd_state_set_trunk_chan_freq(state, 0U, 769006250L);
    if (state->trunk_chan_map_used_count != 2U || state->trunk_chan_map_used[0] != 0x0123U
        || state->trunk_chan_map_used[1] != 0x1234U || state->trunk_chan_map[0x1234] != 851500000L
        || state->trunk_chan_map[0] != 769006250L) {
        fprintf(stderr, "trunk channel-map sparse index mismatch\n");
        freeState(state);
        free(state);
        return 7;
    }

    dsd_state_set_trunk_chan_freq(state, 0x1234U, 0);
    if (state->trunk_chan_map_used_count != 1U || state->trunk_chan_map_used[0] != 0x0123U
        || state->trunk_chan_map[0x1234] != 0) {
        fprintf(stderr, "trunk channel-map sparse removal mismatch\n");
        freeState(state);
        free(state);
        return 8;
    }

    freeState(state);
    free(state);
    return 0;
}
