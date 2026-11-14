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
#include <stdint.h>

#include "../audio/dsd_audio_gate_internal.h"

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

static int
p25_patch_policy_sg_live(const dsd_state* state, uint32_t sg) {
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

/* Judge the members of live entry @p idx against the best so far in @p out. */
static void
p25_patch_policy_consider_entry(const p25_patch_resolve_args* args, int idx, uint32_t sg,
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
        p25_patch_policy_evaluate(args, member, &candidate);
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
    // For media the encryption lockout only denies tuning; every other final reason ends resolution on the SG.
    const uint32_t ending =
        args->purpose == DSD_P25_PATCH_RESOLVE_MEDIA ? (final & ~(uint32_t)DSD_TG_POLICY_BLOCK_ENC_LOCKOUT) : final;
    if (ending != 0U) {
        return;
    }
    out->eligible = p25_patch_policy_eligible(args->purpose, sg_decision) ? 1 : 0;
    const int count = p25_patch_policy_entry_count(args->state);
    for (int i = 0; i < count; i++) {
        if (p25_patch_policy_entry_live(args->state, i, sg)) {
            p25_patch_policy_consider_entry(args, i, sg, out);
        }
    }
    if (final != 0U) {
        out->decision.block_reasons |= final;
        out->decision.tune_allowed = 0;
        out->decision.tg_tune_allowed = 0;
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

uint32_t
dsd_p25_patch_media_target(const dsd_opts* opts, const dsd_state* state, uint32_t sg, uint32_t stored, uint32_t src) {
    if (!p25_patch_policy_sg_live(state, sg)) {
        return sg;
    }
    const p25_patch_resolve_args args = {opts, state, src, 0, 0, DSD_P25_PATCH_RESOLVE_MEDIA, NULL, NULL};
    dsd_tg_policy_decision sg_decision;
    p25_patch_policy_evaluate(&args, sg, &sg_decision);
    const uint32_t final = dsd_tg_policy_ota_final_reasons(state, &sg_decision);
    if ((final & ~(uint32_t)DSD_TG_POLICY_BLOCK_ENC_LOCKOUT) != 0U) {
        return sg;
    }
    if (dsd_p25_patch_is_current_member(state, sg, stored)) {
        dsd_tg_policy_decision kept;
        p25_patch_policy_evaluate(&args, stored, &kept);
        if (dsd_p25_patch_media_eligible(&kept)) {
            return stored;
        }
    }
    dsd_p25_patch_resolution resolution;
    p25_patch_policy_resolve_from_sg(&args, sg, &sg_decision, final, &resolution);
    return resolution.policy_target;
}
