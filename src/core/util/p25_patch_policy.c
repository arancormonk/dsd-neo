// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The talkgroup policy of a patched P25 call (see <dsd-neo/core/p25_patch_policy.h>): one resolver over the
 * supergroup and its current members, pure over the current state.
 */

#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/p25_patch_policy.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include "../audio/dsd_audio_gate_internal.h"
#include "talkgroup_policy_internal.h"

typedef struct {
    const dsd_opts* opts;
    const dsd_state* state;
    uint32_t src;
    int encrypted;
    int data_call;
    dsd_p25_patch_resolve_purpose purpose;
    dsd_p25_patch_adjust_fn adjust;
    const void* adjust_ctx;
} p25_patch_resolve_args;

static int
p25_patch_policy_entry_count(const dsd_state* state) {
    if (!state || state->p25_patch_count < 0) {
        return 0;
    }
    return state->p25_patch_count > DSD_P25_PATCH_MAX ? DSD_P25_PATCH_MAX : state->p25_patch_count;
}

/* An active entry for @p sg whose SG and members are both current. */
static int
p25_patch_policy_entry_live(const dsd_state* state, int idx, uint32_t sg) {
    return state->p25_patch_active[idx] && state->p25_patch_sgid[idx] == sg
           && dsd_p25_patch_entry_seen_current(state, idx) && dsd_p25_patch_members_current(state, idx);
}

int
dsd_p25_patch_sg_live(const dsd_state* state, uint32_t sg) {
    if (!state || sg == 0U) {
        return 0;
    }
    const int count = p25_patch_policy_entry_count(state);
    for (int i = 0; i < count; i++) {
        if (p25_patch_policy_entry_live(state, i, sg) && state->p25_patch_wgid_count[i] > 0U) {
            return 1;
        }
    }
    return 0;
}

int
dsd_p25_patch_media_eligible(const dsd_tg_policy_decision* decision) {
    return dsd_audio_policy_tg_allows_audio(decision);
}

int
dsd_p25_patch_is_current_member(const dsd_state* state, uint32_t sg, uint32_t wg) {
    if (!state || sg == 0U || wg == 0U || sg == wg || wg > UINT16_MAX) {
        return 0;
    }
    const int count = p25_patch_policy_entry_count(state);
    for (int i = 0; i < count; i++) {
        if (p25_patch_policy_entry_live(state, i, sg) && dsd_p25_patch_has_current_wgid(state, i, (uint16_t)wg)) {
            return 1;
        }
    }
    return 0;
}

static void
p25_patch_policy_evaluate(const p25_patch_resolve_args* args, uint32_t target, dsd_tg_policy_decision* out) {
    (void)dsd_tg_policy_evaluate_group_call(args->opts, args->state, target, args->src, args->encrypted,
                                            args->data_call, out);
    if (args->adjust) {
        args->adjust(target, out, args->adjust_ctx);
    }
}

static int
p25_patch_policy_eligible(dsd_p25_patch_resolve_purpose purpose, const dsd_tg_policy_decision* decision) {
    return purpose == DSD_P25_PATCH_RESOLVE_ADMIT ? decision->tg_tune_allowed != 0
                                                  : dsd_p25_patch_media_eligible(decision);
}

/* Whether eligible @p candidate outranks eligible @p best; a full tie keeps @p best (the SG comes first). */
static int
p25_patch_policy_better(dsd_p25_patch_resolve_purpose purpose, const dsd_tg_policy_decision* candidate,
                        const dsd_tg_policy_decision* best) {
    const int candidate_hold = candidate->tg_hold_match != 0;
    const int best_hold = best->tg_hold_match != 0;
    if (candidate_hold != best_hold) {
        return candidate_hold;
    }
    if (purpose == DSD_P25_PATCH_RESOLVE_ADMIT) {
        const int candidate_media = dsd_p25_patch_media_eligible(candidate) != 0;
        const int best_media = dsd_p25_patch_media_eligible(best) != 0;
        if (candidate_media != best_media) {
            return candidate_media;
        }
    }
    if (candidate->priority != best->priority) {
        return candidate->priority > best->priority;
    }
    return purpose == DSD_P25_PATCH_RESOLVE_ADMIT && candidate->preempt_requested && !best->preempt_requested;
}

