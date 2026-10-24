// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The P25p1 chain comparison's ending and the learned modulation it reports (dsd_p25p1_mod_compare). Kept apart from
 * dsd_frame_sync.c so the engine's tune tests link the real functions rather than copies of them. */

#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>

#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

void
frame_sync_p25p1_mod_compare_end(dsd_state* state) {
    state->p25_p1_mod_compare.phase = 0;
    state->p25_p1_mod_probe_active = 0;
}

int
frame_sync_p25p1_mod_compare_leader(const dsd_state* state) {
    const dsd_p25p1_mod_compare* cmp = &state->p25_p1_mod_compare;
    return (cmp->phase == 2 && cmp->trial_read > cmp->incumbent_read) ? !cmp->incumbent_rf_mod : cmp->incumbent_rf_mod;
}

int
dsd_frame_sync_p25p1_learned_rf_mod(const dsd_state* state) {
    if (!state) {
        return -1;
    }
    if (state->p25_p1_mod_compare.phase != 0) {
        return frame_sync_p25p1_mod_compare_leader(state);
    }
    return state->p25_p1_validated_rf_mod;
}

void
dsd_frame_sync_p25p1_mod_compare_end_for_tune(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_p25p1_mod_compare* cmp = &state->p25_p1_mod_compare;
    if (cmp->phase == 0) {
        return;
    }
    const int leader = frame_sync_p25p1_mod_compare_leader(state);
    state->p25_p1_validated_rf_mod = leader;
    cmp->seen_symbolcnt = state->p25_p1_validated_symbolcnt;
    /* The decoder's modulation goes back to the leading chain unless a Phase 2 grant has already claimed it: a TDMA
     * voice channel is CQPSK whatever the control channel's chain, and the grant says so by naming its slot
     * (p25_p2_active_slot) before it asks for the tune. */
    if (state->p25_p2_active_slot == -1 && state->rf_mod != leader) {
        state->rf_mod = leader;
        frame_sync_qpsk_dwell_set(leader == 1);
    }
    frame_sync_p25p1_mod_compare_end(state);
}
