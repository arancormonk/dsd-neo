// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The P25 producers of the verified talkgroup proof (issue #903); see p25_heard_tg.h. */

#include "p25_heard_tg.h"

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/heard_talkgroup.h>
#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* 1 when an active entry of the patch table names @p group as its supergroup, whatever its freshness. */
static int
p25_heard_tg_patch_names_sg(const dsd_state* state, uint32_t group) {
    if (!state || group == 0U || state->p25_patch_count <= 0) {
        return 0;
    }
    const int count = state->p25_patch_count > DSD_P25_PATCH_MAX ? DSD_P25_PATCH_MAX : state->p25_patch_count;
    for (int i = 0; i < count; i++) {
        if (state->p25_patch_active[i] && state->p25_patch_sgid[i] == group) {
            return 1;
        }
    }
    return 0;
}

/* 1 while @p call is an ACTIVE P25 group call naming @p group. */
static int
p25_heard_tg_call_names(const dsd_call_snapshot* call, uint32_t group) {
    return group != 0U && call->phase == DSD_CALL_PHASE_ACTIVE && call->kind == DSD_CALL_KIND_GROUP_VOICE
           && call->ota_target_id == (uint64_t)group && DSD_SYNC_IS_P25(call->protocol);
}

void
p25_heard_tg_note(const dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call, uint32_t group,
                  int supergroup) {
    if (!state || !call || call->heard_noted != 0U || !p25_heard_tg_call_names(call, group)) {
        return;
    }
    dsd_heard_tg_input in;
    DSD_MEMSET(&in, 0, sizeof in);
    in.variant = DSD_HEARD_TG_P25;
    in.id = group;
    /* Read now and frozen with the proof: a patch that expires before the call commits changes nothing. */
    in.p25_supergroup = (uint8_t)(supergroup != 0 || p25_heard_tg_patch_names_sg(state, group));
    dsd_call_state_note_verified_target(opts, state, slot, dsd_call_snapshot_transmission(call), &in);
}

static void
p25p2_heard_tg_clear(dsd_state* state, uint8_t slot) {
    state->p25_p2_heard_vu_epoch[slot] = 0U;
    state->p25_p2_heard_vu_group[slot] = 0U;
    state->p25_p2_heard_vu_regroup[slot] = 0U;
}

/* The slot's ACTIVE P25 Phase 2 call naming @p group, into @p call; 0 for none. */
static int
p25p2_heard_tg_call(const dsd_state* state, uint8_t slot, uint32_t group, dsd_call_snapshot* call) {
    return dsd_call_state_get(state, slot, call) > 0 && p25_heard_tg_call_names(call, group)
           && DSD_SYNC_IS_P25P2(call->protocol);
}

void
p25p2_heard_tg_voice_user(dsd_state* state, uint8_t slot, uint32_t group, int regroup) {
    if (!state || slot > 1U) {
        return;
    }
    dsd_call_snapshot call;
    if (!p25p2_heard_tg_call(state, slot, group, &call) || call.heard_noted != 0U) {
        p25p2_heard_tg_clear(state, slot);
        return;
    }
    /* A regroup voice user's own word that the group is a supergroup holds for the rest of its call. */
    const int kept_regroup = state->p25_p2_heard_vu_epoch[slot] == call.epoch
                             && state->p25_p2_heard_vu_group[slot] == group
                             && state->p25_p2_heard_vu_regroup[slot] != 0U;
    state->p25_p2_heard_vu_epoch[slot] = call.epoch;
    state->p25_p2_heard_vu_group[slot] = group;
    state->p25_p2_heard_vu_regroup[slot] = (uint8_t)(regroup != 0 || kept_regroup);
}

void
p25p2_heard_tg_traffic(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    if (!state || slot > 1U || state->p25_p2_heard_vu_epoch[slot] == 0U) {
        return;
    }
    const uint64_t epoch = state->p25_p2_heard_vu_epoch[slot];
    const uint32_t group = state->p25_p2_heard_vu_group[slot];
    const int regroup = state->p25_p2_heard_vu_regroup[slot] != 0U;
    /* Consumed whatever the slot carries now: another epoch (a call the voice user did not describe, or one an
       addressless voice start reopened or reacquired after the call ended), or a call naming another group, is not
       proven by it, and the next voice user of a call still waiting sets it again. */
    p25p2_heard_tg_clear(state, slot);
    dsd_call_snapshot call;
    if (p25p2_heard_tg_call(state, slot, group, &call) && call.epoch == epoch) {
        p25_heard_tg_note(opts, state, slot, &call, group, regroup);
    }
}

void
p25p2_heard_tg_ptt(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint32_t group) {
    if (!state || slot > 1U) {
        return;
    }
    dsd_call_snapshot call;
    if (!p25p2_heard_tg_call(state, slot, group, &call)) {
        return;
    }
    const int waiting = state->p25_p2_heard_vu_epoch[slot] == call.epoch && state->p25_p2_heard_vu_group[slot] == group;
    const int regroup = waiting && state->p25_p2_heard_vu_regroup[slot] != 0U;
    if (waiting) {
        p25p2_heard_tg_clear(state, slot);
    }
    p25_heard_tg_note(opts, state, slot, &call, group, regroup);
}