/* For media only a call skip or a session avoid on the SG ends resolution. An explicit mode B/DE and the encryption
   lockout are merged into each member's decision as dsd_tg_policy_apply_ota_final_blocks() merges them: a mode block
   then leaves a member playable only through a matching Hold (as a Hold plays a plain call's mode block), and the
   lockout only denies tuning. For ADMIT every final reason ends resolution. */
#define P25_PATCH_MEDIA_ENDING_REASONS                                                                                 \
    ((uint32_t)DSD_TG_POLICY_BLOCK_CALL_SKIP | (uint32_t)DSD_TG_POLICY_BLOCK_SESSION_AVOID)

static uint32_t
p25_patch_policy_ending_reasons(dsd_p25_patch_resolve_purpose purpose, uint32_t final) {
    return purpose == DSD_P25_PATCH_RESOLVE_MEDIA ? (final & P25_PATCH_MEDIA_ENDING_REASONS) : final;
}

/* A member's decision as the call is judged on it: evaluated, adjusted, then the SG's final reasons merged. */
static void
p25_patch_policy_evaluate_member(const p25_patch_resolve_args* args, uint32_t member, uint32_t final,
                                 dsd_tg_policy_decision* out) {
    p25_patch_policy_evaluate(args, member, out);
    dsd_tg_policy_merge_ota_final(out, final);
}

/* Judge the members of live entry @p idx against the best so far in @p out. */
static void
p25_patch_policy_consider_entry(const p25_patch_resolve_args* args, int idx, uint32_t sg, uint32_t final,
                                dsd_p25_patch_resolution* out) {
    const dsd_state* state = args->state;
    const int count = state->p25_patch_wgid_count[idx] > DSD_P25_PATCH_GROUP_MAX ? DSD_P25_PATCH_GROUP_MAX
                                                                                 : state->p25_patch_wgid_count[idx];
    for (int k = 0; k < count; k++) {
        const uint32_t member = state->p25_patch_wgid[idx][k];
        if (member == 0U || member == sg) {
            continue;
        }
        dsd_tg_policy_decision candidate;
        p25_patch_policy_evaluate_member(args, member, final, &candidate);
        if (!p25_patch_policy_eligible(args->purpose, &candidate)) {
            continue;
        }
        if (!out->eligible || p25_patch_policy_better(args->purpose, &candidate, &out->decision)) {
            out->decision = candidate;
            out->policy_target = member;
            out->eligible = 1;
        }
    }
}

/* Resolve from the SG's (adjusted) decision and its final reasons. */
static void
p25_patch_policy_resolve_from_sg(const p25_patch_resolve_args* args, uint32_t sg,
                                 const dsd_tg_policy_decision* sg_decision, uint32_t final,
                                 dsd_p25_patch_resolution* out) {
    out->decision = *sg_decision;
    out->policy_target = sg;
    out->sg_final_reasons = final;
    out->eligible = 0;
    if (p25_patch_policy_ending_reasons(args->purpose, final) != 0U) {
        return;
    }
    out->eligible = p25_patch_policy_eligible(args->purpose, sg_decision) ? 1 : 0;
    const int count = p25_patch_policy_entry_count(args->state);
    for (int i = 0; i < count; i++) {
        if (p25_patch_policy_entry_live(args->state, i, sg)) {
            p25_patch_policy_consider_entry(args, i, sg, final, out);
        }
    }
}

int
dsd_p25_patch_resolve(const dsd_opts* opts, const dsd_state* state, uint32_t sg, uint32_t src, int encrypted,
                      int data_call, dsd_p25_patch_resolve_purpose purpose, dsd_p25_patch_adjust_fn adjust,
                      const void* adjust_ctx, dsd_p25_patch_resolution* out) {
    if (!out) {
        return -1;
    }
    const p25_patch_resolve_args args = {opts, state, src, encrypted, data_call, purpose, adjust, adjust_ctx};
    dsd_tg_policy_decision sg_decision;
    p25_patch_policy_evaluate(&args, sg, &sg_decision);
    p25_patch_policy_resolve_from_sg(&args, sg, &sg_decision, dsd_tg_policy_ota_final_reasons(state, &sg_decision),
                                     out);
    return 0;
}

