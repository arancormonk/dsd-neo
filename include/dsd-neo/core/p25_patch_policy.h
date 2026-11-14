// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The talkgroup policy of a patched P25 call: which of the supergroup and its members judges it.
 *
 * A patched call carries the supergroup (SG) over the air; the patch table (<dsd-neo/core/p25_patch.h>) lists the
 * member work groups (WGs) it currently carries. A patch is heard when the SG or any current member is listened to.
 * dsd_p25_patch_resolve() picks the candidate a call is judged on, from current state only (no cache): the SG, then
 * each member whose membership is current (dsd_p25_patch_members_current()).
 *
 * The SG's final reasons (dsd_tg_policy_ota_final_reasons(): call skip, encryption lockout, session avoid, a mode B/DE
 * row that knows the SG, never the `*` catch-all) end resolution with the SG's decision, except that for media the
 * encryption lockout only denies tuning: members are still judged and the lockout is merged into the winner.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_PATCH_POLICY_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_PATCH_POLICY_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a resolution is for. */
typedef enum {
    /**
     * Following the call: a grant, a voice-start recheck, a held-call description, scan activity or a row-edit
     * release. A candidate needs the talkgroup part of tuning (`tg_tune_allowed`; a blocked source applies to
     * whichever candidate wins, so it never eliminates them). Rank: Hold match, then the talkgroup part of media
     * (dsd_p25_patch_media_eligible()), then priority, then preempt; the SG wins a full tie.
     */
    DSD_P25_PATCH_RESOLVE_ADMIT = 0,
    /**
     * Playing, recording or exporting the call. A candidate needs the talkgroup part of the media gates' audio verdict
     * (a matching Hold, or audio without an allow-list denial); tuning is not needed, so routed audio with group
     * tuning off stays audible. Rank: Hold match, then priority; the SG wins a full tie.
     */
    DSD_P25_PATCH_RESOLVE_MEDIA = 1,
} dsd_p25_patch_resolve_purpose;

/**
 * Adjusts a candidate's fresh decision before anything is judged (the grant path applies its planned encryption-lockout
 * release here). @p target is the candidate's talkgroup, @p ctx the caller's context.
 */
typedef void (*dsd_p25_patch_adjust_fn)(uint32_t target, dsd_tg_policy_decision* decision, const void* ctx);

/** A resolution's answer. */
typedef struct {
    dsd_tg_policy_decision decision; /**< The decision the call is judged by (its `target_id` is policy_target). */
    uint32_t policy_target;          /**< The SG or the chosen member. */
    uint32_t sg_final_reasons;       /**< dsd_tg_policy_ota_final_reasons() of the (adjusted) SG decision. */
    int eligible;                    /**< 1 when policy_target passed the purpose's eligibility. 0 when the SG decision
                                          stands because a final reason ended resolution or no candidate was eligible. */
} dsd_p25_patch_resolution;

/**
 * @brief Resolve the talkgroup policy of a group call on supergroup @p sg.
 *
 * Every candidate is evaluated with dsd_tg_policy_evaluate_group_call() (@p src, @p encrypted, @p data_call) and then
 * passed to @p adjust (may be NULL). With final reasons on the SG, ADMIT ends with the SG decision; MEDIA ends with it
 * too unless the encryption lockout is the only final reason, which is merged into the winner as a tuning denial (its
 * media verdict stands). When no candidate is eligible the SG decision stands. A talkgroup that is no current patch's
 * SG resolves to itself.
 *
 * @return 0 resolved, -1 for NULL @p out.
 */
int dsd_p25_patch_resolve(const dsd_opts* opts, const dsd_state* state, uint32_t sg, uint32_t src, int encrypted,
                          int data_call, dsd_p25_patch_resolve_purpose purpose, dsd_p25_patch_adjust_fn adjust,
                          const void* adjust_ctx, dsd_p25_patch_resolution* out);

/**
 * @brief The talkgroup a media consumer judges a live group call on supergroup @p sg by (keep-or-resolve).
 *
 * @p stored is the call's stored policy target. It is kept while it is a current member of @p sg, the SG has no final
 * reason that ends media resolution, and its decision passes dsd_p25_patch_media_eligible(); otherwise the answer is
 * dsd_p25_patch_resolve() for media (evaluated unencrypted, not a data call, without adjustment). Judged on current
 * state at every call, so expiry and edits need no mutation. @p sg itself when it is no current patch's SG.
 */
uint32_t dsd_p25_patch_media_target(const dsd_opts* opts, const dsd_state* state, uint32_t sg, uint32_t stored,
                                    uint32_t src);

/** @brief Whether @p decision passes the talkgroup part of the media gates' audio verdict (MEDIA eligibility). */
int dsd_p25_patch_media_eligible(const dsd_tg_policy_decision* decision);

/** @brief Whether @p wg is a current member of a live patch on supergroup @p sg (@p wg != @p sg, both non-zero). */
int dsd_p25_patch_is_current_member(const dsd_state* state, uint32_t sg, uint32_t wg);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_P25_PATCH_POLICY_H_H */