/* The full ADMIT resolution's answer to whether it picks @p hold. */
static int
p25_patch_policy_full_admit_picks(const dsd_opts* opts, const dsd_state* state, uint32_t sg, uint32_t src,
                                  uint32_t hold) {
    dsd_p25_patch_resolution resolution;
    return dsd_p25_patch_resolve(opts, state, sg, src, 0, 0, DSD_P25_PATCH_RESOLVE_ADMIT, NULL, NULL, &resolution) == 0
           && resolution.eligible && resolution.policy_target == hold;
}

int
dsd_p25_patch_admits_hold(const dsd_opts* opts, const dsd_state* state, uint32_t sg, uint32_t src) {
    if (!state || sg == 0U || state->tg_hold == 0U) {
        return 0;
    }
    const uint32_t hold = state->tg_hold;
    if (hold != sg && !dsd_p25_patch_is_current_member(state, sg, hold)) {
        return 0;
    }
    const p25_patch_resolve_args args = {opts, state, src, 0, 0, DSD_P25_PATCH_RESOLVE_ADMIT, NULL, NULL};
    dsd_tg_policy_decision sg_decision;
    p25_patch_policy_evaluate(&args, sg, &sg_decision);
    const uint32_t final = dsd_tg_policy_ota_final_reasons(state, &sg_decision);
    if (p25_patch_policy_ending_reasons(DSD_P25_PATCH_RESOLVE_ADMIT, final) != 0U) {
        return 0;
    }
    dsd_tg_policy_decision held = sg_decision;
    if (hold != sg) {
        p25_patch_policy_evaluate_member(&args, hold, final, &held);
    }
    if (!held.tg_hold_match) {
        // Without a matching Hold the held candidate does not rank first: only the full ranking answers.
        return p25_patch_policy_full_admit_picks(opts, state, sg, src, hold);
    }
    return p25_patch_policy_eligible(DSD_P25_PATCH_RESOLVE_ADMIT, &held);
}

/* Keep the stored target with @p decision as the answer. */
static int
p25_patch_policy_keep(dsd_p25_patch_resolution* out, uint32_t target, const dsd_tg_policy_decision* decision,
                      uint32_t final) {
    out->decision = *decision;
    out->policy_target = target;
    out->sg_final_reasons = final;
    out->eligible = 1;
    return 1;
}

int
dsd_p25_patch_media_resolve(const dsd_opts* opts, const dsd_state* state, uint32_t sg, uint32_t stored, uint32_t src,
                            dsd_p25_patch_resolution* out) {
    if (!out) {
        return -1;
    }
    out->policy_target = sg;
    out->sg_final_reasons = 0U;
    out->eligible = 0;
    if (!dsd_p25_patch_sg_live(state, sg)) {
        return 0;
    }
    const p25_patch_resolve_args args = {opts, state, src, 0, 0, DSD_P25_PATCH_RESOLVE_MEDIA, NULL, NULL};
    dsd_tg_policy_decision sg_decision;
    p25_patch_policy_evaluate(&args, sg, &sg_decision);
    const uint32_t final = dsd_tg_policy_ota_final_reasons(state, &sg_decision);
    if (p25_patch_policy_ending_reasons(DSD_P25_PATCH_RESOLVE_MEDIA, final) == 0U) {
        if (stored == sg && dsd_p25_patch_media_eligible(&sg_decision)) {
            return p25_patch_policy_keep(out, sg, &sg_decision, final);
        }
        if (dsd_p25_patch_is_current_member(state, sg, stored)) {
            dsd_tg_policy_decision kept;
            p25_patch_policy_evaluate_member(&args, stored, final, &kept);
            if (dsd_p25_patch_media_eligible(&kept)) {
                return p25_patch_policy_keep(out, stored, &kept, final);
            }
        }
    }
    p25_patch_policy_resolve_from_sg(&args, sg, &sg_decision, final, out);
    return 1;
}

uint32_t
dsd_p25_patch_media_target(const dsd_opts* opts, const dsd_state* state, uint32_t sg, uint32_t stored, uint32_t src) {
    dsd_p25_patch_resolution resolution;
    (void)dsd_p25_patch_media_resolve(opts, state, sg, stored, src, &resolution);
    return resolution.policy_target;
}
